#include <vespera/core_io.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

std::atomic<bool> counting_allocations{false};
std::atomic<uint64_t> allocation_count{0};

}

void *operator new(std::size_t size) {
  if (counting_allocations.load(std::memory_order_relaxed)) {
    allocation_count.fetch_add(1, std::memory_order_relaxed);
  }
  if (void *memory = std::malloc(size == 0 ? 1 : size)) {
    return memory;
  }
  throw std::bad_alloc();
}

void *operator new[](std::size_t size) { return operator new(size); }
void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete[](void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void *memory, std::size_t) noexcept { std::free(memory); }

namespace {

using vespera::CoreIoProcessor;
using vespera::CoreIoSettings;
using vespera::TransposeLatency;
using Signal = std::vector<float>;
using Channels = std::vector<Signal>;

int failures = 0;
const char *current_test = "";

#define CHECK(condition)                                                                           \
  do {                                                                                             \
    if (!(condition)) {                                                                            \
      std::fprintf(stderr, "%s:%d: %s: CHECK(%s) failed\n", __FILE__, __LINE__, current_test,      \
                   #condition);                                                                    \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (false)

constexpr double rate = 48000.0;
constexpr uint32_t capacity = 512;
constexpr double pi = 3.14159265358979323846;

Signal sine(double frequency, double amplitude, std::size_t frames, double sample_rate = rate) {
  Signal signal(frames);
  for (std::size_t n = 0; n < frames; ++n) {
    signal[n] = static_cast<float>(
        amplitude * std::sin(2.0 * pi * frequency * static_cast<double>(n) / sample_rate));
  }
  return signal;
}

Signal noise(std::size_t frames, uint32_t seed, float amplitude = 0.5f) {
  Signal signal(frames);
  uint32_t state = seed;
  for (float &sample : signal) {
    state = state * 1664525u + 1013904223u;
    sample = amplitude * (static_cast<float>(state >> 8) / 8388608.0f - 1.0f);
  }
  return signal;
}

std::vector<uint32_t> uniform_blocks(std::size_t frames, uint32_t block) {
  std::vector<uint32_t> blocks;
  for (std::size_t done = 0; done < frames;) {
    const auto size = static_cast<uint32_t>(std::min<std::size_t>(block, frames - done));
    blocks.push_back(size);
    done += size;
  }
  return blocks;
}

std::vector<uint32_t> random_blocks(std::size_t frames, uint32_t seed) {
  std::vector<uint32_t> blocks;
  uint32_t state = seed;
  for (std::size_t done = 0; done < frames;) {
    state = state * 1664525u + 1013904223u;
    auto size = static_cast<uint32_t>((state >> 16) % capacity);
    size = static_cast<uint32_t>(std::min<std::size_t>(size, frames - done));
    blocks.push_back(size);
    done += size;
  }
  return blocks;
}

using BlockHook = std::function<void(std::size_t)>;

Channels run_input(CoreIoProcessor &processor, const Channels &input,
                   const std::vector<uint32_t> &blocks, const BlockHook &before = {}) {
  const auto channels = static_cast<uint32_t>(input.size());
  Channels output(channels, Signal(input[0].size(), 0.0f));
  std::size_t offset = 0;
  for (uint32_t frames : blocks) {
    if (before) {
      before(offset);
    }
    const float *in[2] = {};
    float *out[2] = {};
    for (uint32_t c = 0; c < channels; ++c) {
      in[c] = input[c].data() + offset;
      out[c] = output[c].data() + offset;
    }
    CHECK(processor.process_input(in, out, channels, frames));
    offset += frames;
  }
  return output;
}

Channels run_input(CoreIoProcessor &processor, const Channels &input, uint32_t block = 256) {
  return run_input(processor, input, uniform_blocks(input[0].size(), block));
}

Channels run_output(CoreIoProcessor &processor, const Channels &input, uint32_t output_channels,
                    const std::vector<uint32_t> &blocks, const BlockHook &before = {}) {
  const auto input_channels = static_cast<uint32_t>(input.size());
  Channels output(output_channels, Signal(input[0].size(), 0.0f));
  std::size_t offset = 0;
  for (uint32_t frames : blocks) {
    if (before) {
      before(offset);
    }
    const float *in[2] = {};
    float *out[2] = {};
    for (uint32_t c = 0; c < input_channels; ++c) {
      in[c] = input[c].data() + offset;
    }
    for (uint32_t c = 0; c < output_channels; ++c) {
      out[c] = output[c].data() + offset;
    }
    CHECK(processor.process_output(in, input_channels, out, output_channels, frames));
    offset += frames;
  }
  return output;
}

Channels run_output(CoreIoProcessor &processor, const Channels &input, uint32_t output_channels,
                    uint32_t block = 256) {
  return run_output(processor, input, output_channels, uniform_blocks(input[0].size(), block));
}

double rms(const Signal &signal, std::size_t begin, std::size_t end) {
  double sum = 0.0;
  for (std::size_t n = begin; n < end; ++n) {
    sum += static_cast<double>(signal[n]) * signal[n];
  }
  return std::sqrt(sum / static_cast<double>(end - begin));
}

double goertzel_power(const Signal &signal, std::size_t begin, std::size_t end, double frequency,
                      double sample_rate = rate) {
  const double coefficient = 2.0 * std::cos(2.0 * pi * frequency / sample_rate);
  double s1 = 0.0;
  double s2 = 0.0;
  for (std::size_t n = begin; n < end; ++n) {
    const double hann = 0.5 - 0.5 * std::cos(2.0 * pi * static_cast<double>(n - begin) /
                                             static_cast<double>(end - begin - 1));
    const double s0 = signal[n] * hann + coefficient * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  return s1 * s1 + s2 * s2 - coefficient * s1 * s2;
}

double max_step(const Signal &signal, std::size_t begin, std::size_t end) {
  double step = 0.0;
  for (std::size_t n = std::max<std::size_t>(begin, 1); n < end; ++n) {
    step = std::max(step, std::abs(static_cast<double>(signal[n]) - signal[n - 1]));
  }
  return step;
}

bool delayed_equal(const Signal &output, const Signal &input, std::size_t delay,
                   std::size_t begin = 0) {
  for (std::size_t n = begin; n < output.size(); ++n) {
    const float expected = n >= delay ? input[n - delay] : 0.0f;
    if (output[n] != expected) {
      return false;
    }
  }
  return true;
}

void prepare(CoreIoProcessor &processor, const CoreIoSettings &settings,
             TransposeLatency latency = TransposeLatency::none, double sample_rate = rate) {
  processor.prepare(sample_rate, capacity, latency);
  CHECK(processor.set_settings(settings));
  processor.reset();
}

void test_settings_validation() {
  CoreIoSettings settings;
  CHECK(vespera::validate_core_io_settings(settings));
  CHECK(!settings.gate_enabled);
  CHECK(!settings.doubler_enabled);
  CHECK(settings.gate_threshold_db == -80.0f);
  CHECK(settings.doubler_delay_ms == 6.0f);

  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  const std::vector<std::pair<float CoreIoSettings::*, std::vector<float>>> invalid = {
      {&CoreIoSettings::input_gain_db, {nan, inf, -60.5f, 24.5f}},
      {&CoreIoSettings::output_gain_db, {nan, -inf, -61.0f, 25.0f}},
      {&CoreIoSettings::gate_threshold_db, {nan, -121.0f, 0.5f}},
      {&CoreIoSettings::transpose_semitones, {nan, inf, -12.01f, 12.01f}},
      {&CoreIoSettings::doubler_delay_ms, {nan, 0.0f, 0.99f, 40.5f}},
  };
  for (const auto &[field, values] : invalid) {
    for (float value : values) {
      CoreIoSettings candidate;
      candidate.*field = value;
      CHECK(!vespera::validate_core_io_settings(candidate));
    }
  }
  settings = {-60.0f, 24.0f, true, -120.0f, -12.0f, true, 40.0f};
  CHECK(vespera::validate_core_io_settings(settings));
  settings = {24.0f, -60.0f, true, 0.0f, 12.0f, true, 1.0f};
  CHECK(vespera::validate_core_io_settings(settings));
}

void test_prepare_validation() {
  CoreIoProcessor processor;
  CHECK(!processor.prepared());
  CHECK(processor.transpose_latency() == TransposeLatency::none);
  const auto throws = [&](double sample_rate, uint32_t frames, TransposeLatency latency) {
    try {
      processor.prepare(sample_rate, frames, latency);
    } catch (const std::invalid_argument &) {
      return true;
    }
    return false;
  };
  CHECK(throws(7999.0, 256, TransposeLatency::none));
  CHECK(throws(std::numeric_limits<double>::quiet_NaN(), 256, TransposeLatency::none));
  CHECK(throws(48000.0, 0, TransposeLatency::none));
  CHECK(throws(48000.0, CoreIoProcessor::max_frame_capacity + 1, TransposeLatency::fixed));
  CHECK(throws(48000.0, 256, static_cast<TransposeLatency>(2)));
  CHECK(!processor.prepared());

  processor.prepare(rate, capacity);
  CHECK(processor.input_latency_frames() == 0);
  processor.prepare(rate, capacity, TransposeLatency::fixed);
  CHECK(processor.input_latency_frames() == 722);
  CHECK(processor.set_settings({0.0f, 0.0f, false, -80.0f, 3.0f, false, 6.0f}));
  CHECK(throws(rate, capacity, TransposeLatency::none));
  CHECK(processor.transpose_latency() == TransposeLatency::fixed);
  CHECK(processor.sample_rate() == rate);
  CHECK(processor.input_latency_frames() == 722);

  CoreIoProcessor cd;
  cd.prepare(44100.0, capacity, TransposeLatency::fixed);
  CHECK(cd.input_latency_frames() == 664);
  CHECK(!cd.set_settings({0.0f, 0.0f, false, -80.0f, 13.0f, false, 6.0f}));
  cd.prepare(44100.0, capacity);
  CHECK(!cd.set_settings({0.0f, 0.0f, false, -80.0f, 1.0f, false, 6.0f}));
  CHECK(cd.settings() == CoreIoSettings{});
}

void test_reprepare_with_settings() {
  CoreIoProcessor processor;
  processor.prepare(rate, capacity);
  const Signal x = noise(4800, 21);
  run_input(processor, {x});
  CoreIoSettings shifted;
  shifted.transpose_semitones = 7.0f;
  CHECK(!processor.set_settings(shifted));

  processor.prepare(rate, capacity, TransposeLatency::fixed, shifted);
  CHECK(processor.transpose_latency() == TransposeLatency::fixed);
  CHECK(processor.input_latency_frames() == 722);
  CHECK(processor.settings() == shifted);
  const Signal tone = sine(220.0, 0.25, 24000);
  const Signal y = run_input(processor, {tone})[0];
  const double fifth = 220.0 * std::exp2(7.0 / 12.0);
  CHECK(goertzel_power(y, 6000, y.size(), fifth) > 1e4 * goertzel_power(y, 6000, y.size(), 220.0));

  processor.prepare(rate, capacity, TransposeLatency::none, {});
  CHECK(processor.transpose_latency() == TransposeLatency::none);
  CHECK(processor.input_latency_frames() == 0);
  CHECK(processor.settings() == CoreIoSettings{});
  CHECK(run_input(processor, {x})[0] == x);

  processor.prepare(44100.0, capacity, TransposeLatency::fixed, shifted);
  CHECK(processor.input_latency_frames() == 664);
  processor.prepare(44100.0, capacity, TransposeLatency::fixed, {});
  CHECK(delayed_equal(run_input(processor, {x})[0], x, 664));
}

void test_failed_prepare_preserves_state() {
  const CoreIoSettings settings{3.0f, -2.0f, true, -50.0f, 5.0f, true, 9.0f};
  CoreIoProcessor a;
  CoreIoProcessor b;
  a.prepare(rate, capacity, TransposeLatency::fixed, settings);
  b.prepare(rate, capacity, TransposeLatency::fixed, settings);
  const Signal x = noise(9000, 22);
  CHECK(run_input(a, {x, x}) == run_input(b, {x, x}));
  CHECK(run_output(a, {x}, 2) == run_output(b, {x}, 2));

  const auto rejected = [&](double sample_rate, uint32_t frames, TransposeLatency latency,
                            const CoreIoSettings &requested) {
    try {
      a.prepare(sample_rate, frames, latency, requested);
    } catch (const std::invalid_argument &) {
      return true;
    }
    return false;
  };
  CoreIoSettings transposing;
  transposing.transpose_semitones = 3.0f;
  CoreIoSettings invalid = settings;
  invalid.input_gain_db = std::numeric_limits<float>::quiet_NaN();
  CHECK(rejected(rate, capacity, TransposeLatency::none, transposing));
  CHECK(rejected(96000.0, capacity, TransposeLatency::fixed, invalid));
  CHECK(rejected(1000.0, capacity, TransposeLatency::none, {}));
  CHECK(rejected(96000.0, 0, TransposeLatency::none, {}));

  CHECK(a.settings() == settings);
  CHECK(a.sample_rate() == rate);
  CHECK(a.max_frames() == capacity);
  CHECK(a.transpose_latency() == TransposeLatency::fixed);
  CHECK(a.input_latency_frames() == 722);
  CHECK(run_input(a, {x, x}) == run_input(b, {x, x}));
  CHECK(run_output(a, {x}, 2) == run_output(b, {x}, 2));
}

void test_process_argument_errors() {
  CoreIoProcessor processor;
  Signal a(16, 1.0f);
  Signal b(16, 1.0f);
  const float *in[2] = {a.data(), b.data()};
  float *out[2] = {a.data(), b.data()};
  CHECK(!processor.process_input(in, out, 2, 16));
  CHECK(a[0] == 0.0f && b[15] == 0.0f);

  processor.prepare(rate, 16);
  std::fill(a.begin(), a.end(), 1.0f);
  CHECK(!processor.process_input(in, out, 3, 16));
  CHECK(!processor.process_input(in, out, 0, 16));
  CHECK(a[0] == 1.0f);
  Signal big(17, 0.0f);
  const float *big_in[1] = {big.data()};
  float *big_out[1] = {big.data()};
  CHECK(!processor.process_input(big_in, big_out, 1, 17));
  CHECK(!processor.process_output(in, 2, out, 1, 16));
  CHECK(!processor.process_output(in, 1, out, 3, 16));
  const float *missing[2] = {a.data(), nullptr};
  CHECK(!processor.process_input(missing, out, 2, 16));
  CHECK(a[0] == 0.0f);
  CHECK(!processor.process_input(nullptr, out, 1, 16));
  CHECK(processor.process_input(in, out, 2, 0));
  CHECK(processor.process_output(in, 1, out, 2, 16));
}

void test_default_transparency() {
  CoreIoProcessor processor;
  processor.prepare(rate, capacity);
  const Signal x = noise(20000, 1);
  const Signal y = noise(20000, 2);
  CHECK(run_input(processor, {x})[0] == x);
  const Channels stereo = run_input(processor, {x, y});
  CHECK(stereo[0] == x && stereo[1] == y);
  CHECK(run_output(processor, {x}, 1)[0] == x);
  const Channels duplicated = run_output(processor, {x}, 2);
  CHECK(duplicated[0] == x && duplicated[1] == x);
  const Channels passed = run_output(processor, {x, y}, 2);
  CHECK(passed[0] == x && passed[1] == y);

  Signal in_place = x;
  const float *in[1] = {in_place.data()};
  float *out[1] = {in_place.data()};
  CHECK(processor.process_input(in, out, 1, 256));
  CHECK(std::equal(in_place.begin(), in_place.begin() + 256, x.begin()));
}

void test_fixed_latency_zero_transpose_is_exact_delay() {
  for (double sample_rate : {44100.0, 48000.0, 96000.0}) {
    CoreIoProcessor processor;
    processor.prepare(sample_rate, capacity, TransposeLatency::fixed);
    const uint32_t latency = processor.input_latency_frames();
    CHECK(latency == 2 + static_cast<uint32_t>(std::lround(sample_rate * 0.01)) +
                         static_cast<uint32_t>(std::lround(sample_rate * 0.005)));
    Signal impulse(4096, 0.0f);
    impulse[10] = 1.0f;
    const Signal output = run_input(processor, {impulse}, 128)[0];
    CHECK(delayed_equal(output, impulse, latency));
  }
  CoreIoProcessor processor;
  processor.prepare(rate, capacity, TransposeLatency::fixed);
  const Signal x = noise(30000, 3);
  CHECK(delayed_equal(run_input(processor, {x}, 333)[0], x, 722));
}

void test_transpose_spectrum() {
  const std::vector<std::pair<float, double>> cases = {
      {12.0f, 2.0}, {-12.0f, 0.5}, {7.0f, std::exp2(7.0 / 12.0)}, {-5.0f, std::exp2(-5.0 / 12.0)}};
  for (const auto &[semitones, ratio] : cases) {
    for (double frequency : {82.41, 196.0, 220.0, 440.0, 1000.0}) {
      CoreIoProcessor processor;
      CoreIoSettings settings;
      settings.transpose_semitones = semitones;
      prepare(processor, settings, TransposeLatency::fixed);
      const Signal x = sine(frequency, 0.25, 48000);
      const Signal y = run_input(processor, {x})[0];
      const std::size_t begin = 12000;
      const double target = goertzel_power(y, begin, y.size(), frequency * ratio);
      const double original = goertzel_power(y, begin, y.size(), frequency);
      CHECK(target > 1e4 * original);
      const double offset = std::max(0.03 * frequency * ratio, 12.0);
      const double detuned = goertzel_power(y, begin, y.size(), frequency * ratio + offset);
      CHECK(target > 100.0 * detuned);
      const double level_db = 20.0 * std::log10(rms(y, begin, y.size()) / rms(x, begin, x.size()));
      CHECK(level_db > -1.0 && level_db < 0.5);
      for (float sample : y) {
        CHECK(std::abs(sample) <= 0.26f);
        if (std::abs(sample) > 0.26f) {
          break;
        }
      }
    }
  }
}

void test_transpose_preserves_duration() {
  for (float semitones : {12.0f, -12.0f, 5.0f}) {
    CoreIoProcessor processor;
    CoreIoSettings settings;
    settings.transpose_semitones = semitones;
    prepare(processor, settings, TransposeLatency::fixed);
    const std::size_t start = 4800;
    const std::size_t length = 9600;
    Signal x(48000, 0.0f);
    const Signal burst = sine(300.0, 0.5, length);
    std::copy(burst.begin(), burst.end(), x.begin() + static_cast<std::ptrdiff_t>(start));
    const Signal y = run_input(processor, {x}, 100)[0];
    std::size_t first = y.size();
    std::size_t last = 0;
    for (std::size_t n = 0; n < y.size(); ++n) {
      if (std::abs(y[n]) > 0.05f) {
        first = std::min(first, n);
        last = n;
      }
    }
    const std::size_t latency = processor.input_latency_frames();
    const std::size_t window = 1440;
    CHECK(first >= start + 2 && first <= start + latency + window / 2);
    CHECK(last + 1 >= start + length && last <= start + length + window + 4);
    CHECK(rms(y, start + window + 8, start + length) > 0.25);
  }
}

void test_transposed_impulse_support() {
  for (float semitones : {12.0f, -12.0f, 7.0f, -1.0f}) {
    CoreIoProcessor processor;
    CoreIoSettings settings;
    settings.transpose_semitones = semitones;
    prepare(processor, settings, TransposeLatency::fixed);
    const std::size_t at = 5000;
    const std::size_t reach = 2 + 960 + 480 + 3;
    Signal x(12000, 0.0f);
    x[at] = 1.0f;
    const Signal y = run_input(processor, {x}, 128)[0];
    double inside = 0.0;
    double outside = 0.0;
    for (std::size_t n = 0; n < y.size(); ++n) {
      CHECK(std::isfinite(y[n]) && std::abs(y[n]) <= 1.5f);
      const double energy = static_cast<double>(y[n]) * y[n];
      (n >= at + 1 && n <= at + reach ? inside : outside) += energy;
    }
    CHECK(outside == 0.0);
    CHECK(inside > 0.05);
  }
}

void test_splice_search_is_bounded_across_rates() {
  for (double sample_rate :
       {8000.0, 22050.0, 44100.0, 48000.0, 50000.0, 96000.0, 192000.0, 384000.0, 768000.0}) {
    for (float semitones : {12.0f, -12.0f, 0.5f}) {
      CoreIoProcessor processor;
      CoreIoSettings settings;
      settings.transpose_semitones = semitones;
      processor.prepare(sample_rate, 128, TransposeLatency::fixed, settings);
      const auto frames = static_cast<std::size_t>(sample_rate * 0.1);
      Signal square(frames);
      for (std::size_t n = 0; n < frames; ++n) {
        square[n] = n % 2 == 0 ? 0.7f : -0.7f;
      }
      const Channels y = run_input(processor, {square, noise(frames, 31)}, 128);
      CHECK(processor.peak_splice_search_terms() > 0);
      CHECK(processor.peak_splice_search_terms() <= CoreIoProcessor::max_splice_search_terms);
      for (const Signal &channel : y) {
        CHECK(std::all_of(channel.begin(), channel.end(), [](float sample) {
          return std::isfinite(sample) && std::abs(sample) <= 1.5f;
        }));
      }
    }
  }
}

void test_transpose_at_768k() {
  constexpr double high_rate = 768000.0;
  CoreIoSettings settings;
  settings.transpose_semitones = 7.0f;
  const auto frames = static_cast<std::size_t>(high_rate * 0.3);
  const Signal tone = sine(440.0, 0.25, frames, high_rate);
  const Signal hiss = noise(frames, 32, 0.3f);
  const auto run = [&](const std::vector<uint32_t> &blocks) {
    CoreIoProcessor processor;
    processor.prepare(high_rate, capacity, TransposeLatency::fixed, settings);
    CHECK(processor.input_latency_frames() == 2 + 7680 + 3840);
    return run_input(processor, {tone, hiss}, blocks);
  };
  const Channels y = run(uniform_blocks(frames, capacity));
  CHECK(y == run(random_blocks(frames, 33)));
  const std::size_t begin = frames / 3;
  const double fifth = 440.0 * std::exp2(7.0 / 12.0);
  const double target = goertzel_power(y[0], begin, frames, fifth, high_rate);
  CHECK(target > 1e4 * goertzel_power(y[0], begin, frames, 440.0, high_rate));
  CHECK(target > 100.0 * goertzel_power(y[0], begin, frames, fifth + 15.0, high_rate));
  const double level_db = 20.0 * std::log10(rms(y[0], begin, frames) / rms(tone, begin, frames));
  CHECK(level_db > -1.0 && level_db < 0.5);
  CHECK(std::all_of(y[1].begin(), y[1].end(), [](float sample) {
    return std::isfinite(sample) && std::abs(sample) <= 0.6f;
  }));
  CHECK(rms(y[1], begin, frames) > 0.5 * rms(hiss, begin, frames));
}

void test_transpose_engage_and_release() {
  CoreIoProcessor processor;
  prepare(processor, {}, TransposeLatency::fixed);
  const Signal x = sine(330.0, 0.5, 96000);
  CoreIoSettings shifted;
  shifted.transpose_semitones = 5.0f;
  CoreIoSettings shifted_down;
  shifted_down.transpose_semitones = -3.0f;
  const Signal y = run_input(processor, {x}, uniform_blocks(x.size(), 240), [&](std::size_t at) {
    if (at == 24000) {
      CHECK(processor.set_settings(shifted));
    } else if (at == 48000) {
      CHECK(processor.set_settings(shifted_down));
    } else if (at == 60000) {
      CHECK(processor.set_settings({}));
    }
  })[0];
  CHECK(delayed_equal(Signal(y.begin(), y.begin() + 24000), x, 722));
  for (float sample : y) {
    CHECK(std::abs(sample) <= 0.51f);
    if (std::abs(sample) > 0.51f) {
      break;
    }
  }
  const double expected_step = 2.0 * pi * 330.0 * std::exp2(5.0 / 12.0) / rate * 0.5;
  CHECK(max_step(y, 0, y.size()) < 2.0 * expected_step);
  const double up = goertzel_power(y, 30000, 48000, 330.0 * std::exp2(5.0 / 12.0));
  CHECK(up > 300.0 * goertzel_power(y, 30000, 48000, 330.0));
  const double down = goertzel_power(y, 50000, 60000, 330.0 * std::exp2(-3.0 / 12.0));
  CHECK(down > 100.0 * goertzel_power(y, 50000, 60000, 330.0));
  for (std::size_t n = 62000; n < y.size(); ++n) {
    CHECK(y[n] == x[n - 722]);
    if (y[n] != x[n - 722]) {
      break;
    }
  }
}

void test_channels_are_independent() {
  CoreIoSettings settings{4.0f, -3.0f, true, -30.0f, 5.0f, true, 6.0f};
  Signal x = sine(250.0, 0.4, 30000);
  const Signal y = noise(30000, 9, 0.01f);
  std::fill(x.begin() + 12000, x.begin() + 20000, 0.0f);
  for (TransposeLatency latency : {TransposeLatency::none, TransposeLatency::fixed}) {
    if (latency == TransposeLatency::none) {
      settings.transpose_semitones = 0.0f;
    } else {
      settings.transpose_semitones = 5.0f;
    }
    CoreIoProcessor stereo;
    CoreIoProcessor left;
    CoreIoProcessor right;
    prepare(stereo, settings, latency);
    prepare(left, settings, latency);
    prepare(right, settings, latency);
    const Channels both = run_input(stereo, {x, y});
    CHECK(both[0] == run_input(left, {x})[0]);
    CHECK(both[1] == run_input(right, {y})[0]);

    const Channels out = run_output(stereo, {x, y}, 2);
    const Channels mono = run_output(left, {x}, 1);
    CHECK(out[0] == mono[0]);
  }
}

void test_gains() {
  CoreIoProcessor processor;
  prepare(processor, {});
  const Signal dc(9600, 0.5f);
  CHECK(run_input(processor, {dc})[0] == dc);
  CHECK(run_output(processor, {dc}, 2)[1] == dc);
  CoreIoSettings settings;
  settings.input_gain_db = 12.0f;
  CHECK(processor.set_settings(settings));
  Signal y = run_input(processor, {dc})[0];
  const double boosted = 0.5 * std::pow(10.0, 12.0 / 20.0);
  CHECK(std::abs(y.back() - boosted) < 1e-5);
  CHECK(std::abs(y[959] - boosted) < 1e-5);
  CHECK(y[0] > 0.5f && y[0] < 0.51f);
  CHECK(max_step(y, 0, y.size()) <= (boosted - 0.5) / 960.0 * 1.01);

  settings.output_gain_db = -20.0f;
  CHECK(processor.set_settings(settings));
  y = run_output(processor, {dc}, 2)[1];
  CHECK(std::abs(y.back() - 0.05) < 1e-6);
  CHECK(max_step(y, 0, y.size()) <= 0.45 / 960.0 * 1.01);

  processor.reset();
  CHECK(std::abs(run_input(processor, {dc})[0][0] - boosted) < 1e-5);
  CHECK(std::abs(run_output(processor, {dc}, 1)[0][0] - 0.05) < 1e-6);
}

double level_db(double amplitude) { return 20.0 * std::log10(amplitude); }

void test_gate() {
  CoreIoProcessor processor;
  CoreIoSettings settings;
  settings.gate_threshold_db = -40.0f;
  prepare(processor, settings);

  const Signal quiet_disabled = sine(200.0, 1e-5, 4800);
  CHECK(run_input(processor, {quiet_disabled})[0] == quiet_disabled);

  settings.gate_enabled = true;
  CHECK(processor.set_settings(settings));
  const auto segment = [](double db, std::size_t frames) {
    return sine(200.0, std::pow(10.0, db / 20.0), frames);
  };
  Signal x;
  const auto append = [&](const Signal &part) { x.insert(x.end(), part.begin(), part.end()); };
  append(segment(-30.0, 9600));
  append(segment(-50.0, 24000));
  append(segment(-43.0, 24000));
  append(segment(-30.0, 9600));
  append(segment(-43.0, 24000));
  const Signal y = run_input(processor, {x}, 64)[0];
  const auto ratio = [&](std::size_t begin, std::size_t end) {
    return rms(y, begin, end) / rms(x, begin, end);
  };

  CHECK(std::abs(ratio(2400, 9600) - 1.0) < 1e-4);
  CHECK(std::abs(ratio(9600, 9600 + 480) - 1.0) < 1e-3);
  CHECK(level_db(ratio(9600 + 14400, 33600)) < -60.0);
  CHECK(ratio(33600 + 4800, 57600) == 0.0);
  CHECK(ratio(57600 + 240, 57600 + 480) > 0.95);
  CHECK(std::abs(ratio(57600 + 2400, 67200) - 1.0) < 1e-4);
  CHECK(std::abs(ratio(67200 + 4800, 91200) - 1.0) < 1e-4);
  CHECK(max_step(y, 0, y.size()) < 2.0 * pi * 200.0 / rate * std::pow(10.0, -30.0 / 20.0) * 1.5);

  CoreIoProcessor fresh;
  prepare(fresh, settings);
  const Signal hum = segment(-43.0, 24000);
  CHECK(run_input(fresh, {hum})[0] == hum);
  const Signal floor = segment(-50.0, 24000);
  const Signal closed = run_input(fresh, {floor})[0];
  CHECK(std::abs(rms(closed, 0, 2400) / rms(floor, 0, 2400) - 1.0) < 1e-3);
  CHECK(rms(closed, 14400, 24000) == 0.0);
}

void test_doubler() {
  CoreIoProcessor processor;
  CoreIoSettings settings;
  settings.doubler_enabled = true;
  prepare(processor, settings);
  const Signal x = noise(20000, 4);
  const Signal z = noise(20000, 5);

  Channels out = run_output(processor, {x}, 2, 300);
  CHECK(out[0] == x);
  CHECK(delayed_equal(out[1], x, 288));
  CHECK(out[0] != out[1]);

  processor.reset();
  out = run_output(processor, {x, z}, 2, 300);
  CHECK(out[0] == x);
  CHECK(delayed_equal(out[1], z, 288));

  processor.reset();
  CHECK(run_output(processor, {x}, 1)[0] == x);

  processor.reset();
  const Signal silent(1000, 0.0f);
  run_output(processor, {x}, 2);
  processor.reset();
  out = run_output(processor, {silent}, 2);
  CHECK(out[1] == silent);

  CoreIoProcessor live;
  prepare(live, {});
  const Signal tone = sine(500.0, 0.5, 48000);
  CoreIoSettings wider = settings;
  wider.doubler_delay_ms = 12.0f;
  out = run_output(live, {tone}, 2, uniform_blocks(tone.size(), 128), [&](std::size_t at) {
    if (at == 4096) {
      CHECK(live.set_settings(settings));
    } else if (at == 16384) {
      CHECK(live.set_settings(wider));
    } else if (at == 32768) {
      CHECK(live.set_settings({}));
    }
  });
  CHECK(out[0] == tone);
  const double tone_step = 2.0 * pi * 500.0 / rate * 0.5;
  CHECK(max_step(out[1], 0, out[1].size()) < tone_step + 2.0 * 0.5 / 960.0);
  for (std::size_t n = 0; n < 4096; ++n) {
    CHECK(out[1][n] == tone[n]);
  }
  for (std::size_t n = 4096 + 960; n < 16384; ++n) {
    CHECK(out[1][n] == tone[n - 288]);
  }
  for (std::size_t n = 16384 + 960; n < 32768; ++n) {
    CHECK(out[1][n] == tone[n - 576]);
  }
  for (std::size_t n = 32768 + 960; n < tone.size(); ++n) {
    CHECK(out[1][n] == tone[n]);
  }
}

void test_invalid_settings_leave_state() {
  CoreIoSettings settings{3.0f, -2.0f, true, -50.0f, 4.0f, true, 9.0f};
  CoreIoProcessor a;
  CoreIoProcessor b;
  prepare(a, settings, TransposeLatency::fixed);
  prepare(b, settings, TransposeLatency::fixed);
  const Signal x = noise(16000, 6);
  const Channels first_a = run_input(a, {x});
  const Channels first_b = run_input(b, {x});
  CHECK(first_a == first_b);
  CoreIoSettings invalid = settings;
  invalid.input_gain_db = std::numeric_limits<float>::quiet_NaN();
  invalid.transpose_semitones = -2.0f;
  CHECK(!a.set_settings(invalid));
  invalid = settings;
  invalid.doubler_delay_ms = 100.0f;
  CHECK(!a.set_settings(invalid));
  CHECK(a.settings() == settings);
  CHECK(run_input(a, {x}) == run_input(b, {x}));
  CHECK(run_output(a, {x}, 2) == run_output(b, {x}, 2));
}

void test_segmentation_invariance() {
  CoreIoSettings settings{2.0f, -1.0f, true, -45.0f, -7.0f, true, 6.0f};
  CoreIoSettings changed{-3.0f, 1.0f, true, -35.0f, 3.5f, true, 14.0f};
  Signal x = noise(60000, 7, 0.3f);
  std::fill(x.begin() + 20000, x.begin() + 30000, 0.0f);
  const Signal z = sine(180.0, 0.3, 60000);
  const std::size_t change_at = 40000;
  const auto split_at_change = [&](std::vector<uint32_t> blocks) {
    std::vector<uint32_t> result;
    std::size_t offset = 0;
    for (uint32_t block : blocks) {
      if (offset < change_at && offset + block > change_at) {
        result.push_back(static_cast<uint32_t>(change_at - offset));
        result.push_back(static_cast<uint32_t>(offset + block - change_at));
      } else {
        result.push_back(block);
      }
      offset += block;
    }
    return result;
  };
  const auto run = [&](const std::vector<uint32_t> &blocks) {
    CoreIoProcessor processor;
    prepare(processor, settings, TransposeLatency::fixed);
    const auto hook = [&](std::size_t at) {
      if (at == change_at) {
        CHECK(processor.set_settings(changed));
      }
    };
    Channels result = run_input(processor, {x, z}, blocks, hook);
    const Channels mono_out = run_output(processor, {result[0]}, 2, blocks, hook);
    result.insert(result.end(), mono_out.begin(), mono_out.end());
    return result;
  };
  const Channels reference = run(split_at_change(uniform_blocks(x.size(), capacity)));
  CHECK(reference == run(split_at_change(random_blocks(x.size(), 11))));
  CHECK(reference == run(split_at_change(random_blocks(x.size(), 12))));
  CHECK(reference == run(split_at_change(uniform_blocks(x.size(), 1))));
}

void test_non_finite_input() {
  CoreIoSettings settings{0.0f, 0.0f, true, -60.0f, 5.0f, true, 6.0f};
  CoreIoProcessor a;
  CoreIoProcessor b;
  prepare(a, settings, TransposeLatency::fixed);
  prepare(b, settings, TransposeLatency::fixed);
  Signal x = noise(8000, 8);
  Signal clean = x;
  x[100] = std::numeric_limits<float>::quiet_NaN();
  x[200] = std::numeric_limits<float>::infinity();
  x[300] = -std::numeric_limits<float>::infinity();
  clean[100] = clean[200] = clean[300] = 0.0f;
  const Channels dirty_in = run_input(a, {x});
  CHECK(dirty_in == run_input(b, {clean}));
  CHECK(run_output(a, {x}, 2) == run_output(b, {clean}, 2));
}

void test_layout_change_resets() {
  CoreIoSettings settings{0.0f, 0.0f, true, -60.0f, -4.0f, true, 6.0f};
  CoreIoProcessor used;
  CoreIoProcessor fresh;
  prepare(used, settings, TransposeLatency::fixed);
  prepare(fresh, settings, TransposeLatency::fixed);
  const Signal x = noise(6000, 10);
  run_input(used, {x, x});
  run_output(used, {x, x}, 2);
  CHECK(run_input(used, {x}) == run_input(fresh, {x}));
  CHECK(run_output(used, {x}, 2) == run_output(fresh, {x}, 2));
}

void test_no_allocations_after_prepare() {
  CoreIoProcessor processor;
  prepare(processor, {}, TransposeLatency::fixed);
  Signal a = noise(capacity, 13);
  Signal b = noise(capacity, 14);
  const std::vector<CoreIoSettings> sequence = {
      {6.0f, -6.0f, true, -50.0f, 12.0f, true, 6.0f},
      {0.0f, 0.0f, false, -80.0f, -12.0f, true, 30.0f},
      {-10.0f, 3.0f, true, -20.0f, 0.0f, false, 6.0f},
      {},
  };
  allocation_count.store(0);
  counting_allocations.store(true);
  for (int round = 0; round < 40; ++round) {
    CHECK(processor.set_settings(sequence[static_cast<std::size_t>(round) % sequence.size()]));
    CoreIoSettings invalid;
    invalid.output_gain_db = 99.0f;
    CHECK(!processor.set_settings(invalid));
    const float *in[2] = {a.data(), b.data()};
    float *out[2] = {a.data(), b.data()};
    const uint32_t channels = round % 3 == 0 ? 1 : 2;
    CHECK(processor.process_input(in, out, channels, capacity));
    CHECK(processor.process_output(in, channels, out, 2, capacity));
    CHECK(processor.process_output(in, 1, out, 1, capacity / 2));
    if (round % 7 == 0) {
      processor.reset();
    }
  }
  counting_allocations.store(false);
  CHECK(allocation_count.load() == 0);
  for (float sample : a) {
    CHECK(std::isfinite(sample));
  }
}

void run(const char *name, void (*test)()) {
  current_test = name;
  test();
}

}

int main() {
  run("settings_validation", test_settings_validation);
  run("prepare_validation", test_prepare_validation);
  run("reprepare_with_settings", test_reprepare_with_settings);
  run("failed_prepare_preserves_state", test_failed_prepare_preserves_state);
  run("process_argument_errors", test_process_argument_errors);
  run("default_transparency", test_default_transparency);
  run("fixed_latency_zero_transpose_is_exact_delay",
      test_fixed_latency_zero_transpose_is_exact_delay);
  run("transpose_spectrum", test_transpose_spectrum);
  run("transpose_preserves_duration", test_transpose_preserves_duration);
  run("transposed_impulse_support", test_transposed_impulse_support);
  run("splice_search_is_bounded_across_rates", test_splice_search_is_bounded_across_rates);
  run("transpose_at_768k", test_transpose_at_768k);
  run("transpose_engage_and_release", test_transpose_engage_and_release);
  run("channels_are_independent", test_channels_are_independent);
  run("gains", test_gains);
  run("gate", test_gate);
  run("doubler", test_doubler);
  run("invalid_settings_leave_state", test_invalid_settings_leave_state);
  run("segmentation_invariance", test_segmentation_invariance);
  run("non_finite_input", test_non_finite_input);
  run("layout_change_resets", test_layout_change_resets);
  run("no_allocations_after_prepare", test_no_allocations_after_prepare);
  if (failures != 0) {
    std::fprintf(stderr, "%d core IO check(s) failed\n", failures);
    return 1;
  }
  std::puts("core IO tests passed");
  return 0;
}
