#ifndef VESPERA_DSP_ALLPASS_HPP
#define VESPERA_DSP_ALLPASS_HPP

#include <vespera/dsp/common.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <vector>

namespace vespera::dsp {

inline constexpr std::size_t maximum_allpass_delay_samples = std::size_t{1} << 22;

// First-order allpass section:
//
//   y[n] = a * x[n] + x[n - 1] - a * y[n - 1]
//   H(z) = (a + z^-1) / (1 + a * z^-1)
//
// |H| is exactly 1 at every frequency for real a: unity gain at DC, -1 (phase inversion) at
// Nyquist, and phase falling monotonically from 0 to -pi radians in between. The impulse
// response is h[0] = a and h[n] = (1 - a^2) * (-a)^(n - 1) for n >= 1, so the worst-case
// peak output is (1 + 2|a|) times the peak input. a = 0 is a plain one-sample delay.
//
// The coefficient must be finite with |a| < 1: a = +-1 would place the pole exactly on the
// unit circle and larger magnitudes move it outside, so those boundary values are rejected
// and a rejected setter leaves the coefficient unchanged. The state is the direct-form
// history (x[n - 1], y[n - 1]) in double precision and is retained across coefficient
// changes, which apply the new a from the current sample on; reset() clears it. Non-finite
// input samples are processed as silence. Coefficient moves belong to the caller's smoothing
// policy. While a moves the section is not allpass, but if |a[n]| <= A < 1 throughout, the
// output stays within (1 + A) / (1 - A) times the input peak and, once input is silent, decays
// at least as fast as A^n; A close to 1 makes that bound arbitrarily large.
class FirstOrderAllpass {
public:
  FirstOrderAllpass() = default;

  explicit FirstOrderAllpass(double coefficient) { set_coefficient(coefficient); }

  // Throws std::invalid_argument and keeps the previous coefficient unless the value is
  // finite with |a| < 1.
  void set_coefficient(double coefficient) {
    if (!is_valid_coefficient(coefficient)) {
      throw std::invalid_argument("First-order allpass coefficient must be finite with |a| < 1");
    }
    coefficient_ = coefficient;
  }

  // Real-time variant of set_coefficient(): rejects invalid values without changing state.
  [[nodiscard]] bool try_set_coefficient(double coefficient) noexcept {
    if (!is_valid_coefficient(coefficient)) {
      return false;
    }
    coefficient_ = coefficient;
    return true;
  }

  void reset() noexcept {
    previous_input_ = 0.0;
    previous_output_ = 0.0;
  }

  float process(float input) noexcept {
    const double x = std::isfinite(input) ? static_cast<double>(input) : 0.0;
    const double y = coefficient_ * (x - previous_output_) + previous_input_;
    previous_input_ = x;
    previous_output_ = y;
    return static_cast<float>(y);
  }

  void process(std::span<float> samples) noexcept {
    for (float &sample : samples) {
      sample = process(sample);
    }
  }

  // Requires output.size() >= input.size(); the spans may be identical but must not otherwise
  // overlap.
  void process(std::span<const float> input, std::span<float> output) noexcept {
    for (std::size_t i = 0; i < input.size(); ++i) {
      output[i] = process(input[i]);
    }
  }

  [[nodiscard]] double coefficient() const noexcept { return coefficient_; }

  [[nodiscard]] static bool is_valid_coefficient(double coefficient) noexcept {
    return std::isfinite(coefficient) && coefficient > -1.0 && coefficient < 1.0;
  }

private:
  double coefficient_ = 0.0;
  double previous_input_ = 0.0;
  double previous_output_ = 0.0;
};

struct AllpassConfig {
  double sample_rate = 48000.0;
  std::size_t delay_samples = 1;
  double gain = 0.5;
};

// Integer-delay Schroeder allpass section:
//
//   y[n] = x[n - D] - g * x[n] + g * y[n - D]
//   H(z) = (z^-D - g) / (1 - g * z^-D)
//
// The direct path carries -g and the feedback path +g. |H| is exactly 1 at every frequency
// for real g, and H(1) = 1, so DC passes with unity gain. The impulse response is h[0] = -g
// with echoes h[m * D] = (1 - g^2) * g^(m - 1) for m >= 1 and zero elsewhere: echo m decays
// as |g|^m and the total impulse energy is exactly 1. Poles sit at |z| = |g|^(1/D), so the
// gain must be finite with |g| < 1 (|g| >= 1 is rejected, leaving the previous gain), and the
// worst-case peak output is (1 + 2|g|) times the peak input.
//
// The loop discipline is structural: two internal D-sample lines hold the input and output
// history, process() reads both at one position and only then writes them and steps, so the
// feedback always spans exactly D samples and zero-delay algebraic feedback cannot arise
// from call order. D must be at least 1 and is fixed at prepare(); changing it requires
// re-preparation. The gain may move in process through try_set_gain(), which applies the new
// g from the current sample on; the recurrence always uses the current g against the raw
// stored histories. While g moves the section is not allpass, but if |g[n]| <= G < 1
// throughout, the output stays within (1 + G) / (1 - G) times the input peak and, once input
// has been silent for D samples, each D-sample echo is at most G times the previous one.
// prepare() allocates and must run while quiescent; a rejected
// configuration leaves the previous state intact. reset() clears both lines in O(D) without
// allocating. Storage is 2 * D floats, about 32 MiB at the 2^22-sample delay maximum.
// Non-finite input samples are processed as silence.
class SchroederAllpass {
public:
  SchroederAllpass() = default;

  explicit SchroederAllpass(const AllpassConfig &config) { prepare(config); }

  // Strong guarantee: validation and allocation happen before any state changes. A successful
  // prepare clears both history lines.
  void prepare(const AllpassConfig &config) {
    validate_sample_rate(config.sample_rate);
    if (config.delay_samples == 0 || config.delay_samples > maximum_allpass_delay_samples) {
      throw std::invalid_argument("Allpass delay must be within [1, 2^22] samples");
    }
    if (!is_valid_gain(config.gain)) {
      throw std::invalid_argument("Allpass gain must be finite with |g| < 1");
    }
    std::vector<float> input(config.delay_samples, 0.0f);
    std::vector<float> output(config.delay_samples, 0.0f);
    input_.swap(input);
    output_.swap(output);
    sample_rate_ = config.sample_rate;
    delay_samples_ = config.delay_samples;
    gain_ = config.gain;
    position_ = 0;
  }

  // Throws std::invalid_argument and keeps the previous gain unless the value is finite with
  // |g| < 1.
  void set_gain(double gain) {
    if (!is_valid_gain(gain)) {
      throw std::invalid_argument("Allpass gain must be finite with |g| < 1");
    }
    gain_ = gain;
  }

  // Real-time variant of set_gain(): rejects invalid values without changing state.
  [[nodiscard]] bool try_set_gain(double gain) noexcept {
    if (!is_valid_gain(gain)) {
      return false;
    }
    gain_ = gain;
    return true;
  }

  // Clears both history lines in O(D) time; capacity, delay and gain are kept.
  void reset() noexcept {
    std::fill(input_.begin(), input_.end(), 0.0f);
    std::fill(output_.begin(), output_.end(), 0.0f);
    position_ = 0;
  }

  float process(float input) noexcept {
    if (input_.empty()) {
      return std::isfinite(input) ? input : 0.0f;
    }
    const double x = std::isfinite(input) ? static_cast<double>(input) : 0.0;
    const double delayed_input = static_cast<double>(input_[position_]);
    const double delayed_output = static_cast<double>(output_[position_]);
    const double y = delayed_input - gain_ * x + gain_ * delayed_output;
    input_[position_] = static_cast<float>(x);
    output_[position_] = static_cast<float>(y);
    position_ = position_ + 1 == input_.size() ? 0 : position_ + 1;
    return static_cast<float>(y);
  }

  void process(std::span<float> samples) noexcept {
    for (float &sample : samples) {
      sample = process(sample);
    }
  }

  // Requires output.size() >= input.size(); the spans may be identical but must not otherwise
  // overlap.
  void process(std::span<const float> input, std::span<float> output) noexcept {
    for (std::size_t i = 0; i < input.size(); ++i) {
      output[i] = process(input[i]);
    }
  }

  [[nodiscard]] bool prepared() const noexcept { return !input_.empty(); }
  [[nodiscard]] double sample_rate() const noexcept { return sample_rate_; }
  [[nodiscard]] std::size_t delay_samples() const noexcept { return delay_samples_; }
  [[nodiscard]] double gain() const noexcept { return gain_; }

  [[nodiscard]] static bool is_valid_gain(double gain) noexcept {
    return std::isfinite(gain) && gain > -1.0 && gain < 1.0;
  }

private:
  std::vector<float> input_;
  std::vector<float> output_;
  double sample_rate_ = 0.0;
  std::size_t delay_samples_ = 0;
  double gain_ = 0.0;
  std::size_t position_ = 0;
};

} // namespace vespera

#endif
