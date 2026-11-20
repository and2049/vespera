// A raw-ABI module using host services; see services_control.h for its behaviour.
#include "services_control.h"

#include <cmath>
#include <cstring>
#include <new>

namespace {

ServicesControl control;

const vs_parameter_descriptor parameters[] = {{1, "Level", "", 0.0f, 1.0f, 0.5f}};

void record(char call) {
  std::lock_guard lock(control.calls_mutex);
  const size_t length = std::strlen(control.calls);
  if (length + 1 < sizeof(control.calls)) {
    control.calls[length] = call;
  }
}

struct Instance {
  float level = 0.5f;
  const vs_host_services *services = nullptr;
  vs_work *work = nullptr;
  void *current = nullptr;
  float own = 0.0f;
  bool prepared = false;
  uint32_t channels = 0;
};

vs_result run(void *, const vs_work_run *run, void **result) {
  ++control.runs;
  if (control.hold) {
    ++control.held;
    while (control.hold && !run->cancelled(run)) {
    }
  }
  *result = new float(static_cast<float>(run->token) / 1000.0f);
  return VS_OK;
}

void dispose(void *, void *result) {
  ++control.disposed;
  delete static_cast<float *>(result);
}

vs_result create(void **instance) {
  *instance = new (std::nothrow) Instance;
  ++control.created;
  record('c');
  return *instance ? VS_OK : VS_ERROR_PROCESS;
}

void destroy(void *instance) {
  ++control.destroyed;
  record('d');
  delete static_cast<Instance *>(instance);
}

vs_result prepare(void *instance, const vs_prepare_info *info, vs_module_timing *timing) {
  auto &self = *static_cast<Instance *>(instance);
  record('p');
  {
    std::unique_lock lock(control.prepare_mutex);
    control.prepare_entered = true;
    control.prepare_changed.notify_all();
    control.prepare_changed.wait(lock, [] { return !control.hold_prepare; });
  }
  self.channels = info->channels;
  self.prepared = true;
  timing->latency_frames = control.latency.load();
  if (control.timing_on_prepare) {
    control.latency = 11;
    self.services->notify(self.services->context, VS_NOTIFY_TIMING);
  }
  timing->tail_frames = 0;
  const auto token = static_cast<uint64_t>(std::lround(self.level * 1000.0f));
  if (self.services) {
    const vs_work_desc desc{sizeof(vs_work_desc), nullptr, ::run, dispose};
    self.work = self.services->open_work(self.services->context, &desc);
    if (!self.work) {
      return VS_ERROR_PROCESS;
    }
    self.services->request(self.services->context, self.work, token);
  } else {
    self.own = static_cast<float>(token) / 1000.0f;
  }
  return VS_OK;
}

void release(void *instance) {
  auto &self = *static_cast<Instance *>(instance);
  record('r');
  if (self.work && !control.leave_open) {
    self.services->close_work(self.services->context, self.work, &self.current, 1);
  }
  self.work = nullptr;
  self.current = nullptr;
  self.prepared = false;
}

void reset(void *) {}

vs_result process(void *instance, const vs_process_block *block) {
  auto &self = *static_cast<Instance *>(instance);
  if (!self.prepared) {
    return VS_ERROR_STATE;
  }
  float value = self.own;
  if (self.work) {
    uint64_t token = 0;
    self.services->adopt(self.services->context, self.work, &self.current, &token);
    value = self.current ? *static_cast<float *>(self.current) : 0.0f;
  }
  for (uint32_t c = 0; c < block->channels; ++c) {
    for (uint32_t i = 0; i < block->frames; ++i) {
      block->outputs[c][i] = value;
    }
  }
  return VS_OK;
}

vs_result get_value(void *instance, uint32_t id, float *value) {
  if (id != 1 || !value) {
    return VS_ERROR_ARGUMENT;
  }
  *value = static_cast<Instance *>(instance)->level;
  return VS_OK;
}

vs_result set_value(void *instance, uint32_t id, float value) {
  if (id != 1) {
    return VS_ERROR_ARGUMENT;
  }
  record('v');
  static_cast<Instance *>(instance)->level = value;
  return VS_OK;
}

vs_result state_size(void *, uint64_t *bytes) {
  *bytes = sizeof(float);
  return VS_OK;
}

vs_result save(void *instance, uint8_t *buffer, uint64_t capacity, uint64_t *written) {
  if (capacity < sizeof(float)) {
    return VS_ERROR_ARGUMENT;
  }
  std::memcpy(buffer, &static_cast<Instance *>(instance)->level, sizeof(float));
  *written = sizeof(float);
  return VS_OK;
}

vs_result load(void *instance, uint32_t schema, const uint8_t *buffer, uint64_t bytes) {
  if (schema != 1 || bytes != sizeof(float)) {
    return VS_ERROR_ARGUMENT;
  }
  record('s');
  float level = 0.0f;
  std::memcpy(&level, buffer, sizeof(float));
  if (!std::isfinite(level) || level < 0.0f || level > 1.0f) {
    return VS_ERROR_ARGUMENT;
  }
  static_cast<Instance *>(instance)->level = level;
  return VS_OK;
}

vs_result attach(void *instance, const vs_host_services *services) {
  record('a');
  control.attached = services;
  if (control.attach_result != VS_OK) {
    return control.attach_result;
  }
  static_cast<Instance *>(instance)->services = services;
  return VS_OK;
}

vs_result status(void *, vs_module_status *status) {
  if (control.status_result != VS_OK) {
    return control.status_result;
  }
  status->kind = control.status_kind;
  status->progress = -1.0f;
  std::memcpy(status->message, control.status_message, sizeof(status->message));
  if (!control.terminate_message) {
    std::memset(status->message, 'x', sizeof(status->message));
  }
  return VS_OK;
}

void restore() {
  control.api = {sizeof(vs_module_services_api), VS_MODULE_SERVICES_ABI_VERSION, attach, status};
  control.decline = false;
  control.attach_result = VS_OK;
  control.leave_open = false;
  control.hold = false;
  control.held = 0;
  control.latency = 0;
  control.timing_on_prepare = false;
  {
    std::lock_guard lock(control.prepare_mutex);
    control.hold_prepare = false;
    control.prepare_entered = false;
    control.prepare_changed.notify_all();
  }
  control.status_kind = VS_STATUS_READY;
  std::memset(control.status_message, 0, sizeof(control.status_message));
  control.terminate_message = true;
  control.status_result = VS_OK;
  std::memset(control.calls, 0, sizeof(control.calls));
  control.runs = 0;
  control.disposed = 0;
  control.created = 0;
  control.destroyed = 0;
  control.attached = nullptr;
}

const bool initialised = [] {
  control.restore = restore;
  restore();
  return true;
}();

const vs_module_api api{sizeof(vs_module_api),
                        VS_MODULE_ABI_VERSION,
                        "test.services",
                        "Services",
                        "1.0.0",
                        VS_LAYOUT_MONO | VS_LAYOUT_STEREO,
                        parameters,
                        1,
                        create,
                        destroy,
                        prepare,
                        release,
                        reset,
                        process,
                        0,
                        0};
const vs_module_state_api state_api{
    sizeof(vs_module_state_api), VS_MODULE_STATE_ABI_VERSION, 1, state_size, save, load};
const vs_module_parameter_api parameter_api{sizeof(vs_module_parameter_api),
                                            VS_MODULE_PARAMETER_ABI_VERSION, get_value, set_value};

}

extern "C" VS_MODULE_EXPORT const vs_module_api *vespera_module_entry(uint32_t version) {
  return version == VS_MODULE_ABI_VERSION && initialised ? &api : nullptr;
}
extern "C" VS_MODULE_EXPORT const vs_module_state_api *
vespera_module_state_entry(uint32_t version) {
  return version == VS_MODULE_STATE_ABI_VERSION ? &state_api : nullptr;
}
extern "C" VS_MODULE_EXPORT const vs_module_parameter_api *
vespera_module_parameter_entry(uint32_t version) {
  return version == VS_MODULE_PARAMETER_ABI_VERSION ? &parameter_api : nullptr;
}
extern "C" VS_MODULE_EXPORT const vs_module_services_api *
vespera_module_services_entry(uint32_t version) {
  return version == VS_MODULE_SERVICES_ABI_VERSION && !control.decline ? &control.api : nullptr;
}
extern "C" VS_MODULE_EXPORT ServicesControl *vespera_services_control() { return &control; }
