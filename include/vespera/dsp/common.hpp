#ifndef VESPERA_DSP_COMMON_HPP
#define VESPERA_DSP_COMMON_HPP

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace vespera::dsp {

inline constexpr double minimum_sample_rate = 8000.0;
inline constexpr double maximum_sample_rate = 768000.0;
inline constexpr double nyquist_safe_frequency_ratio = 0.45;

inline void validate_sample_rate(double sample_rate) {
  if (!std::isfinite(sample_rate) || sample_rate < minimum_sample_rate ||
      sample_rate > maximum_sample_rate) {
    throw std::invalid_argument("DSP sample rate must be within [8000, 768000] Hz");
  }
}

inline void validate_frequency(double sample_rate, double frequency) {
  validate_sample_rate(sample_rate);
  if (!std::isfinite(frequency) || frequency <= 0.0 || frequency >= sample_rate * 0.5) {
    throw std::invalid_argument("DSP frequency must be positive and below Nyquist");
  }
}

// Lowers `frequency` to nyquist_safe_frequency_ratio times the sample rate when it is above it,
// so fixed voicing frequencies still yield valid filter designs at low sample rates.
// Smaller frequencies are returned unchanged and a NaN frequency stays NaN; this does not
// validate, so the result still goes through the filter factories' checks.
inline double nyquist_safe_frequency(double sample_rate, double frequency) noexcept {
  return std::min(frequency, sample_rate * nyquist_safe_frequency_ratio);
}

}

#endif
