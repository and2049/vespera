// A Special FX module on the module kit with two routes for vespera-module-check tests. Route 0 is
// serial: the module mixes its own shaped signal with the input by Mix and bypasses exactly to the
// input. Route 1 is a branch the host mixes: the module outputs only its shaped signal, whatever
// Mix and Enabled say. FIXTURE_STATIC_BRANCH always takes route 1. One defect at a time:
//   FIXTURE_FOLLOWS_MIX  the branch output is scaled by Mix
//   FIXTURE_LEAKS_DRY    the branch outputs the input while Enabled is off
// FIXTURE_STEPPED_MIX snaps control writes and events to 101 steps; FIXTURE_UNSNAPPED_MIX
// describes the same steps but stores values unchanged.
#include <vespera/kit/module.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#ifndef FIXTURE_ID
#define FIXTURE_ID "test.module-check.routed"
#endif

namespace {
namespace kit = vespera::kit;

constexpr const char *routes[] = {"Serial", "Branch"};
constexpr uint32_t mix_id = 2;
constexpr auto mix_knob = kit::knob(mix_id, "Mix", 0.5f);
#if defined(FIXTURE_STEPPED_MIX) || defined(FIXTURE_UNSNAPPED_MIX)
constexpr auto mix_parameter = mix_knob.steps(101);
#else
constexpr auto mix_parameter = mix_knob;
#endif

struct Routed {
  static constexpr const char *id = FIXTURE_ID;
  static constexpr const char *name = "Routed";
  static constexpr const char *version = "1.0.0";
  static constexpr uint32_t layouts = VS_LAYOUT_MONO | VS_LAYOUT_STEREO;
  static constexpr auto bypass = kit::BypassPolicy::custom;
  static constexpr auto parameters =
      kit::parameters(kit::toggle(1, "Enabled", true).bypass(), mix_parameter,
                      kit::choice(3, "Route", routes), kit::knob(4, "Gain", 0.5f));

  struct Processor {
    static constexpr uint32_t latency_frames = 0;
    static constexpr uint64_t tail_frames = 0;
    float enabled;
    float mix;
    float route;
    float gain;

    explicit Processor(const kit::Setup &setup)
        : enabled(setup.values[0]), mix(setup.values[1]), route(setup.values[2]),
          gain(setup.values[3]) {}
    void reset() noexcept {}
    void set_parameter(size_t index, float value) noexcept {
      (index == 0 ? enabled : index == 1 ? mix : index == 2 ? route : gain) = value;
    }

    bool branch() const noexcept {
#ifdef FIXTURE_STATIC_BRANCH
      return true;
#else
      return route >= 0.5f;
#endif
    }

    float sample(float input) const noexcept {
      const float shaped = gain * std::tanh(2.0f * input);
      if (branch()) {
#if defined(FIXTURE_FOLLOWS_MIX)
        return mix * shaped;
#elif defined(FIXTURE_LEAKS_DRY)
        return enabled >= 0.5f ? shaped : input;
#else
        return shaped;
#endif
      }
      return enabled >= 0.5f ? input + mix * (shaped - input) : input;
    }

    kit::Frame process_frame(const kit::Frame &input) noexcept {
      return {sample(input[0]), sample(input[1])};
    }
  };
};

#ifdef FIXTURE_STEPPED_MIX
using Module = kit::Module<Routed>;

float canonical_mix(uint32_t id, float value) {
  if (id == mix_id && value >= 0.0f && value <= 1.0f) {
    constexpr double intervals = mix_parameter.step_count - 1;
    return static_cast<float>(std::round(value * intervals) / intervals);
  }
  return value;
}

vs_result set_value(void *instance, uint32_t id, float value) {
  return Module::parameter_api.set_value(instance, id, canonical_mix(id, value));
}

vs_result process(void *instance, const vs_process_block *block) {
  if (!block || block->struct_size < VS_PROCESS_BLOCK_V1_SIZE || !block->events ||
      block->event_count == 0) {
    return Module::api.process(instance, block);
  }
  std::array<vs_parameter_event, 256> events;
  if (block->event_count > events.size()) {
    return VS_ERROR_ARGUMENT;
  }
  for (uint32_t i = 0; i < block->event_count; ++i) {
    events[i] = block->events[i];
    events[i].value = canonical_mix(events[i].parameter_id, events[i].value);
  }
  vs_process_block copy{};
  std::memcpy(&copy, block, std::min<size_t>(block->struct_size, sizeof(copy)));
  copy.events = events.data();
  return Module::api.process(instance, &copy);
}

const vs_module_api module_api = [] {
  vs_module_api snapping = Module::api;
  snapping.process = process;
  return snapping;
}();

const vs_module_parameter_api parameter_api = [] {
  vs_module_parameter_api snapping = Module::parameter_api;
  snapping.set_value = set_value;
  return snapping;
}();
#endif
}

#ifdef FIXTURE_STEPPED_MIX
extern "C" VS_MODULE_EXPORT const vs_module_api *vespera_module_entry(uint32_t version) {
  return version == VS_MODULE_ABI_VERSION ? &module_api : nullptr;
}
extern "C" VS_MODULE_EXPORT const vs_module_state_api *
vespera_module_state_entry(uint32_t version) {
  return version == VS_MODULE_STATE_ABI_VERSION ? &Module::state_api : nullptr;
}
extern "C" VS_MODULE_EXPORT const vs_module_parameter_api *
vespera_module_parameter_entry(uint32_t version) {
  return version == VS_MODULE_PARAMETER_ABI_VERSION ? &parameter_api : nullptr;
}
extern "C" VS_MODULE_EXPORT const vs_module_metadata_api *
vespera_module_metadata_entry(uint32_t version) {
  return version == VS_MODULE_METADATA_ABI_VERSION ? &Module::metadata_api : nullptr;
}
extern "C" VS_MODULE_EXPORT const vs_module_services_api *
vespera_module_services_entry(uint32_t version) {
  return version == VS_MODULE_SERVICES_ABI_VERSION ? &Module::services_api : nullptr;
}
extern "C" VS_MODULE_EXPORT const vs_module_files_api *
vespera_module_files_entry(uint32_t version) {
  return version == VS_MODULE_FILES_ABI_VERSION && Module::has_files ? &Module::files_api : nullptr;
}
#else
VESPERA_KIT_MODULE(Routed)
#endif
