#ifndef VESPERA_DSP_OSCILLATOR_HPP
#define VESPERA_DSP_OSCILLATOR_HPP

#include <vespera/dsp/common.hpp>

#include <cmath>
#include <numbers>
#include <stdexcept>

namespace vespera::dsp {

enum class LfoWaveform { sine, triangle, saw, square };

struct LfoConfig {
  double sample_rate = 48000.0;
  double frequency = 1.0;
  LfoWaveform waveform = LfoWaveform::sine;
  double initial_phase = 0.0;
};

// Naive (non-band-limited) low-frequency oscillator. Phase is normalized to [0, 1) and maps to:
//   sine      sin(2 pi p)
//   triangle  0 at p = 0, +1 at 0.25, -1 at 0.75, back to 0 at 1
//   saw       2p - 1, rising from -1 and jumping back at the cycle start
//   square    +1 for p < 0.5, -1 otherwise
// Every output lies in [-1, 1]. process() returns the value at the current phase and then advances
// by frequency / sample_rate, so a reset oscillator always starts with the initial phase value.
class Lfo {
public:
  explicit Lfo(const LfoConfig &config)
      : config_(validated(config)), increment_(config.frequency / config.sample_rate),
        phase_(config.initial_phase) {}

  // Strong guarantee. The running phase is kept so frequency or waveform changes are
  // phase-continuous; the new initial phase only applies to later resets.
  void configure(const LfoConfig &config) {
    config_ = validated(config);
    increment_ = config.frequency / config.sample_rate;
  }

  void reset() noexcept { phase_ = config_.initial_phase; }

  void set_phase(double phase) {
    validate_phase(phase);
    phase_ = phase;
  }

  float process() noexcept {
    const float output = value_at(config_.waveform, phase_);
    phase_ += increment_;
    if (phase_ >= 1.0) {
      phase_ -= 1.0;
    }
    return output;
  }

  [[nodiscard]] double phase() const noexcept { return phase_; }
  [[nodiscard]] double phase_increment() const noexcept { return increment_; }
  [[nodiscard]] const LfoConfig &config() const noexcept { return config_; }

  // phase must be finite and in [0, 1), as it is for process().
  [[nodiscard]] static float value_at(LfoWaveform waveform, double phase) noexcept {
    switch (waveform) {
    case LfoWaveform::sine:
      return static_cast<float>(std::sin(2.0 * std::numbers::pi * phase));
    case LfoWaveform::triangle:
      if (phase < 0.25) {
        return static_cast<float>(4.0 * phase);
      }
      if (phase < 0.75) {
        return static_cast<float>(2.0 - 4.0 * phase);
      }
      return static_cast<float>(4.0 * phase - 4.0);
    case LfoWaveform::saw:
      return static_cast<float>(2.0 * phase - 1.0);
    case LfoWaveform::square:
      return phase < 0.5 ? 1.0f : -1.0f;
    }
    return 0.0f;
  }

private:
  static void validate_phase(double phase) {
    if (!std::isfinite(phase) || phase < 0.0 || phase >= 1.0) {
      throw std::invalid_argument("LFO phase must be within [0, 1)");
    }
  }

  static const LfoConfig &validated(const LfoConfig &config) {
    validate_sample_rate(config.sample_rate);
    if (!std::isfinite(config.frequency) || config.frequency < 0.0 ||
        config.frequency > config.sample_rate * 0.5) {
      throw std::invalid_argument("LFO frequency must be within [0, Nyquist]");
    }
    switch (config.waveform) {
    case LfoWaveform::sine:
    case LfoWaveform::triangle:
    case LfoWaveform::saw:
    case LfoWaveform::square:
      break;
    default:
      throw std::invalid_argument("Unknown LFO waveform");
    }
    validate_phase(config.initial_phase);
    return config;
  }

  LfoConfig config_;
  double increment_;
  double phase_;
};

}

#endif
