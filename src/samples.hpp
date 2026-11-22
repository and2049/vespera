#ifndef VESPERA_SRC_SAMPLES_HPP
#define VESPERA_SRC_SAMPLES_HPP

#include <cmath>
#include <cstdint>

namespace vespera::detail {

// Whether every sample of `count` planar channels is finite.
inline bool finite_samples(const float *const *channels, uint32_t count, uint32_t frames) noexcept {
  for (uint32_t c = 0; c < count; ++c) {
    for (uint32_t i = 0; i < frames; ++i) {
      if (!std::isfinite(channels[c][i])) {
        return false;
      }
    }
  }
  return true;
}

// Copies `count` planar channels, replacing every non-finite sample with zero. A destination may
// alias its source.
inline void copy_zeroing_non_finite(const float *const *sources, float *const *destinations,
                                    uint32_t count, uint32_t frames) noexcept {
  for (uint32_t c = 0; c < count; ++c) {
    for (uint32_t i = 0; i < frames; ++i) {
      const float sample = sources[c][i];
      destinations[c][i] = std::isfinite(sample) ? sample : 0.0f;
    }
  }
}

} // namespace vespera::detail

#endif
