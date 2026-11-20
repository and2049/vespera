#ifndef VESPERA_DSP_OVERSAMPLING_HPP
#define VESPERA_DSP_OVERSAMPLING_HPP

#include <array>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <numbers>
#include <span>
#include <stdexcept>
#include <type_traits>

// Mono 2x/4x oversampling around a per-sample nonlinearity.
//
// Each 2x step is a linear-phase, Kaiser-windowed halfband FIR run in polyphase form: the
// interpolator inserts properly band-limited samples (not repeats or zeros), and the decimator
// low-pass filters before discarding samples. 4x cascades a second, shorter halfband stage
// whose transition band may be wide because everything above the base-rate passband is removed
// again by the first stage. Coefficients are computed at compile time; all state lives in fixed
// arrays, so processing never allocates.
//
// Characteristics relative to the base sample rate fs (fixed by the compile-time design and
// verified by dsp/tests/oversampling_test.cpp):
//   * DC gain is 1 up to rounding. The 127-tap first stage is within +/-0.0003 dB up to 0.45 fs
//     and -6 dB at 0.5 fs; the 31-tap second stage is flat to 0.0001 dB over the same band. A full
//     round trip with an identity callback stays within 0.001 dB up to 0.43 fs (20.6 kHz at 48 kHz)
//     and loses about 0.005 dB at 20 kHz for fs = 44.1 kHz.
//   * Stopbands start at 0.56 fs (first stage) and are at least 99 dB down, so images of, and
//     aliases into, content below 0.44 fs are suppressed by at least 99 dB. Content between 0.44 fs
//     and 0.5 fs is in the transition band and only partially protected.
//   * Latency is an integer number of base-rate samples: latency(factor) is 63 for 2x and 71 for
//     4x, so a dry path delayed by that many samples is phase-aligned with the wet path. The
//     response is linear phase with an impulse response symmetric about that latency.
//   * Cost per base-rate sample is roughly 64 multiply-adds for 2x and 96 for 4x, plus the
//     callback.
//
// Oversampling only removes aliasing from harmonics below the oversampled Nyquist frequency;
// harmonics a nonlinearity creates above it still fold back.
//
// Signal contract: input is expected to be finite audio of roughly unit scale. Interpolated
// samples overshoot the base-rate peak (about 27 percent for a full-scale naive square wave, at
// most callback_input_bound()), so the callback must accept a range beyond its nominal input. The
// callback must return finite values; a non-finite value contaminates subsequent samples until
// the FIR history drains or reset() clears it.
//
// Threading: an instance is not synchronized; its owner must not call members concurrently.

namespace vespera::dsp {

enum class OversamplingFactor : int { two = 2, four = 4 };

namespace detail {

constexpr double constexpr_sqrt(double value) noexcept {
  if (value <= 0.0) {
    return 0.0;
  }
  double estimate = value < 1.0 ? 1.0 : value;
  for (int i = 0; i < 64; ++i) {
    estimate = 0.5 * (estimate + value / estimate);
  }
  return estimate;
}

constexpr double bessel_i0(double x) noexcept {
  double sum = 1.0;
  double term = 1.0;
  const double quarter_square = 0.25 * x * x;
  for (int k = 1; k < 200 && term > sum * 1.0e-18; ++k) {
    term *= quarter_square / (static_cast<double>(k) * static_cast<double>(k));
    sum += term;
  }
  return sum;
}

// Nonzero off-centre taps of a halfband filter of length 4 * Order + 3, whose centre tap is 0.5.
// Entry j holds the tap at offset 2 * j - (2 * Order + 1) from the centre. The taps are
// renormalized to sum to 0.5 so DC passes with exactly unit gain.
template <int Order> constexpr std::array<double, 2 * Order + 2> halfband_taps(double beta) {
  constexpr int count = 2 * Order + 2;
  constexpr int half_length = 2 * Order + 1;
  std::array<double, count> taps{};
  double sum = 0.0;
  for (int j = 0; j < count; ++j) {
    const int offset = 2 * j - half_length;
    const int magnitude = offset < 0 ? -offset : offset;
    const double sinc_sign = ((magnitude - 1) / 2) % 2 == 0 ? 1.0 : -1.0;
    const double ratio = static_cast<double>(offset) / half_length;
    const double window = bessel_i0(beta * constexpr_sqrt(1.0 - ratio * ratio)) / bessel_i0(beta);
    taps[static_cast<std::size_t>(j)] =
        sinc_sign / (std::numbers::pi * static_cast<double>(magnitude)) * window;
    sum += taps[static_cast<std::size_t>(j)];
  }
  for (double &tap : taps) {
    tap *= 0.5 / sum;
  }
  return taps;
}

template <std::size_t Count>
constexpr double interpolation_gain_bound(const std::array<double, Count> &taps) noexcept {
  double sum = 0.0;
  for (double tap : taps) {
    sum += tap < 0.0 ? -tap : tap;
  }
  return 2.0 * sum;
}

template <int Order> struct HalfbandKernel {
  static constexpr std::size_t taps = 2 * Order + 2;
  static constexpr int delay = 2 * Order + 1;
};

template <int Order, const std::array<double, 2 * Order + 2> &Taps> class HalfbandInterpolator {
public:
  static constexpr std::size_t count = HalfbandKernel<Order>::taps;

  // Produces the two output-rate samples that follow one input sample, in time order.
  void process(double input, double &first, double &second) noexcept {
    position_ = position_ == 0 ? count - 1 : position_ - 1;
    history_[position_] = input;
    history_[position_ + count] = input;
    const double *newest = history_.data() + position_;
    double sum = 0.0;
    for (std::size_t j = 0; j < count / 2; ++j) {
      sum += Taps[j] * (newest[j] + newest[count - 1 - j]);
    }
    first = 2.0 * sum;
    second = newest[Order];
  }

  void reset() noexcept {
    history_.fill(0.0);
    position_ = 0;
  }

private:
  std::array<double, 2 * count> history_{};
  std::size_t position_ = 0;
};

template <int Order, const std::array<double, 2 * Order + 2> &Taps> class HalfbandDecimator {
public:
  static constexpr std::size_t count = HalfbandKernel<Order>::taps;
  static constexpr std::size_t odd_delay = Order + 1;

  // Consumes two input-rate samples in time order and returns one output sample.
  double process(double first, double second) noexcept {
    position_ = position_ == 0 ? count - 1 : position_ - 1;
    even_[position_] = first;
    even_[position_ + count] = first;
    const double *newest = even_.data() + position_;
    double sum = 0.0;
    for (std::size_t j = 0; j < count / 2; ++j) {
      sum += Taps[j] * (newest[j] + newest[count - 1 - j]);
    }
    const double centre = odd_[odd_position_];
    odd_[odd_position_] = second;
    odd_position_ = odd_position_ + 1 == odd_delay ? 0 : odd_position_ + 1;
    return sum + 0.5 * centre;
  }

  void reset() noexcept {
    even_.fill(0.0);
    odd_.fill(0.0);
    position_ = 0;
    odd_position_ = 0;
  }

private:
  std::array<double, 2 * count> even_{};
  std::array<double, odd_delay> odd_{};
  std::size_t position_ = 0;
  std::size_t odd_position_ = 0;
};

inline constexpr int first_stage_order = 31;
inline constexpr int second_stage_order = 7;
inline constexpr double halfband_kaiser_beta = 10.0;
inline constexpr auto first_stage_taps = halfband_taps<first_stage_order>(halfband_kaiser_beta);
inline constexpr auto second_stage_taps = halfband_taps<second_stage_order>(halfband_kaiser_beta);

}

class Oversampler {
public:
  // Base-rate samples by which the output lags the input. 4x inserts one 2x-rate sample of delay
  // so that its total is also an integer.
  static constexpr int latency(OversamplingFactor factor) noexcept {
    constexpr int first = detail::HalfbandKernel<detail::first_stage_order>::delay;
    constexpr int second = detail::HalfbandKernel<detail::second_stage_order>::delay;
    return factor == OversamplingFactor::four ? (2 * first + (2 * second) / 2 + 1) / 2
                                              : (2 * first) / 2;
  }

  // Upper bound on |sample passed to the callback| / max|input|, reached only by adversarial
  // input patterns (about 2.75 for 2x and 5.05 for 4x). Band-limited audio overshoots far less.
  static constexpr double callback_input_bound(OversamplingFactor factor) noexcept {
    constexpr double first = detail::interpolation_gain_bound(detail::first_stage_taps);
    constexpr double second = detail::interpolation_gain_bound(detail::second_stage_taps);
    return factor == OversamplingFactor::four ? first * second : first;
  }

  // Default-constructed instances run at 2x with cleared history.
  Oversampler() noexcept = default;
  explicit Oversampler(OversamplingFactor factor) { prepare(factor); }

  // Selects the factor and clears all history. Throws std::invalid_argument for a value outside
  // the enumeration and then leaves the instance unchanged. Allocation-free.
  void prepare(OversamplingFactor factor) {
    if (factor != OversamplingFactor::two && factor != OversamplingFactor::four) {
      throw std::invalid_argument("Oversampling factor must be 2 or 4");
    }
    factor_ = factor;
    reset();
  }

  OversamplingFactor factor() const noexcept { return factor_; }
  int ratio() const noexcept { return static_cast<int>(factor_); }
  int latency_samples() const noexcept { return latency(factor_); }

  void reset() noexcept {
    first_up_.reset();
    first_down_.reset();
    second_up_.reset();
    second_down_.reset();
    pending_ = 0.0;
  }

  // Runs `nonlinearity` ratio() times per input sample, in time order, at ratio() times the base
  // sample rate, and returns the band-limited result delayed by latency_samples(). The callable
  // is invoked through an lvalue reference, so a stateful callable (for example one owning a
  // filter configured for the oversampled rate) keeps its state across calls. It must be
  // invocable as double(double) and noexcept. Filters from filters.hpp only accept rates up to
  // maximum_sample_rate, so they can run inside the callback only when ratio() * fs fits.
  template <std::floating_point Sample, typename Nonlinearity>
  Sample process(Sample input, Nonlinearity &&nonlinearity) noexcept {
    static_assert(std::is_nothrow_invocable_r_v<double, Nonlinearity &, double>,
                  "oversampling callback must be callable as double(double) noexcept");
    double up0 = 0.0;
    double up1 = 0.0;
    first_up_.process(static_cast<double>(input), up0, up1);
    if (factor_ == OversamplingFactor::two) {
      const double wet0 = nonlinearity(up0);
      const double wet1 = nonlinearity(up1);
      return static_cast<Sample>(first_down_.process(wet0, wet1));
    }
    const double down0 = run_second_stage(up0, nonlinearity);
    const double down1 = run_second_stage(up1, nonlinearity);
    const double delayed = pending_;
    pending_ = down1;
    return static_cast<Sample>(first_down_.process(delayed, down0));
  }

  template <std::floating_point Sample, typename Nonlinearity>
  void process(std::span<Sample> samples, Nonlinearity &&nonlinearity) noexcept {
    for (Sample &sample : samples) {
      sample = process(sample, nonlinearity);
    }
  }

  // Requires output.size() >= input.size(); the spans may be identical but must not otherwise
  // overlap.
  template <std::floating_point Sample, typename Nonlinearity>
  void process(std::span<const Sample> input, std::span<Sample> output,
               Nonlinearity &&nonlinearity) noexcept {
    assert(output.size() >= input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
      output[i] = process(input[i], nonlinearity);
    }
  }

private:
  template <typename Nonlinearity>
  double run_second_stage(double input, Nonlinearity &nonlinearity) noexcept {
    double up0 = 0.0;
    double up1 = 0.0;
    second_up_.process(input, up0, up1);
    const double wet0 = nonlinearity(up0);
    const double wet1 = nonlinearity(up1);
    return second_down_.process(wet0, wet1);
  }

  OversamplingFactor factor_ = OversamplingFactor::two;
  detail::HalfbandInterpolator<detail::first_stage_order, detail::first_stage_taps> first_up_;
  detail::HalfbandDecimator<detail::first_stage_order, detail::first_stage_taps> first_down_;
  detail::HalfbandInterpolator<detail::second_stage_order, detail::second_stage_taps> second_up_;
  detail::HalfbandDecimator<detail::second_stage_order, detail::second_stage_taps> second_down_;
  double pending_ = 0.0;
};

}

#endif
