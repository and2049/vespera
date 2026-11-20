#ifndef VESPERA_DSP_FILTERS_HPP
#define VESPERA_DSP_FILTERS_HPP

#include <vespera/dsp/common.hpp>

#include <array>
#include <cassert>
#include <cmath>
#include <complex>
#include <concepts>
#include <cstddef>
#include <numbers>
#include <span>
#include <stdexcept>

// First-order and second-order (RBJ "Audio EQ Cookbook") IIR filters.
//
// Design and processing are separate. Coefficient types can only hold finite, strictly stable
// coefficients: their factories validate every control and throw std::invalid_argument, so a
// failed design never reaches a processor. Processors accept coefficients through a noexcept,
// allocation-free set_coefficients(), which makes coefficient updates safe on the audio thread
// when the coefficients were designed elsewhere and handed over by the caller's own mechanism.
//
// Coefficients and filter state are double precision; samples may be float or double and are
// widened for the arithmetic. Processors run in transposed direct form II.
//
// Threading: factories are pure functions callable from any thread. A processor instance is not
// synchronized; its owner must not call any member concurrently with another.
//
// Signal contract: input is expected to be finite audio of roughly unit scale. The filters are
// linear, so larger finite inputs only scale the output, but non-finite input poisons the state
// until reset(). Denormal state values are not flushed; audio threads should enable FTZ/DAZ.

namespace vespera::dsp {

// Filter frequencies are additionally limited to this fraction of the sample rate. The lower bound
// keeps pole radii representably below 1 in double precision; the upper bound keeps resonant
// designs away from the marginally stable Nyquist limit of the bilinear transform.
inline constexpr double minimum_normalized_filter_frequency = 1.0e-5;
inline constexpr double maximum_normalized_filter_frequency = 0.49;
inline constexpr double minimum_filter_q = 0.05;
inline constexpr double maximum_filter_q = 50.0;
inline constexpr double maximum_filter_gain_db = 48.0;
inline constexpr double default_dc_blocker_cutoff = 10.0;

inline void validate_filter_frequency(double sample_rate, double frequency) {
  validate_frequency(sample_rate, frequency);
  if (frequency < sample_rate * minimum_normalized_filter_frequency ||
      frequency > sample_rate * maximum_normalized_filter_frequency) {
    throw std::invalid_argument(
        "Filter frequency must be within [1e-5, 0.49] times the sample rate");
  }
}

inline void validate_filter_q(double q) {
  if (!std::isfinite(q) || q < minimum_filter_q || q > maximum_filter_q) {
    throw std::invalid_argument("Filter Q must be within [0.05, 50]");
  }
}

inline void validate_filter_gain_db(double gain_db) {
  if (!std::isfinite(gain_db) || std::abs(gain_db) > maximum_filter_gain_db) {
    throw std::invalid_argument("Filter gain must be within [-48, 48] dB");
  }
}

// H(z) = (b0 + b1 z^-1) / (1 + a1 z^-1). Default-constructed coefficients are the identity.
class FirstOrderCoefficients {
public:
  constexpr FirstOrderCoefficients() noexcept = default;

  // Throws unless every coefficient is finite and |a1| < 1.
  static FirstOrderCoefficients from_raw(double b0, double b1, double a1) {
    if (!std::isfinite(b0) || !std::isfinite(b1) || !std::isfinite(a1) || !(std::abs(a1) < 1.0)) {
      throw std::invalid_argument("First-order coefficients must be finite and stable");
    }
    return FirstOrderCoefficients(b0, b1, a1);
  }

  // Bilinear-transform one-pole lowpass: unity at DC, exactly -3.01 dB at `frequency`, zero at
  // Nyquist.
  static FirstOrderCoefficients lowpass(double sample_rate, double frequency) {
    validate_filter_frequency(sample_rate, frequency);
    const double k = std::tan(std::numbers::pi * frequency / sample_rate);
    const double norm = 1.0 / (1.0 + k);
    return from_raw(k * norm, k * norm, (k - 1.0) * norm);
  }

  // Bilinear-transform one-pole highpass: zero at DC, exactly -3.01 dB at `frequency`, unity at
  // Nyquist.
  static FirstOrderCoefficients highpass(double sample_rate, double frequency) {
    validate_filter_frequency(sample_rate, frequency);
    const double k = std::tan(std::numbers::pi * frequency / sample_rate);
    const double norm = 1.0 / (1.0 + k);
    return from_raw(norm, -norm, (k - 1.0) * norm);
  }

  // DC blocker: the one-pole highpass with a sub-audio default cutoff that is valid at every
  // supported sample rate.
  static FirstOrderCoefficients dc_blocker(double sample_rate,
                                           double cutoff = default_dc_blocker_cutoff) {
    return highpass(sample_rate, cutoff);
  }

  constexpr double b0() const noexcept { return b0_; }
  constexpr double b1() const noexcept { return b1_; }
  constexpr double a1() const noexcept { return a1_; }

  // Evaluates H at `normalized_frequency` cycles per sample (0 is DC, 0.5 is Nyquist).
  std::complex<double> response(double normalized_frequency) const noexcept {
    const std::complex<double> z1 = std::polar(1.0, -2.0 * std::numbers::pi * normalized_frequency);
    return (b0_ + b1_ * z1) / (1.0 + a1_ * z1);
  }

  friend constexpr bool operator==(const FirstOrderCoefficients &,
                                   const FirstOrderCoefficients &) = default;

private:
  constexpr FirstOrderCoefficients(double b0, double b1, double a1) noexcept
      : b0_(b0), b1_(b1), a1_(a1) {}

  double b0_ = 1.0;
  double b1_ = 0.0;
  double a1_ = 0.0;
};

// H(z) = (b0 + b1 z^-1 + b2 z^-2) / (1 + a1 z^-1 + a2 z^-2). Default-constructed coefficients are
// the identity. Factories take the centre/corner frequency in Hz, Q within
// [minimum_filter_q, maximum_filter_q], and, where applicable, gain in dB within
// +/-maximum_filter_gain_db. Q = 1/sqrt(2) gives Butterworth lowpass/highpass and overshoot-free
// shelves; higher shelf Q adds a resonant bump next to the corner.
class BiquadCoefficients {
public:
  constexpr BiquadCoefficients() noexcept = default;

  // Throws unless every coefficient is finite and both poles lie strictly inside the unit circle.
  static BiquadCoefficients from_raw(double b0, double b1, double b2, double a1, double a2) {
    if (!std::isfinite(b0) || !std::isfinite(b1) || !std::isfinite(b2) || !std::isfinite(a1) ||
        !std::isfinite(a2) || !(std::abs(a2) < 1.0) || !(std::abs(a1) < 1.0 + a2)) {
      throw std::invalid_argument("Biquad coefficients must be finite and stable");
    }
    return BiquadCoefficients(b0, b1, b2, a1, a2);
  }

  // |H| is 1 at DC, Q at `frequency`, 0 at Nyquist.
  static BiquadCoefficients lowpass(double sample_rate, double frequency, double q) {
    const Prototype p(sample_rate, frequency, q);
    const double b = p.one_minus_cos / 2.0;
    return p.normalize(b, 2.0 * b, b, 1.0 + p.alpha, -2.0 * p.cos_w0, 1.0 - p.alpha);
  }

  // |H| is 0 at DC, Q at `frequency`, 1 at Nyquist.
  static BiquadCoefficients highpass(double sample_rate, double frequency, double q) {
    const Prototype p(sample_rate, frequency, q);
    const double b = (1.0 + p.cos_w0) / 2.0;
    return p.normalize(b, -2.0 * b, b, 1.0 + p.alpha, -2.0 * p.cos_w0, 1.0 - p.alpha);
  }

  // Constant 0 dB peak gain: |H| is 1 at `frequency`, 0 at DC and Nyquist; Q sets the bandwidth.
  static BiquadCoefficients bandpass(double sample_rate, double frequency, double q) {
    const Prototype p(sample_rate, frequency, q);
    return p.normalize(p.alpha, 0.0, -p.alpha, 1.0 + p.alpha, -2.0 * p.cos_w0, 1.0 - p.alpha);
  }

  // |H| is 0 at `frequency`, 1 at DC and Nyquist.
  static BiquadCoefficients notch(double sample_rate, double frequency, double q) {
    const Prototype p(sample_rate, frequency, q);
    return p.normalize(1.0, -2.0 * p.cos_w0, 1.0, 1.0 + p.alpha, -2.0 * p.cos_w0, 1.0 - p.alpha);
  }

  // |H| is gain_db at `frequency`, 1 at DC and Nyquist.
  static BiquadCoefficients peak(double sample_rate, double frequency, double q, double gain_db) {
    validate_filter_gain_db(gain_db);
    const Prototype p(sample_rate, frequency, q);
    const double a = std::pow(10.0, gain_db / 40.0);
    return p.normalize(1.0 + p.alpha * a, -2.0 * p.cos_w0, 1.0 - p.alpha * a, 1.0 + p.alpha / a,
                       -2.0 * p.cos_w0, 1.0 - p.alpha / a);
  }

  // |H| is gain_db at DC, gain_db / 2 at `frequency`, 1 at Nyquist.
  static BiquadCoefficients low_shelf(double sample_rate, double frequency, double q,
                                      double gain_db) {
    validate_filter_gain_db(gain_db);
    const Prototype p(sample_rate, frequency, q);
    const double a = std::pow(10.0, gain_db / 40.0);
    const double s = 2.0 * std::sqrt(a) * p.alpha;
    const double c = p.cos_w0;
    return p.normalize(a * ((a + 1.0) - (a - 1.0) * c + s), 2.0 * a * ((a - 1.0) - (a + 1.0) * c),
                       a * ((a + 1.0) - (a - 1.0) * c - s), (a + 1.0) + (a - 1.0) * c + s,
                       -2.0 * ((a - 1.0) + (a + 1.0) * c), (a + 1.0) + (a - 1.0) * c - s);
  }

  // |H| is 1 at DC, gain_db / 2 at `frequency`, gain_db at Nyquist.
  static BiquadCoefficients high_shelf(double sample_rate, double frequency, double q,
                                       double gain_db) {
    validate_filter_gain_db(gain_db);
    const Prototype p(sample_rate, frequency, q);
    const double a = std::pow(10.0, gain_db / 40.0);
    const double s = 2.0 * std::sqrt(a) * p.alpha;
    const double c = p.cos_w0;
    return p.normalize(a * ((a + 1.0) + (a - 1.0) * c + s), -2.0 * a * ((a - 1.0) + (a + 1.0) * c),
                       a * ((a + 1.0) + (a - 1.0) * c - s), (a + 1.0) - (a - 1.0) * c + s,
                       2.0 * ((a - 1.0) - (a + 1.0) * c), (a + 1.0) - (a - 1.0) * c - s);
  }

  constexpr double b0() const noexcept { return b0_; }
  constexpr double b1() const noexcept { return b1_; }
  constexpr double b2() const noexcept { return b2_; }
  constexpr double a1() const noexcept { return a1_; }
  constexpr double a2() const noexcept { return a2_; }

  // Evaluates H at `normalized_frequency` cycles per sample (0 is DC, 0.5 is Nyquist).
  std::complex<double> response(double normalized_frequency) const noexcept {
    const std::complex<double> z1 = std::polar(1.0, -2.0 * std::numbers::pi * normalized_frequency);
    const std::complex<double> z2 = z1 * z1;
    return (b0_ + b1_ * z1 + b2_ * z2) / (1.0 + a1_ * z1 + a2_ * z2);
  }

  friend constexpr bool operator==(const BiquadCoefficients &,
                                   const BiquadCoefficients &) = default;

private:
  struct Prototype {
    Prototype(double sample_rate, double frequency, double q) {
      validate_filter_frequency(sample_rate, frequency);
      validate_filter_q(q);
      const double w0 = 2.0 * std::numbers::pi * frequency / sample_rate;
      const double half_sin = std::sin(w0 / 2.0);
      cos_w0 = std::cos(w0);
      one_minus_cos = 2.0 * half_sin * half_sin;
      alpha = std::sin(w0) / (2.0 * q);
    }

    static BiquadCoefficients normalize(double b0, double b1, double b2, double a0, double a1,
                                        double a2) {
      const double inv = 1.0 / a0;
      return from_raw(b0 * inv, b1 * inv, b2 * inv, a1 * inv, a2 * inv);
    }

    double cos_w0 = 1.0;
    double one_minus_cos = 0.0;
    double alpha = 0.0;
  };

  constexpr BiquadCoefficients(double b0, double b1, double b2, double a1, double a2) noexcept
      : b0_(b0), b1_(b1), b2_(b2), a1_(a1), a2_(a2) {}

  double b0_ = 1.0;
  double b1_ = 0.0;
  double b2_ = 0.0;
  double a1_ = 0.0;
  double a2_ = 0.0;
};

// Shared processing interface of FirstOrderFilter and Biquad. A default-constructed processor is
// a usable identity filter with cleared history. set_coefficients() keeps the history so a
// parameter change does not click by itself, but a large abrupt change can still produce a
// bounded transient; smooth control values externally and update coefficients at a modest rate.
// Block overloads give bit-identical results to calling the per-sample overload in sequence, for
// any partition of the signal.
class FirstOrderFilter {
public:
  constexpr FirstOrderFilter() noexcept = default;
  constexpr explicit FirstOrderFilter(const FirstOrderCoefficients &coefficients) noexcept
      : coefficients_(coefficients) {}

  constexpr void set_coefficients(const FirstOrderCoefficients &coefficients) noexcept {
    coefficients_ = coefficients;
  }
  constexpr const FirstOrderCoefficients &coefficients() const noexcept { return coefficients_; }

  constexpr void reset() noexcept { state_ = 0.0; }

  template <std::floating_point Sample> Sample process(Sample input) noexcept {
    const double x = static_cast<double>(input);
    const double y = coefficients_.b0() * x + state_;
    state_ = coefficients_.b1() * x - coefficients_.a1() * y;
    return static_cast<Sample>(y);
  }

  template <std::floating_point Sample> void process(std::span<Sample> samples) noexcept {
    for (Sample &sample : samples) {
      sample = process(sample);
    }
  }

  // Requires output.size() >= input.size(); the spans may be identical but must not otherwise
  // overlap.
  template <std::floating_point Sample>
  void process(std::span<const Sample> input, std::span<Sample> output) noexcept {
    assert(output.size() >= input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
      output[i] = process(input[i]);
    }
  }

private:
  FirstOrderCoefficients coefficients_;
  double state_ = 0.0;
};

// See FirstOrderFilter for the processing contract.
class Biquad {
public:
  constexpr Biquad() noexcept = default;
  constexpr explicit Biquad(const BiquadCoefficients &coefficients) noexcept
      : coefficients_(coefficients) {}

  constexpr void set_coefficients(const BiquadCoefficients &coefficients) noexcept {
    coefficients_ = coefficients;
  }
  constexpr const BiquadCoefficients &coefficients() const noexcept { return coefficients_; }

  constexpr void reset() noexcept {
    state1_ = 0.0;
    state2_ = 0.0;
  }

  template <std::floating_point Sample> Sample process(Sample input) noexcept {
    const double x = static_cast<double>(input);
    const BiquadCoefficients &c = coefficients_;
    const double y = c.b0() * x + state1_;
    state1_ = c.b1() * x - c.a1() * y + state2_;
    state2_ = c.b2() * x - c.a2() * y;
    return static_cast<Sample>(y);
  }

  template <std::floating_point Sample> void process(std::span<Sample> samples) noexcept {
    for (Sample &sample : samples) {
      sample = process(sample);
    }
  }

  // Requires output.size() >= input.size(); the spans may be identical but must not otherwise
  // overlap.
  template <std::floating_point Sample>
  void process(std::span<const Sample> input, std::span<Sample> output) noexcept {
    assert(output.size() >= input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
      output[i] = process(input[i]);
    }
  }

private:
  BiquadCoefficients coefficients_;
  double state1_ = 0.0;
  double state2_ = 0.0;
};

// Biquads in series, stage 0 first. The signal stays in double precision between stages and is
// converted to Sample once at the output, so a float cascade can differ in the last bit from a
// chain of float Biquad calls. Otherwise follows the FirstOrderFilter processing contract.
template <std::size_t Stages> class BiquadCascade {
  static_assert(Stages > 0, "a biquad cascade needs at least one stage");

public:
  using Coefficients = std::array<BiquadCoefficients, Stages>;

  constexpr BiquadCascade() noexcept = default;
  constexpr explicit BiquadCascade(const Coefficients &coefficients) noexcept {
    set_coefficients(coefficients);
  }

  static constexpr std::size_t size() noexcept { return Stages; }

  constexpr void set_coefficients(const Coefficients &coefficients) noexcept {
    for (std::size_t i = 0; i < Stages; ++i) {
      stages_[i].set_coefficients(coefficients[i]);
    }
  }

  // Requires stage < size().
  constexpr void set_coefficients(std::size_t stage,
                                  const BiquadCoefficients &coefficients) noexcept {
    assert(stage < Stages);
    stages_[stage].set_coefficients(coefficients);
  }

  // Requires stage < size().
  constexpr const BiquadCoefficients &coefficients(std::size_t stage) const noexcept {
    assert(stage < Stages);
    return stages_[stage].coefficients();
  }

  constexpr void reset() noexcept {
    for (Biquad &stage : stages_) {
      stage.reset();
    }
  }

  template <std::floating_point Sample> Sample process(Sample input) noexcept {
    double sample = static_cast<double>(input);
    for (Biquad &stage : stages_) {
      sample = stage.process(sample);
    }
    return static_cast<Sample>(sample);
  }

  template <std::floating_point Sample> void process(std::span<Sample> samples) noexcept {
    for (Sample &sample : samples) {
      sample = process(sample);
    }
  }

  // Requires output.size() >= input.size(); the spans may be identical but must not otherwise
  // overlap.
  template <std::floating_point Sample>
  void process(std::span<const Sample> input, std::span<Sample> output) noexcept {
    assert(output.size() >= input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
      output[i] = process(input[i]);
    }
  }

private:
  std::array<Biquad, Stages> stages_{};
};

}

#endif
