#ifndef VESPERA_DSP_DYNAMICS_HPP
#define VESPERA_DSP_DYNAMICS_HPP

#include <vespera/dsp/common.hpp>
#include <vespera/dsp/utility.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace vespera::dsp {

inline constexpr double maximum_envelope_time_seconds = 60.0;

struct EnvelopeConfig {
  double sample_rate = 48000.0;
  double attack_seconds = 0.01;
  double release_seconds = 0.1;
};

// Peak envelope follower with one-pole attack and release smoothing. A time constant of T seconds
// moves the envelope 1 - 1/e of the way toward a held input magnitude after T * sample_rate
// samples; a time of zero makes that direction instantaneous. Inputs are expected to be finite
// normalized samples; non-finite inputs are treated as silence so the state always stays finite.
class EnvelopeFollower {
public:
  explicit EnvelopeFollower(const EnvelopeConfig &config)
      : attack_coefficient_(coefficient_for(config.sample_rate, config.attack_seconds)),
        release_coefficient_(coefficient_for(config.sample_rate, config.release_seconds)),
        config_(config) {}

  // Strong guarantee: on invalid configuration nothing changes. The current envelope is kept so
  // retuning does not cause a discontinuity.
  void configure(const EnvelopeConfig &config) {
    const double attack = coefficient_for(config.sample_rate, config.attack_seconds);
    const double release = coefficient_for(config.sample_rate, config.release_seconds);
    attack_coefficient_ = attack;
    release_coefficient_ = release;
    config_ = config;
  }

  void reset() noexcept { envelope_ = 0.0; }

  float process(float input) noexcept {
    const double magnitude = std::isfinite(input) ? std::fabs(static_cast<double>(input)) : 0.0;
    const double coefficient = magnitude > envelope_ ? attack_coefficient_ : release_coefficient_;
    envelope_ = magnitude + coefficient * (envelope_ - magnitude);
    if (envelope_ < denormal_floor) {
      envelope_ = 0.0;
    }
    return static_cast<float>(envelope_);
  }

  [[nodiscard]] float value() const noexcept { return static_cast<float>(envelope_); }
  [[nodiscard]] const EnvelopeConfig &config() const noexcept { return config_; }
  [[nodiscard]] double attack_coefficient() const noexcept { return attack_coefficient_; }
  [[nodiscard]] double release_coefficient() const noexcept { return release_coefficient_; }

  static double coefficient_for(double sample_rate, double time_seconds) {
    validate_sample_rate(sample_rate);
    if (!std::isfinite(time_seconds) || time_seconds < 0.0 ||
        time_seconds > maximum_envelope_time_seconds) {
      throw std::invalid_argument("Envelope time must be within [0, 60] seconds");
    }
    if (time_seconds == 0.0) {
      return 0.0;
    }
    return std::exp(-1.0 / (time_seconds * sample_rate));
  }

private:
  static constexpr double denormal_floor = 1e-30;

  double attack_coefficient_;
  double release_coefficient_;
  EnvelopeConfig config_;
  double envelope_ = 0.0;
};

struct SoftKneeConfig {
  double threshold_db = -20.0;
  double knee_db = 6.0;
  double ratio = 4.0;
  double maximum_reduction_db = 24.0;
};

// Static compressor curve: maps a linear envelope level to the gain that compresses it. With
// over = level_db - threshold_db and slope = 1 / ratio - 1, the reduction in dB is 0 below the
// knee (over <= -knee_db / 2), slope * over above it (over >= knee_db / 2), and the quadratic
// slope * (over + knee_db / 2)^2 / (2 * knee_db) inside it, which joins both with matching value
// and slope. The reduction is then limited to maximum_reduction_db. A knee of 0 is a hard knee.
//
// The envelope is expected to be a non-negative linear level such as EnvelopeFollower's output.
// Zero, negative and NaN envelopes give unity gain; an infinite envelope gives the maximum
// reduction, except at ratio 1, which is always unity. The gain is always within
// [decibels_to_gain(-maximum_reduction_db), 1].
class SoftKneeGainComputer {
public:
  explicit SoftKneeGainComputer(const SoftKneeConfig &config) { configure(config); }

  // Strong guarantee: on invalid configuration nothing changes.
  void configure(const SoftKneeConfig &config) {
    if (!std::isfinite(config.threshold_db) || !std::isfinite(config.knee_db) ||
        config.knee_db < 0.0) {
      throw std::invalid_argument("Compressor threshold must be finite and knee non-negative");
    }
    if (!std::isfinite(config.ratio) || config.ratio < 1.0) {
      throw std::invalid_argument("Compressor ratio must be finite and at least 1");
    }
    if (!std::isfinite(config.maximum_reduction_db) || config.maximum_reduction_db < 0.0) {
      throw std::invalid_argument("Compressor maximum reduction must be finite and non-negative");
    }
    config_ = config;
    slope_ = 1.0 / config.ratio - 1.0;
    half_knee_db_ = config.knee_db / 2.0;
    reduction_floor_db_ = -config.maximum_reduction_db;
  }

  // Non-positive gain change in dB for `envelope`.
  [[nodiscard]] double reduction_db(double envelope) const noexcept {
    if (slope_ == 0.0) {
      return 0.0;
    }
    const double over = gain_to_decibels(envelope) - config_.threshold_db;
    double reduction = 0.0;
    if (over >= half_knee_db_) {
      reduction = slope_ * over;
    } else if (over > -half_knee_db_) {
      const double knee = over + half_knee_db_;
      reduction = slope_ * knee * knee / (2.0 * config_.knee_db);
    }
    return std::max(reduction, reduction_floor_db_);
  }

  [[nodiscard]] double gain(double envelope) const noexcept {
    return decibels_to_gain(reduction_db(envelope));
  }

  [[nodiscard]] const SoftKneeConfig &config() const noexcept { return config_; }

private:
  SoftKneeConfig config_;
  double slope_ = 0.0;
  double half_knee_db_ = 0.0;
  double reduction_floor_db_ = 0.0;
};

}

#endif
