// A module whose saved state holds settings that are not parameters: a bias and a polarity that
// only state loading can change. Verifies that hosts carry opaque state into live processing
// rather than reconstructing modules from parameter values alone.
#include <vespera/module.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <new>

#ifndef OPAQUE_MODULE_ID
#define OPAQUE_MODULE_ID "dev.vespera.test.opaque-state"
#endif

// Nonzero makes every prepare fail, so tests can make a host's commit fail after validation.
namespace {
int fail_prepare = 0;
}

extern "C" VS_MODULE_EXPORT int *opaque_state_prepare_control() { return &fail_prepare; }

namespace {
constexpr uint32_t schema = 3;
constexpr size_t state_bytes = 16;

struct Instance {
  float enabled = 1;
  float gain = 1;
  float bias = 0;
  float polarity = 1;
  bool prepared = false;
};

constexpr std::array<vs_parameter_descriptor, 2> parameters{{
    {1, "Enabled", "bool", 0, 1, 1},
    {2, "Gain", "", 0, 2, 1},
}};

bool valid(uint32_t id, float value) {
  for (const auto &p : parameters) {
    if (p.id == id) {
      return std::isfinite(value) && value >= p.minimum && value <= p.maximum;
    }
  }
  return false;
}

vs_result create(void **out) {
  if (!out) {
    return VS_ERROR_ARGUMENT;
  }
  *out = new (std::nothrow) Instance;
  return *out ? VS_OK : VS_ERROR_STATE;
}
void destroy(void *instance) { delete static_cast<Instance *>(instance); }
vs_result prepare(void *instance, const vs_prepare_info *, vs_module_timing *timing) {
  if (fail_prepare) {
    return VS_ERROR_STATE;
  }
  static_cast<Instance *>(instance)->prepared = true;
  if (timing) {
    timing->latency_frames = 0;
    timing->tail_frames = 0;
  }
  return VS_OK;
}
void release(void *instance) { static_cast<Instance *>(instance)->prepared = false; }
void reset(void *) {}

void apply(Instance &self, uint32_t id, float value) {
  (id == 1 ? self.enabled : self.gain) = value;
}

vs_result process(void *instance, const vs_process_block *block) {
  auto &self = *static_cast<Instance *>(instance);
  uint32_t next = 0;
  for (uint32_t i = 0; i < block->frames; ++i) {
    while (next < block->event_count && block->events[next].frame_offset == i) {
      const auto &event = block->events[next++];
      if (valid(event.parameter_id, event.value)) {
        apply(self, event.parameter_id, event.value);
      }
    }
    for (uint32_t c = 0; c < block->channels; ++c) {
      const float input = block->inputs[c][i];
      block->outputs[c][i] =
          self.enabled >= 0.5f ? input * self.gain * self.polarity + self.bias : input;
    }
  }
  return VS_OK;
}

vs_result get_value(void *instance, uint32_t id, float *value) {
  if (!instance || !value || !valid(id, 0)) {
    return VS_ERROR_ARGUMENT;
  }
  const auto &self = *static_cast<Instance *>(instance);
  *value = id == 1 ? self.enabled : self.gain;
  return VS_OK;
}

vs_result set_value(void *instance, uint32_t id, float value) {
  if (!instance || !valid(id, value)) {
    return VS_ERROR_ARGUMENT;
  }
  apply(*static_cast<Instance *>(instance), id, value);
  return VS_OK;
}

vs_result size(void *, uint64_t *bytes) {
  *bytes = state_bytes;
  return VS_OK;
}

vs_result save(void *instance, uint8_t *buffer, uint64_t capacity, uint64_t *written) {
  if (!instance || !buffer || !written || capacity < state_bytes) {
    return VS_ERROR_ARGUMENT;
  }
  const auto &self = *static_cast<Instance *>(instance);
  const std::array<float, 4> values{self.enabled, self.gain, self.bias, self.polarity};
  for (size_t i = 0; i < values.size(); ++i) {
    const auto bits = std::bit_cast<uint32_t>(values[i]);
    for (size_t b = 0; b < 4; ++b) {
      buffer[i * 4 + b] = static_cast<uint8_t>(bits >> (8 * b));
    }
  }
  *written = state_bytes;
  return VS_OK;
}

vs_result load(void *instance, uint32_t version, const uint8_t *buffer, uint64_t bytes) {
  if (!instance || !buffer) {
    return VS_ERROR_ARGUMENT;
  }
  if (version != schema) {
    return VS_ERROR_UNSUPPORTED;
  }
  auto &self = *static_cast<Instance *>(instance);
  if (self.prepared) {
    return VS_ERROR_STATE;
  }
  if (bytes != state_bytes) {
    return VS_ERROR_ARGUMENT;
  }
  std::array<float, 4> values{};
  for (size_t i = 0; i < values.size(); ++i) {
    uint32_t bits = 0;
    for (size_t b = 0; b < 4; ++b) {
      bits |= static_cast<uint32_t>(buffer[i * 4 + b]) << (8 * b);
    }
    values[i] = std::bit_cast<float>(bits);
  }
  if (!valid(1, values[0]) || !valid(2, values[1]) || !std::isfinite(values[2]) ||
      std::abs(values[2]) > 0.5f || (values[3] != 1 && values[3] != -1)) {
    return VS_ERROR_ARGUMENT;
  }
  self.enabled = values[0];
  self.gain = values[1];
  self.bias = values[2];
  self.polarity = values[3];
  return VS_OK;
}

const vs_module_api api{sizeof(vs_module_api),
                        VS_MODULE_ABI_VERSION,
                        OPAQUE_MODULE_ID,
                        "Opaque State",
                        "2.1.0",
                        VS_LAYOUT_MONO | VS_LAYOUT_STEREO,
                        parameters.data(),
                        static_cast<uint32_t>(parameters.size()),
                        create,
                        destroy,
                        prepare,
                        release,
                        reset,
                        process,
                        VS_MODULE_BYPASS_PARAMETER,
                        1};
const vs_module_state_api state_api{
    sizeof(vs_module_state_api), VS_MODULE_STATE_ABI_VERSION, schema, size, save, load};
const vs_module_parameter_api parameter_api{sizeof(vs_module_parameter_api),
                                            VS_MODULE_PARAMETER_ABI_VERSION, get_value, set_value};
}

extern "C" VS_MODULE_EXPORT const vs_module_api *vespera_module_entry(uint32_t version) {
  return version == VS_MODULE_ABI_VERSION ? &api : nullptr;
}
extern "C" VS_MODULE_EXPORT const vs_module_state_api *
vespera_module_state_entry(uint32_t version) {
  return version == VS_MODULE_STATE_ABI_VERSION ? &state_api : nullptr;
}
extern "C" VS_MODULE_EXPORT const vs_module_parameter_api *
vespera_module_parameter_entry(uint32_t version) {
  return version == VS_MODULE_PARAMETER_ABI_VERSION ? &parameter_api : nullptr;
}
