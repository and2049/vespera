#ifndef VESPERA_DSP_STATE_VARIABLE_FILTER_HPP
#define VESPERA_DSP_STATE_VARIABLE_FILTER_HPP

#include <vespera/dsp/filters.hpp>

#include <cmath>
#include <concepts>
#include <numbers>

// Linear two-integrator state-variable filter, discretized with the trapezoidal (bilinear) rule.
//
// The analog prototype is the classic two-integrator loop: the input minus the lowpass output
// minus R times the bandpass output feeds the first integrator, whose output (the bandpass
// signal) feeds the second integrator (the lowpass output), with R = 1/Q. Each integrator is
// replaced by its trapezoidal equivalent and the resulting algebraic loop is solved exactly, so
// this is the TPT/ZDF construction: the feedback is a linear relation solved algebraically, not
// an iterative or nonlinear solver. The integrator gain is prewarped with k = tan(pi f / fs), so
// the digital response at frequency f equals the analog prototype's response at the bilinear
// image of f and the cutoff lands exactly on its design frequency.
//
// Discrete transfer functions with q = z^-1 and D = 1 + k^2 + k R:
//   low:  k^2 (1 + 2q + q^2) / (D + 2(k^2 - 1) q + (1 + k^2 - k R) q^2)
//   band: k (1 - q^2)         / (same denominator)
//   high: (1 - 2q + q^2)      / (same denominator)
//
// Output normalization (analytic; evaluated in double they hold to rounding): low is unity at DC
// and zero at Nyquist, high is zero at DC and unity at Nyquist, band is zero at both, and at the
// cutoff all three magnitudes equal Q. band / Q is the unity-peak bandpass.
//
// Coefficients and integrator state are double precision; samples may be float or double and are
// widened for the arithmetic. The stored states s = (ic1, ic2) are the trapezoidal integrator
// memories and a control update keeps them. With zero input one sample maps s to A s with
//   A = [[P - kR, -2k], [2k, P + kR]] / D,  P = 1 - k^2,  D = 1 + k^2 + kR,
// and D^2 I - (D A)^T (D A) = 4kR [1, -k]^T [1, -k], which is positive semidefinite, so
// |A s| <= |s| in the Euclidean norm for every valid cutoff and Q. The state norm therefore never
// grows with zero input under any sequence of valid control changes, including a change on every
// sample. This is not a bounded-input bounded-output guarantee for arbitrary modulation with
// input present, and an abrupt control change still produces a transient in the output.
//
// Design ranges match the biquad factories: sample rates 8 kHz through 768 kHz, cutoff in
// [1e-5, 0.49] x sample rate, Q in [0.05, 50]. These keep the solved denominator comfortably
// conditioned in double precision and keep the prewarped k finite and positive.
//
// Threading: an instance is not synchronized; its owner must not call any member concurrently
// with another. Signal contract: input is expected to be finite audio of roughly unit scale.
// Non-finite input poisons the state until reset(). Denormal state values are not flushed; audio
// threads should enable FTZ/DAZ. Processing is O(1) per sample and never allocates, locks or
// throws. The filter is causal with zero algorithmic latency: output frame n uses input frame
// n. Its phase is filter phase, not pure delay, and its tail is asymptotic; no finite tail is
// implied.

namespace vespera::dsp {

class StateVariableFilter {
public:
  // The three shared outputs of one sample. All come from the same integrator state, so taking
  // several of them costs no extra filtering.
  struct Output {
    double low = 0.0;
    double band = 0.0;
    double high = 0.0;
  };

  // Default: 48 kHz, 1 kHz cutoff, Butterworth Q, cleared state.
  StateVariableFilter() { prepare(48000.0, 1000.0, butterworth_q); }

  StateVariableFilter(double sample_rate, double cutoff_hz, double q) {
    prepare(sample_rate, cutoff_hz, q);
  }

  // Installs a full configuration while quiescent. Validates everything and throws
  // std::invalid_argument, leaving the previous configuration and state unchanged, on any
  // non-finite value or out-of-range rate/cutoff/Q. Clears the integrator history: the sample
  // rate or the whole design changed under it. Use this at construction and on sample-rate
  // changes, not as the per-control update path.
  void prepare(double sample_rate, double cutoff_hz, double q) {
    validate_filter_frequency(sample_rate, cutoff_hz);
    validate_filter_q(q);
    sample_rate_ = sample_rate;
    design(cutoff_hz, q);
    ic1_ = 0.0;
    ic2_ = 0.0;
  }

  // Real-time cutoff update. Returns false and changes nothing (neither coefficients nor
  // state) when the value is non-finite or outside [1e-5, 0.49] x the prepared sample rate.
  // Keeps the integrator history.
  bool try_set_cutoff(double cutoff_hz) noexcept {
    if (!valid_cutoff(cutoff_hz)) {
      return false;
    }
    design(cutoff_hz, q_);
    return true;
  }

  // Real-time Q update with the same contract as try_set_cutoff; Q outside [0.05, 50] is
  // rejected.
  bool try_set_q(double q) noexcept {
    if (!valid_q(q)) {
      return false;
    }
    design(cutoff_, q);
    return true;
  }

  // Atomic cutoff and Q update: either both are valid and both take effect, or nothing changes.
  bool try_set_controls(double cutoff_hz, double q) noexcept {
    if (!valid_cutoff(cutoff_hz) || !valid_q(q)) {
      return false;
    }
    design(cutoff_hz, q);
    return true;
  }

  constexpr double sample_rate() const noexcept { return sample_rate_; }
  constexpr double cutoff() const noexcept { return cutoff_; }
  constexpr double q() const noexcept { return q_; }

  // Clears the integrator history in O(1); the configuration is kept.
  void reset() noexcept {
    ic1_ = 0.0;
    ic2_ = 0.0;
  }

  template <std::floating_point Sample> Output process(Sample input) noexcept {
    const double x = static_cast<double>(input);
    const double band = (k_ * (x - ic2_) + ic1_) * inverse_denominator_;
    const double low = k_ * band + ic2_;
    const double high = x - low - resonance_ * band;
    ic1_ = 2.0 * band - ic1_;
    ic2_ = k_ * band + low;
    return Output{low, band, high};
  }

private:
  // sqrt2 / 2 is exact, so this is the correctly rounded 1/sqrt(2).
  static constexpr double butterworth_q = std::numbers::sqrt2 / 2.0;

  bool valid_cutoff(double cutoff_hz) const noexcept {
    return std::isfinite(cutoff_hz) &&
           cutoff_hz >= sample_rate_ * minimum_normalized_filter_frequency &&
           cutoff_hz <= sample_rate_ * maximum_normalized_filter_frequency;
  }

  static bool valid_q(double q) noexcept {
    return std::isfinite(q) && q >= minimum_filter_q && q <= maximum_filter_q;
  }

  // Requires pre-validated controls. Keeps the integrator state.
  void design(double cutoff_hz, double q) noexcept {
    cutoff_ = cutoff_hz;
    q_ = q;
    k_ = std::tan(std::numbers::pi * cutoff_hz / sample_rate_);
    resonance_ = 1.0 / q;
    inverse_denominator_ = 1.0 / (1.0 + k_ * k_ + k_ * resonance_);
  }

  double sample_rate_ = 48000.0;
  double cutoff_ = 1000.0;
  double q_ = butterworth_q;
  double k_ = 0.0;
  double resonance_ = 1.0 / butterworth_q;
  double inverse_denominator_ = 1.0;
  double ic1_ = 0.0;
  double ic2_ = 0.0;
};

}

#endif
