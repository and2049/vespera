#ifndef VESPERA_DSP_BENCH_CONVOLUTION_BENCH_HPP
#define VESPERA_DSP_BENCH_CONVOLUTION_BENCH_HPP

#include "bench.hpp"

#include <vespera/dsp/convolution.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

namespace vespera::dsp::bench {

constexpr std::size_t block_pattern[] = {1, 16, 64, 128, 512};

struct ConvolutionCase {
  double sample_rate = 48000.0;
  std::size_t channels = 2;
  double response_seconds = 0.2;
  std::size_t block_frames = 64;
  double audio_seconds = 2.0;
  std::size_t repeats = 3;
  // Request a new response whenever the previous switch has finished, so that most blocks are
  // priming or fading.
  bool switching = false;
  // Process digital silence instead of noise: the grid work still runs, so this measures the
  // silent-tail cost.
  bool silent_input = false;
  // Cycle the block size through 1/16/64/128/512 frames instead of block_frames.
  bool mixed_blocks = false;
};

struct ConvolutionMeasurement {
  std::size_t response_frames = 0;
  ConvolutionLayout layout;
  // Processing time as a percentage of the audio duration, for the fastest repeat.
  double average_percent = 0.0;
  // The slowest single block of that repeat, as a percentage of the block's duration. It includes
  // any preemption by the operating system.
  double worst_block_percent = 0.0;
  // The 99.9th percentile of that repeat's block times, which separates scheduled work from
  // occasional preemption.
  double percentile_block_percent = 0.0;
  double checksum = 0.0;
};

// Deterministic decaying noise standing in for a measured cabinet response.
inline std::vector<float> synthetic_response(std::size_t frames, std::uint64_t seed) {
  std::vector<float> response = white_noise(frames, 0.5, seed);
  for (std::size_t n = 0; n < frames; ++n) {
    response[n] *= static_cast<float>(std::exp(
        -6.0 * static_cast<double>(n) / static_cast<double>(std::max<std::size_t>(frames, 1))));
  }
  return response;
}

inline ConvolutionMeasurement measure_convolution(const ConvolutionCase &test) {
  detail::require_rate(test.sample_rate);
  if (test.channels < 1 || test.channels > 2 || test.block_frames == 0 || test.repeats == 0 ||
      !(test.response_seconds > 0.0) || !(test.audio_seconds > 0.0)) {
    throw std::invalid_argument("convolution case needs 1-2 channels, blocks, repeats and lengths");
  }
  ConvolutionMeasurement result;
  result.response_frames = std::max<std::size_t>(
      1, static_cast<std::size_t>(std::llround(test.response_seconds * test.sample_rate)));
  ConvolverConfig config;
  config.sample_rate = test.sample_rate;
  config.channels = test.channels;
  config.max_response_frames = result.response_frames;
  config.fade_seconds = 0.02;
  std::array<std::unique_ptr<PreparedResponse>, 2> responses;
  for (std::size_t r = 0; r < responses.size(); ++r) {
    std::vector<std::vector<float>> data;
    std::vector<std::span<const float>> spans;
    for (std::size_t c = 0; c < test.channels; ++c) {
      data.push_back(synthetic_response(result.response_frames, 1 + 2 * r + c));
    }
    for (const std::vector<float> &channel : data) {
      spans.emplace_back(channel);
    }
    responses[r] = std::make_unique<PreparedResponse>(config, spans);
  }
  Convolver convolver(config);
  result.layout = convolver.layout();
  const auto frames = static_cast<std::size_t>(std::llround(test.audio_seconds * test.sample_rate));
  std::vector<std::vector<float>> input;
  std::vector<std::vector<float>> output(test.channels, std::vector<float>(frames));
  for (std::size_t c = 0; c < test.channels; ++c) {
    input.push_back(test.silent_input ? std::vector<float>(frames, 0.0f)
                                      : white_noise(frames, 0.5, 11 + c));
  }
  double best = HUGE_VAL;
  for (std::size_t repeat = 0; repeat < test.repeats; ++repeat) {
    convolver.set_response(responses[0].get());
    while (convolver.take_retired() != nullptr) {
    }
    std::size_t next = 1;
    double total = 0.0;
    std::vector<double> loads;
    loads.reserve(frames / test.block_frames + 1);
    std::size_t start = 0;
    for (std::size_t block_index = 0; start < frames; ++block_index) {
      const std::size_t block_frames = test.mixed_blocks
                                           ? block_pattern[block_index % std::size(block_pattern)]
                                           : test.block_frames;
      const std::size_t count = std::min(block_frames, frames - start);
      std::array<const float *, 2> in{};
      std::array<float *, 2> out{};
      for (std::size_t c = 0; c < test.channels; ++c) {
        in[c] = input[c].data() + start;
        out[c] = output[c].data() + start;
      }
      if (test.switching && convolver.switch_state() == ConvolverSwitch::steady) {
        if (!convolver.crossfade_to(responses[next].get())) {
          throw std::logic_error("convolution bench switch was refused");
        }
        next = 1 - next;
      }
      const auto before = std::chrono::steady_clock::now();
      convolver.process(std::span<const float *const>(in.data(), test.channels),
                        std::span<float *const>(out.data(), test.channels), count);
      const auto after = std::chrono::steady_clock::now();
      while (convolver.take_retired() != nullptr) {
      }
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
    }
  }
  for (const std::vector<float> &channel : output) {
    result.checksum += measure(channel).rms;
  }
  return result;
}

struct DirectMeasurement {
  double average_percent = 0.0;
  double checksum = 0.0;
};

// Time-domain convolution over a contiguous history per channel, with sixteen partial sums so
// that the compiler can vectorize it: the cost a module pays without partitioning.
inline DirectMeasurement measure_direct_convolution(const ConvolutionCase &test) {
  constexpr std::size_t lanes = 16;
  const auto taps = std::max<std::size_t>(
      1, static_cast<std::size_t>(std::llround(test.response_seconds * test.sample_rate)));
  const std::size_t padded = (taps + lanes - 1) / lanes * lanes;
  const auto frames = static_cast<std::size_t>(std::llround(test.audio_seconds * test.sample_rate));
  std::vector<float> reversed(padded, 0.0f);
  const std::vector<float> response = synthetic_response(taps, 1);
  std::reverse_copy(response.begin(), response.end(), reversed.begin());
  std::vector<std::vector<float>> histories(test.channels,
                                            std::vector<float>(padded + frames, 0.0f));
  for (std::size_t c = 0; c < test.channels; ++c) {
    const std::vector<float> noise = white_noise(frames, 0.5, 11 + c);
    std::copy(noise.begin(), noise.end(),
              histories[c].begin() + static_cast<std::ptrdiff_t>(padded - 1));
  }
  std::vector<float> output(frames);
  DirectMeasurement result;
  double best = HUGE_VAL;
  for (std::size_t repeat = 0; repeat < test.repeats; ++repeat) {
    const auto before = std::chrono::steady_clock::now();
    for (const std::vector<float> &history : histories) {
      for (std::size_t n = 0; n < frames; ++n) {
        const float *window = history.data() + n;
        float partial[lanes] = {};
        for (std::size_t k = 0; k < padded; k += lanes) {
          for (std::size_t l = 0; l < lanes; ++l) {
            partial[l] += reversed[k + l] * window[k + l];
          }
        }
        float sum = 0.0f;
        for (float value : partial) {
          sum += value;
        }
        output[n] = sum;
      }
    }
    const auto after = std::chrono::steady_clock::now();
    best = std::min(best, std::chrono::duration<double>(after - before).count());
  }
  result.average_percent = 100.0 * best * test.sample_rate / static_cast<double>(frames);
  result.checksum = measure(output).rms;
  return result;
}

}

#endif
