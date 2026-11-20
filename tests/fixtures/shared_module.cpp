// Deterministic shared-engine fixture. Each build is one section of a fake multi-section engine:
// section k owns targets 10k (enable), 10k + 1 (gain) and 10k + 2 (offset). The engine applies
// enabled sections in ascending section order as x * gain + offset, so section order matters.
// Compile definitions select the variant; see tests/CMakeLists.txt.
#include <vespera/module.h>

#include <array>
#include <cmath>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "shared_control.h"

#ifndef SHARED_ID
#error SHARED_ID is required
#endif
#ifndef SHARED_ENGINE
#define SHARED_ENGINE "test.engine/1"
#endif
#ifndef SHARED_SECTION
#define SHARED_SECTION SHARED_STAGE
#endif
#ifndef SHARED_LEADING
#define SHARED_LEADING 0
#endif
#ifndef SHARED_MIDI
#define SHARED_MIDI 0
#endif
#ifndef SHARED_RETAINS
#define SHARED_RETAINS 0
#endif
#ifndef SHARED_TEMPO
#define SHARED_TEMPO 0
#endif
#ifndef SHARED_MODEL
#define SHARED_MODEL 0
#endif
#ifndef SHARED_ENABLE_PREFIX
#define SHARED_ENABLE_PREFIX "Section "
#endif
#ifndef SHARED_BAD_ROLE
#define SHARED_BAD_ROLE 0
#endif
#ifndef SHARED_SERVICES
#define SHARED_SERVICES 0
#endif

namespace {
constexpr uint32_t sections = 12;
constexpr uint32_t target_space = 10 * sections;
constexpr uint32_t model_target = 1;
constexpr uint32_t latency = 7;
constexpr uint32_t base = 10 * SHARED_SECTION;

SharedFixtureStats stats{};

const std::array<vs_parameter_descriptor, 3> parameters{
    {{1, "Enabled", "bool", 0, 1, 1}, {2, "Gain", "", 0, 4, 1}, {3, "Offset", "", 0, 1, 0}}};

std::vector<vs_shared_target> make_targets() {
  static const std::array<std::string, sections> enable_names = [] {
    std::array<std::string, sections> names;
    for (uint32_t k = 0; k < sections; ++k) {
      names[k] = SHARED_ENABLE_PREFIX + std::to_string(k) + " Active";
    }
    return names;
  }();
  std::vector<vs_shared_target> result;
  result.push_back({base, enable_names[SHARED_SECTION].c_str(), 0, 1, VS_SHARED_TARGET_OWNED, 0});
  result.push_back(
      {base + 1, "Gain", 0, 4, SHARED_BAD_ROLE ? 9u : uint32_t(VS_SHARED_TARGET_OWNED), 0});
  result.push_back({base + 2, "Offset", 0, 1, VS_SHARED_TARGET_OWNED, 0});
  if (SHARED_MODEL != 0) {
    result.push_back({model_target, "Model", 0, 8, VS_SHARED_TARGET_REQUIRED, float(SHARED_MODEL)});
  } else {
    result.push_back({model_target, "Model", 0, 8, VS_SHARED_TARGET_NEUTRAL, 0});
  }
  for (uint32_t k = 1; k < sections; ++k) {
    if (k != SHARED_SECTION) {
      result.push_back({10 * k, enable_names[k].c_str(), 0, 1, VS_SHARED_TARGET_NEUTRAL, 0});
    }
  }
  return result;
}
const std::vector<vs_shared_target> targets = make_targets();

// Stands in for an expensive hosted engine instance that prepared engines take from and return to
// the host instance cache.
struct Pooled {
  uint32_t uses = 0;
};
constexpr const char *pool_key = "test.engine.pool";

void dispose_pooled(void *pooled, void *payload) {
  delete static_cast<Pooled *>(pooled);
  delete static_cast<std::string *>(payload);
  ++stats.disposed;
}

struct Instance {
  std::array<float, 3> values{1, 1, 0};
  std::array<float, target_space> engine{};
  bool prepared = false;
  uint32_t channels = 0;
  uint32_t max_frames = 0;
  const vs_host_instance_cache *cache = nullptr;
  Pooled *pooled = nullptr;
  std::string *origin = nullptr;
  std::string signature;
  bool retired = false;
  const vs_host_services *services = nullptr;
  vs_work *work = nullptr;
  void *current = nullptr;
};

vs_result run_work(void *, const vs_work_run *run, void **result) {
  *result = new uint64_t(run->token);
  return VS_OK;
}
void dispose_work(void *, void *result) { delete static_cast<uint64_t *>(result); }

void close_work(Instance &self) {
  if (self.work) {
    self.services->close_work(self.services->context, self.work, &self.current, 1);
    self.work = nullptr;
    self.current = nullptr;
    ++stats.works_closed;
  }
}

void return_pooled(Instance &self) {
  if (!self.pooled) {
    return;
  }
  if (self.cache && !self.retired) {
    self.cache->release(self.cache->context, pool_key, self.signature.c_str(), self.pooled,
                        self.origin, dispose_pooled);
  } else {
    dispose_pooled(self.pooled, self.origin);
  }
  self.pooled = nullptr;
  self.origin = nullptr;
  self.cache = nullptr;
}

uint32_t parameter_index(uint32_t id) { return id >= 1 && id <= 3 ? id - 1 : 3; }

vs_result create(void **instance) {
  if (!instance) {
    return VS_ERROR_ARGUMENT;
  }
  *instance = new (std::nothrow) Instance;
  return *instance ? VS_OK : VS_ERROR_PROCESS;
}
void destroy(void *instance) {
  return_pooled(*static_cast<Instance *>(instance));
  delete static_cast<Instance *>(instance);
}

void own_state(Instance &self) {
  self.engine.fill(0);
  for (uint32_t k = 1; k < sections; ++k) {
    self.engine[10 * k + 1] = 1;
  }
  self.engine[base] = self.values[0];
  self.engine[base + 1] = self.values[1];
  self.engine[base + 2] = self.values[2];
}

vs_result check_info(const vs_prepare_info *info, vs_module_timing *timing) {
  return info && timing && info->struct_size >= sizeof(*info) && info->max_frames &&
                 (info->channels == 1 || info->channels == 2)
             ? VS_OK
             : VS_ERROR_ARGUMENT;
}

vs_result prepare(void *instance, const vs_prepare_info *info, vs_module_timing *timing) {
  auto &self = *static_cast<Instance *>(instance);
  if (check_info(info, timing) != VS_OK) {
    return VS_ERROR_ARGUMENT;
  }
  if (stats.fail_prepare) {
    return VS_ERROR_PROCESS;
  }
  own_state(self);
  self.channels = info->channels;
  self.max_frames = info->max_frames;
  self.prepared = true;
  timing->latency_frames = latency;
  timing->tail_frames = 0;
  ++stats.prepares;
  ++stats.live;
  return VS_OK;
}

void release(void *instance) {
  auto &self = *static_cast<Instance *>(instance);
  if (self.prepared) {
    --stats.live;
  }
  self.prepared = false;
  close_work(self);
  return_pooled(self);
}
void reset(void *) { ++stats.resets; }

void render(Instance &self, const vs_process_block &block, uint32_t from, uint32_t to) {
  for (uint32_t c = 0; c < block.channels; ++c) {
    for (uint32_t f = from; f < to; ++f) {
      float x = block.inputs[c][f];
      for (uint32_t k = 1; k < sections; ++k) {
        if (self.engine[10 * k] >= .5f) {
          x = x * self.engine[10 * k + 1] + self.engine[10 * k + 2];
        }
      }
      block.outputs[c][f] = x;
    }
  }
}

vs_result run(Instance &self, const vs_process_block *block, const vs_parameter_event *changes,
              uint32_t change_count) {
  if (!self.prepared || !block || block->channels != self.channels ||
      block->frames > self.max_frames) {
    return VS_ERROR_STATE;
  }
  if (self.work) {
    uint64_t token = 0;
    stats.adopted += self.services->adopt(self.services->context, self.work, &self.current, &token);
  }
  const bool has_midi =
      block->struct_size >= offsetof(vs_process_block, midi_event_size) + sizeof(uint32_t);
  const uint32_t midi = has_midi ? block->midi_event_count : 0;
  if (midi && !SHARED_MIDI) {
    return VS_ERROR_ARGUMENT;
  }
  stats.midi_events += midi;
  uint32_t offset = 0, e = 0, t = 0;
  while (offset < block->frames) {
    while (e < block->event_count && block->events[e].frame_offset == offset) {
      const auto &event = block->events[e++];
      const uint32_t i = parameter_index(event.parameter_id);
      if (i > 2) {
        return VS_ERROR_ARGUMENT;
      }
      self.values[i] = event.value;
      self.engine[base + i] = event.value;
    }
    while (t < change_count && changes[t].frame_offset == offset) {
      const auto &change = changes[t++];
      if (change.parameter_id >= target_space) {
        return VS_ERROR_ARGUMENT;
      }
      self.engine[change.parameter_id] = change.value;
      ++stats.target_events;
    }
    uint32_t end = block->frames;
    if (e < block->event_count) {
      end = std::min(end, block->events[e].frame_offset);
    }
    if (t < change_count) {
      end = std::min(end, changes[t].frame_offset);
    }
    render(self, *block, offset, end);
    offset = end;
  }
  return VS_OK;
}

vs_result process(void *instance, const vs_process_block *block) {
  return run(*static_cast<Instance *>(instance), block, nullptr, 0);
}

vs_result get_value(void *instance, uint32_t id, float *value) {
  const uint32_t i = parameter_index(id);
  if (!value || i > 2) {
    return VS_ERROR_ARGUMENT;
  }
  *value = static_cast<Instance *>(instance)->values[i];
  return VS_OK;
}

vs_result set_value(void *instance, uint32_t id, float value) {
  auto &self = *static_cast<Instance *>(instance);
  const uint32_t i = parameter_index(id);
  if (i > 2) {
    return VS_ERROR_ARGUMENT;
  }
  self.values[i] = value;
  if (self.prepared) {
    self.engine[base + i] = value;
  }
  return VS_OK;
}

vs_result assignments(void *instance, vs_shared_assignment *out, uint32_t capacity,
                      uint32_t *written) {
  auto &self = *static_cast<Instance *>(instance);
  if (capacity < 3 || !out || !written) {
    return VS_ERROR_ARGUMENT;
  }
  for (uint32_t i = 0; i < 3; ++i) {
    out[i] = {base + i, self.values[i]};
  }
  *written = 3;
  return VS_OK;
}

vs_result translate(void *instance, uint32_t id, float value, vs_shared_assignment *out,
                    uint32_t capacity, uint32_t *written) {
  const uint32_t i = parameter_index(id);
  if (i > 2 || capacity < 1 || !out || !written) {
    return VS_ERROR_ARGUMENT;
  }
  static_cast<Instance *>(instance)->values[i] = value;
  out[0] = {base + i, value};
  *written = 1;
  return VS_OK;
}

vs_result engine_prepare(void *instance, const vs_prepare_info *info,
                         const vs_host_instance_cache *cache, const char *signature, uint32_t flags,
                         const vs_shared_target *, uint32_t target_count, uint32_t target_size,
                         const vs_shared_assignment *state, uint32_t state_count,
                         vs_module_timing *timing) {
  auto &self = *static_cast<Instance *>(instance);
  if (check_info(info, timing) != VS_OK || !signature || target_size < sizeof(vs_shared_target)) {
    return VS_ERROR_ARGUMENT;
  }
  if (stats.fail_prepare) {
    return VS_ERROR_PROCESS;
  }
  self.engine.fill(0);
  for (uint32_t k = 1; k < sections; ++k) {
    self.engine[10 * k + 1] = 1;
  }
  for (uint32_t i = 0; i < state_count; ++i) {
    if (state[i].target >= target_space) {
      return VS_ERROR_ARGUMENT;
    }
    self.engine[state[i].target] = state[i].value;
  }
  self.channels = info->channels;
  self.max_frames = info->max_frames;
  self.prepared = true;
  if (cache) {
    void *payload = nullptr;
    self.cache = cache;
    self.signature = signature;
    self.retired = (flags & VS_SHARED_RETAINS_AUDIO) != 0;
    self.pooled =
        static_cast<Pooled *>(cache->acquire(cache->context, pool_key, signature, &payload));
    self.origin = static_cast<std::string *>(payload);
    if (self.pooled) {
      ++stats.reused;
      if (*self.origin != SHARED_ID) {
        ++stats.reused_foreign;
      }
    } else {
      self.pooled = new Pooled;
      self.origin = new std::string(SHARED_ID);
      ++stats.pool_created;
    }
    ++self.pooled->uses;
  }
  if (self.services) {
    const vs_work_desc desc{sizeof(vs_work_desc), nullptr, run_work, dispose_work};
    self.work = self.services->open_work(self.services->context, &desc);
    if (self.work) {
      ++stats.works_opened;
      self.services->request(self.services->context, self.work, 1);
    }
  }
  timing->latency_frames = latency;
  timing->tail_frames = 0;
  ++stats.engine_prepares;
  ++stats.live;
  stats.last_cache = cache;
  stats.last_flags = flags;
  stats.last_target_count = target_count;
  stats.last_model = self.engine[model_target];
  std::strncpy(stats.last_signature, signature, sizeof(stats.last_signature) - 1);
  return VS_OK;
}

vs_result engine_process(void *instance, const vs_process_block *block,
                         const vs_parameter_event *changes, uint32_t change_count) {
  return run(*static_cast<Instance *>(instance), block, changes, change_count);
}

vs_result engine_apply(void *instance, const vs_shared_assignment *values, uint32_t count) {
  auto &self = *static_cast<Instance *>(instance);
  for (uint32_t i = 0; i < count; ++i) {
    if (values[i].target >= target_space) {
      return VS_ERROR_ARGUMENT;
    }
    self.engine[values[i].target] = values[i].value;
    if (stats.fail_apply > 0) {
      --stats.fail_apply;
      return VS_ERROR_PROCESS;
    }
  }
  ++stats.applies;
  return VS_OK;
}

vs_result attach(void *instance, const vs_host_services *services) {
  static_cast<Instance *>(instance)->services = services;
  return VS_OK;
}
vs_result status(void *, vs_module_status *) { return VS_OK; }
const vs_module_services_api services_api{sizeof(vs_module_services_api),
                                          VS_MODULE_SERVICES_ABI_VERSION, attach, status};

const vs_module_api api{sizeof(vs_module_api),
                        VS_MODULE_ABI_VERSION,
                        SHARED_ID,
                        SHARED_ID,
                        "1.0.0",
                        VS_LAYOUT_MONO | VS_LAYOUT_STEREO,
                        parameters.data(),
                        3,
                        create,
                        destroy,
                        prepare,
                        release,
                        reset,
                        process,
                        VS_MODULE_BYPASS_PARAMETER |
                            (SHARED_MIDI ? uint32_t(VS_MODULE_MIDI_INPUT) : 0u) |
                            (SHARED_TEMPO ? uint32_t(VS_MODULE_HOST_TEMPO) : 0u),
                        1};
const vs_module_parameter_api parameter_api{sizeof(vs_module_parameter_api),
                                            VS_MODULE_PARAMETER_ABI_VERSION, get_value, set_value};
const vs_module_shared_api shared_api{sizeof(vs_module_shared_api),
                                      VS_MODULE_SHARED_ABI_VERSION,
                                      SHARED_ENGINE,
                                      SHARED_STAGE,
                                      (SHARED_LEADING ? uint32_t(VS_SHARED_LEADING_ONLY) : 0u) |
                                          (SHARED_RETAINS ? uint32_t(VS_SHARED_RETAINS_AUDIO) : 0u),
                                      targets.data(),
                                      static_cast<uint32_t>(targets.size()),
                                      sizeof(vs_shared_target),
                                      assignments,
                                      translate,
                                      engine_prepare,
                                      engine_process,
                                      engine_apply};
}

extern "C" VS_MODULE_EXPORT const vs_module_api *vespera_module_entry(uint32_t version) {
  return version == VS_MODULE_ABI_VERSION ? &api : nullptr;
}
extern "C" VS_MODULE_EXPORT const vs_module_parameter_api *
vespera_module_parameter_entry(uint32_t version) {
  return version == VS_MODULE_PARAMETER_ABI_VERSION ? &parameter_api : nullptr;
}
extern "C" VS_MODULE_EXPORT const vs_module_shared_api *
vespera_module_shared_entry(uint32_t version) {
  return version == VS_MODULE_SHARED_ABI_VERSION ? &shared_api : nullptr;
}
extern "C" VS_MODULE_EXPORT SharedFixtureStats *shared_fixture_stats() { return &stats; }
#if SHARED_SERVICES
extern "C" VS_MODULE_EXPORT const vs_module_services_api *
vespera_module_services_entry(uint32_t version) {
  return version == VS_MODULE_SERVICES_ABI_VERSION ? &services_api : nullptr;
}
#endif
