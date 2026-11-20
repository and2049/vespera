#ifndef VESPERA_DSP_BENCH_FILTERS_BENCH_HPP
#define VESPERA_DSP_BENCH_FILTERS_BENCH_HPP

#include "bench.hpp"

#include <vespera/dsp/crossover.hpp>
#include <vespera/dsp/state_variable_filter.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace vespera::dsp::bench {

// Filter-motion modes for the processing measurement. "moving" updates cutoff and Q every
// sample from a precomputed 4 Hz / 1 Hz modulation, so the timed loop contains the control
// updates but no trigonometry.
enum class FilterMotion { steady, moving };

struct FilterCase {
  double sample_rate = 48000.0;
  std::size_t channels = 2;
  std::size_t block_frames = 64;
  double audio_seconds = 2.0;
  std::size_t repeats = 3;
  FilterMotion motion = FilterMotion::steady;
  double low_cutoff = 100.0;
  double high_cutoff = 8000.0;
};

struct FilterMeasurement {
  // Processing time as a percentage of the audio duration, for the fastest repeat.
  double average_percent = 0.0;
  // The slowest single block of that repeat, as a percentage of the block's duration. It
  // includes any preemption by the operating system.
  double worst_block_percent = 0.0;
  // The 99.9th percentile of that repeat's block times, which separates scheduled work from
  // occasional preemption.
  double percentile_block_percent = 0.0;
  double checksum = 0.0;
};

namespace detail {

inline void validate_filter_case(const FilterCase &test, bool allow_moving) {
  require_rate(test.sample_rate);
  if (test.channels < 1 || test.channels > 2 || test.block_frames == 0 || test.repeats == 0 ||
      !(test.audio_seconds > 0.0)) {
    throw std::invalid_argument("filter case needs 1-2 channels, blocks, repeats and seconds");
  }
  if (test.motion == FilterMotion::moving && !allow_moving) {
    throw std::invalid_argument("moving controls are only measured for the state-variable filter");
  }
  require_frequency(test.sample_rate, test.low_cutoff);
  require_frequency(test.sample_rate, test.high_cutoff);
}

// Cutoffs and Q values cycling between their low and high bounds, at 4 Hz and 1 Hz.
inline std::vector<std::pair<double, double>> moving_controls(double sample_rate,
                                                              const FilterCase &test) {
  const auto frames = static_cast<std::size_t>(std::llround(test.audio_seconds * sample_rate));
  std::vector<std::pair<double, double>> controls(frames);
  const double cutoff_centre = std::sqrt(test.low_cutoff * test.high_cutoff);
  const double cutoff_radius = 0.5 * std::log(test.high_cutoff / test.low_cutoff);
  for (std::size_t n = 0; n < frames; ++n) {
    const double cutoff_phase = 2.0 * std::numbers::pi * 4.0 * static_cast<double>(n) / sample_rate;
    const double q_phase = 2.0 * std::numbers::pi * 1.0 * static_cast<double>(n) / sample_rate;
    controls[n] = {cutoff_centre * std::exp(cutoff_radius * std::sin(cutoff_phase)),
                   0.7071067811865476 * std::exp(0.5 * std::sin(q_phase))};
  }
  return controls;
}

inline std::vector<std::vector<float>> filter_input(const FilterCase &test) {
  const auto frames = static_cast<std::size_t>(std::llround(test.audio_seconds * test.sample_rate));
  std::vector<std::vector<float>> input;
  for (std::size_t c = 0; c < test.channels; ++c) {
    input.push_back(white_noise(frames, 0.5, 11 + c));
  }
  return input;
}

} // namespace detail

// Times the state-variable filter over white noise. In the steady mode the controls never
// change; in the moving mode every sample installs a new valid cutoff and Q. The result is the
// cost of one filter instance per channel, all three outputs evaluated.
inline FilterMeasurement measure_svf(const FilterCase &test) {
  detail::validate_filter_case(test, true);
  FilterMeasurement result;
  const auto frames = static_cast<std::size_t>(std::llround(test.audio_seconds * test.sample_rate));
  const std::vector<std::pair<double, double>> controls =
      test.motion == FilterMotion::moving ? detail::moving_controls(test.sample_rate, test)
                                          : std::vector<std::pair<double, double>>{};
  const std::vector<std::vector<float>> input = detail::filter_input(test);
  double best = HUGE_VAL;
  for (std::size_t repeat = 0; repeat < test.repeats; ++repeat) {
    std::vector<StateVariableFilter> filters(
        test.channels, StateVariableFilter(test.sample_rate, 1000.0, 0.7071067811865476));
    double checksum = 0.0;
    double total = 0.0;
    std::vector<double> loads;
    loads.reserve(frames / test.block_frames + 1);
    for (std::size_t start = 0; start < frames; start += test.block_frames) {
      const std::size_t count = std::min(test.block_frames, frames - start);
      const auto before = std::chrono::steady_clock::now();
      for (std::size_t i = 0; i < count; ++i) {
        if (test.motion == FilterMotion::moving) {
          for (StateVariableFilter &filter : filters) {
            if (!filter.try_set_controls(controls[start + i].first, controls[start + i].second)) {
              throw std::logic_error("svf bench control update was rejected");
            }
          }
        }
        for (std::size_t c = 0; c < test.channels; ++c) {
          const StateVariableFilter::Output output = filters[c].process(input[c][start + i]);
          checksum += output.low + output.band + output.high;
        }
      }
      const auto after = std::chrono::steady_clock::now();
      const double seconds = std::chrono::duration<double>(after - before).count();
      total += seconds;
      loads.push_back(seconds * test.sample_rate / static_cast<double>(count));
    }
    if (total < best) {
      best = total;
      result.average_percent = 100.0 * total * test.sample_rate / static_cast<double>(frames);
      std::sort(loads.begin(), loads.end());
      result.worst_block_percent = 100.0 * loads.back();
      const auto index = static_cast<std::size_t>(0.999 * static_cast<double>(loads.size() - 1));
      result.percentile_block_percent = 100.0 * loads[index];
      result.checksum = checksum;
    }
  }
  return result;
}

// Times the Linkwitz-Riley crossover over white noise with fixed controls; its cutoff changes
// are a quiescent operation, so no moving mode exists.
inline FilterMeasurement measure_crossover(const FilterCase &test) {
  detail::validate_filter_case(test, false);
  FilterMeasurement result;
  const auto frames = static_cast<std::size_t>(std::llround(test.audio_seconds * test.sample_rate));
  const std::vector<std::vector<float>> input = detail::filter_input(test);
  double best = HUGE_VAL;
  for (std::size_t repeat = 0; repeat < test.repeats; ++repeat) {
    std::vector<LinkwitzRileyCrossover> filters(test.channels,
                                                LinkwitzRileyCrossover(test.sample_rate, 1000.0));
    double checksum = 0.0;
    double total = 0.0;
    std::vector<double> loads;
    loads.reserve(frames / test.block_frames + 1);
    for (std::size_t start = 0; start < frames; start += test.block_frames) {
      const std::size_t count = std::min(test.block_frames, frames - start);
      const auto before = std::chrono::steady_clock::now();
      for (std::size_t i = 0; i < count; ++i) {
        for (std::size_t c = 0; c < test.channels; ++c) {
          const LinkwitzRileyCrossover::Output output = filters[c].process(input[c][start + i]);
          checksum += output.low + output.high;
        }
      }
      const auto after = std::chrono::steady_clock::now();
      const double seconds = std::chrono::duration<double>(after - before).count();
      total += seconds;
      loads.push_back(seconds * test.sample_rate / static_cast<double>(count));
    }
    if (total < best) {
      best = total;
      result.average_percent = 100.0 * total * test.sample_rate / static_cast<double>(frames);
      std::sort(loads.begin(), loads.end());
      result.worst_block_percent = 100.0 * loads.back();
      const auto index = static_cast<std::size_t>(0.999 * static_cast<double>(loads.size() - 1));
      result.percentile_block_percent = 100.0 * loads[index];
      result.checksum = checksum;
    }
  }
  return result;
}

struct FilterUpdateCost {
  // Nanoseconds per sample for one SVF processing a sample with fixed controls, and for one
  // try_set_controls call (validation and design) followed by processing a sample. Their
  // difference estimates the control update cost.
  double svf_sample_nanoseconds = 0.0;
  double svf_update_and_sample_nanoseconds = 0.0;
  // The same pair for the crossover's quiescent configure (four biquad designs and validation).
  double crossover_sample_nanoseconds = 0.0;
  double crossover_configure_and_sample_nanoseconds = 0.0;
  double checksum = 0.0;
};

// Times control updates against distinct precomputed log-uniform cutoffs within
// [low_cutoff, high_cutoff] and Q within [0.5, 8]. Every update is followed by a processed sample
// whose output is accumulated, so the compiler cannot hoist the design out of the loop or discard
// it; the matching fixed-control loop gives the processing share to subtract.
inline FilterUpdateCost measure_filter_update_cost(double sample_rate, double low_cutoff,
                                                   double high_cutoff) {
  detail::require_rate(sample_rate);
  detail::require_frequency(sample_rate, low_cutoff);
  detail::require_frequency(sample_rate, high_cutoff);
  constexpr std::size_t table = 4096;
  constexpr std::size_t calls = 200000;
  std::vector<double> cutoffs(table);
  std::vector<double> qs(table);
  const std::vector<float> noise = white_noise(table * 2, 0.5, 5);
  for (std::size_t i = 0; i < table; ++i) {
    const double u = 0.5 + static_cast<double>(noise[i]);
    const double v = 0.5 + static_cast<double>(noise[table + i]);
    cutoffs[i] = low_cutoff * std::pow(high_cutoff / low_cutoff, u);
    qs[i] = 0.5 * std::pow(16.0, v);
  }
  const auto per_call = [](auto before, auto after) {
    return std::chrono::duration<double>(after - before).count() * 1e9 / static_cast<double>(calls);
  };
  FilterUpdateCost result;
  double checksum = 0.0;

  StateVariableFilter svf(sample_rate, low_cutoff, 1.0);
  auto before = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < calls; ++i) {
    checksum += svf.process(noise[i % table]).band;
  }
  auto after = std::chrono::steady_clock::now();
  result.svf_sample_nanoseconds = per_call(before, after);
  svf.reset();
  before = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < calls; ++i) {
    if (!svf.try_set_controls(cutoffs[i % table], qs[i % table])) {
      throw std::logic_error("svf bench control update was rejected");
    }
    checksum += svf.process(noise[i % table]).band;
  }
  after = std::chrono::steady_clock::now();
  result.svf_update_and_sample_nanoseconds = per_call(before, after);

  LinkwitzRileyCrossover crossover(sample_rate, low_cutoff);
  before = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < calls; ++i) {
    checksum += crossover.process(noise[i % table]).high;
  }
  after = std::chrono::steady_clock::now();
  result.crossover_sample_nanoseconds = per_call(before, after);
  crossover.reset();
  before = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < calls; ++i) {
    crossover.configure(sample_rate, cutoffs[i % table]);
    checksum += crossover.process(noise[i % table]).high;
  }
  after = std::chrono::steady_clock::now();
  result.crossover_configure_and_sample_nanoseconds = per_call(before, after);
  result.checksum = checksum;
  return result;
}

}

#endif
