#ifndef VESPERA_DSP_BENCH_BASELINE_BENCH_HPP
#define VESPERA_DSP_BENCH_BASELINE_BENCH_HPP

#include "bench.hpp"
#include "convolution_bench.hpp"

#include <vespera/dsp/convolution.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <malloc/malloc.h>
#include <sys/sysctl.h>
#endif

namespace vespera::dsp::bench {

// One M1 baseline scenario over a fixed per-channel toolkit graph: FractionalDelay (12.5 samples,
// 2048-sample capacity) -> Biquad lowpass -> envelope-driven limiter (1 ms attack, 100 ms release,
// -12 dB threshold) -> LinearSmoother gain (20 ms) -> Lfo tremolo (4 Hz). The scenarios differ
// only in controls and input:
//   steady: fixed controls, white-noise input;
//   moving: every block retargets the delay, the gain smoother and redesigns the lowpass, so
//           control-rate work and filter redesign are measured with the block's processing;
//   silent: zero input with fixed controls, the tail/idle work;
//   reset: fixed controls and a reset of every primitive every 32nd block, timed with the block.
enum class ToolkitWorkload { steady, moving, silent, reset };

struct ToolkitCase {
  double sample_rate = 48000.0;
  std::size_t channels = 2;
  std::size_t block_frames = 64;
  double audio_seconds = 2.0;
  std::size_t repeats = 3;
  ToolkitWorkload workload = ToolkitWorkload::steady;
  bool mixed_blocks = false;
};

struct ToolkitMeasurement {
  // Processing time of the best repeat as a percentage of the audio duration, the 99.9th
  // percentile and the slowest single block of that repeat, plus its block and reset counts.
  double average_percent = 0.0;
  double percentile_block_percent = 0.0;
  double worst_block_percent = 0.0;
  double checksum = 0.0;
  std::size_t blocks = 0;
  std::size_t resets = 0;
};

namespace detail {

inline std::size_t baseline_block_at(const ToolkitCase &test, std::size_t block_index) {
  if (!test.mixed_blocks) {
    return test.block_frames;
  }
  return block_pattern[block_index % std::size(block_pattern)];
}

inline double baseline_load_average() {
#if defined(__APPLE__) || defined(__linux__)
  double load = 0.0;
  return getloadavg(&load, 1) == 1 ? load : -1.0;
#else
  return -1.0;
#endif
}

inline double baseline_lowpass_hz(double sample_rate) {
  return std::min(8000.0, 0.3 * sample_rate);
}

struct BaselineChannel {
  explicit BaselineChannel(double sample_rate)
      : delay(DelayConfig{sample_rate, 2048, 12.5}),
        filter(
            BiquadCoefficients::lowpass(sample_rate, baseline_lowpass_hz(sample_rate), lowpass_q)),
        envelope(EnvelopeConfig{sample_rate, limit_attack_seconds, limit_release_seconds}),
        tremolo(LfoConfig{sample_rate, 4.0, LfoWaveform::sine, 0.0}) {
    gain.configure(sample_rate, 0.02);
    gain.set_target(0.5);
  }

  FractionalDelay delay;
  Biquad filter;
  EnvelopeFollower envelope;
  Lfo tremolo;
  LinearSmoother gain;
  double limit_scale = 0.251188643150958; // -12 dB
};

} // namespace detail

inline ToolkitMeasurement measure_toolkit_case(const ToolkitCase &test) {
  detail::require_rate(test.sample_rate);
  if (test.channels < 1 || test.channels > 2 || test.block_frames == 0 || test.repeats == 0 ||
      !(test.audio_seconds > 0.0)) {
    throw std::invalid_argument("toolkit case needs 1-2 channels, blocks, repeats and length");
  }
  ToolkitMeasurement result;
  const auto frames = static_cast<std::size_t>(std::llround(test.audio_seconds * test.sample_rate));
  std::vector<detail::BaselineChannel> channels(test.channels,
                                                detail::BaselineChannel(test.sample_rate));
  std::vector<std::vector<float>> input;
  for (std::size_t c = 0; c < test.channels; ++c) {
    input.push_back(test.workload == ToolkitWorkload::silent ? std::vector<float>(frames, 0.0f)
                                                             : white_noise(frames, 0.5, 31 + c));
  }
  std::vector<std::vector<float>> output(test.channels, std::vector<float>(frames, 0.0f));
  double best = HUGE_VAL;
  for (std::size_t repeat = 0; repeat < test.repeats; ++repeat) {
    for (detail::BaselineChannel &channel : channels) {
      channel = detail::BaselineChannel(test.sample_rate);
    }
    double total = 0.0;
    std::vector<double> loads;
    loads.reserve(frames / test.block_frames + 1);
    std::size_t resets = 0;
    std::size_t block_index = 0;
    for (std::size_t start = 0; start < frames;) {
      const std::size_t block_frames = detail::baseline_block_at(test, block_index++);
      const std::size_t count = std::min(block_frames, frames - start);
      const auto before = std::chrono::steady_clock::now();
      if (test.workload == ToolkitWorkload::moving) {
        const double phase =
            0.5 + 0.5 * std::sin(6.283185307179586 * static_cast<double>(block_index) / 64.0);
        const double cutoff =
            1000.0 + (detail::baseline_lowpass_hz(test.sample_rate) - 1000.0) * phase;
        const BiquadCoefficients redesign =
            BiquadCoefficients::lowpass(test.sample_rate, cutoff, lowpass_q);
        for (detail::BaselineChannel &channel : channels) {
          (void)channel.delay.try_set_delay_samples(2047.5 * phase);
          channel.filter.set_coefficients(redesign);
          channel.gain.set_target(0.25 + 0.5 * phase);
        }
      }
      if (test.workload == ToolkitWorkload::reset && block_index % 32 == 0) {
        for (detail::BaselineChannel &channel : channels) {
          channel.delay.reset();
          channel.filter.reset();
          channel.envelope.reset();
          channel.tremolo.reset();
          channel.gain.reset(0.5);
        }
        ++resets;
      }
      for (std::size_t c = 0; c < test.channels; ++c) {
        detail::BaselineChannel &channel = channels[c];
        float *data = output[c].data() + start;
        const float *source = input[c].data() + start;
        for (std::size_t i = 0; i < count; ++i) {
          const double delayed = channel.delay.process(source[i]);
          const double filtered = channel.filter.process(delayed);
          const double envelope = channel.envelope.process(static_cast<float>(filtered));
          const double limited = envelope > channel.limit_scale
                                     ? filtered * (channel.limit_scale / envelope)
                                     : filtered;
          const double tremolo = 0.5 * (1.0 + channel.tremolo.process());
          data[i] = static_cast<float>(limited * channel.gain.process() * tremolo);
        }
      }
      const auto after = std::chrono::steady_clock::now();
      const double seconds = std::chrono::duration<double>(after - before).count();
      total += seconds;
      loads.push_back(seconds * test.sample_rate / static_cast<double>(count));
      start += count;
    }
    if (total < best) {
      best = total;
      result.average_percent = 100.0 * total * test.sample_rate / static_cast<double>(frames);
      std::sort(loads.begin(), loads.end());
      result.worst_block_percent = 100.0 * loads.back();
      const auto index = static_cast<std::size_t>(0.999 * static_cast<double>(loads.size() - 1));
      result.percentile_block_percent = 100.0 * loads[index];
      result.blocks = loads.size();
      result.resets = resets;
    }
  }
  result.checksum = 0.0;
  for (const std::vector<float> &channel : output) {
    result.checksum += measure(channel).rms;
  }
  return result;
}

// Cost of Convolver::reset(), which is allocation-free but clears memory proportional to the
// configured capacity: the average over `reset_count` resets of a convolver that has just
// processed at least its capacity.
struct ResetMeasurement {
  double seconds_per_reset = 0.0;
  std::size_t reset_count = 0;
  std::size_t capacity_frames = 0;
  double checksum = 0.0;
};

inline ResetMeasurement measure_convolution_reset(const ConvolutionCase &test,
                                                  std::size_t reset_count = 32) {
  detail::require_rate(test.sample_rate);
  if (test.channels < 1 || test.channels > 2 || !(test.response_seconds > 0.0) ||
      reset_count == 0) {
    throw std::invalid_argument("reset case needs 1-2 channels and a positive length");
  }
  ResetMeasurement result;
  result.reset_count = reset_count;
  result.capacity_frames = std::max<std::size_t>(
      1, static_cast<std::size_t>(std::llround(test.response_seconds * test.sample_rate)));
  ConvolverConfig config;
  config.sample_rate = test.sample_rate;
  config.channels = test.channels;
  config.max_response_frames = result.capacity_frames;
  config.fade_seconds = 0.02;
  std::vector<std::vector<float>> data;
  std::vector<std::span<const float>> spans;
  for (std::size_t c = 0; c < test.channels; ++c) {
    data.push_back(synthetic_response(result.capacity_frames, 1 + c));
    spans.emplace_back(data.back());
  }
  const auto response = std::make_unique<PreparedResponse>(config, spans);
  Convolver convolver(config);
  convolver.set_response(response.get());
  const std::size_t warm = result.capacity_frames + 4 * convolver.layout().largest_partition();
  const std::vector<float> input(warm, 0.25f);
  std::vector<std::vector<float>> output(test.channels, std::vector<float>(warm));
  std::array<const float *, 2> in{};
  std::array<float *, 2> out{};
  for (std::size_t c = 0; c < test.channels; ++c) {
    in[c] = input.data();
    out[c] = output[c].data();
  }
  convolver.process(std::span<const float *const>(in.data(), test.channels),
                    std::span<float *const>(out.data(), test.channels), warm);
  const auto before = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < reset_count; ++i) {
    convolver.reset();
  }
  const auto after = std::chrono::steady_clock::now();
  result.seconds_per_reset =
      std::chrono::duration<double>(after - before).count() / static_cast<double>(reset_count);
  convolver.process(std::span<const float *const>(in.data(), test.channels),
                    std::span<float *const>(out.data(), test.channels), warm);
  for (const std::vector<float> &channel : output) {
    result.checksum += measure(std::span<const float>(channel).first(1024)).rms;
  }
  return result;
}

// Bytes of allocator memory retained by one two-response set and by a convolver, measured as the
// change in allocator bytes in use around their construction. Reported as unmeasured where no
// supported allocator introspection exists.
struct ConstructionMemory {
  double response_bytes = -1.0;
  double convolver_bytes = -1.0;
  bool measured = false;
};

inline ConstructionMemory measure_construction_memory(const ConvolutionCase &test) {
  ConstructionMemory result;
  // ThreadSanitizer replaces the allocator, so mstats no longer observes these constructions.
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define BENCH_THREAD_SANITIZER 1
#endif
#elif defined(__SANITIZE_THREAD__)
#define BENCH_THREAD_SANITIZER 1
#endif
#if defined(__APPLE__) && !defined(BENCH_THREAD_SANITIZER)
  result.measured = true;
  detail::require_rate(test.sample_rate);
  if (test.channels < 1 || test.channels > 2 || !(test.response_seconds > 0.0)) {
    throw std::invalid_argument("memory case needs 1-2 channels and a positive length");
  }
  const std::size_t frames = std::max<std::size_t>(
      1, static_cast<std::size_t>(std::llround(test.response_seconds * test.sample_rate)));
  ConvolverConfig config;
  config.sample_rate = test.sample_rate;
  config.channels = test.channels;
  config.max_response_frames = frames;
  config.fade_seconds = 0.02;
  std::vector<std::vector<float>> data;
  std::vector<std::span<const float>> spans;
  for (std::size_t c = 0; c < test.channels; ++c) {
    data.push_back(synthetic_response(frames, 3 + c));
    spans.emplace_back(data.back());
  }
  const struct mstats before = mstats();
  const std::unique_ptr<PreparedResponse> response_a =
      std::make_unique<PreparedResponse>(config, spans);
  const std::unique_ptr<PreparedResponse> response_b =
      std::make_unique<PreparedResponse>(config, spans);
  const struct mstats after_responses = mstats();
  const Convolver convolver(config);
  const struct mstats after_convolver = mstats();
  result.response_bytes =
      static_cast<double>(after_responses.bytes_used) - static_cast<double>(before.bytes_used);
  result.convolver_bytes = static_cast<double>(after_convolver.bytes_used) -
                           static_cast<double>(after_responses.bytes_used);
  if (!(measure(data.front()).rms >= 0.0) || convolver.channels() != test.channels) {
    throw std::logic_error("baseline memory case failed to construct");
  }
#else
  (void)test;
#endif
  return result;
}

// Compiler, CPU and build context for the report header; interference is reported per case by
// baseline_load_average().
inline std::string environment_report() {
  std::string report;
#if defined(__clang__)
  report += "clang " + std::to_string(__clang_major__) + "." + std::to_string(__clang_minor__);
#elif defined(__GNUC__)
  report += "gcc " + std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__);
#else
  report += "unknown compiler";
#endif
#if defined(__aarch64__)
  report += ", aarch64";
#elif defined(__x86_64__)
  report += ", x86-64";
#endif
#if defined(NDEBUG)
  report += ", NDEBUG";
#endif
#if defined(VESPERA_DSP_BENCH_BUILD_TYPE)
  report += ", build type " + std::string(VESPERA_DSP_BENCH_BUILD_TYPE);
#endif
#if defined(__APPLE__)
  char buffer[256] = {};
  std::size_t size = sizeof buffer;
  if (sysctlbyname("machdep.cpu.brand_string", buffer, &size, nullptr, 0) == 0) {
    report += ", " + std::string(buffer);
  }
#endif
  return report;
}

} // namespace vespera::dsp::bench

#endif
