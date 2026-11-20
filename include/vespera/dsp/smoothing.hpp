#ifndef VESPERA_DSP_SMOOTHING_HPP
#define VESPERA_DSP_SMOOTHING_HPP

#include <vespera/dsp/common.hpp>

#include <cstdint>

namespace vespera::dsp {

// Instances belong to one processing thread. Values must be finite; configuration is quiescent.
class LinearSmoother {
public:
  // Reconfiguration settles an existing ramp at its target; use set_target for continuous ramps.
  void configure(double sample_rate, double seconds) {
    validate_sample_rate(sample_rate);
    if (!std::isfinite(seconds) || seconds < 0.0 || seconds > 60.0) {
      throw std::invalid_argument("Ramp duration must be within [0, 60] seconds");
    }
    frames_ = static_cast<uint32_t>(std::round(sample_rate * seconds));
    remaining_ = 0;
    current_ = target_;
  }

  void reset(double value = 0.0) noexcept {
    current_ = target_ = value;
    remaining_ = 0;
  }

  void set_target(double value) noexcept {
    if (value == target_) {
      return;
    }
    target_ = value;
    start_ = current_;
    remaining_ = frames_;
    if (frames_ == 0) {
      current_ = target_;
    }
  }

  double process() noexcept {
    if (remaining_ != 0) {
      --remaining_;
      const double weight = static_cast<double>(frames_ - remaining_) / frames_;
      current_ = remaining_ == 0 ? target_ : start_ * (1.0 - weight) + target_ * weight;
    }
    return current_;
  }

  double current() const noexcept { return current_; }
  double target() const noexcept { return target_; }
  bool active() const noexcept { return remaining_ != 0; }

private:
  double current_ = 0.0;
  double target_ = 0.0;
  double start_ = 0.0;
  uint32_t frames_ = 0;
  uint32_t remaining_ = 0;
};

class ExponentialSmoother {
public:
  // One time constant covers 1-exp(-1) of the remaining distance, rather than a full ramp.
  void configure(double sample_rate, double seconds) {
    validate_sample_rate(sample_rate);
    if (!std::isfinite(seconds) || seconds < 0.0 || seconds > 60.0) {
      throw std::invalid_argument("Smoothing time must be within [0, 60] seconds");
    }
    amount_ = seconds == 0.0 ? 1.0 : -std::expm1(-1.0 / (sample_rate * seconds));
  }
  void reset(double value = 0.0) noexcept { current_ = target_ = value; }
  void set_target(double value) noexcept { target_ = value; }
  double process() noexcept {
    current_ = current_ * (1.0 - amount_) + target_ * amount_;
    if (std::abs(current_) < 1e-300) {
      current_ = 0.0;
    }
    return current_;
  }
  double current() const noexcept { return current_; }

private:
  double amount_ = 1.0;
  double current_ = 0.0;
  double target_ = 0.0;
};

}

#endif
