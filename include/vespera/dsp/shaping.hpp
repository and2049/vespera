#ifndef VESPERA_DSP_SHAPING_HPP
#define VESPERA_DSP_SHAPING_HPP

#include <cmath>
#include <stdexcept>

// Stateless waveshapers, callable as double(double) noexcept so they can run directly inside
// Oversampler::process.

namespace vespera::dsp {

// Algebraic soft saturation f(x) = x / sqrt(1 + (drive * x)^2), evaluated as
// x / hypot(1, x * drive) so the square cannot overflow.
//
// For drive > 0 the curve is odd, has slope 1 at zero and rises towards +/-1 / drive; near that
// bound rounding can make it step back or exceed it by a few ulps. It approaches that bound more
// gradually than tanh(drive * x) / drive. Drive 0 is the exact identity; negative drive behaves
// like its magnitude.
//
// Non-finite input gives NaN, including +/-infinity. Finite input whose product with drive
// overflows gives a signed zero. Callers that cannot rule out such input should sanitise it first.
inline double soft_saturate(double x, double drive) noexcept {
  return x / std::hypot(1.0, x * drive);
}

// soft_saturate with a validated, fixed drive.
class SoftSaturator {
public:
  explicit SoftSaturator(double drive = 1.0) { set_drive(drive); }

  // Throws std::invalid_argument, leaving the drive unchanged, unless drive is finite and >= 0.
  void set_drive(double drive) {
    if (!std::isfinite(drive) || drive < 0.0) {
      throw std::invalid_argument("Saturation drive must be finite and non-negative");
    }
    drive_ = drive;
  }

  [[nodiscard]] double drive() const noexcept { return drive_; }

  double operator()(double x) const noexcept { return soft_saturate(x, drive_); }

private:
  double drive_ = 1.0;
};

}

#endif
