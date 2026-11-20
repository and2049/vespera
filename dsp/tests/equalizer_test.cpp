#include <vespera/dsp/common.hpp>
#include <vespera/dsp/equalizer.hpp>
#include <vespera/dsp/filters.hpp>
#include <vespera/dsp/smoothing.hpp>

#include "support.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <span>
#include <string>
#include <vector>

namespace {

std::atomic<bool> counting_allocations{false};
std::atomic<unsigned> allocation_count{0};

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

using namespace test_support;
using vespera::dsp::BiquadCoefficients;
using vespera::dsp::GraphicEqualizer;
using vespera::dsp::GraphicEqualizerConfig;

using Controls = std::array<float, 11>;

constexpr std::array<double, 9> octave_centres{65.0,   125.0,  250.0,  500.0,  1000.0,
                                               2000.0, 4000.0, 8000.0, 16000.0};

// Reference recurrence: an 11-control graphic equalizer with smoothed controls, coefficients
// redesigned every 32 frames while any control moves, and high-pass and low-pass stages blended
// in and out by 20 ms ramps at the ends of their controls.
class ReferenceEqualizer {
public:
  ReferenceEqualizer(double sample_rate, uint32_t channels, const Controls &values)
      : sample_rate_(sample_rate), channels_(channels) {
    for (size_t i = 0; i < controls_.size(); ++i) {
      controls_[i].configure(sample_rate, 0.02);
      controls_[i].reset(values[i]);
    }
    hpf_mix_.configure(sample_rate, 0.02);
    lpf_mix_.configure(sample_rate, 0.02);
    hpf_mix_.reset(values[0] > 0.0f ? 1.0 : 0.0);
    lpf_mix_.reset(values[10] < 1.0f ? 1.0 : 0.0);
    update_coefficients();
  }

  void set_parameter(size_t index, float value) noexcept {
    if (index < 12) {
      controls_[index - 1].set_target(value);
      if (index == 1) {
        hpf_mix_.set_target(value > 0.0f ? 1.0 : 0.0);
      } else if (index == 11) {
        lpf_mix_.set_target(value < 1.0f ? 1.0 : 0.0);
      }
    }
  }

  void reset() noexcept {
    for (auto &control : controls_) {
      control.reset(control.target());
    }
    hpf_mix_.reset(hpf_mix_.target());
    lpf_mix_.reset(lpf_mix_.target());
    for (auto &channel : filters_) {
      for (auto &filter : channel) {
        filter.reset();
      }
    }
    coefficient_countdown_ = 0;
    coefficients_dirty_ = true;
  }

  std::array<float, 2> process_frame(const std::array<float, 2> &input) noexcept {
    for (auto &control : controls_) {
      coefficients_dirty_ = coefficients_dirty_ || control.active();
      control.process();
    }
    if (coefficient_countdown_ == 0) {
      if (coefficients_dirty_) {
        update_coefficients();
      }
      coefficient_countdown_ = 32;
    }
    --coefficient_countdown_;
    const double hpf = hpf_mix_.process();
    const double lpf = lpf_mix_.process();
    std::array<float, 2> wet{};
    for (uint32_t c = 0; c < channels_; ++c) {
      auto &filters = filters_[c];
      double sample = std::clamp<double>(input[c], -16.0, 16.0);
      const double highpassed = filters[0].process(sample);
      sample += (highpassed - sample) * hpf;
      for (size_t band = 1; band <= 9; ++band) {
        sample = filters[band].process(sample);
      }
      const double lowpassed = filters[10].process(sample);
      sample += (lowpassed - sample) * lpf;
      wet[c] = static_cast<float>(sample);
    }
    return wet;
  }

private:
  void update_coefficients() noexcept {
    using Coefficients = vespera::dsp::BiquadCoefficients;
    constexpr std::array frequencies{65.0,   125.0,  250.0,  500.0,  1000.0,
                                     2000.0, 4000.0, 8000.0, 16000.0};
    constexpr double q = 0.7071067811865476;
    std::array<Coefficients, 11> coefficients;
    coefficients[0] = Coefficients::highpass(
        sample_rate_, 20.0 + 480.0 * std::pow(controls_[0].current(), 1.0 / 0.6), q);
    for (size_t band = 0; band < frequencies.size(); ++band) {
      const double gain = (controls_[band + 1].current() - 0.5) * 24.0;
      if (frequencies[band] < sample_rate_ * 0.49 && gain != 0.0) {
        coefficients[band + 1] =
            Coefficients::peak(sample_rate_, frequencies[band], 1.4142135623730951, gain);
      }
    }
    coefficients[10] = Coefficients::lowpass(
        sample_rate_,
        std::min(1000.0 + 19000.0 * std::pow(controls_[10].current(), 1.0 / 0.6),
                 sample_rate_ * 0.45),
        q);
    for (auto &channel : filters_) {
      for (size_t i = 0; i < coefficients.size(); ++i) {
        channel[i].set_coefficients(coefficients[i]);
      }
    }
    coefficients_dirty_ = false;
  }

  double sample_rate_;
  uint32_t channels_;
  std::array<vespera::dsp::LinearSmoother, 11> controls_;
  vespera::dsp::LinearSmoother hpf_mix_;
  vespera::dsp::LinearSmoother lpf_mix_;
  std::array<std::array<vespera::dsp::Biquad, 11>, 2> filters_;
  uint32_t coefficient_countdown_ = 0;
  bool coefficients_dirty_ = false;
};

// The same controls, smoothing and coefficient cadence, driving GraphicEqualizer.
class ToolkitEqualizer {
public:
  ToolkitEqualizer(double sample_rate, uint32_t channels, const Controls &values)
      : sample_rate_(sample_rate), channels_(channels),
        equalizer_({sample_rate, octave_centres, 1.4142135623730951, 0.7071067811865476,
                    0.7071067811865476, 0.02}) {
    for (size_t i = 0; i < controls_.size(); ++i) {
      controls_[i].configure(sample_rate, 0.02);
      controls_[i].reset(values[i]);
    }
    equalizer_.set_highpass_enabled(values[0] > 0.0f);
    equalizer_.set_lowpass_enabled(values[10] < 1.0f);
    equalizer_.reset();
    update_coefficients();
  }

  void set_parameter(size_t index, float value) noexcept {
    if (index < 12) {
      controls_[index - 1].set_target(value);
      if (index == 1) {
        equalizer_.set_highpass_enabled(value > 0.0f);
      } else if (index == 11) {
        equalizer_.set_lowpass_enabled(value < 1.0f);
      }
    }
  }

  void reset() noexcept {
    for (auto &control : controls_) {
      control.reset(control.target());
    }
    equalizer_.reset();
    coefficient_countdown_ = 0;
    coefficients_dirty_ = true;
  }

  std::array<float, 2> process_frame(const std::array<float, 2> &input) {
    for (auto &control : controls_) {
      coefficients_dirty_ = coefficients_dirty_ || control.active();
      control.process();
    }
    if (coefficient_countdown_ == 0) {
      if (coefficients_dirty_) {
        update_coefficients();
      }
      coefficient_countdown_ = 32;
    }
    --coefficient_countdown_;
    std::array<double, 2> frame{};
    for (uint32_t c = 0; c < channels_; ++c) {
      frame[c] = std::clamp<double>(input[c], -16.0, 16.0);
    }
    equalizer_.process_frame(std::span<double>(frame.data(), channels_));
    return {static_cast<float>(frame[0]), static_cast<float>(frame[1])};
  }

private:
  void update_coefficients() {
    equalizer_.set_highpass_frequency(20.0 + 480.0 * std::pow(controls_[0].current(), 1.0 / 0.6));
    std::array<double, 9> gains{};
    for (size_t band = 0; band < gains.size(); ++band) {
      gains[band] = (controls_[band + 1].current() - 0.5) * 24.0;
    }
    equalizer_.set_band_gains(gains);
    equalizer_.set_lowpass_frequency(vespera::dsp::nyquist_safe_frequency(
        sample_rate_, 1000.0 + 19000.0 * std::pow(controls_[10].current(), 1.0 / 0.6)));
    coefficients_dirty_ = false;
  }

  double sample_rate_;
  uint32_t channels_;
  std::array<vespera::dsp::LinearSmoother, 11> controls_;
  GraphicEqualizer<9> equalizer_;
  uint32_t coefficient_countdown_ = 0;
  bool coefficients_dirty_ = false;
};

struct Event {
  std::size_t frame;
  std::size_t index;
  float value;
};

constexpr Controls flat{0.0f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 1.0f};
constexpr Controls shaped{0.3f, 0.9f, 0.1f, 0.65f, 0.5f, 0.2f, 1.0f, 0.0f, 0.75f, 0.85f, 0.4f};

// Automation, including the high-pass control arriving at and leaving 0, the low-pass control at
// 1, bands returning to exactly 0 dB, and a reset; frames are fractions of `count`.
std::vector<Event> automation(std::size_t count) {
  const auto at = [count](double fraction) {
    return static_cast<std::size_t>(fraction * static_cast<double>(count));
  };
  return {
      {at(0.05), 1, 0.6f},  {at(0.05), 5, 0.95f}, {at(0.1), 11, 0.3f},  {at(0.15), 2, 0.5f},
      {at(0.2), 1, 0.0f},   {at(0.22), 9, 0.0f},  {at(0.3), 11, 1.0f},  {at(0.33), 6, 0.5f},
      {at(0.4), 1, 1.0f},   {at(0.45), 10, 1.0f}, {at(0.5), 0, 0.0f},   {at(0.55), 11, 0.0f},
      {at(0.6), 3, 0.25f},  {at(0.62), 3, 0.5f},  {at(0.7), 1, 0.0f},   {at(0.75), 11, 1.0f},
      {at(0.8), 4, 0.123f}, {at(0.85), 1, 0.01f}, {at(0.9), 11, 0.99f}, {at(0.95), 7, 0.5f},
  };
}

template <typename Equalizer>
std::vector<float> run_equalizer(Equalizer &equalizer, const std::vector<double> &left,
                                 const std::vector<double> &right,
                                 const std::vector<Event> &events) {
  std::vector<float> output;
  std::size_t next = 0;
  for (std::size_t n = 0; n < left.size(); ++n) {
    while (next < events.size() && events[next].frame == n) {
      if (events[next].index == 0) {
        equalizer.reset();
      } else {
        equalizer.set_parameter(events[next].index, events[next].value);
      }
      ++next;
    }
    const auto frame =
        equalizer.process_frame({static_cast<float>(left[n]), static_cast<float>(right[n])});
    output.push_back(frame[0]);
    output.push_back(frame[1]);
  }
  return output;
}

void compare_with_reference(double rate) {
  const std::size_t count = static_cast<std::size_t>(rate / 4.0);
  const auto inputs = signals(rate, count);
  for (std::size_t s = 0; s < inputs.size(); ++s) {
    const auto &[name, left] = inputs[s];
    const auto &right = inputs[(s + 3) % inputs.size()].second;
    for (uint32_t channels : {1u, 2u}) {
      for (const Controls *initial : {&flat, &shaped}) {
        for (bool automated : {false, true}) {
          const std::vector<Event> events = automated ? automation(count) : std::vector<Event>{};
          ReferenceEqualizer reference(rate, channels, *initial);
          ToolkitEqualizer toolkit(rate, channels, *initial);
          const std::string label = name + " at " + std::to_string(rate) + ", " +
                                    std::to_string(channels) + " channel(s)" +
                                    (initial == &flat ? ", flat" : ", shaped") +
                                    (automated ? ", automated" : "");
          CHECK(count_mismatches(run_equalizer(toolkit, left, right, events),
                                 run_equalizer(reference, left, right, events), label) == 0);
        }
      }
    }
  }
}

void test_matches_reference() {
  for (double rate : rates) {
    compare_with_reference(rate);
  }
}

void test_matches_reference_where_bands_are_inactive() {
  compare_with_reference(32000.0);
  compare_with_reference(8000.0);
}

GraphicEqualizerConfig<3> small_config(double rate = 48000.0) {
  return {rate, {100.0, 1000.0, 10000.0}, 1.0, 0.7071067811865476, 0.7071067811865476, 0.01};
}

void test_rejects_invalid_configuration() {
  const auto rejects = [](GraphicEqualizerConfig<3> config) {
    return throws_invalid([&] { (void)GraphicEqualizer<3>(config); });
  };
  CHECK(!rejects(small_config()));
  for (double rate : {0.0, 7999.0, 768001.0, qnan, inf}) {
    CHECK(rejects(small_config(rate)));
  }
  for (double frequency : {0.0, -100.0, qnan, inf, 48000.0 * 0.99e-5}) {
    auto config = small_config();
    config.band_frequencies[1] = frequency;
    CHECK(rejects(config));
  }
  for (double q : {0.0, 0.049, 50.01, qnan}) {
    auto config = small_config();
    config.band_q = q;
    CHECK(rejects(config));
    config = small_config();
    config.highpass_q = q;
    CHECK(rejects(config));
    config = small_config();
    config.lowpass_q = q;
    CHECK(rejects(config));
  }
  for (double seconds : {-0.001, 60.01, qnan}) {
    auto config = small_config();
    config.stage_fade_seconds = seconds;
    CHECK(rejects(config));
  }
  auto above = small_config();
  above.band_frequencies[2] = 48000.0 * 0.49;
  CHECK(!rejects(above));
  above.band_frequencies[2] = 1.0e9;
  CHECK(!rejects(above));
}

void test_band_activity_limit() {
  auto config = small_config();
  config.band_frequencies = {std::nextafter(48000.0 * 0.49, 0.0), 48000.0 * 0.49, 30000.0};
  GraphicEqualizer<3> equalizer(config);
  CHECK(equalizer.band_active(0));
  CHECK(!equalizer.band_active(1));
  CHECK(!equalizer.band_active(2));
  equalizer.set_band_gains({0.0, 12.0, -12.0});
  std::array<double, 1> frame{};
  for (double x : noise(512, 4u)) {
    frame[0] = x;
    equalizer.process_frame(std::span<double>(frame));
    CHECK(same_bits(frame[0], x));
  }
}

void test_failed_setters_change_nothing() {
  GraphicEqualizer<3, 1> equalizer(small_config());
  equalizer.set_band_gains({3.0, -6.0, 9.0});
  equalizer.set_highpass_frequency(80.0);
  equalizer.set_lowpass_frequency(9000.0);
  equalizer.set_highpass_enabled(true);
  equalizer.set_lowpass_enabled(true);
  equalizer.reset();
  GraphicEqualizer<3, 1> untouched = equalizer;
  CHECK(throws_invalid([&] { equalizer.set_band_gain(3, 1.0); }));
  CHECK(throws_invalid([&] { equalizer.set_band_gain(0, qnan); }));
  CHECK(throws_invalid([&] { equalizer.set_band_gain(1, 48.5); }));
  CHECK(throws_invalid([&] { equalizer.set_band_gains({1.0, 2.0, -inf}); }));
  CHECK(throws_invalid([&] { equalizer.set_highpass_frequency(0.0); }));
  CHECK(throws_invalid([&] { equalizer.set_highpass_frequency(qnan); }));
  CHECK(throws_invalid([&] { equalizer.set_lowpass_frequency(24000.0); }));
  CHECK(throws_invalid([&] { equalizer.set_lowpass_frequency(-1.0); }));
  auto inactive = small_config();
  inactive.band_frequencies[2] = 40000.0;
  GraphicEqualizer<3, 1> with_inactive(inactive);
  CHECK(throws_invalid([&] { with_inactive.set_band_gain(2, qnan); }));
  std::array<double, 1> a{};
  std::array<double, 1> b{};
  std::size_t mismatches = 0;
  for (double x : noise(1024, 12u)) {
    a[0] = b[0] = x;
    equalizer.process_frame(std::span<double>(a));
    untouched.process_frame(std::span<double>(b));
    mismatches += same_bits(a[0], b[0]) ? 0 : 1;
  }
  CHECK(mismatches == 0);
}

void test_flat_and_off_is_bit_transparent() {
  GraphicEqualizer<3> equalizer(small_config());
  equalizer.set_highpass_frequency(500.0);
  equalizer.set_lowpass_frequency(2000.0);
  equalizer.set_band_gains({0.0, -0.0, 0.0});
  std::array<float, 2> frame{};
  std::size_t mismatches = 0;
  const auto signal = noise(4096, 13u, 4.0);
  for (double x : signal) {
    frame = {static_cast<float>(x), static_cast<float>(-x)};
    const auto input = frame;
    equalizer.process_frame(std::span<float>(frame));
    mismatches += same_bits(frame[0], input[0]) && same_bits(frame[1], input[1]) ? 0 : 1;
  }
  CHECK(mismatches == 0);
}

void test_stage_fades_in_and_out_exactly() {
  GraphicEqualizer<3, 1> equalizer(small_config(48000.0));
  equalizer.set_highpass_frequency(1000.0);
  CHECK(equalizer.highpass_weight() == 0.0);
  equalizer.set_highpass_enabled(true);
  equalizer.set_highpass_enabled(true);
  const auto signal = noise(2000, 14u);
  std::array<double, 1> frame{};
  for (std::size_t n = 0; n < 480; ++n) {
    frame[0] = signal[n];
    equalizer.process_frame(std::span<double>(frame));
    CHECK(equalizer.highpass_weight() == (n + 1 == 480 ? 1.0 : static_cast<double>(n + 1) / 480.0));
  }
  equalizer.set_highpass_enabled(false);
  for (std::size_t n = 480; n < 960; ++n) {
    frame[0] = signal[n];
    equalizer.process_frame(std::span<double>(frame));
  }
  CHECK(equalizer.highpass_weight() == 0.0);
  std::size_t mismatches = 0;
  for (std::size_t n = 960; n < signal.size(); ++n) {
    frame[0] = signal[n];
    equalizer.process_frame(std::span<double>(frame));
    mismatches += same_bits(frame[0], signal[n]) ? 0 : 1;
  }
  CHECK(mismatches == 0);
  equalizer.set_lowpass_frequency(200.0);
  equalizer.set_lowpass_enabled(true);
  equalizer.reset();
  CHECK(equalizer.lowpass_weight() == 1.0);
}

double sine_gain_db(GraphicEqualizer<3, 1> equalizer, double rate, double frequency) {
  const auto signal = sine(static_cast<std::size_t>(rate), rate, frequency, 0.5);
  std::array<double, 1> frame{};
  double input_energy = 0.0;
  double output_energy = 0.0;
  for (std::size_t n = 0; n < signal.size(); ++n) {
    frame[0] = signal[n];
    equalizer.process_frame(std::span<double>(frame));
    if (n >= signal.size() / 2) {
      input_energy += signal[n] * signal[n];
      output_energy += frame[0] * frame[0];
    }
  }
  return 10.0 * std::log10(output_energy / input_energy);
}

void test_band_and_stage_responses() {
  constexpr double rate = 48000.0;
  GraphicEqualizer<3, 1> equalizer(small_config(rate));
  equalizer.set_band_gain(1, 12.0);
  CHECK_NEAR(sine_gain_db(equalizer, rate, 1000.0), 12.0, 0.05);
  CHECK_NEAR(sine_gain_db(equalizer, rate, 100.0), 0.0, 0.3);
  equalizer.set_band_gain(1, 0.0);
  equalizer.set_highpass_frequency(1000.0);
  equalizer.set_highpass_enabled(true);
  equalizer.reset();
  CHECK(sine_gain_db(equalizer, rate, 100.0) < -35.0);
  CHECK_NEAR(sine_gain_db(equalizer, rate, 1000.0), -3.01, 0.05);
  equalizer.set_highpass_enabled(false);
  equalizer.set_lowpass_frequency(1000.0);
  equalizer.set_lowpass_enabled(true);
  equalizer.reset();
  CHECK(sine_gain_db(equalizer, rate, 10000.0) < -35.0);
  CHECK_NEAR(sine_gain_db(equalizer, rate, 100.0), 0.0, 0.05);
}

void test_processing_does_not_allocate() {
  GraphicEqualizer<9> equalizer({48000.0, octave_centres});
  equalizer.set_highpass_frequency(80.0);
  equalizer.set_lowpass_frequency(8000.0);
  std::array<float, 2> frame{0.25f, -0.25f};
  allocation_count.store(0);
  counting_allocations.store(true);
  for (int i = 0; i < 10000; ++i) {
    if (i % 32 == 0) {
      equalizer.set_band_gain(static_cast<std::size_t>(i / 32) % 9, (i % 64 == 0) ? 6.0 : 0.0);
      equalizer.set_highpass_enabled(i % 128 == 0);
      equalizer.set_lowpass_enabled(i % 96 == 0);
    }
    frame = {0.25f, -0.25f};
    equalizer.process_frame(std::span<float>(frame));
    if (i % 1000 == 0) {
      equalizer.reset();
    }
  }
  counting_allocations.store(false);
  CHECK(allocation_count.load() == 0);
  CHECK(std::isfinite(frame[0]) && std::isfinite(frame[1]));
}

}

int main() {
  run("matches_reference", test_matches_reference);
  run("matches_reference_where_bands_are_inactive",
      test_matches_reference_where_bands_are_inactive);
  run("rejects_invalid_configuration", test_rejects_invalid_configuration);
  run("band_activity_limit", test_band_activity_limit);
  run("failed_setters_change_nothing", test_failed_setters_change_nothing);
  run("flat_and_off_is_bit_transparent", test_flat_and_off_is_bit_transparent);
  run("stage_fades_in_and_out_exactly", test_stage_fades_in_and_out_exactly);
  run("band_and_stage_responses", test_band_and_stage_responses);
  run("processing_does_not_allocate", test_processing_does_not_allocate);
  return finish("equalizer");
}
