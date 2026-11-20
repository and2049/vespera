#ifndef VESPERA_DSP_RESPONSE_HPP
#define VESPERA_DSP_RESPONSE_HPP

#include <vespera/dsp/common.hpp>
#include <vespera/dsp/fft.hpp>
#include <vespera/dsp/utility.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <numeric>
#include <span>
#include <stdexcept>
#include <vector>

// Offline preparation of impulse responses before they are turned into PreparedResponse objects.
// Everything here allocates and may throw std::invalid_argument; none of it is meant for the
// audio thread. Inputs are sample spans; file reading and decoding belong to the caller.

namespace vespera::dsp {

// Windowed-sinc resampler quality, relative to the lower of the two rates (f_min): the response
// is flat within +/-0.001 dB up to resampler_passband_ratio * f_min, and content above
// resampler_stopband_ratio * f_min (including images when upsampling) is attenuated by at least
// resampler_stopband_db. Between the two edges the response rolls off; it is -6 dB at
// 0.5 * f_min.
inline constexpr double resampler_passband_ratio = 0.45;
inline constexpr double resampler_stopband_ratio = 0.55;
inline constexpr double resampler_stopband_db = 110.0;

struct ResampledResponse {
  std::vector<float> samples;
  // samples[m] is the response at time (m - delay_frames) / target_rate. The leading frames keep
  // the pre-ringing of the band limit at the response onset; drop them with trim_range() or by
  // hand when the onset must stay at frame 0.
  std::size_t delay_frames = 0;
};

namespace detail {

inline constexpr double resampler_half_width = 48.0;
inline constexpr double resampler_kaiser_beta = 12.3;

inline double bessel_i0_series(double x) noexcept {
  double sum = 1.0;
  double term = 1.0;
  const double quarter_square = 0.25 * x * x;
  for (int k = 1; k < 500 && term > sum * 1.0e-17; ++k) {
    term *= quarter_square / (static_cast<double>(k) * static_cast<double>(k));
    sum += term;
  }
  return sum;
}

// Kaiser-windowed sinc low-pass at half the lower rate, as a function of the offset u in input
// samples. Its sum over integer offsets is 1, so it interpolates without changing the level.
class ResamplingKernel {
public:
  ResamplingKernel(double source_rate, double target_rate)
      : bandwidth_(std::min(source_rate, target_rate) / source_rate),
        half_width_(resampler_half_width / bandwidth_),
        window_scale_(1.0 / bessel_i0_series(resampler_kaiser_beta)) {}

  double half_width() const noexcept { return half_width_; }

  double operator()(double u) const noexcept {
    const double ratio = u / half_width_;
    if (ratio <= -1.0 || ratio >= 1.0) {
      return 0.0;
    }
    const double window =
        bessel_i0_series(resampler_kaiser_beta * std::sqrt(1.0 - ratio * ratio)) * window_scale_;
    const double x = bandwidth_ * u;
    const double sinc = x == 0.0 ? 1.0 : std::sin(std::numbers::pi * x) / (std::numbers::pi * x);
    return bandwidth_ * sinc * window;
  }

private:
  double bandwidth_;
  double half_width_;
  double window_scale_;
};

inline void require_finite(std::span<const float> samples) {
  if (!std::all_of(samples.begin(), samples.end(), [](float v) { return std::isfinite(v); })) {
    throw std::invalid_argument("Response samples must be finite");
  }
}

inline std::int64_t floor_divide(std::int64_t numerator, std::int64_t denominator) noexcept {
  const std::int64_t quotient = numerator / denominator;
  return quotient * denominator > numerator ? quotient - 1 : quotient;
}

}

// Converts a response sampled at source_rate to target_rate, preserving its frequency response
// in hertz within the quality above: the band-limited interpolation is scaled by
// source_rate / target_rate so that, for example, the DC gain (sum of samples) is unchanged.
// The kernel is a Kaiser-windowed sinc (beta 12.3) spanning 48 periods of the lower rate on each
// side, which sets delay_frames = ceil(48 * target_rate / min(rates)). Equal rates return a copy
// with no delay. Throws for rates outside [8000, 768000] Hz or non-finite samples.
inline ResampledResponse resample_response(std::span<const float> response, double source_rate,
                                           double target_rate) {
  validate_sample_rate(source_rate);
  validate_sample_rate(target_rate);
  detail::require_finite(response);
  ResampledResponse result;
  if (source_rate == target_rate || response.empty()) {
    result.samples.assign(response.begin(), response.end());
    return result;
  }
  const detail::ResamplingKernel kernel(source_rate, target_rate);
  const double step = source_rate / target_rate;
  const double output_half_width = kernel.half_width() / step;
  result.delay_frames = static_cast<std::size_t>(std::ceil(output_half_width - 1e-9));
  const double last_time = static_cast<double>(response.size() - 1) / step + output_half_width;
  const std::size_t count =
      result.delay_frames + static_cast<std::size_t>(std::ceil(last_time)) + 1;
  result.samples.assign(count, 0.0f);

  const auto reach = static_cast<std::int64_t>(std::ceil(kernel.half_width()));
  const auto taps = static_cast<std::size_t>(2 * reach);
  std::int64_t numerator_step = 0;
  std::int64_t denominator = 0;
  if (source_rate == std::floor(source_rate) && target_rate == std::floor(target_rate)) {
    const auto source = static_cast<std::int64_t>(source_rate);
    const auto target = static_cast<std::int64_t>(target_rate);
    const std::int64_t divisor = std::gcd(source, target);
    numerator_step = source / divisor;
    denominator = target / divisor;
  }
  const bool integral = denominator > 0 && denominator <= 4096;
  // Rows of kernel values per fractional phase, filled on first use when the ratio is rational.
  std::vector<std::vector<double>> rows(integral ? static_cast<std::size_t>(denominator) : 0);
  std::vector<double> scratch(taps);
  const double scale = step;
  for (std::size_t m = 0; m < count; ++m) {
    const auto shifted =
        static_cast<std::int64_t>(m) - static_cast<std::int64_t>(result.delay_frames);
    std::int64_t whole = 0;
    double fraction = 0.0;
    const std::vector<double> *row = &scratch;
    if (integral) {
      const std::int64_t numerator = shifted * numerator_step;
      whole = detail::floor_divide(numerator, denominator);
      const auto phase = static_cast<std::size_t>(numerator - whole * denominator);
      fraction = static_cast<double>(phase) / static_cast<double>(denominator);
      std::vector<double> &cached = rows[phase];
      if (cached.empty()) {
        cached.resize(taps);
        for (std::size_t j = 0; j < taps; ++j) {
          cached[j] = kernel(fraction + static_cast<double>(reach - 1) - static_cast<double>(j));
        }
      }
      row = &cached;
    } else {
      const double time = static_cast<double>(shifted) * step;
      whole = static_cast<std::int64_t>(std::floor(time));
      fraction = time - static_cast<double>(whole);
      for (std::size_t j = 0; j < taps; ++j) {
        scratch[j] = kernel(fraction + static_cast<double>(reach - 1) - static_cast<double>(j));
      }
    }
    double sum = 0.0;
    const std::int64_t first = whole - reach + 1;
    for (std::size_t j = 0; j < taps; ++j) {
      const std::int64_t n = first + static_cast<std::int64_t>(j);
      if (n >= 0 && n < static_cast<std::int64_t>(response.size())) {
        sum += (*row)[j] * static_cast<double>(response[static_cast<std::size_t>(n)]);
      }
    }
    result.samples[m] = static_cast<float>(sum * scale);
  }
  return result;
}

struct ResponseRange {
  std::size_t offset = 0;
  std::size_t frames = 0;
};

// The span of frames to keep, shared by all channels so they stay aligned. offset is the first
// frame in which any channel's magnitude reaches leading_threshold_db relative to the largest
// magnitude in any channel; the range ends after the last frame in which any channel reaches
// trailing_threshold_db. Silence gives {0, 0}. Thresholds must be finite and at most 0 dB.
inline ResponseRange trim_range(std::span<const std::span<const float>> channels,
                                double leading_threshold_db = -96.0,
                                double trailing_threshold_db = -80.0) {
  if (!std::isfinite(leading_threshold_db) || !std::isfinite(trailing_threshold_db) ||
      leading_threshold_db > 0.0 || trailing_threshold_db > 0.0) {
    throw std::invalid_argument("Trim thresholds must be finite and at most 0 dB");
  }
  double largest = 0.0;
  std::size_t frames = 0;
  for (std::span<const float> channel : channels) {
    detail::require_finite(channel);
    frames = std::max(frames, channel.size());
    for (float sample : channel) {
      largest = std::max(largest, std::abs(static_cast<double>(sample)));
    }
  }
  if (largest == 0.0) {
    return {};
  }
  const double leading = largest * decibels_to_gain(leading_threshold_db);
  const double trailing = largest * decibels_to_gain(trailing_threshold_db);
  std::size_t first = frames;
  std::size_t end = 0;
  for (std::span<const float> channel : channels) {
    for (std::size_t n = 0; n < channel.size(); ++n) {
      const double magnitude = std::abs(static_cast<double>(channel[n]));
      if (magnitude >= leading) {
        first = std::min(first, n);
      }
      if (magnitude >= trailing) {
        end = std::max(end, n + 1);
      }
    }
  }
  if (first >= end) {
    return {};
  }
  return {first, end - first};
}

inline ResponseRange trim_range(std::span<const float> response,
                                double leading_threshold_db = -96.0,
                                double trailing_threshold_db = -80.0) {
  return trim_range(std::span<const std::span<const float>>(&response, 1), leading_threshold_db,
                    trailing_threshold_db);
}

// energy: the mean over channels of the sum of squared samples becomes 1, so white noise keeps
// its level on average across channels. peak_gain: the largest magnitude of any channel's
// frequency response, sampled on a DFT of the next power of two at least four times the response
// length (at least 64), becomes 1 (0 dB).
enum class ResponseNormalisation { energy, peak_gain };

// The gain to multiply every channel by. Throws for silent or non-finite responses and for more
// than 2^19 frames with peak_gain.
inline double normalisation_gain(std::span<const std::span<const float>> channels,
                                 ResponseNormalisation rule) {
  std::size_t frames = 0;
  for (std::span<const float> channel : channels) {
    detail::require_finite(channel);
    frames = std::max(frames, channel.size());
  }
  double measure = 0.0;
  if (rule == ResponseNormalisation::energy) {
    for (std::span<const float> channel : channels) {
      for (float sample : channel) {
        measure += static_cast<double>(sample) * static_cast<double>(sample);
      }
    }
    measure = channels.empty() ? 0.0 : std::sqrt(measure / static_cast<double>(channels.size()));
  } else {
    if (frames > maximum_fft_size / 4) {
      throw std::invalid_argument("Response is too long for peak-gain normalisation");
    }
    const std::size_t size =
        std::max<std::size_t>(64, std::bit_ceil(4 * std::max<std::size_t>(frames, 1)));
    RealFft fft(size);
    std::vector<float> padded(size);
    std::vector<float> re(fft.bins());
    std::vector<float> im(fft.bins());
    for (std::span<const float> channel : channels) {
      std::fill(padded.begin(), padded.end(), 0.0f);
      std::copy(channel.begin(), channel.end(), padded.begin());
      fft.forward(padded, re, im);
      for (std::size_t k = 0; k < fft.bins(); ++k) {
        measure =
            std::max(measure, std::hypot(static_cast<double>(re[k]), static_cast<double>(im[k])));
      }
    }
  }
  if (!(measure > 0.0)) {
    throw std::invalid_argument("A silent response cannot be normalised");
  }
  return 1.0 / measure;
}

inline double normalisation_gain(std::span<const float> response, ResponseNormalisation rule) {
  return normalisation_gain(std::span<const std::span<const float>>(&response, 1), rule);
}

// (1 - weight) * first + weight * second per frame, the shorter response extended with zeros.
// Both must have the same sample rate. Responses captured at different distances arrive at
// different times; blending them unaligned produces comb filtering, while aligning their onsets
// first removes the natural delay difference between the microphones. Throws for a weight
// outside [0, 1] or non-finite samples.
inline std::vector<float> blend_responses(std::span<const float> first,
                                          std::span<const float> second, double weight) {
  if (!std::isfinite(weight) || weight < 0.0 || weight > 1.0) {
    throw std::invalid_argument("Blend weight must be within [0, 1]");
  }
  detail::require_finite(first);
  detail::require_finite(second);
  std::vector<float> result(std::max(first.size(), second.size()));
  for (std::size_t n = 0; n < result.size(); ++n) {
    const float a = n < first.size() ? first[n] : 0.0f;
    const float b = n < second.size() ? second[n] : 0.0f;
    result[n] = crossfade(a, b, weight);
  }
  return result;
}

}

#endif
