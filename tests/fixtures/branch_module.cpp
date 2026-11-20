#include <vespera/module.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

#include "branch_control.h"

#ifndef BRANCH_ID
#define BRANCH_ID "test.branch"
#endif
#ifndef BRANCH_VALUES
#define BRANCH_VALUES 1
#endif
#ifndef BRANCH_BYPASS
#define BRANCH_BYPASS 1
#endif
#ifndef BRANCH_MIDI
#define BRANCH_MIDI 0
#endif
#ifndef BRANCH_STEPPED_MIX
#define BRANCH_STEPPED_MIX 0
#endif

// Output y[n] = gain * x[n - latency] + offset, plus with BRANCH_MIDI the velocity / 127 of the
// held note, released by note off and VS_PROCESS_MIDI_PANIC. Power and Mix never change the
// output, as a branch module's must not.
namespace {

constexpr uint32_t mix_steps = 101;

float stored_value(uint32_t id, float value) {
  if (BRANCH_STEPPED_MIX && id == BRANCH_PARAM_MIX) {
    constexpr double intervals = mix_steps - 1;
    return static_cast<float>(std::round(value * intervals) / intervals);
  }
  return value;
}

const std::array<vs_parameter_descriptor, 8> parameters{{
    {BRANCH_PARAM_POWER, "Power", "", 0, 1, 1},
    {BRANCH_PARAM_GAIN, "Gain", "", -4, 4, 1},
    {BRANCH_PARAM_OFFSET, "Offset", "", -1, 1, 0},
    {BRANCH_PARAM_LATENCY, "Latency", "frames", 0, 16777216, 0},
    {BRANCH_PARAM_TAIL, "Tail", "frames", 0, 1e9f, 0},
    {BRANCH_PARAM_FAULT, "Fault", "", 0, 1, 0},
    {BRANCH_PARAM_MIX, "Mix", "", 0, 1, 0.5f},
    {BRANCH_PARAM_WIDE, "Wide", "", 0, 2, 0},
}};

int index_of(uint32_t id) {
  for (size_t i = 0; i < parameters.size(); ++i) {
    if (parameters[i].id == id) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

struct Instance {
  std::array<float, parameters.size()> values{};
  std::vector<float> line;
  uint32_t length = 0;
  uint32_t position = 0;
  uint32_t channels = 0;
  uint32_t max_frames = 0;
  bool prepared = false;
  float gate = 0.0f;

  Instance() {
    for (size_t i = 0; i < parameters.size(); ++i) {
      values[i] = parameters[i].default_value;
    }
  }

  float value(uint32_t id) const { return values[static_cast<size_t>(index_of(id))]; }
};

vs_result create(void **instance) {
  *instance = new (std::nothrow) Instance;
  return *instance ? VS_OK : VS_ERROR_PROCESS;
}

void destroy(void *instance) { delete static_cast<Instance *>(instance); }

vs_result prepare(void *instance, const vs_prepare_info *info, vs_module_timing *timing) {
  auto &self = *static_cast<Instance *>(instance);
  self.length = static_cast<uint32_t>(self.value(BRANCH_PARAM_LATENCY));
  self.line.assign(static_cast<size_t>(self.length) * info->channels, 0.0f);
  self.position = 0;
  self.channels = info->channels;
  self.max_frames = info->max_frames;
  self.prepared = true;
  timing->latency_frames = self.length;
  timing->tail_frames = static_cast<uint64_t>(self.value(BRANCH_PARAM_TAIL));
  return VS_OK;
}

void release(void *instance) { static_cast<Instance *>(instance)->prepared = false; }

void reset(void *instance) {
  auto &self = *static_cast<Instance *>(instance);
  std::fill(self.line.begin(), self.line.end(), 0.0f);
  self.position = 0;
  self.gate = 0.0f;
}

void render(Instance &self, const vs_process_block &block, uint32_t from, uint32_t to) {
  const float gain = self.value(BRANCH_PARAM_GAIN);
  const float offset = self.value(BRANCH_PARAM_OFFSET);
  const bool fault = self.value(BRANCH_PARAM_FAULT) >= 0.5f;
  uint32_t position = self.position;
  for (uint32_t c = 0; c < block.channels; ++c) {
    position = self.position;
    for (uint32_t f = from; f < to; ++f) {
      float x = block.inputs[c][f];
      if (self.length > 0) {
        float &slot = self.line[static_cast<size_t>(c) * self.length + position];
        std::swap(slot, x);
        position = position + 1 == self.length ? 0 : position + 1;
      }
      block.outputs[c][f] = fault
                                ? std::numeric_limits<float>::quiet_NaN()
                                : (BRANCH_MIDI ? gain * x + offset + self.gate : gain * x + offset);
    }
  }
  self.position = position;
}

vs_result process(void *instance, const vs_process_block *block) {
  auto &self = *static_cast<Instance *>(instance);
  if (!self.prepared || block->channels != self.channels || block->frames > self.max_frames) {
    return VS_ERROR_STATE;
  }
  if (block->flags & VS_PROCESS_DISCONTINUITY) {
    reset(instance);
  }
  const uint32_t midi_count = BRANCH_MIDI ? block->midi_event_count : 0;
  if (!BRANCH_MIDI && (block->midi_event_count != 0 || (block->flags & VS_PROCESS_MIDI_PANIC))) {
    return VS_ERROR_ARGUMENT;
  }
  if (block->flags & VS_PROCESS_MIDI_PANIC) {
    self.gate = 0.0f;
  }
  auto midi = [&](uint32_t index) -> const vs_midi_event & {
    return *reinterpret_cast<const vs_midi_event *>(
        static_cast<const uint8_t *>(block->midi_events) +
        static_cast<size_t>(index) * block->midi_event_size);
  };
  uint32_t e = 0;
  uint32_t m = 0;
  for (uint32_t frame = 0; frame < block->frames;) {
    while (e < block->event_count && block->events[e].frame_offset <= frame) {
      const int i = index_of(block->events[e].parameter_id);
      if (i < 0) {
        return VS_ERROR_ARGUMENT;
      }
      self.values[static_cast<size_t>(i)] =
          stored_value(block->events[e].parameter_id, block->events[e].value);
      ++e;
    }
    while (m < midi_count && midi(m).frame_offset <= frame) {
      const vs_midi_event &event = midi(m++);
      const uint8_t kind = event.status & 0xf0;
      if (kind == 0x90 && event.data2 > 0) {
        self.gate = static_cast<float>(event.data2) / 127.0f;
      } else if (kind == 0x80 || kind == 0x90) {
        self.gate = 0.0f;
      }
    }
    uint32_t end = e < block->event_count ? block->events[e].frame_offset : block->frames;
    if (m < midi_count) {
      end = std::min(end, midi(m).frame_offset);
    }
    render(self, *block, frame, end);
    frame = end;
  }
  return VS_OK;
}

vs_result get_value(void *instance, uint32_t id, float *value) {
  const int i = index_of(id);
  if (i < 0) {
    return VS_ERROR_ARGUMENT;
  }
  *value = static_cast<Instance *>(instance)->values[static_cast<size_t>(i)];
  return VS_OK;
}

vs_result set_value(void *instance, uint32_t id, float value) {
  const int i = index_of(id);
  if (i < 0) {
    return VS_ERROR_ARGUMENT;
  }
  static_cast<Instance *>(instance)->values[static_cast<size_t>(i)] = stored_value(id, value);
  return VS_OK;
}

constexpr uint64_t state_bytes = sizeof(float) * parameters.size();

vs_result state_size(void *, uint64_t *bytes) {
  *bytes = state_bytes;
  return VS_OK;
}

vs_result save(void *instance, uint8_t *buffer, uint64_t capacity, uint64_t *written) {
  if (capacity < state_bytes) {
    return VS_ERROR_ARGUMENT;
  }
  std::memcpy(buffer, static_cast<Instance *>(instance)->values.data(), state_bytes);
  *written = state_bytes;
  return VS_OK;
}

vs_result load(void *instance, uint32_t schema, const uint8_t *buffer, uint64_t bytes) {
  if (schema != 1) {
    return VS_ERROR_UNSUPPORTED;
  }
  if (bytes != state_bytes) {
    return VS_ERROR_ARGUMENT;
  }
  std::array<float, parameters.size()> values{};
  std::memcpy(values.data(), buffer, state_bytes);
  for (size_t i = 0; i < parameters.size(); ++i) {
    if (!std::isfinite(values[i]) || values[i] < parameters[i].minimum ||
        values[i] > parameters[i].maximum) {
      return VS_ERROR_ARGUMENT;
    }
    values[i] = stored_value(parameters[i].id, values[i]);
  }
  static_cast<Instance *>(instance)->values = values;
  return VS_OK;
}

const vs_module_api api{sizeof(vs_module_api),
                        VS_MODULE_ABI_VERSION,
                        BRANCH_ID,
                        "Branch Fixture",
                        "1.0.0",
                        VS_LAYOUT_MONO | VS_LAYOUT_STEREO,
                        parameters.data(),
                        static_cast<uint32_t>(parameters.size()),
                        create,
                        destroy,
                        prepare,
                        release,
                        reset,
                        process,
                        (BRANCH_BYPASS ? uint32_t(VS_MODULE_BYPASS_PARAMETER) : 0u) |
                            (BRANCH_MIDI ? uint32_t(VS_MODULE_MIDI_INPUT) : 0u),
                        BRANCH_BYPASS ? uint32_t(BRANCH_PARAM_POWER) : 0u};
const vs_module_parameter_api parameter_api{sizeof(vs_module_parameter_api),
                                            VS_MODULE_PARAMETER_ABI_VERSION, get_value, set_value};
const vs_module_state_api state_api{
    sizeof(vs_module_state_api), VS_MODULE_STATE_ABI_VERSION, 1, state_size, save, load};

constexpr auto mix_metadata = [] {
  vs_parameter_metadata metadata{};
  metadata.parameter_id = BRANCH_PARAM_MIX;
  metadata.kind = VS_PARAMETER_STEPPED;
  metadata.steps = mix_steps;
  return metadata;
}();
const vs_module_metadata_api metadata_api{sizeof(vs_module_metadata_api),
                                          VS_MODULE_METADATA_ABI_VERSION, &mix_metadata, 1,
                                          sizeof(vs_parameter_metadata)};

}

extern "C" VS_MODULE_EXPORT const vs_module_api *vespera_module_entry(uint32_t version) {
  return version == VS_MODULE_ABI_VERSION ? &api : nullptr;
}

extern "C" VS_MODULE_EXPORT const vs_module_parameter_api *
vespera_module_parameter_entry(uint32_t version) {
  return BRANCH_VALUES && version == VS_MODULE_PARAMETER_ABI_VERSION ? &parameter_api : nullptr;
}

extern "C" VS_MODULE_EXPORT const vs_module_state_api *
vespera_module_state_entry(uint32_t version) {
  return version == VS_MODULE_STATE_ABI_VERSION ? &state_api : nullptr;
}

extern "C" VS_MODULE_EXPORT const vs_module_metadata_api *
vespera_module_metadata_entry(uint32_t version) {
  return BRANCH_STEPPED_MIX && version == VS_MODULE_METADATA_ABI_VERSION ? &metadata_api : nullptr;
}
