#include <vespera/dsp/delay.hpp>
#include <vespera/dsp/history.hpp>
#include <vespera/dsp/utility.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

using vespera::dsp::FractionalDelay;
using vespera::dsp::HistoryBuffer;
using vespera::dsp::HistoryConfig;
using vespera::dsp::Interpolation;

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

// Deterministic uniform noise, independent of the bench generator.
double unit(std::uint64_t &state) {
  state += 0x9E3779B97F4A7C15ull;
  std::uint64_t z = state;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  z ^= z >> 31;
  return static_cast<double>(z >> 11) * 0x1.0p-53;
}

std::vector<float> noise(std::size_t frames, std::uint64_t seed) {
  std::vector<float> out(frames);
  std::uint64_t state = seed;
  for (float &sample : out) {
    sample = static_cast<float>(2.0 * unit(state) - 1.0);
  }
  return out;
}

// Four interpolation weights for the window of sample delays [base, base + 3], derived by
// solving the Vandermonde system sum_j w_j t_j^k = delay^k for k = 0..3. This is independent
// of the closed-form Lagrange basis used by HistoryBuffer.
std::array<double, 4> window_weights(std::size_t base, double delay) {
  std::array<std::array<double, 4>, 4> a{};
  std::array<double, 4> b{};
  for (std::size_t k = 0; k < 4; ++k) {
    for (std::size_t j = 0; j < 4; ++j) {
      a[k][j] = std::pow(static_cast<double>(base + j), static_cast<double>(k));
    }
    b[k] = std::pow(delay, static_cast<double>(k));
  }
  for (std::size_t column = 0; column < 4; ++column) {
    std::size_t pivot = column;
    for (std::size_t row = column + 1; row < 4; ++row) {
      if (std::fabs(a[row][column]) > std::fabs(a[pivot][column])) {
        pivot = row;
      }
    }
    std::swap(a[column], a[pivot]);
    std::swap(b[column], b[pivot]);
    for (std::size_t row = column + 1; row < 4; ++row) {
      const double factor = a[row][column] / a[column][column];
      for (std::size_t j = column; j < 4; ++j) {
        a[row][j] -= factor * a[column][j];
      }
      b[row] -= factor * b[column];
    }
  }
  std::array<double, 4> w{};
  for (std::size_t row = 4; row-- > 0;) {
    double sum = b[row];
    for (std::size_t j = row + 1; j < 4; ++j) {
      sum -= a[row][j] * w[j];
    }
    w[row] = sum / a[row][row];
  }
  return w;
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

std::vector<float> sine(double frequency, double sample_rate, double amplitude,
                        std::size_t frames) {
  std::vector<float> out(frames);
  for (std::size_t n = 0; n < frames; ++n) {
    out[n] = static_cast<float>(
        amplitude * std::sin(2.0 * pi * frequency * static_cast<double>(n) / sample_rate));
  }
  return out;
}

void history_rejects_invalid_configuration() {
  current_test = "history_rejects_invalid_configuration";
  CHECK(throws_invalid([] { HistoryBuffer{{1000.0, 16}}; }));
  CHECK(throws_invalid([] { HistoryBuffer{{768001.0, 16}}; }));
  CHECK(throws_invalid([] { HistoryBuffer{{nan, 16}}; }));
  CHECK(throws_invalid([] { HistoryBuffer{{48000.0, 0}}; }));
  CHECK(
      throws_invalid([] { HistoryBuffer{{48000.0, vespera::dsp::maximum_history_samples + 1}}; }));

  HistoryBuffer buffer({48000.0, 64});
  for (std::size_t n = 0; n < 10; ++n) {
    buffer.push(static_cast<float>(n) + 0.5f);
    if (n == 9) {
      CHECK(buffer.read(3.0) == 6.5f);
      CHECK(buffer.read(0.0) == 9.5f);
    }
    buffer.advance();
  }
  CHECK(throws_invalid([&] { buffer.prepare({48000.0, 0}); }));
  CHECK(throws_invalid(
      [&] { buffer.prepare({48000.0, vespera::dsp::maximum_history_samples + 1}); }));
  CHECK(throws_invalid([&] { buffer.prepare({1000.0, 128}); }));
  CHECK(buffer.capacity_samples() == 64);
  CHECK(buffer.sample_rate() == 48000.0);
  buffer.push(99.5f);
  CHECK(buffer.read(1.0) == 9.5f);
  CHECK(buffer.read(0.0) == 99.5f);

  CHECK(!throws_invalid([&] { buffer.prepare({8000.0, 1}); }));
  CHECK(
      !throws_invalid([&] { buffer.prepare({768000.0, vespera::dsp::maximum_history_samples}); }));
  CHECK(buffer.read(0.0) == 0.0f);
  buffer.reset();
  CHECK(buffer.read(0.0) == 0.0f);
  CHECK(
      HistoryBuffer::maximum_delay(vespera::dsp::maximum_history_samples, Interpolation::linear) ==
      static_cast<double>(vespera::dsp::maximum_history_samples - 1));
  CHECK(HistoryBuffer::maximum_delay(vespera::dsp::maximum_history_samples,
                                     Interpolation::lagrange3) ==
        static_cast<double>(vespera::dsp::maximum_history_samples - 3));
}

void history_integer_taps_match_indexed_reference() {
  current_test = "history_integer_taps_match_indexed_reference";
  constexpr std::size_t capacity = 37;
  HistoryBuffer buffer({48000.0, capacity});
  CHECK(buffer.prepared());
  CHECK(!HistoryBuffer().prepared());
  CHECK(buffer.read(0.0) == 0.0f);

  const std::vector<float> input = noise(10000, 7);
  std::vector<float> stored(input.size(), 0.0f);
  const std::size_t taps[] = {0, 1, 5, 36};
  for (std::size_t n = 0; n < input.size(); ++n) {
    buffer.push(input[n]);
    stored[n] = input[n];
    for (const std::size_t tap : taps) {
      const float expected = n >= tap ? stored[n - tap] : 0.0f;
      CHECK(buffer.read(static_cast<double>(tap)) == expected);
    }
    buffer.advance();
  }

  HistoryBuffer pushing({48000.0, 16});
  pushing.push(std::numeric_limits<float>::quiet_NaN());
  CHECK(pushing.read(0.0) == 0.0f);
  pushing.advance();
  pushing.push(2.0f);
  CHECK(pushing.read(1.0) == 0.0f);
  CHECK(pushing.read(0.0) == 2.0f);

  HistoryBuffer unprepared;
  unprepared.push(0.5f);
  unprepared.advance();
  CHECK(unprepared.read(0.0) == 0.0f);
}

void history_fractional_taps_match_independent_weights() {
  current_test = "history_fractional_taps_match_independent_weights";
  constexpr std::size_t capacity = 64;
  HistoryBuffer buffer({48000.0, capacity});
  const std::vector<float> input = noise(256, 11);
  const double lagrange_delays[] = {1.0, 1.25, 2.5, 3.9, 7.125, 16.0625, 30.5, 60.9};
  const double linear_delays[] = {0.0, 0.25, 0.5, 0.75, 1.5, 10.0625, 62.0};
  std::vector<float> stored(input.size(), 0.0f);
  for (std::size_t n = 0; n < input.size(); ++n) {
    buffer.push(input[n]);
    stored[n] = input[n];
    if (n + 1 == input.size()) {
      for (const double delay : lagrange_delays) {
        const auto whole = static_cast<std::size_t>(delay);
        const std::array<double, 4> weights = window_weights(whole - 1, delay);
        double expected = 0.0;
        for (std::size_t j = 0; j < 4; ++j) {
          expected += weights[j] * static_cast<double>(stored[n - (whole - 1 + j)]);
        }
        const double actual = buffer.read(delay, Interpolation::lagrange3);
        CHECK(near(actual, expected, 1e-6 + 1e-6 * std::fabs(expected)));
      }
      for (const double delay : linear_delays) {
        const auto whole = static_cast<std::size_t>(delay);
        const double fraction = delay - static_cast<double>(whole);
        const double newer = stored[n - whole];
        const double older = whole + 1 <= n ? static_cast<double>(stored[n - whole - 1]) : 0.0;
        const double expected = (1.0 - fraction) * newer + fraction * older;
        CHECK(near(buffer.read(delay), expected, 1e-6 + 1e-6 * std::fabs(expected)));
      }
    }
    buffer.advance();
  }
}

void history_matches_fractional_delay_bits() {
  current_test = "history_matches_fractional_delay_bits";
  constexpr std::size_t max_delay = 37;
  FractionalDelay delay_line({48000.0, max_delay, 0.0});
  HistoryBuffer buffer({48000.0, max_delay + 2});
  std::uint64_t state = 3;
  for (std::size_t n = 0; n < 5000; ++n) {
    const double raw = 2.0 * unit(state) - 1.0;
    const float sample =
        n % 97 == 0 ? std::numeric_limits<float>::quiet_NaN() : static_cast<float>(raw);
    double delay = unit(state) * static_cast<double>(max_delay);
    if (n % 53 == 0) {
      delay = static_cast<double>(max_delay);
    }
    if (n % 71 == 0) {
      delay = 0.0;
    }
    if (!delay_line.try_set_delay_samples(delay)) {
      check(false, "try_set_delay_samples", __LINE__);
    }
    buffer.push(sample);
    const float from_delay_line = delay_line.process(sample);
    const float from_history = buffer.read(delay);
    check(from_history == from_delay_line, "bit-identical fractional delay output", __LINE__);
    buffer.advance();
  }
}

void history_linear_reproduces_first_degree() {
  current_test = "history_linear_reproduces_first_degree";
  constexpr std::size_t capacity = 256;
  HistoryBuffer buffer({48000.0, capacity});
  const auto value = [](double t) { return 0.5 * t + 3.0; };
  for (std::size_t n = 0; n < 120; ++n) {
    buffer.push(static_cast<float>(value(static_cast<double>(n))));
    if (n == 119) {
      for (const double delay : {2.0, 2.25, 2.5, 5.75, 40.5, 100.0}) {
        const double expected = value(119.0 - delay);
        CHECK(buffer.read(delay) == static_cast<float>(expected));
      }
    }
    buffer.advance();
  }

  const auto square = [](double t) { return t * t; };
  HistoryBuffer quadratic({48000.0, capacity});
  for (std::size_t n = 0; n < 120; ++n) {
    quadratic.push(static_cast<float>(square(static_cast<double>(n))));
    if (n == 119) {
      double worst = 0.0;
      for (const double delay : {2.5, 10.5, 50.5}) {
        const double expected = square(119.0 - delay);
        worst = std::max(worst, std::fabs(quadratic.read(delay) - expected));
      }
      CHECK(worst > 0.1);
      CHECK(worst < 0.5);
    }
    quadratic.advance();
  }
}

void history_lagrange3_reproduces_third_degree() {
  current_test = "history_lagrange3_reproduces_third_degree";
  constexpr std::size_t capacity = 256;
  const auto cubic = [](double t) { return 3.0 * t * t * t - 5.0 * t * t + 7.0 * t - 2.0; };
  HistoryBuffer buffer({48000.0, capacity});
  for (std::size_t n = 0; n < 100; ++n) {
    buffer.push(static_cast<float>(cubic(static_cast<double>(n))));
    if (n == 99) {
      for (const double delay : {1.0, 1.25, 1.5, 3.125, 7.5, 20.0625, 60.5}) {
        const double expected = cubic(99.0 - delay);
        CHECK(near(buffer.read(delay, Interpolation::lagrange3), expected,
                   1e-6 + 1e-6 * std::fabs(expected)));
      }
    }
    buffer.advance();
  }

  const auto quartic = [](double t) { return t * t * t * t; };
  HistoryBuffer failing({48000.0, capacity});
  for (std::size_t n = 0; n < 40; ++n) {
    failing.push(static_cast<float>(quartic(static_cast<double>(n))));
    if (n == 39) {
      double worst = 0.0;
      for (const double delay : {5.25, 10.5, 20.5}) {
        const double expected = quartic(39.0 - delay);
        worst =
            std::max(worst, std::fabs(failing.read(delay, Interpolation::lagrange3) - expected));
      }
      CHECK(worst > 0.1);
    }
    failing.advance();
  }
}

void history_sine_response_matches_analytic_kernel() {
  current_test = "history_sine_response_matches_analytic_kernel";
  constexpr double rate = 48000.0;
  constexpr std::size_t capacity = 4096;
  constexpr std::size_t count = 4800;
  const double frequencies[] = {100.0, 1230.0, 3050.0, 9000.0};
  const double delays[] = {3.25, 17.5, 40.125, 1281.5};
  for (const double frequency : frequencies) {
    const auto cycles = static_cast<std::size_t>(frequency * count / rate + 0.5);
    check(static_cast<double>(cycles) * rate == frequency * static_cast<double>(count),
          "coherent projection frequency", __LINE__);
    const std::vector<float> signal = sine(frequency, rate, 0.7, 2 * count);
    for (const double delay : delays) {
      const double omega = 2.0 * pi * frequency / rate;
      const auto whole = static_cast<std::size_t>(delay);
      const std::array<double, 4> weights = window_weights(whole - 1, delay);
      std::complex<double> analytic(0.0, 0.0);
      double weight_sum = 0.0;
      for (std::size_t j = 0; j < 4; ++j) {
        analytic +=
            weights[j] *
            std::exp(std::complex<double>(0.0, -omega * static_cast<double>(whole - 1 + j)));
        weight_sum += weights[j];
      }
      CHECK(near(weight_sum, 1.0, 1e-12));

      std::vector<float> lagrange_output(signal.size());
      std::vector<float> linear_output(signal.size());
      HistoryBuffer buffer({rate, capacity});
      for (std::size_t n = 0; n < signal.size(); ++n) {
        buffer.push(signal[n]);
        lagrange_output[n] = buffer.read(delay, Interpolation::lagrange3);
        linear_output[n] = buffer.read(delay);
        buffer.advance();
      }

      std::vector<float> expected(signal.size());
      for (std::size_t n = 0; n < signal.size(); ++n) {
        double value = 0.0;
        for (std::size_t j = 0; j < 4; ++j) {
          const auto tap =
              static_cast<std::ptrdiff_t>(n) - static_cast<std::ptrdiff_t>(whole - 1 + j);
          value += weights[j] *
                   static_cast<double>(tap >= 0 ? signal[static_cast<std::size_t>(tap)] : 0.0f);
        }
        expected[n] = static_cast<float>(value);
      }
      double worst = 0.0;
      for (std::size_t n = 0; n < signal.size(); ++n) {
        worst = std::max<double>(worst, std::fabs(lagrange_output[n] - expected[n]));
      }
      CHECK(worst < 2e-7);

      const std::size_t start = signal.size() - count;
      const Projection measured = project(lagrange_output, start, count, cycles);
      CHECK(near(measured.amplitude, std::abs(analytic) * 0.7, 2e-6));
      const double expected_phase = omega * static_cast<double>(start) + std::arg(analytic);
      CHECK(near(std::remainder(measured.phase - expected_phase, 2.0 * pi), 0.0, 2e-5));

      const double fraction = delay - static_cast<double>(whole);
      const std::complex<double> linear_analytic =
          (1.0 - fraction) *
              std::exp(std::complex<double>(0.0, -omega * static_cast<double>(whole))) +
          fraction * std::exp(std::complex<double>(0.0, -omega * static_cast<double>(whole + 1)));
      const Projection linear_measured = project(linear_output, start, count, cycles);
      CHECK(near(linear_measured.amplitude, std::abs(linear_analytic) * 0.7, 2e-6));
      CHECK(near(std::remainder(linear_measured.phase - omega * static_cast<double>(start) -
                                    std::arg(linear_analytic),
                                2.0 * pi),
                 0.0, 2e-5));
    }
  }
}

void history_kernel_quality_bounds() {
  current_test = "history_kernel_quality_bounds";
  constexpr double rate = 48000.0;
  constexpr std::size_t count = 4800;
  HistoryBuffer buffer({rate, 4096});
  const auto magnitude = [&](double frequency, double delay, Interpolation kernel) {
    const auto cycles = static_cast<std::size_t>(frequency * count / rate + 0.5);
    const std::vector<float> signal = sine(frequency, rate, 0.7, 2 * count);
    std::vector<float> output(signal.size());
    for (std::size_t n = 0; n < signal.size(); ++n) {
      buffer.push(signal[n]);
      output[n] = buffer.read(delay, kernel);
      buffer.advance();
    }
    buffer.reset();
    return project(output, output.size() - count, count, cycles).amplitude / 0.7;
  };
  for (const double delay : {3.25, 17.5, 1281.5}) {
    CHECK(near(magnitude(100.0, delay, Interpolation::linear), 1.0, 3e-4));
    CHECK(near(magnitude(1230.0, delay, Interpolation::linear), 1.0, 0.02));
    CHECK(magnitude(9000.0, delay, Interpolation::linear) < 0.9);
    CHECK(near(magnitude(100.0, delay, Interpolation::lagrange3), 1.0, 2e-6));
    CHECK(near(magnitude(1230.0, delay, Interpolation::lagrange3), 1.0, 5e-5));
    CHECK(near(magnitude(9000.0, delay, Interpolation::lagrange3), 1.0, 0.05));
  }
}

void history_support_boundaries_and_clamping() {
  current_test = "history_support_boundaries_and_clamping";
  HistoryBuffer buffer({48000.0, 8});
  CHECK(near(HistoryBuffer::minimum_delay(Interpolation::linear), 0.0, 0.0));
  CHECK(near(HistoryBuffer::minimum_delay(Interpolation::lagrange3), 1.0, 0.0));
  CHECK(near(HistoryBuffer::maximum_delay(8, Interpolation::linear), 7.0, 0.0));
  CHECK(near(HistoryBuffer::maximum_delay(8, Interpolation::lagrange3), 5.0, 0.0));
  CHECK(near(HistoryBuffer::maximum_delay(3, Interpolation::lagrange3), 0.0, 0.0));

  for (std::size_t n = 0; n < 8; ++n) {
    buffer.push(static_cast<float>(n) + 1.0f);
    if (n == 7) {
      CHECK(buffer.read(7.0) == 1.0f);
      CHECK(buffer.read(8.5) == 1.0f);
      CHECK(buffer.read(-3.0) == 8.0f);
      CHECK(buffer.read(5.0, Interpolation::lagrange3) == 3.0f);
      CHECK(buffer.read(6.0, Interpolation::lagrange3) == 3.0f);
      CHECK(buffer.read(0.5, Interpolation::lagrange3) == 7.0f);
      CHECK(buffer.read(nan) == 0.0f);
      CHECK(buffer.read(inf, Interpolation::lagrange3) == 0.0f);
    }
    buffer.advance();
  }

  HistoryBuffer tiny({48000.0, 1});
  tiny.push(-0.5f);
  CHECK(tiny.read(0.0) == -0.5f);
  CHECK(tiny.read(0.5) == -0.5f);
  CHECK(tiny.read(3.0) == -0.5f);
  CHECK(tiny.read(1.0, Interpolation::lagrange3) == 0.0f);

  HistoryBuffer small({48000.0, 3});
  small.push(0.25f);
  small.advance();
  small.push(0.5f);
  CHECK(small.read(1.0) == 0.25f);
  CHECK(small.read(0.0, Interpolation::lagrange3) == 0.0f);
  CHECK(small.read(2.0, Interpolation::lagrange3) == 0.0f);

  HistoryBuffer dc({48000.0, 32});
  for (std::size_t n = 0; n < 40; ++n) {
    dc.push(0.5f);
    if (n == 39) {
      CHECK(dc.read(12.5) == 0.5f);
      CHECK(dc.read(13.5, Interpolation::lagrange3) == 0.5f);
    }
    dc.advance();
  }
  dc.push(0.7f);
  CHECK(dc.read(0.0) == 0.7f);
  CHECK(dc.read(1.0) == 0.5f);
  CHECK(near(dc.read(13.5, Interpolation::lagrange3), 0.5, 1e-7));
}

void history_reset_and_repeated_prepare() {
  current_test = "history_reset_and_repeated_prepare";
  HistoryBuffer buffer({48000.0, 16});
  for (std::size_t n = 0; n < 20; ++n) {
    buffer.push(static_cast<float>(n) + 1.0f);
    if (n == 19) {
      CHECK(buffer.read(5.0) == 15.0f);
    }
    buffer.advance();
  }
  buffer.reset();
  for (const double delay : {0.0, 1.0, 5.0, 15.0}) {
    CHECK(buffer.read(delay) == 0.0f);
    CHECK(buffer.read(delay, Interpolation::lagrange3) == 0.0f);
  }
  buffer.push(3.0f);
  CHECK(buffer.read(0.0) == 3.0f);
  CHECK(buffer.read(1.0) == 0.0f);
  buffer.advance();

  buffer.prepare({96000.0, 8});
  CHECK(buffer.sample_rate() == 96000.0);
  CHECK(buffer.capacity_samples() == 8);
  CHECK(buffer.read(0.0) == 0.0f);
  buffer.push(1.0f);
  buffer.advance();
  buffer.push(2.0f);
  CHECK(buffer.read(1.0) == 1.0f);
  buffer.advance();
}

// Runs s[n] = x[n] + g * read(D) in feedback order (read before push) and returns the worst
// difference from the independently indexed recurrence s[n] = x[n] + g * sum_j w_j s[n - t_j],
// where t_j are the kernel's window delays and w_j its weights.
double feedback_loop_error(double delay, Interpolation kernel) {
  constexpr double gain = 0.6;
  constexpr std::size_t capacity = 64;
  const std::vector<float> input = noise(3000, 19);
  HistoryBuffer buffer({48000.0, capacity});
  std::vector<double> reference(input.size(), 0.0);
  const auto whole = static_cast<std::size_t>(delay);
  const double fraction = delay - static_cast<double>(whole);
  std::vector<std::size_t> taps;
  std::vector<double> weights;
  if (kernel == Interpolation::linear) {
    taps = {whole, whole + 1};
    weights = {1.0 - fraction, fraction};
  } else {
    const std::array<double, 4> w = window_weights(whole - 1, delay);
    taps = {whole - 1, whole, whole + 1, whole + 2};
    weights.assign(w.begin(), w.end());
  }
  double worst = 0.0;
  for (std::size_t n = 0; n < input.size(); ++n) {
    double feedback = 0.0;
    for (std::size_t j = 0; j < taps.size(); ++j) {
      if (taps[j] >= 1 && n >= taps[j]) {
        feedback += weights[j] * reference[n - taps[j]];
      }
    }
    reference[n] = static_cast<double>(input[n]) + gain * feedback;
    const auto looped = static_cast<float>(static_cast<double>(input[n]) +
                                           gain * static_cast<double>(buffer.read(delay, kernel)));
    buffer.push(looped);
    buffer.advance();
    worst = std::max(worst, std::fabs(static_cast<double>(looped) - reference[n]));
  }
  return worst;
}

void history_feedback_order_loop_delay() {
  current_test = "history_feedback_order_loop_delay";
  CHECK(HistoryBuffer::minimum_feedback_delay(Interpolation::linear) == 1.0);
  CHECK(HistoryBuffer::minimum_feedback_delay(Interpolation::lagrange3) == 2.0);
  for (const double delay : {1.0, 1.5, 7.0, 7.25, 60.0}) {
    CHECK(feedback_loop_error(delay, Interpolation::linear) < 1e-5);
  }
  for (const double delay : {2.0, 2.5, 7.25, 30.75, 61.0}) {
    CHECK(feedback_loop_error(delay, Interpolation::lagrange3) < 1e-5);
  }
  // Below the feedback minimum the Lagrange window reaches the cursor slot, which still holds
  // the sample from capacity pushes ago rather than the current one: the documented hazard.
  CHECK(feedback_loop_error(1.5, Interpolation::lagrange3) > 1e-2);

  HistoryBuffer impulse({48000.0, 16});
  for (std::size_t n = 0; n < 40; ++n) {
    const float looped = (n == 0 ? 1.0f : 0.0f) + 0.5f * impulse.read(5.0);
    impulse.push(looped);
    impulse.advance();
    const float expected = n % 5 == 0 ? std::pow(0.5f, static_cast<float>(n / 5)) : 0.0f;
    CHECK(looped == expected);
  }
}

void history_doppler_experiment() {
  current_test = "history_doppler_experiment";
  constexpr double rate = 48000.0;
  constexpr double shift = 0.02;
  constexpr double base_delay = 1024.0;
  constexpr std::size_t window = 4096;
  constexpr std::size_t count = 24000;
  const double input_frequency = 100.0;
  const double output_frequency = input_frequency * (1.0 - shift);
  const std::vector<float> signal = sine(input_frequency, rate, 0.5, window + count);
  std::vector<float> output(signal.size());
  HistoryBuffer buffer({rate, 4096});
  for (std::size_t n = 0; n < signal.size(); ++n) {
    const double delay = base_delay + shift * static_cast<double>(n);
    buffer.push(signal[n]);
    output[n] = buffer.read(delay, Interpolation::lagrange3);
    buffer.advance();
  }
  const auto shifted = static_cast<std::size_t>(output_frequency * count / rate + 0.5);
  CHECK(shifted == 49);
  const Projection doppler = project(output, window, count, shifted);
  CHECK(doppler.amplitude > 0.49);
  const auto original = static_cast<std::size_t>(input_frequency * count / rate + 0.5);
  CHECK(original == 50);
  const Projection unshifted = project(output, window, count, original);
  CHECK(unshifted.amplitude < 0.01);

  HistoryBuffer unwrapped({rate, 4096});
  for (std::size_t n = 0; n < signal.size(); ++n) {
    unwrapped.push(signal[n]);
    output[n] = unwrapped.read(base_delay, Interpolation::lagrange3);
    unwrapped.advance();
  }
  const Projection still = project(output, window, count, original);
  CHECK(still.amplitude > 0.49);
}

void history_crossfaded_taps_experiment() {
  current_test = "history_crossfaded_taps_experiment";
  constexpr double rate = 48000.0;
  constexpr std::size_t frames = 30000;
  const double frequency = 100.0;
  const std::vector<float> signal = sine(frequency, rate, 0.5, frames);

  HistoryBuffer buffer({rate, 1024});
  std::vector<float> output(frames);
  double worst = 0.0;
  for (std::size_t n = 0; n < frames; ++n) {
    const double weight = 0.5 * (1.0 - std::cos(2.0 * pi * static_cast<double>(n) / 24000.0));
    buffer.push(signal[n]);
    const float first = buffer.read(100.0, Interpolation::lagrange3);
    const float second = buffer.read(300.0, Interpolation::lagrange3);
    output[n] = vespera::dsp::crossfade(first, second, weight);
    buffer.advance();
    if (n < 400) {
      continue;
    }
    const double time = static_cast<double>(n);
    const double expected_first = 0.5 * std::sin(2.0 * pi * frequency * (time - 100.0) / rate);
    const double expected_second = 0.5 * std::sin(2.0 * pi * frequency * (time - 300.0) / rate);
    worst = std::max(
        worst, std::fabs(output[n] - ((1.0 - weight) * expected_first + weight * expected_second)));
  }
  CHECK(worst < 1e-4);
  const Projection carrier = project(output, 4000, 24000, 50);
  CHECK(carrier.amplitude > 0.05);

  const auto measure_static = [&](double weight, std::size_t first, std::size_t second,
                                  double probe_frequency) {
    const std::vector<float> probe = sine(probe_frequency, rate, 0.5, frames);
    std::vector<float> static_output(frames);
    HistoryBuffer static_buffer({rate, 1024});
    for (std::size_t n = 0; n < frames; ++n) {
      static_buffer.push(probe[n]);
      static_output[n] =
          vespera::dsp::crossfade(static_buffer.read(static_cast<double>(first)),
                                  static_buffer.read(static_cast<double>(second)), weight);
      static_buffer.advance();
    }
    const auto cycles = static_cast<std::size_t>(probe_frequency * 24000.0 / rate + 0.5);
    return project(static_output, 4000, 24000, cycles).amplitude;
  };
  const double null_depth = measure_static(0.5, 10, 13, 8000.0);
  CHECK(null_depth < 1e-3);
  const double comb = measure_static(0.5, 10, 11, 8000.0);
  CHECK(near(comb, 0.25 * std::sqrt(3.0), 1e-3));
}

void history_block_partition_is_exact() {
  current_test = "history_block_partition_is_exact";
  const std::vector<float> input = noise(2048, 5);
  const auto process_stream = [&](const std::vector<std::size_t> &blocks) {
    HistoryBuffer buffer({48000.0, 256});
    std::vector<float> outputs;
    outputs.reserve(input.size());
    std::size_t n = 0;
    std::size_t index = 0;
    while (n < input.size()) {
      const std::size_t block = blocks[index % blocks.size()];
      index += 1;
      for (std::size_t i = 0; i < block && n < input.size(); ++i, ++n) {
        const double delay = 10.0 + 5.0 * std::sin(2.0 * pi * static_cast<double>(n) / 97.0);
        buffer.push(input[n]);
        const double first = buffer.read(delay);
        const double second = buffer.read(0.5 * delay + 3.0, Interpolation::lagrange3);
        outputs.push_back(static_cast<float>(first + second));
        buffer.advance();
      }
    }
    return outputs;
  };
  const std::vector<float> reference = process_stream({input.size()});
  const std::vector<std::vector<std::size_t>> partitions = {
      {1}, {2}, {3}, {16}, {64}, {511}, {1, 2, 3, 16, 64, 511, 1000, 1}};
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
  history_rejects_invalid_configuration();
  history_integer_taps_match_indexed_reference();
  history_fractional_taps_match_independent_weights();
  history_matches_fractional_delay_bits();
  history_linear_reproduces_first_degree();
  history_lagrange3_reproduces_third_degree();
  history_sine_response_matches_analytic_kernel();
  history_kernel_quality_bounds();
  history_support_boundaries_and_clamping();
  history_reset_and_repeated_prepare();
  history_feedback_order_loop_delay();
  history_doppler_experiment();
  history_crossfaded_taps_experiment();
  history_block_partition_is_exact();
  if (failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  std::puts("dsp history tests passed");
  return 0;
}
