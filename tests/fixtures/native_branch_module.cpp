#include "native_branch_control.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <string_view>

namespace {
using namespace native_fixture;
constexpr uint32_t main_target = 1;
constexpr uint32_t mix_target = 2;
constexpr uint32_t power_target = 3;
constexpr uint32_t post_target = 4;
constexpr uint32_t extra_target = 5;
constexpr uint32_t required_target = 6;
constexpr bool is_branch = NATIVE_SECTION == 1;
constexpr uint32_t section_target = NATIVE_SECTION == 0   ? main_target
                                    : NATIVE_SECTION == 2 ? post_target
                                                          : extra_target;
constexpr uint32_t default_stage = NATIVE_SECTION == 0 ? 60 : NATIVE_SECTION == 1 ? 10 : 90;
Control control;

constexpr vs_parameter_descriptor params[] = {
    {selector, "Route", "", 0, 1, 1},
    {power, "Power", "", 0, 1, 1},
    {mix, "Mix", "", 0, 1, 0.5f},
    {alternate_mix, "Other", "", 0, 1, 0.5f},
};
constexpr auto metadata = [] {
  std::array<vs_parameter_metadata, 2> values{};
  values[0].parameter_id = selector;
  values[0].kind = VS_PARAMETER_STEPPED;
  values[0].steps = 2;
  values[1].parameter_id = mix;
  values[1].kind = VS_PARAMETER_STEPPED;
  values[1].steps = 101;
  return values;
}();
const vs_module_metadata_api metadata_api{sizeof(vs_module_metadata_api),
                                          VS_MODULE_METADATA_ABI_VERSION, metadata.data(), 2,
                                          sizeof(vs_parameter_metadata)};

struct Instance {
  std::array<float, 4> values{1, 1, 0.5f, 0.5f};
  std::array<float, 7> engine{};
  std::array<float, 2> history{};
  bool composed = false;
};

vs_result create(void **out) {
  *out = new (std::nothrow) Instance;
  return *out ? VS_OK : VS_ERROR_PROCESS;
}
void destroy(void *in) { delete static_cast<Instance *>(in); }
void reset(void *in) { static_cast<Instance *>(in)->history = {}; }
void release(void *) { ++control.releases; }

vs_result get(void *in, uint32_t id, float *out) {
  *out = static_cast<Instance *>(in)->values[id];
  return VS_OK;
}
vs_result set(void *in, uint32_t id, float value) {
  if (id == selector) {
    value = std::round(value);
  } else if (id == mix) {
    value = static_cast<float>(std::round(100.0 * value) / 100.0);
  }
  static_cast<Instance *>(in)->values[id] = value;
  return VS_OK;
}

void corrupt(Fault fault, vs_shared_assignment *out, uint32_t capacity, uint32_t *written) {
  switch (fault) {
  case Fault::foreign_target:
    out[0].target = main_target;
    break;
  case Fault::bad_value:
    out[0].value = std::numeric_limits<float>::infinity();
    break;
  case Fault::duplicate:
    out[1] = out[0];
    *written = 2;
    break;
  case Fault::overflow:
    *written = capacity + 1;
    break;
  case Fault::missing:
    *written = 0;
    break;
  default:
    break;
  }
}

vs_result assignments(void *in, vs_shared_assignment *out, uint32_t capacity, uint32_t *written) {
  auto &self = *static_cast<Instance *>(in);
  if (capacity < (is_branch ? 2u : 1u)) {
    return VS_ERROR_ARGUMENT;
  }
  if constexpr (is_branch) {
    out[0] = {mix_target, 1};
    out[1] = {power_target, 1};
    *written = 2;
  } else {
    out[0] = {section_target, self.values[power]};
    *written = 1;
  }
  return VS_OK;
}
[[maybe_unused]] vs_result native_assignments(void *in, vs_shared_assignment *out,
                                              uint32_t capacity, uint32_t *written) {
  ++control.native_assignments;
  if (capacity < 2) {
    return VS_ERROR_ARGUMENT;
  }
  auto &self = *static_cast<Instance *>(in);
  out[0] = {mix_target, self.values[mix]};
  out[1] = {power_target, self.values[power]};
  *written = 2;
  corrupt(control.assignments, out, capacity, written);
  return VS_OK;
}
vs_result translate(void *in, uint32_t id, float value, vs_shared_assignment *out,
                    uint32_t capacity, uint32_t *written) {
  set(in, id, value);
  *written = 0;
  if (!is_branch && id == power && capacity) {
    out[0] = {section_target, value};
    *written = 1;
  }
  return VS_OK;
}
[[maybe_unused]] vs_result native_translate(void *in, uint32_t id, float value,
                                            vs_shared_assignment *out, uint32_t capacity,
                                            uint32_t *written) {
  ++control.native_translations;
  if (id == selector && std::round(value) != 1.0f) {
    return VS_ERROR_ARGUMENT;
  }
  set(in, id, value);
  *written = 0;
  if ((id == power || id == mix) && capacity) {
    out[0] = {id == mix ? mix_target : power_target, static_cast<Instance *>(in)->values[id]};
    *written = 1;
    corrupt(control.translation, out, capacity, written);
  }
  return VS_OK;
}

vs_result prepare(void *in, const vs_prepare_info *, vs_module_timing *timing) {
  ++control.standalone_prepares;
  auto &self = *static_cast<Instance *>(in);
  self.composed = false;
  reset(in);
  timing->latency_frames = 0;
  timing->tail_frames = 1;
  return VS_OK;
}
vs_result engine_prepare(void *in, const vs_prepare_info *, const vs_host_instance_cache *cache,
                         const char *signature, uint32_t, const vs_shared_target *, uint32_t,
                         uint32_t, const vs_shared_assignment *state, uint32_t count,
                         vs_module_timing *timing) {
  ++control.engine_prepares;
  control.cache_received += cache != nullptr;
  std::snprintf(control.signature, sizeof(control.signature), "%s", signature);
  auto &self = *static_cast<Instance *>(in);
  self.composed = std::string_view(signature).starts_with("native-branch/v1/");
  if (control.fail_prepare) {
    return VS_ERROR_PROCESS;
  }
  self.engine = {};
  reset(in);
  for (uint32_t i = 0; i < count; ++i) {
    self.engine[state[i].target] = state[i].value;
  }
  timing->latency_frames = self.composed ? 17 : 0;
  timing->tail_frames = self.composed ? 29 : 1;
  return VS_OK;
}

vs_result engine_apply(void *in, const vs_shared_assignment *changes, uint32_t count) {
  auto &self = *static_cast<Instance *>(in);
  for (uint32_t i = 0; i < count; ++i) {
    self.engine[changes[i].target] = changes[i].value;
    if (control.fail_apply_once) {
      control.fail_apply_once = false;
      return VS_ERROR_PROCESS;
    }
  }
  return VS_OK;
}

float main_signal(float x) { return x * x + 0.125f; }
float wet_signal(float x) { return 2.0f * x - 0.25f; }
float post_signal(float x, float &history) {
  const float previous = history;
  history = x;
  return x + 0.5f * previous;
}
vs_result process_block(void *in, const vs_process_block *block, const vs_parameter_event *changes,
                        uint32_t count, bool engine) {
  ++control.processes;
  auto &self = *static_cast<Instance *>(in);
  uint32_t e = 0, t = 0;
  for (uint32_t i = 0; i < block->frames; ++i) {
    while (e < block->event_count && block->events[e].frame_offset == i) {
      const auto event = block->events[e++];
      set(in, event.parameter_id, event.value);
      if (engine && !is_branch && event.parameter_id == power) {
        self.engine[section_target] = event.value;
      }
    }
    while (t < count && changes[t].frame_offset == i) {
      const auto change = changes[t++];
      self.engine[change.parameter_id] = change.value;
    }
    for (uint32_t c = 0; c < block->channels; ++c) {
      const float x = block->inputs[c][i];
      float y = x;
      if (self.composed) {
        const float m = self.engine[power_target] >= 0.5f ? self.engine[mix_target] : 0;
        const float main = self.engine[main_target] >= 0.5f ? main_signal(x) : x;
        y = (1 - m) * main + m * wet_signal(x);
        if (self.engine[post_target] >= 0.5f) {
          y = post_signal(y, self.history[c]);
        }
      } else if (is_branch) {
        y = wet_signal(x);
      } else if ((engine ? self.engine[section_target] : self.values[power]) >= 0.5f) {
        if constexpr (NATIVE_SECTION == 0) {
          y = main_signal(x);
        } else if constexpr (NATIVE_SECTION == 2) {
          y = post_signal(x, self.history[c]);
        }
      }
      block->outputs[c][i] = y;
    }
  }
  return VS_OK;
}
vs_result process(void *in, const vs_process_block *block) {
  return process_block(in, block, nullptr, 0, false);
}
vs_result engine_process(void *in, const vs_process_block *block, const vs_parameter_event *changes,
                         uint32_t count) {
  return process_block(in, block, changes, count, true);
}

constexpr uint64_t state_size = sizeof(Instance::values);
vs_result size(void *, uint64_t *out) {
  *out = state_size;
  return VS_OK;
}
vs_result save(void *in, uint8_t *out, uint64_t capacity, uint64_t *written) {
  if (capacity < state_size) {
    return VS_ERROR_ARGUMENT;
  }
  std::memcpy(out, static_cast<Instance *>(in)->values.data(), state_size);
  *written = state_size;
  return VS_OK;
}
vs_result load(void *in, uint32_t version, const uint8_t *data, uint64_t bytes) {
  if (version != 1 || bytes != state_size) {
    return VS_ERROR_ARGUMENT;
  }
  std::memcpy(static_cast<Instance *>(in)->values.data(), data, state_size);
  return VS_OK;
}
const vs_module_state_api state_api{
    sizeof(vs_module_state_api), VS_MODULE_STATE_ABI_VERSION, 1, size, save, load};
const vs_module_parameter_api value_api{sizeof(vs_module_parameter_api),
                                        VS_MODULE_PARAMETER_ABI_VERSION, get, set};
vs_module_api api{sizeof(vs_module_api),
                  VS_MODULE_ABI_VERSION,
                  NATIVE_ID,
                  "Native branch fixture",
                  "1.0.0",
                  VS_LAYOUT_MONO | VS_LAYOUT_STEREO,
                  params,
                  4,
                  create,
                  destroy,
                  prepare,
                  release,
                  reset,
                  process,
                  VS_MODULE_BYPASS_PARAMETER,
                  power};
std::array<vs_shared_target, 3> targets{};
vs_module_shared_api shared{sizeof(vs_module_shared_api),
                            VS_MODULE_SHARED_ABI_VERSION,
                            "test.native/1",
                            default_stage,
                            0,
                            targets.data(),
                            0,
                            sizeof(vs_shared_target),
                            assignments,
                            translate,
                            engine_prepare,
                            engine_process,
                            engine_apply};
[[maybe_unused]] vs_module_shared_branch_api branch{};
}

extern "C" VS_MODULE_EXPORT Control *native_branch_fixture_control() { return &control; }

extern "C" VS_MODULE_EXPORT const vs_module_api *vespera_module_entry(uint32_t version) {
  api.capabilities = VS_MODULE_BYPASS_PARAMETER | (control.midi ? VS_MODULE_MIDI_INPUT : 0u);
  return version == VS_MODULE_ABI_VERSION ? &api : nullptr;
}
extern "C" VS_MODULE_EXPORT const vs_module_parameter_api *
vespera_module_parameter_entry(uint32_t version) {
  return version == VS_MODULE_PARAMETER_ABI_VERSION ? &value_api : nullptr;
}
extern "C" VS_MODULE_EXPORT const vs_module_metadata_api *
vespera_module_metadata_entry(uint32_t version) {
  return version == VS_MODULE_METADATA_ABI_VERSION ? &metadata_api : nullptr;
}
extern "C" VS_MODULE_EXPORT const vs_module_state_api *
vespera_module_state_entry(uint32_t version) {
  return version == VS_MODULE_STATE_ABI_VERSION ? &state_api : nullptr;
}
extern "C" VS_MODULE_EXPORT const vs_module_shared_api *
vespera_module_shared_entry(uint32_t version) {
  if (version != VS_MODULE_SHARED_ABI_VERSION || control.no_shared) {
    return nullptr;
  }
  constexpr auto owned = VS_SHARED_TARGET_OWNED;
  if constexpr (is_branch) {
    targets[0] = {mix_target, "Mix", 0, 1, owned, 0};
    targets[1] = {power_target, "Branch Power", 0, 1, owned, 0};
  } else {
    targets[0] = {section_target, NATIVE_ID, 0, 1, owned, 0};
  }
  shared.target_count = is_branch ? 2 : 1;
  targets[shared.target_count++] = {required_target,       "Model", 0, 1, VS_SHARED_TARGET_REQUIRED,
                                    control.required_value};
  if (control.conflict_role) {
    targets[shared.target_count++] = {mix_target, "Mix", 0, 1, control.conflict_role, 0};
  }
  shared.stage = control.stage ? control.stage : default_stage;
  shared.engine = control.foreign_engine ? "test.foreign/1" : "test.native/1";
  shared.flags = control.leading ? VS_SHARED_LEADING_ONLY : 0u;
  return &shared;
}
#ifndef NATIVE_OLDER
extern "C" VS_MODULE_EXPORT const vs_module_shared_branch_api *
vespera_module_shared_branch_entry(uint32_t version) {
  if (!is_branch || version != VS_MODULE_SHARED_BRANCH_ABI_VERSION ||
      control.descriptor == Fault::absent) {
    return nullptr;
  }
  branch = {sizeof(branch),
            version,
            mix,
            VS_SHARED_BRANCH_CROSSFADE,
            90,
            control.static_branch ? 0u : VS_SHARED_BRANCH_HAS_SELECTOR,
            selector,
            1,
            native_assignments,
            native_translate};
  switch (control.descriptor) {
  case Fault::short_descriptor:
    branch.struct_size = 8;
    break;
  case Fault::bad_version:
    ++branch.abi_version;
    break;
  case Fault::bad_callback:
    branch.native_translate = nullptr;
    break;
  case Fault::bad_law:
    ++branch.mix_law;
    break;
  case Fault::bad_flags:
    branch.flags = 2;
    break;
  case Fault::bad_fence:
    branch.first_after_stage = default_stage;
    break;
  case Fault::bad_mix:
    branch.mix_parameter = power;
    break;
  case Fault::bad_selector:
    branch.selector_value = 0.5f;
    break;
  default:
    break;
  }
  return &branch;
}
#endif
