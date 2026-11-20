#include <vespera/dsp/allpass.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace {

using vespera::dsp::AllpassConfig;
using vespera::dsp::FirstOrderAllpass;
using vespera::dsp::SchroederAllpass;

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
constexpr double pi = std::numbers::pi;

std::vector<float> sine(double frequency, double sample_rate, double amplitude,
                        std::size_t frames) {
  std::vector<float> out(frames);
  for (std::size_t n = 0; n < frames; ++n) {
    out[n] = static_cast<float>(
        amplitude * std::sin(2.0 * pi * frequency * static_cast<double>(n) / sample_rate));
  }
  return out;
}

struct Projection {
  double amplitude = 0.0;
  double phase = 0.0;
};

// Least-squares fit of signal[start, start + count) to A sin(2 pi cycles k / count + phi).
// Requires an integer `cycles` so the basis is exactly orthogonal over the window.
Projection project(const std::vector<float> &signal, std::size_t start, std::size_t count,
                   std::size_t cycles) {
  double re = 0.0;
  double im = 0.0;
  for (std::size_t k = 0; k < count; ++k) {
    const double angle = 2.0 * pi * static_cast<double>(cycles * k) / static_cast<double>(count);
    re += signal[start + k] * std::cos(angle);
    im += signal[start + k] * std::sin(angle);
  }
  return {2.0 * std::hypot(re, im) / static_cast<double>(count), std::atan2(-im, re) + pi / 2.0};
}

std::size_t settling_echoes(double gain) {
  return static_cast<std::size_t>(std::ceil(7.0 / -std::log10(std::fabs(gain)))) + 2;
}

void first_order_allpass_rejects_boundary_coefficients() {
  current_test = "first_order_allpass_rejects_boundary_coefficients";
  CHECK(throws_invalid([] { FirstOrderAllpass{1.0}; }));
  CHECK(throws_invalid([] { FirstOrderAllpass{-1.0}; }));
  CHECK(throws_invalid([] { FirstOrderAllpass{1.0000001}; }));
  CHECK(throws_invalid([] { FirstOrderAllpass{nan}; }));
  CHECK(throws_invalid([] { FirstOrderAllpass{inf}; }));

  FirstOrderAllpass allpass(0.5);
  allpass.process(0.25f);
  CHECK(throws_invalid([&] { allpass.set_coefficient(1.0); }));
  CHECK(throws_invalid([&] { allpass.set_coefficient(-1.0); }));
  CHECK(throws_invalid([&] { allpass.set_coefficient(-1.5); }));
  CHECK(throws_invalid([&] { allpass.set_coefficient(nan); }));
  CHECK(!allpass.try_set_coefficient(1.0));
  CHECK(!allpass.try_set_coefficient(-1.0000000001));
  CHECK(!allpass.try_set_coefficient(inf));
  CHECK(allpass.coefficient() == 0.5);

  CHECK(allpass.try_set_coefficient(std::nextafter(1.0, 0.0)));
  CHECK(allpass.try_set_coefficient(std::nextafter(-1.0, 0.0)));
  CHECK(allpass.try_set_coefficient(0.0));
  CHECK(allpass.coefficient() == 0.0);

  FirstOrderAllpass fresh;
  CHECK(fresh.coefficient() == 0.0);
  float previous = 0.5f;
  float delayed = fresh.process(previous);
  CHECK(delayed == 0.0f);
  CHECK(fresh.process(0.0f) == previous);
}

void first_order_allpass_impulse_matches_closed_form() {
  current_test = "first_order_allpass_impulse_matches_closed_form";
  const double coefficients[] = {-0.95, -0.5, -0.1, 0.0, 0.3, 0.9, 0.999};
  for (const double a : coefficients) {
    FirstOrderAllpass allpass(a);
    std::vector<float> response(64);
    for (std::size_t n = 0; n < response.size(); ++n) {
      response[n] = allpass.process(n == 0 ? 1.0f : 0.0f);
    }
    CHECK(near(response[0], a, 1e-6));
    for (std::size_t n = 1; n < response.size(); ++n) {
      const double expected = (1.0 - a * a) * std::pow(-a, static_cast<double>(n - 1));
      CHECK(near(response[n], expected, 1e-6 + 1e-6 * std::fabs(expected)));
    }
    double energy = 0.0;
    for (float sample : response) {
      energy += static_cast<double>(sample) * static_cast<double>(sample);
    }
    const double expected_energy =
        a * a + (1.0 - a * a) * (1.0 - std::pow(a * a, static_cast<double>(response.size() - 1)));
    CHECK(near(energy, expected_energy, 1e-5));
  }
}

void first_order_allpass_matches_independent_recurrence() {
  current_test = "first_order_allpass_matches_independent_recurrence";
  const double a = 0.37;
  FirstOrderAllpass allpass(a);
  double x_prev = 0.0;
  double y_prev = 0.0;
  std::uint64_t state = 13;
  for (std::size_t n = 0; n < 4096; ++n) {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    const float input =
        static_cast<float>(static_cast<double>((state >> 33) % 2001) / 1000.0 - 1.0);
    const double x = std::isfinite(input) ? static_cast<double>(input) : 0.0;
    const double reference = a * x + (x_prev - a * y_prev);
    x_prev = x;
    y_prev = reference;
    const float output = allpass.process(input);
    check(near(output, reference, 1e-6 + 1e-6 * std::fabs(reference)),
          "independent first-order recurrence", __LINE__);
  }
}

void first_order_allpass_unity_magnitude_and_phase() {
  current_test = "first_order_allpass_unity_magnitude_and_phase";
  constexpr double rate = 48000.0;
  constexpr std::size_t count = 4800;
  const double frequencies[] = {100.0, 1230.0, 3050.0, 9000.0};
  const double coefficients[] = {-0.95, -0.5, 0.3, 0.9};
  for (const double a : coefficients) {
    FirstOrderAllpass allpass(a);
    for (const double frequency : frequencies) {
      const auto cycles = static_cast<std::size_t>(frequency * count / rate + 0.5);
      const std::vector<float> signal = sine(frequency, rate, 0.7, 2 * count);
      std::vector<float> output(signal.size());
      for (std::size_t n = 0; n < signal.size(); ++n) {
        output[n] = allpass.process(signal[n]);
      }
      const Projection measured = project(output, count, count, cycles);
      CHECK(near(measured.amplitude, 0.7, 2e-6));
      const double omega = 2.0 * pi * frequency / rate;
      const std::complex<double> z1 = std::exp(std::complex<double>(0.0, -omega));
      const std::complex<double> analytic = (a + z1) / (1.0 + a * z1);
      CHECK(near(std::abs(analytic), 1.0, 1e-15));
      CHECK(near(
          std::remainder(measured.phase - (omega * static_cast<double>(count) + std::arg(analytic)),
                         2.0 * pi),
          0.0, 2e-5));
    }
  }
}

void first_order_allpass_dc_and_nyquist() {
  current_test = "first_order_allpass_dc_and_nyquist";
  for (const double a : {-0.9, 0.0, 0.5, 0.95}) {
    FirstOrderAllpass allpass(a);
    for (std::size_t n = 0; n < 10000; ++n) {
      allpass.process(0.25f);
    }
    CHECK(near(allpass.process(0.25f), 0.25, 1e-7));

    FirstOrderAllpass alternating(a);
    for (std::size_t n = 0; n < 10000; ++n) {
      alternating.process(n % 2 == 0 ? 0.25f : -0.25f);
    }
    CHECK(near(alternating.process(0.25f), -0.25, 1e-7));
    CHECK(near(alternating.process(-0.25f), 0.25, 1e-7));
  }

  FirstOrderAllpass poisoned(0.5);
  poisoned.process(std::numeric_limits<float>::quiet_NaN());
  poisoned.process(std::numeric_limits<float>::infinity());
  CHECK(poisoned.process(0.0f) == 0.0f);
}

void first_order_allpass_state_reset_and_spans() {
  current_test = "first_order_allpass_state_reset_and_spans";
  const std::vector<float> input = sine(700.0, 48000.0, 0.4, 300);
  FirstOrderAllpass allpass(0.6);
  double x_prev = 0.0;
  double y_prev = 0.0;
  for (std::size_t n = 0; n < input.size(); ++n) {
    const double a = n < 100 ? 0.6 : -0.4;
    if (n == 100) {
      allpass.set_coefficient(-0.4);
    }
    const double x = static_cast<double>(input[n]);
    const double reference = a * x + (x_prev - a * y_prev);
    x_prev = x;
    y_prev = reference;
    check(near(allpass.process(input[n]), reference, 1e-6 + 1e-6 * std::fabs(reference)),
          "coefficient change retains the direct-form history", __LINE__);
  }

  FirstOrderAllpass reset_filter(0.6);
  reset_filter.process(0.5f);
  reset_filter.process(-0.25f);
  reset_filter.reset();
  FirstOrderAllpass fresh(0.6);
  for (std::size_t n = 0; n < input.size(); ++n) {
    check(reset_filter.process(input[n]) == fresh.process(input[n]),
          "reset restores the initial state", __LINE__);
  }

  FirstOrderAllpass in_place(0.3);
  std::vector<float> buffered(input);
  in_place.process(buffered);
  FirstOrderAllpass separate(0.3);
  std::vector<float> output(input.size());
  separate.process(input, output);
  FirstOrderAllpass scalar(0.3);
  for (std::size_t i = 0; i < input.size(); ++i) {
    check(buffered[i] == output[i] && output[i] == scalar.process(input[i]),
          "span overloads agree with the scalar loop", __LINE__);
  }
}

void schroeder_allpass_rejects_invalid_configuration() {
  current_test = "schroeder_allpass_rejects_invalid_configuration";
  CHECK(throws_invalid([] { SchroederAllpass{{48000.0, 0, 0.5}}; }));
  CHECK(throws_invalid(
      [] { SchroederAllpass{{48000.0, vespera::dsp::maximum_allpass_delay_samples + 1, 0.5}}; }));
  CHECK(throws_invalid([] { SchroederAllpass{{48000.0, 8, 1.0}}; }));
  CHECK(throws_invalid([] { SchroederAllpass{{48000.0, 8, -1.0}}; }));
  CHECK(throws_invalid([] { SchroederAllpass{{48000.0, 8, 1.0000001}}; }));
  CHECK(throws_invalid([] { SchroederAllpass{{48000.0, 8, nan}}; }));
  CHECK(throws_invalid([] { SchroederAllpass{{1000.0, 8, 0.5}}; }));

  SchroederAllpass allpass({48000.0, 8, 0.5});
  allpass.process(0.25f);
  CHECK(throws_invalid([&] { allpass.prepare({48000.0, 0, 0.5}); }));
  CHECK(throws_invalid([&] { allpass.prepare({48000.0, 8, -1.5}); }));
  CHECK(throws_invalid([&] { allpass.set_gain(1.0); }));
  CHECK(!allpass.try_set_gain(-1.0));
  CHECK(!allpass.try_set_gain(nan));
  CHECK(!allpass.try_set_gain(inf));
  CHECK(allpass.gain() == 0.5);
  CHECK(allpass.delay_samples() == 8);
  CHECK(allpass.sample_rate() == 48000.0);
  CHECK(allpass.prepared());
  CHECK(!SchroederAllpass().prepared());

  SchroederAllpass passthrough;
  CHECK(passthrough.process(0.5f) == 0.5f);
  CHECK(passthrough.process(std::numeric_limits<float>::quiet_NaN()) == 0.0f);
  CHECK(passthrough.process(-2.0f) == -2.0f);

  CHECK(allpass.try_set_gain(std::nextafter(1.0, 0.0)));
  CHECK(allpass.try_set_gain(std::nextafter(-1.0, 0.0)));
  allpass.set_gain(0.5);
}

void schroeder_allpass_impulse_matches_closed_form() {
  current_test = "schroeder_allpass_impulse_matches_closed_form";
  const std::size_t delays[] = {1, 2, 7, 64};
  const double gains[] = {-0.7, 0.5, 0.9};
  for (const std::size_t delay : delays) {
    for (const double g : gains) {
      SchroederAllpass allpass({48000.0, delay, g});
      const std::size_t length = 16 * delay + 4;
      std::vector<float> response(length);
      for (std::size_t n = 0; n < length; ++n) {
        response[n] = allpass.process(n == 0 ? 1.0f : 0.0f);
      }
      CHECK(near(response[0], -g, 1e-6));
      for (std::size_t n = 1; n < length; ++n) {
        double expected = 0.0;
        if (n % delay == 0) {
          const auto echo = n / delay;
          expected = (1.0 - g * g) * std::pow(g, static_cast<double>(echo - 1));
        }
        CHECK(near(response[n], expected, 1e-6 + 1e-6 * std::fabs(expected)));
      }

      std::vector<float> x_history(length, 0.0f);
      std::vector<float> y_history(length, 0.0f);
      for (std::size_t n = 0; n < length; ++n) {
        const float x = n == 0 ? 1.0f : 0.0f;
        const double xd = n >= delay ? static_cast<double>(x_history[n - delay]) : 0.0;
        const double yd = n >= delay ? static_cast<double>(y_history[n - delay]) : 0.0;
        const double y = xd + g * (yd - x);
        x_history[n] = x;
        y_history[n] = static_cast<float>(y);
        check(near(response[n], y, 1e-6 + 1e-6 * std::fabs(y)), "independent Schroeder recurrence",
              __LINE__);
      }
    }
  }
}

void schroeder_allpass_decay_and_energy() {
  current_test = "schroeder_allpass_decay_and_energy";
  const std::size_t delay = 64;
  for (const double g : {0.25, 0.5, -0.6, 0.85}) {
    SchroederAllpass allpass({48000.0, delay, g});
    const std::size_t length = 12 * delay;
    std::vector<float> response(length);
    for (std::size_t n = 0; n < length; ++n) {
      response[n] = allpass.process(n == 0 ? 1.0f : 0.0f);
    }
    for (std::size_t echo = 2; echo <= 8; ++echo) {
      const double ratio = static_cast<double>(response[echo * delay]) /
                           static_cast<double>(response[(echo - 1) * delay]);
      CHECK(near(ratio, g, 1e-5));
    }
    double energy = 0.0;
    for (float sample : response) {
      energy += static_cast<double>(sample) * static_cast<double>(sample);
    }
    const double expected_energy = g * g + (1.0 - g * g) * (1.0 - std::pow(g * g, 11.0));
    CHECK(near(energy, expected_energy, 1e-5));

    allpass.reset();
    for (std::size_t n = 0; n < length; ++n) {
      response[n] = allpass.process(n == 0 ? 1.0f : 0.0f);
    }
    CHECK(near(response[0], -g, 1e-6));
    CHECK(near(response[delay], 1.0 - g * g, 1e-6));
  }

  const std::vector<float> input = sine(1000.0, 48000.0, 0.5, 256);
  SchroederAllpass in_place({48000.0, 16, 0.5});
  std::vector<float> buffered(input);
  in_place.process(buffered);
  SchroederAllpass separate({48000.0, 16, 0.5});
  std::vector<float> output(input.size());
  separate.process(input, output);
  SchroederAllpass scalar({48000.0, 16, 0.5});
  for (std::size_t i = 0; i < input.size(); ++i) {
    check(buffered[i] == output[i] && output[i] == scalar.process(input[i]),
          "span overloads agree with the scalar loop", __LINE__);
  }
}

void schroeder_allpass_unity_static_magnitude() {
  current_test = "schroeder_allpass_unity_static_magnitude";
  constexpr double rate = 48000.0;
  constexpr std::size_t count = 4800;
  const struct {
    std::size_t delay;
    double gain;
  } cases[] = {{1, 0.9}, {1, -0.95}, {2, 0.8}, {64, 0.5}, {64, -0.6}, {1000, 0.5}};
  const double frequencies[] = {100.0, 1230.0, 3050.0, 9000.0};
  for (const auto &test : cases) {
    SchroederAllpass allpass({rate, test.delay, test.gain});
    for (const double frequency : frequencies) {
      const auto cycles = static_cast<std::size_t>(frequency * count / rate + 0.5);
      const std::size_t frames = settling_echoes(test.gain) * test.delay + 2 * count;
      const std::vector<float> signal = sine(frequency, rate, 0.7, frames);
      std::vector<float> output(signal.size());
      for (std::size_t n = 0; n < signal.size(); ++n) {
        output[n] = allpass.process(signal[n]);
      }
      const Projection measured = project(output, signal.size() - count, count, cycles);
      CHECK(near(measured.amplitude, 0.7, 3e-6));
    }

    SchroederAllpass dc({rate, test.delay, test.gain});
    for (std::size_t n = 0; n < 100000; ++n) {
      dc.process(0.25f);
    }
    CHECK(near(dc.process(0.25f), 0.25, 5e-7));

    SchroederAllpass alternating({rate, test.delay, test.gain});
    for (std::size_t n = 0; n < 100000; ++n) {
      alternating.process(n % 2 == 0 ? 0.25f : -0.25f);
    }
    const double expected = test.delay % 2 == 0 ? 0.25 : -0.25;
    CHECK(near(alternating.process(0.25f), expected, 5e-7));
    CHECK(near(alternating.process(-0.25f), -expected, 5e-7));
  }
}

void schroeder_allpass_time_varying_gain_matches_recurrence() {
  current_test = "schroeder_allpass_time_varying_gain_matches_recurrence";
  constexpr std::size_t delay = 37;
  constexpr double rate = 48000.0;
  const std::vector<float> signal = sine(437.0, rate, 0.5, 24000);
  SchroederAllpass allpass({rate, delay, 0.4});
  std::vector<float> x_history(signal.size(), 0.0f);
  std::vector<float> y_history(signal.size(), 0.0f);
  for (std::size_t n = 0; n < signal.size(); ++n) {
    const double gain = 0.3 + 0.4 * std::sin(2.0 * pi * static_cast<double>(n) / 997.0);
    if (!allpass.try_set_gain(gain)) {
      check(false, "try_set_gain", __LINE__);
    }
    const float output = allpass.process(signal[n]);
    const float x = signal[n];
    const double xd = n >= delay ? static_cast<double>(x_history[n - delay]) : 0.0;
    const double yd = n >= delay ? static_cast<double>(y_history[n - delay]) : 0.0;
    const double reference = xd + gain * (yd - x);
    x_history[n] = x;
    y_history[n] = static_cast<float>(reference);
    check(near(output, reference, 1e-6 + 1e-6 * std::fabs(reference)),
          "time-varying gain recurrence", __LINE__);
  }
}

// With |a[n]| <= A and |g[n]| <= G the moving sections are not allpass but stay within
// (1 + A) / (1 - A) and (1 + G) / (1 - G) of the input peak and decay geometrically once the
// input is silent: |y[n]| <= A |y[n-1]| and |y[n]| <= G |y[n-D]|.
void allpass_time_varying_bounds_and_decay() {
  current_test = "allpass_time_varying_bounds_and_decay";
  constexpr double bound = 0.95;
  constexpr std::size_t delay = 29;
  constexpr std::size_t driven = 20000;
  constexpr std::size_t total = 30000;
  FirstOrderAllpass first_order(0.0);
  SchroederAllpass schroeder({48000.0, delay, 0.0});
  std::uint64_t state = 77;
  const auto next_unit = [&state] {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<double>(state >> 11) * 0x1.0p-53;
  };
  std::vector<double> first_outputs(total);
  std::vector<double> schroeder_outputs(total);
  std::vector<double> gains(total);
  double first_peak = 0.0;
  double schroeder_peak = 0.0;
  for (std::size_t n = 0; n < total; ++n) {
    const double a = bound * (2.0 * next_unit() - 1.0);
    const double g = bound * (2.0 * next_unit() - 1.0);
    gains[n] = g;
    CHECK(first_order.try_set_coefficient(a) && schroeder.try_set_gain(g));
    const float x = n < driven ? static_cast<float>(next_unit() < 0.5 ? -1.0 : 1.0) : 0.0f;
    first_outputs[n] = first_order.process(x);
    schroeder_outputs[n] = schroeder.process(x);
    first_peak = std::max(first_peak, std::fabs(first_outputs[n]));
    schroeder_peak = std::max(schroeder_peak, std::fabs(schroeder_outputs[n]));
    if (n > driven) {
      check(std::fabs(first_outputs[n]) <=
                std::fabs(a) * std::fabs(first_outputs[n - 1]) * (1.0 + 1e-6) + 1e-12,
            "first-order silent decay", __LINE__);
    }
    if (n >= driven + delay) {
      check(std::fabs(schroeder_outputs[n]) <=
                std::fabs(g) * std::fabs(schroeder_outputs[n - delay]) * (1.0 + 1e-6) + 1e-12,
            "Schroeder silent decay", __LINE__);
    }
  }
  const double peak_bound = (1.0 + bound) / (1.0 - bound);
  CHECK(first_peak <= peak_bound);
  CHECK(schroeder_peak <= peak_bound);
  CHECK(first_peak > 1.0);
  CHECK(std::fabs(first_outputs.back()) < 1e-20);
  CHECK(std::fabs(schroeder_outputs.back()) < 1e-20);
}

void allpass_block_partition_is_exact() {
  current_test = "allpass_block_partition_is_exact";
  const std::vector<float> input = sine(700.0, 48000.0, 0.4, 4096);
  const auto process_stream = [&](const std::vector<std::size_t> &blocks) {
    SchroederAllpass allpass({48000.0, 53, 0.5});
    FirstOrderAllpass first_order(0.4);
    std::vector<float> outputs;
    outputs.reserve(input.size());
    std::size_t n = 0;
    std::size_t index = 0;
    while (n < input.size()) {
      const std::size_t block = blocks[index % blocks.size()];
      index += 1;
      for (std::size_t i = 0; i < block && n < input.size(); ++i, ++n) {
        const double gain = 0.5 + 0.3 * std::sin(2.0 * pi * static_cast<double>(n) / 71.0);
        const double coefficient = 0.5 * std::sin(2.0 * pi * static_cast<double>(n) / 43.0);
        if (!allpass.try_set_gain(gain) || !first_order.try_set_coefficient(coefficient)) {
          check(false, "real-time setters", __LINE__);
        }
        const float delayed = allpass.process(input[n]);
        outputs.push_back(first_order.process(delayed));
      }
    }
    return outputs;
  };
  const std::vector<float> reference = process_stream({input.size()});
  const std::vector<std::vector<std::size_t>> partitions = {
      {1}, {2}, {3}, {16}, {64}, {511}, {1, 2, 3, 16, 64, 511, 2000, 1}};
  for (const std::vector<std::size_t> &blocks : partitions) {
    const std::vector<float> outputs = process_stream(blocks);
    check(outputs.size() == reference.size(), "partition length", __LINE__);
    for (std::size_t i = 0; i < reference.size(); ++i) {
      check(outputs[i] == reference[i], "bit-identical partition output", __LINE__);
    }
  }
}

}

int main() {
  first_order_allpass_rejects_boundary_coefficients();
  first_order_allpass_impulse_matches_closed_form();
  first_order_allpass_matches_independent_recurrence();
  first_order_allpass_unity_magnitude_and_phase();
  first_order_allpass_dc_and_nyquist();
  first_order_allpass_state_reset_and_spans();
  schroeder_allpass_rejects_invalid_configuration();
  schroeder_allpass_impulse_matches_closed_form();
  schroeder_allpass_decay_and_energy();
  schroeder_allpass_unity_static_magnitude();
  schroeder_allpass_time_varying_gain_matches_recurrence();
  allpass_time_varying_bounds_and_decay();
  allpass_block_partition_is_exact();
  if (failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  std::puts("dsp allpass tests passed");
  return 0;
}
