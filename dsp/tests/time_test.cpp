#include <vespera/dsp/delay.hpp>
#include <vespera/dsp/dynamics.hpp>
#include <vespera/dsp/oscillator.hpp>

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace {

using vespera::dsp::DelayConfig;
using vespera::dsp::EnvelopeConfig;
using vespera::dsp::EnvelopeFollower;
using vespera::dsp::FractionalDelay;
using vespera::dsp::Lfo;
using vespera::dsp::LfoConfig;
using vespera::dsp::LfoWaveform;

int failures = 0;
const char *current_test = "";

void check(bool condition, const char *expression, int line) {
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "%s:%d: %s failed\n", current_test, line, expression);
  }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

bool near(double actual, double expected, double tolerance) {
  return std::fabs(actual - expected) <= tolerance;
}

bool throws_invalid(const std::function<void()> &action) {
  try {
    action();
  } catch (const std::invalid_argument &) {
    return true;
  }
  return false;
}

constexpr double nan = std::numeric_limits<double>::quiet_NaN();
constexpr double inf = std::numeric_limits<double>::infinity();

void envelope_rejects_invalid_configuration() {
  current_test = "envelope_rejects_invalid_configuration";
  CHECK(throws_invalid([] { EnvelopeFollower{{7999.0, 0.01, 0.1}}; }));
  CHECK(throws_invalid([] { EnvelopeFollower{{nan, 0.01, 0.1}}; }));
  CHECK(throws_invalid([] { EnvelopeFollower{{768001.0, 0.01, 0.1}}; }));
  CHECK(throws_invalid([] { EnvelopeFollower{{48000.0, -1e-9, 0.1}}; }));
  CHECK(throws_invalid([] { EnvelopeFollower{{48000.0, 0.01, 60.000001}}; }));
  CHECK(throws_invalid([] { EnvelopeFollower{{48000.0, inf, 0.1}}; }));
  CHECK(throws_invalid([] { EnvelopeFollower{{48000.0, 0.01, nan}}; }));
  CHECK(!throws_invalid([] { EnvelopeFollower{{8000.0, 0.0, 60.0}}; }));
  CHECK(!throws_invalid([] { EnvelopeFollower{{768000.0, 60.0, 0.0}}; }));

  EnvelopeFollower follower({48000.0, 0.01, 0.1});
  follower.process(0.5f);
  const float before = follower.value();
  const double attack = follower.attack_coefficient();
  CHECK(throws_invalid([&] { follower.configure({48000.0, 0.02, -0.1}); }));
  CHECK(follower.value() == before);
  CHECK(follower.attack_coefficient() == attack);
  CHECK(follower.config().attack_seconds == 0.01);

  follower.configure({96000.0, 0.02, 0.2});
  CHECK(follower.value() == before);
  CHECK(follower.config().sample_rate == 96000.0);
}

void envelope_matches_analytical_time_constants() {
  current_test = "envelope_matches_analytical_time_constants";
  const double sample_rate = 48000.0;
  const double attack = 0.005;
  const double release = 0.05;
  EnvelopeFollower follower({sample_rate, attack, release});
  CHECK(near(follower.attack_coefficient(), std::exp(-1.0 / (attack * sample_rate)), 1e-15));

  const auto attack_samples = static_cast<int>(attack * sample_rate);
  for (int i = 0; i < attack_samples; ++i) {
    follower.process(1.0f);
  }
  CHECK(near(follower.value(), 1.0 - std::exp(-1.0), 1e-6));

  for (int i = 0; i < 48000; ++i) {
    follower.process(i % 2 == 0 ? 1.0f : -1.0f);
  }
  CHECK(near(follower.value(), 1.0, 1e-6));

  const auto release_samples = static_cast<int>(release * sample_rate);
  for (int i = 0; i < release_samples; ++i) {
    follower.process(0.0f);
  }
  CHECK(near(follower.value(), std::exp(-1.0), 1e-5));

  follower.reset();
  CHECK(follower.value() == 0.0f);
  CHECK(follower.process(0.0f) == 0.0f);
}

void envelope_zero_times_are_instantaneous() {
  current_test = "envelope_zero_times_are_instantaneous";
  EnvelopeFollower follower({48000.0, 0.0, 0.0});
  CHECK(follower.attack_coefficient() == 0.0);
  CHECK(follower.process(-0.75f) == 0.75f);
  CHECK(follower.process(0.25f) == 0.25f);
  CHECK(follower.process(0.0f) == 0.0f);

  EnvelopeFollower held({48000.0, 0.0, 1.0});
  CHECK(held.process(1.0f) == 1.0f);
  CHECK(held.process(0.0f) < 1.0f);
  CHECK(held.process(0.0f) > 0.99f);
}

void envelope_sanitizes_non_finite_input() {
  current_test = "envelope_sanitizes_non_finite_input";
  EnvelopeFollower follower({48000.0, 0.0, 0.0});
  CHECK(follower.process(std::numeric_limits<float>::quiet_NaN()) == 0.0f);
  CHECK(follower.process(std::numeric_limits<float>::infinity()) == 0.0f);

  EnvelopeFollower decaying({48000.0, 0.0, 0.0001});
  decaying.process(1.0f);
  for (int i = 0; i < 100000; ++i) {
    decaying.process(0.0f);
  }
  CHECK(decaying.value() == 0.0f);
}

void delay_rejects_invalid_configuration() {
  current_test = "delay_rejects_invalid_configuration";
  CHECK(throws_invalid([] { FractionalDelay{{1000.0, 8, 0.0}}; }));
  CHECK(throws_invalid([] { FractionalDelay{{48000.0, 8, 8.5}}; }));
  CHECK(throws_invalid([] { FractionalDelay{{48000.0, 8, -0.1}}; }));
  CHECK(throws_invalid([] { FractionalDelay{{48000.0, 8, nan}}; }));
  CHECK(throws_invalid(
      [] { FractionalDelay{{48000.0, vespera::dsp::maximum_delay_samples + 1, 0.0}}; }));

  FractionalDelay delay({48000.0, 4, 2.0});
  delay.process(1.0f);
  CHECK(throws_invalid([&] { delay.prepare({48000.0, 16, 17.0}); }));
  CHECK(throws_invalid([&] { delay.set_delay_samples(4.01); }));
  CHECK(!delay.try_set_delay_samples(inf));
  CHECK(!delay.try_set_delay_samples(-1.0));
  CHECK(delay.max_delay_samples() == 4);
  CHECK(delay.delay_samples() == 2.0);
  CHECK(delay.process(0.0f) == 0.0f);
  CHECK(delay.process(0.0f) == 1.0f);

  CHECK(delay.try_set_delay_samples(4.0));
  CHECK(delay.delay_samples() == 4.0);
}

std::vector<float> impulse_response(FractionalDelay &delay, std::size_t length) {
  std::vector<float> output;
  for (std::size_t i = 0; i < length; ++i) {
    output.push_back(delay.process(i == 0 ? 1.0f : 0.0f));
  }
  return output;
}

void delay_integer_impulses_including_zero_and_max() {
  current_test = "delay_integer_impulses_including_zero_and_max";
  FractionalDelay unprepared;
  CHECK(!unprepared.prepared());
  CHECK(unprepared.process(0.3f) == 0.3f);

  for (const double delay_samples : {0.0, 1.0, 3.0, 7.0}) {
    FractionalDelay delay({48000.0, 7, delay_samples});
    const auto response = impulse_response(delay, 40);
    for (std::size_t i = 0; i < response.size(); ++i) {
      CHECK(response[i] == (i == static_cast<std::size_t>(delay_samples) ? 1.0f : 0.0f));
    }
  }

  FractionalDelay zero_capacity({48000.0, 0, 0.0});
  CHECK(zero_capacity.process(0.4f) == 0.4f);
  CHECK(zero_capacity.process(-0.2f) == -0.2f);
}

void delay_fractional_interpolation_and_wrap() {
  current_test = "delay_fractional_interpolation_and_wrap";
  FractionalDelay delay({48000.0, 5, 2.25});
  for (int round = 0; round < 4; ++round) {
    const auto response = impulse_response(delay, 9);
    for (std::size_t i = 0; i < response.size(); ++i) {
      const float expected = i == 2 ? 0.75f : (i == 3 ? 0.25f : 0.0f);
      CHECK(near(response[i], expected, 1e-7));
    }
  }

  FractionalDelay edge({48000.0, 5, 4.5});
  const auto response = impulse_response(edge, 14);
  for (std::size_t i = 0; i < response.size(); ++i) {
    const float expected = (i == 4 || i == 5) ? 0.5f : 0.0f;
    CHECK(near(response[i], expected, 1e-7));
  }

  FractionalDelay ramp({48000.0, 3, 0.5});
  float previous = 0.0f;
  for (int i = 0; i < 20; ++i) {
    const auto input = static_cast<float>(i);
    CHECK(near(ramp.process(input), (input + previous) * 0.5, 1e-6));
    previous = input;
  }
}

void delay_reset_and_prepare_clear_history() {
  current_test = "delay_reset_and_prepare_clear_history";
  FractionalDelay delay({48000.0, 4, 3.0});
  delay.process(1.0f);
  delay.process(0.5f);
  delay.reset();
  CHECK(delay.delay_samples() == 3.0);
  for (int i = 0; i < 8; ++i) {
    CHECK(delay.process(0.0f) == 0.0f);
  }

  delay.process(1.0f);
  delay.prepare({44100.0, 6, 1.5});
  CHECK(delay.sample_rate() == 44100.0);
  CHECK(delay.max_delay_samples() == 6);
  for (int i = 0; i < 8; ++i) {
    CHECK(delay.process(0.0f) == 0.0f);
  }

  delay.process(std::numeric_limits<float>::quiet_NaN());
  CHECK(delay.process(0.0f) == 0.0f);
  CHECK(delay.process(0.0f) == 0.0f);
}

void lfo_rejects_invalid_configuration() {
  current_test = "lfo_rejects_invalid_configuration";
  CHECK(throws_invalid([] { Lfo{{48000.0, -0.001, LfoWaveform::sine, 0.0}}; }));
  CHECK(throws_invalid([] { Lfo{{48000.0, 24000.001, LfoWaveform::sine, 0.0}}; }));
  CHECK(throws_invalid([] { Lfo{{48000.0, nan, LfoWaveform::sine, 0.0}}; }));
  CHECK(throws_invalid([] { Lfo{{5.0, 1.0, LfoWaveform::sine, 0.0}}; }));
  CHECK(throws_invalid([] { Lfo{{48000.0, 1.0, LfoWaveform::sine, 1.0}}; }));
  CHECK(throws_invalid([] { Lfo{{48000.0, 1.0, LfoWaveform::sine, -0.1}}; }));
  CHECK(throws_invalid([] { Lfo{{48000.0, 1.0, static_cast<LfoWaveform>(42), 0.0}}; }));
  CHECK(!throws_invalid([] { Lfo{{48000.0, 0.0, LfoWaveform::sine, 0.0}}; }));
  CHECK(!throws_invalid([] { Lfo{{48000.0, 24000.0, LfoWaveform::square, 0.0}}; }));

  Lfo lfo({48000.0, 100.0, LfoWaveform::saw, 0.25});
  lfo.process();
  const double phase = lfo.phase();
  CHECK(throws_invalid([&] { lfo.configure({48000.0, 30000.0, LfoWaveform::sine, 0.0}); }));
  CHECK(throws_invalid([&] { lfo.set_phase(1.0); }));
  CHECK(lfo.phase() == phase);
  CHECK(lfo.config().waveform == LfoWaveform::saw);
  CHECK(lfo.phase_increment() == 100.0 / 48000.0);
}

void lfo_waveform_segments() {
  current_test = "lfo_waveform_segments";
  const auto value = [](LfoWaveform waveform, double phase) {
    return static_cast<double>(Lfo::value_at(waveform, phase));
  };
  CHECK(near(value(LfoWaveform::sine, 0.0), 0.0, 1e-7));
  CHECK(near(value(LfoWaveform::sine, 0.25), 1.0, 1e-7));
  CHECK(near(value(LfoWaveform::sine, 0.75), -1.0, 1e-7));

  CHECK(value(LfoWaveform::triangle, 0.0) == 0.0);
  CHECK(value(LfoWaveform::triangle, 0.125) == 0.5);
  CHECK(value(LfoWaveform::triangle, 0.25) == 1.0);
  CHECK(value(LfoWaveform::triangle, 0.5) == 0.0);
  CHECK(value(LfoWaveform::triangle, 0.75) == -1.0);
  CHECK(value(LfoWaveform::triangle, 0.875) == -0.5);
  CHECK(near(value(LfoWaveform::triangle, std::nextafter(1.0, 0.0)), 0.0, 1e-7));

  CHECK(value(LfoWaveform::saw, 0.0) == -1.0);
  CHECK(value(LfoWaveform::saw, 0.5) == 0.0);
  CHECK(near(value(LfoWaveform::saw, std::nextafter(1.0, 0.0)), 1.0, 1e-7));

  CHECK(value(LfoWaveform::square, 0.0) == 1.0);
  CHECK(value(LfoWaveform::square, std::nextafter(0.5, 0.0)) == 1.0);
  CHECK(value(LfoWaveform::square, 0.5) == -1.0);

  for (const auto waveform :
       {LfoWaveform::sine, LfoWaveform::triangle, LfoWaveform::saw, LfoWaveform::square}) {
    for (int i = 0; i < 1000; ++i) {
      const double v = value(waveform, i / 1000.0);
      CHECK(std::isfinite(v) && v >= -1.0 && v <= 1.0);
    }
  }
}

void lfo_phase_advance_reset_and_continuity() {
  current_test = "lfo_phase_advance_reset_and_continuity";
  Lfo lfo({48000.0, 12000.0, LfoWaveform::triangle, 0.0});
  const float expected[] = {0.0f, 1.0f, 0.0f, -1.0f, 0.0f, 1.0f};
  for (const float sample : expected) {
    CHECK(lfo.process() == sample);
  }

  Lfo nyquist({48000.0, 24000.0, LfoWaveform::square, 0.0});
  for (int i = 0; i < 16; ++i) {
    CHECK(nyquist.process() == (i % 2 == 0 ? 1.0f : -1.0f));
    CHECK(nyquist.phase() >= 0.0 && nyquist.phase() < 1.0);
  }

  Lfo still({48000.0, 0.0, LfoWaveform::sine, 0.25});
  for (int i = 0; i < 8; ++i) {
    CHECK(near(still.process(), 1.0, 1e-7));
  }

  Lfo sine({44100.0, 3.7, LfoWaveform::sine, 0.1});
  std::vector<float> first;
  for (int i = 0; i < 100000; ++i) {
    first.push_back(sine.process());
    CHECK(sine.phase() >= 0.0 && sine.phase() < 1.0);
  }
  sine.reset();
  CHECK(sine.phase() == 0.1);
  for (const float sample : first) {
    CHECK(sine.process() == sample);
  }

  Lfo moving({48000.0, 10.0, LfoWaveform::sine, 0.0});
  for (int i = 0; i < 1234; ++i) {
    moving.process();
  }
  const double phase = moving.phase();
  moving.configure({48000.0, 20.0, LfoWaveform::saw, 0.5});
  CHECK(moving.phase() == phase);
  CHECK(moving.process() == Lfo::value_at(LfoWaveform::saw, phase));
  CHECK(near(moving.phase(), phase + 20.0 / 48000.0, 1e-12));
  moving.reset();
  CHECK(moving.phase() == 0.5);

  moving.set_phase(0.75);
  CHECK(moving.process() == -1.0f + 2.0f * 0.75f);
}

void lfo_cycle_count_matches_frequency() {
  current_test = "lfo_cycle_count_matches_frequency";
  Lfo lfo({48000.0, 5.0, LfoWaveform::saw, 0.0});
  int wraps = 0;
  double previous = lfo.phase();
  for (int i = 0; i < 48000 * 3; ++i) {
    lfo.process();
    if (lfo.phase() < previous) {
      ++wraps;
    }
    previous = lfo.phase();
  }
  CHECK(wraps == 15 || wraps == 14);
  CHECK(near(lfo.phase(), 0.0, 1e-9) || near(lfo.phase(), 1.0, 1e-9));
}

}

int main() {
  envelope_rejects_invalid_configuration();
  envelope_matches_analytical_time_constants();
  envelope_zero_times_are_instantaneous();
  envelope_sanitizes_non_finite_input();
  delay_rejects_invalid_configuration();
  delay_integer_impulses_including_zero_and_max();
  delay_fractional_interpolation_and_wrap();
  delay_reset_and_prepare_clear_history();
  lfo_rejects_invalid_configuration();
  lfo_waveform_segments();
  lfo_phase_advance_reset_and_continuity();
  lfo_cycle_count_matches_frequency();
  if (failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  std::puts("dsp time tests passed");
  return 0;
}
