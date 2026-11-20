#ifndef VESPERA_DSP_CROSSOVER_HPP
#define VESPERA_DSP_CROSSOVER_HPP

#include <vespera/dsp/filters.hpp>

#include <concepts>
#include <numbers>

// Fourth-order Linkwitz-Riley two-way crossover: two cascaded Butterworth biquads per branch at
// a common cutoff, low and high, built from the existing RBJ biquad factories.
//
// Reconstruction convention: the outputs are summed as low + high with no polarity inversion.
// Each branch is -6.02 dB (amplitude 1/2) at the cutoff, where both are in phase, and the sum is
// the second-order allpass (s^2 - sqrt(2) s + 1) / (s^2 + sqrt(2) s + 1): unity magnitude at every
// frequency, phase falling from 0 at DC through -180 degrees at the cutoff towards -360 degrees.
// The sum is therefore not a delayed copy of the input and does not sample-null against it. The
// split is allpass-complementary, not power-complementary: |low|^2 + |high|^2 is 1/2 at the
// cutoff.
//
// Each RBJ biquad section is the bilinear image of the normalized analog second-order
// Butterworth section 1/(s^2 + sqrt(2) s + 1) evaluated at s = j tan(pi f / fs) / tan(pi fc /
// fs), so the branch responses equal the analog fourth-order responses and the analysis holds
// exactly for the digital filter too.
//
// Channel ownership: one instance filters one channel; a stereo user instantiates two. State is
// double precision per biquad section; samples may be float or double.
//
// Cutoff and sample-rate changes are quiescent operations: configure validates and throws
// std::invalid_argument, leaving configuration and state unchanged on rejection, and keeps the
// biquad histories (an abrupt change can therefore produce a bounded transient, like a biquad
// coefficient change). This first contract makes no claim that moving the cutoff during
// processing is stable, click-free or suitable for sweeps; continuous crossover movement needs
// a separately verified state and transition policy. reset() clears both branch histories.
//
// Processing is O(1) per sample per branch, allocation-, lock- and exception-free, causal with
// zero algorithmic latency. The tail is asymptotic (fourth-order IIR); declare a settling
// threshold at the module level. Non-finite input poisons the branch histories until reset();
// denormals are not flushed. An instance is not synchronized.

namespace vespera::dsp {

class LinkwitzRileyCrossover {
public:
  struct Output {
    double low = 0.0;
    double high = 0.0;
  };

  // Default: 48 kHz, 1 kHz crossover, cleared state.
  LinkwitzRileyCrossover() { configure(48000.0, 1000.0); }

  LinkwitzRileyCrossover(double sample_rate, double cutoff_hz) {
    configure(sample_rate, cutoff_hz);
  }

  // Quiescent (re)configuration. Throws std::invalid_argument for a non-finite or out-of-range
  // sample rate or a cutoff outside [1e-5, 0.49] x the sample rate, changing nothing.
  void configure(double sample_rate, double cutoff_hz) {
    validate_filter_frequency(sample_rate, cutoff_hz);
    // sqrt2 / 2 is exact, so this is the correctly rounded 1/sqrt(2); 1 / sqrt2 is one ulp low.
    constexpr double section_q = std::numbers::sqrt2 / 2.0;
    const auto low = BiquadCoefficients::lowpass(sample_rate, cutoff_hz, section_q);
    const auto high = BiquadCoefficients::highpass(sample_rate, cutoff_hz, section_q);
    sample_rate_ = sample_rate;
    cutoff_ = cutoff_hz;
    low_.set_coefficients({low, low});
    high_.set_coefficients({high, high});
  }

  constexpr double sample_rate() const noexcept { return sample_rate_; }
  constexpr double cutoff() const noexcept { return cutoff_; }

  void reset() noexcept {
    low_.reset();
    high_.reset();
  }

  template <std::floating_point Sample> Output process(Sample input) noexcept {
    const double x = static_cast<double>(input);
    return Output{low_.process(x), high_.process(x)};
  }

private:
  double sample_rate_ = 48000.0;
  double cutoff_ = 1000.0;
  BiquadCascade<2> low_;
  BiquadCascade<2> high_;
};

}

#endif
