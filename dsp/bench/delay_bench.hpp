#ifndef VESPERA_DSP_BENCH_DELAY_BENCH_HPP
#define VESPERA_DSP_BENCH_DELAY_BENCH_HPP

#include "bench.hpp"

#include <vespera/dsp/allpass.hpp>
#include <vespera/dsp/delay.hpp>
#include <vespera/dsp/history.hpp>
#include <vespera/dsp/utility.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <vector>

namespace vespera::dsp::bench {

enum class DelayMode { steady, modulated, crossfade };

struct DelayCase {
  double sample_rate = 48000.0;
  double capacity_seconds = 0.2;
  std::size_t taps = 4;
  std::size_t block_frames = 64;
  double audio_seconds = 2.0;
  std::size_t repeats = 3;
  DelayMode mode = DelayMode::steady;
  // One shared HistoryBuffer against one FractionalDelay instance per tap.
  bool shared = true;
  // Shared reads use the third-order Lagrange kernel instead of linear.
  bool lagrange = false;
};

struct DelayMeasurement {
  std::size_t capacity_samples = 0;
  // Processing time as a percentage of the audio duration, for the fastest repeat.
  double average_percent = 0.0;
  // The 99.9th percentile of that repeat's block times, separating scheduled work from
  // occasional preemption.
  double percentile_block_percent = 0.0;
  // The slowest single block of that repeat, as a percentage of the block's duration. It
  // includes any preemption by the operating system.
  double worst_block_percent = 0.0;
  double checksum = 0.0;
  std::size_t bytes = 0;
};

namespace detail {

inline void record_best(std::vector<double> &loads, double total, std::size_t frames,
                        double sample_rate, double &best, DelayMeasurement &result) {
  if (total < best) {
    best = total;
    result.average_percent = 100.0 * total * sample_rate / static_cast<double>(frames);
    std::sort(loads.begin(), loads.end());
    result.worst_block_percent = 100.0 * loads.back();
    const auto index = static_cast<std::size_t>(0.999 * static_cast<double>(loads.size() - 1));
    result.percentile_block_percent = 100.0 * loads[index];
  }
}

} // namespace detail

// Times one multi-tap delay strategy over white noise. Steady mode reads fixed fractional
// delays, modulated sweeps each tap with a 0.5 Hz sine around three quarters of its base
// delay, and crossfade reads two fixed taps blended with a slowly moving weight. The
// modulation and blend signals are precomputed, so only the delay machinery is measured.
inline DelayMeasurement measure_delay(const DelayCase &test) {
  detail::require_rate(test.sample_rate);
  if (test.taps < 1 || test.taps > 16 || test.block_frames == 0 || test.repeats == 0 ||
      !(test.capacity_seconds > 0.0) || !(test.audio_seconds > 0.0)) {
    throw std::invalid_argument("delay case needs 1-16 taps, blocks, repeats and lengths");
  }
  DelayMeasurement result;
  result.capacity_samples = std::max<std::size_t>(
      32, static_cast<std::size_t>(std::llround(test.capacity_seconds * test.sample_rate)));
  const std::size_t frames = std::max<std::size_t>(
      1, static_cast<std::size_t>(std::llround(test.audio_seconds * test.sample_rate)));
  const std::size_t tap_count = test.mode == DelayMode::crossfade ? 2 : test.taps;
  std::vector<double> base(tap_count);
  for (std::size_t k = 0; k < tap_count; ++k) {
    base[k] = static_cast<double>(result.capacity_samples) * static_cast<double>(k + 1) /
                  static_cast<double>(tap_count + 1) +
              0.25;
  }
  std::vector<double> modulation(frames, 1.0);
  std::vector<double> blend(frames, 0.5);
  if (test.mode == DelayMode::modulated) {
    for (std::size_t n = 0; n < frames; ++n) {
      modulation[n] = 0.75 + 0.25 * std::sin(2.0 * std::numbers::pi * 0.5 * static_cast<double>(n) /
                                             test.sample_rate);
    }
  }
  if (test.mode == DelayMode::crossfade) {
    for (std::size_t n = 0; n < frames; ++n) {
      blend[n] = 0.5 * (1.0 - std::cos(2.0 * std::numbers::pi * 0.25 * static_cast<double>(n) /
                                       test.sample_rate));
    }
  }
  const std::vector<float> input = white_noise(frames, 0.5, 17);
  std::vector<float> output(frames, 0.0f);
  double best = std::numeric_limits<double>::max();

  if (test.shared) {
    HistoryBuffer history({test.sample_rate, result.capacity_samples});
    result.bytes = result.capacity_samples * 4;
    const Interpolation kernel = test.lagrange ? Interpolation::lagrange3 : Interpolation::linear;
    for (std::size_t repeat = 0; repeat < test.repeats; ++repeat) {
      history.reset();
      double total = 0.0;
      std::vector<double> loads;
      loads.reserve(frames / test.block_frames + 1);
      std::size_t n = 0;
      for (std::size_t start = 0; start < frames; start += test.block_frames) {
        const std::size_t count = std::min(test.block_frames, frames - start);
        const auto before = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < count; ++i, ++n) {
          history.push(input[n]);
          double sum = 0.0;
          if (test.mode == DelayMode::crossfade) {
            sum = crossfade(history.read(base[0], kernel), history.read(base[1], kernel), blend[n]);
          } else {
            for (const double delay : base) {
              sum += history.read(delay * modulation[n], kernel);
            }
          }
          history.advance();
          output[n] = static_cast<float>(sum);
        }
        const auto after = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(after - before).count();
        total += seconds;
        loads.push_back(seconds * test.sample_rate / static_cast<double>(count));
      }
      detail::record_best(loads, total, frames, test.sample_rate, best, result);
    }
  } else {
    if (test.lagrange) {
      throw std::invalid_argument("separate FractionalDelay buffers only interpolate linearly");
    }
    std::vector<FractionalDelay> lines;
    lines.reserve(tap_count);
    for (const double delay : base) {
      lines.emplace_back(DelayConfig{test.sample_rate, result.capacity_samples, delay});
    }
    result.bytes = tap_count * (result.capacity_samples + 2) * 4;
    for (std::size_t repeat = 0; repeat < test.repeats; ++repeat) {
      double total = 0.0;
      std::vector<double> loads;
      loads.reserve(frames / test.block_frames + 1);
      std::size_t n = 0;
      for (std::size_t start = 0; start < frames; start += test.block_frames) {
        const std::size_t count = std::min(test.block_frames, frames - start);
        const auto before = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < count; ++i, ++n) {
          if (test.mode == DelayMode::modulated) {
            for (std::size_t k = 0; k < tap_count; ++k) {
              if (!lines[k].try_set_delay_samples(base[k] * modulation[n])) {
                throw std::logic_error("delay bench modulation left the valid range");
              }
            }
          }
          double sum = 0.0;
          if (test.mode == DelayMode::crossfade) {
            sum = crossfade(lines[0].process(input[n]), lines[1].process(input[n]), blend[n]);
          } else {
            for (FractionalDelay &line : lines) {
              sum += line.process(input[n]);
            }
          }
          output[n] = static_cast<float>(sum);
        }
        const auto after = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(after - before).count();
        total += seconds;
        loads.push_back(seconds * test.sample_rate / static_cast<double>(count));
      }
      detail::record_best(loads, total, frames, test.sample_rate, best, result);
    }
  }
  result.checksum = measure(output).rms;
  return result;
}

struct DelayResetMeasurement {
  std::size_t capacity_samples = 0;
  double shared_nanoseconds = 0.0;
  double separate_nanoseconds = 0.0;
  std::size_t shared_bytes = 0;
  std::size_t separate_bytes_per_tap = 0;
};

// Times reset() of one HistoryBuffer against the equal-size clear a separate buffer pays.
// FractionalDelay's only read is relative to its own write cursor, so repeated full clears
// cannot be made observable through its API; the separate row therefore times the identical
// std::fill over a bench-owned vector of max_delay + 2 samples, which is exactly the work
// FractionalDelay::reset performs. Each clear is bracketed by a store and a load so the
// clearing work stays observable to the optimizer, and the final sink check fails if any
// clear was elided.
inline DelayResetMeasurement measure_delay_reset(double sample_rate, double capacity_seconds,
                                                 std::size_t repeats, std::size_t runs) {
  detail::require_rate(sample_rate);
  if (!(capacity_seconds > 0.0) || repeats == 0 || runs == 0) {
    throw std::invalid_argument("reset case needs a capacity, repeats and runs");
  }
  DelayResetMeasurement result;
  result.capacity_samples = std::max<std::size_t>(
      32, static_cast<std::size_t>(std::llround(capacity_seconds * sample_rate)));
  result.shared_bytes = result.capacity_samples * 4;
  result.separate_bytes_per_tap = (result.capacity_samples + 2) * 4;
  HistoryBuffer history({sample_rate, result.capacity_samples});
  std::vector<float> separate(result.capacity_samples + 2, 0.0f);
  double sink = 0.0;
  bool have_shared = false;
  bool have_separate = false;
  for (std::size_t repeat = 0; repeat < repeats; ++repeat) {
    auto before = std::chrono::steady_clock::now();
    for (std::size_t r = 0; r < runs; ++r) {
      history.push(1.0f);
      history.advance();
      history.reset();
      sink += history.read(static_cast<double>((r * 97) % result.capacity_samples));
    }
    auto after = std::chrono::steady_clock::now();
    const double shared =
        std::chrono::duration<double>(after - before).count() * 1e9 / static_cast<double>(runs);
    if (!have_shared || shared < result.shared_nanoseconds) {
      result.shared_nanoseconds = shared;
      have_shared = true;
    }
    before = std::chrono::steady_clock::now();
    for (std::size_t r = 0; r < runs; ++r) {
      separate[(r * 89) % separate.size()] = 1.0f;
      std::fill(separate.begin(), separate.end(), 0.0f);
      sink += separate[(r * 97) % separate.size()];
    }
    after = std::chrono::steady_clock::now();
    const double clear =
        std::chrono::duration<double>(after - before).count() * 1e9 / static_cast<double>(runs);
    if (!have_separate || clear < result.separate_nanoseconds) {
      result.separate_nanoseconds = clear;
      have_separate = true;
    }
  }
  if (!std::isfinite(sink) || sink != 0.0) {
    throw std::logic_error("delay bench reset measurements lost their observable effect");
  }
  return result;
}

struct AllpassMeasurement {
  double average_percent = 0.0;
  double checksum = 0.0;
};

// Steady Schroeder allpass cost over white noise at one integer loop delay.
inline AllpassMeasurement measure_schroeder_allpass(double sample_rate, std::size_t delay_samples,
                                                    std::size_t block_frames, double audio_seconds,
                                                    std::size_t repeats) {
  detail::require_rate(sample_rate);
  if (delay_samples == 0 || delay_samples > maximum_allpass_delay_samples || block_frames == 0 ||
      repeats == 0 || !(audio_seconds > 0.0)) {
    throw std::invalid_argument("allpass case needs a positive delay, blocks and repeats");
  }
  SchroederAllpass allpass(AllpassConfig{sample_rate, delay_samples, 0.5});
  const std::size_t frames =
      std::max<std::size_t>(1, static_cast<std::size_t>(std::llround(audio_seconds * sample_rate)));
  const std::vector<float> input = white_noise(frames, 0.5, 23);
  std::vector<float> output(frames);
  double best = std::numeric_limits<double>::max();
  AllpassMeasurement result;
  for (std::size_t repeat = 0; repeat < repeats; ++repeat) {
    allpass.reset();
    const auto before = std::chrono::steady_clock::now();
    std::size_t start = 0;
    while (start < frames) {
      const std::size_t count = std::min(block_frames, frames - start);
      allpass.process(std::span<const float>(input.data() + start, count),
                      std::span<float>(output.data() + start, count));
      start += count;
    }
    const auto after = std::chrono::steady_clock::now();
    best = std::min(best, std::chrono::duration<double>(after - before).count());
  }
  result.average_percent = 100.0 * best * sample_rate / static_cast<double>(frames);
  result.checksum = measure(output).rms;
  return result;
}

// Steady first-order allpass cost over white noise.
inline AllpassMeasurement measure_first_order_allpass(double sample_rate, std::size_t block_frames,
                                                      double audio_seconds, std::size_t repeats) {
  detail::require_rate(sample_rate);
  if (block_frames == 0 || repeats == 0 || !(audio_seconds > 0.0)) {
    throw std::invalid_argument("allpass case needs blocks and repeats");
  }
  FirstOrderAllpass allpass(0.5);
  const std::size_t frames =
      std::max<std::size_t>(1, static_cast<std::size_t>(std::llround(audio_seconds * sample_rate)));
  const std::vector<float> input = white_noise(frames, 0.5, 29);
  std::vector<float> output(frames);
  double best = std::numeric_limits<double>::max();
  AllpassMeasurement result;
  for (std::size_t repeat = 0; repeat < repeats; ++repeat) {
    allpass.reset();
    const auto before = std::chrono::steady_clock::now();
    std::size_t start = 0;
    while (start < frames) {
      const std::size_t count = std::min(block_frames, frames - start);
      allpass.process(std::span<const float>(input.data() + start, count),
                      std::span<float>(output.data() + start, count));
      start += count;
    }
    const auto after = std::chrono::steady_clock::now();
    best = std::min(best, std::chrono::duration<double>(after - before).count());
  }
  result.average_percent = 100.0 * best * sample_rate / static_cast<double>(frames);
  result.checksum = measure(output).rms;
  return result;
}

} // namespace vespera::dsp::bench

#endif
