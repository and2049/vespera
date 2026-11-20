#include <vespera/dsp/state_variable_filter.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <numbers>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

std::atomic<bool> counting_allocations{false};
std::atomic<std::uint64_t> allocation_count{0};

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

using vespera::dsp::StateVariableFilter;
using Output = StateVariableFilter::Output;

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

#define CHECK_NEAR(actual, expected, tolerance)                                                    \
  do {                                                                                             \
    const double check_actual = (actual);                                                          \
    const double check_expected = (expected);                                                      \
    if (!(std::abs(check_actual - check_expected) <= (tolerance))) {                               \
      std::fprintf(stderr, "%s:%d: %s: %s = %.12g, expected %.12g +/- %g\n", __FILE__, __LINE__,   \
                   current_test, #actual, check_actual, check_expected,                            \
                   static_cast<double>(tolerance));                                                \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (false)

template <typename Function> bool throws_invalid(Function &&function) {
  try {
    function();
  } catch (const std::invalid_argument &) {
    return true;
  } catch (...) {
    return false;
  }
  return false;
}

constexpr double nan = std::numeric_limits<double>::quiet_NaN();
constexpr double inf = std::numeric_limits<double>::infinity();

std::vector<double> noise(std::size_t count, std::uint32_t seed) {
  std::vector<double> result(count);
  std::uint32_t state = seed;
  for (double &sample : result) {
    state = state * 1664525u + 1013904223u;
    sample = static_cast<double>(state >> 8) / static_cast<double>(1u << 24) * 2.0 - 1.0;
  }
  return result;
}

enum class Band { low, band, high };

std::vector<Output> impulse_response(double sample_rate, double cutoff, double q,
                                     std::size_t count) {
  StateVariableFilter filter(sample_rate, cutoff, q);
  std::vector<Output> result(count);
  for (std::size_t i = 0; i < count; ++i) {
    result[i] = filter.process(i == 0 ? 1.0 : 0.0);
  }
  return result;
}

double pick(const Output &output, Band band) {
  switch (band) {
  case Band::low:
    return output.low;
  case Band::band:
    return output.band;
  case Band::high:
    return output.high;
  }
  return 0.0;
}

std::complex<double> dtft(const std::vector<Output> &response, Band band,
                          double normalized_frequency) {
  std::complex<double> sum = 0.0;
  for (std::size_t n = 0; n < response.size(); ++n) {
    sum += pick(response[n], band) *
           std::polar(1.0, -2.0 * std::numbers::pi * normalized_frequency * static_cast<double>(n));
  }
  return sum;
}

// Analytic discrete transfer functions, written out from the prewarped bilinear design:
// with k = tan(pi f / fs) and R = 1/Q,
//   low  = k^2 (1 + q)^2 / D(q),  band = k (1 - q^2) / D(q),  high = (1 - q)^2 / D(q),
//   D(q) = (1 + k^2 + k R) + 2 (k^2 - 1) q + (1 + k^2 - k R) q^2,  q = z^-1.
struct Analytic {
  double k = 0.0;
  double r = 0.0;
  double d0 = 1.0;
  double d1 = 0.0;
  double d2 = 1.0;

  Analytic(double sample_rate, double cutoff, double q)
      : k(std::tan(std::numbers::pi * cutoff / sample_rate)), r(1.0 / q) {
    d0 = 1.0 + k * k + k * r;
    d1 = 2.0 * (k * k - 1.0);
    d2 = 1.0 + k * k - k * r;
  }

  std::complex<double> response(Band band, double normalized_frequency) const {
    const std::complex<double> q = std::polar(1.0, -2.0 * std::numbers::pi * normalized_frequency);
    const std::complex<double> denominator = d0 + d1 * q + d2 * q * q;
    switch (band) {
    case Band::low:
      return k * k * (1.0 + q) * (1.0 + q) / denominator;
    case Band::band:
      return k * (1.0 - q * q) / denominator;
    case Band::high:
      return (1.0 - q) * (1.0 - q) / denominator;
    }
    return 0.0;
  }
};

// Independent reference: the prewarped analog two-integrator ODE
//   dx1/dt = w (u - x2 - R x1),  dx2/dt = w x1,  w = (2 / T) tan(pi f / fs),
// integrated by the implicit trapezoidal rule, solved as a 2x2 linear system by Cramer's rule
// at every sample:
//   (I - k J) x[n] = (I + k J) x[n-1] + k [u[n] + u[n-1], 0]^T,  J = [[-R, -1], [1, 0]].
// x1 is the band output, x2 the low output; the high output is the instantaneous loop node.
struct Reference {
  double sample_rate;
  double cutoff;
  double q;
  double x1 = 0.0;
  double x2 = 0.0;
  double previous_input = 0.0;

  Output process(double input) {
    const double k = std::tan(std::numbers::pi * cutoff / sample_rate);
    const double r = 1.0 / q;
    const double rhs0 = (1.0 - k * r) * x1 - k * x2 + k * (input + previous_input);
    const double rhs1 = k * x1 + x2;
    const double determinant = 1.0 + k * r + k * k;
    const double band = (rhs0 - k * rhs1) / determinant;
    const double low = ((1.0 + k * r) * rhs1 + k * rhs0) / determinant;
    x1 = band;
    x2 = low;
    previous_input = input;
    return Output{low, band, input - low - r * band};
  }
};

// Amplitude of a whole-cycle coherent tone in the window, by correlation.
double coherent_amplitude(std::span<const double> signal, double sample_rate, double frequency) {
  const double cycles = std::round(frequency * static_cast<double>(signal.size()) / sample_rate);
  const double step = 2.0 * std::numbers::pi * cycles / static_cast<double>(signal.size());
  double re = 0.0;
  double im = 0.0;
  for (std::size_t n = 0; n < signal.size(); ++n) {
    re += signal[n] * std::cos(step * static_cast<double>(n));
    im -= signal[n] * std::sin(step * static_cast<double>(n));
  }
  return 2.0 * std::hypot(re, im) / static_cast<double>(signal.size());
}

void check_outputs_match(const Output &actual, const Output &expected) {
  CHECK(actual.low == expected.low);
  CHECK(actual.band == expected.band);
  CHECK(actual.high == expected.high);
}

void test_rejects_invalid_controls() {
  CHECK(throws_invalid([] { StateVariableFilter f(7999.0, 100.0, 0.7); }));
  CHECK(throws_invalid([] { StateVariableFilter f(768001.0, 100.0, 0.7); }));
  CHECK(throws_invalid([] { StateVariableFilter f(nan, 100.0, 0.7); }));
  CHECK(throws_invalid([] { StateVariableFilter f(48000.0, 0.0, 0.7); }));
  CHECK(throws_invalid([] { StateVariableFilter f(48000.0, -1.0, 0.7); }));
  CHECK(throws_invalid([] { StateVariableFilter f(48000.0, 24000.0, 0.7); }));
  CHECK(throws_invalid([] { StateVariableFilter f(48000.0, inf, 0.7); }));
  CHECK(throws_invalid([] { StateVariableFilter f(48000.0, nan, 0.7); }));
  CHECK(throws_invalid([] { StateVariableFilter f(48000.0, 48000.0 * 0.4901, 0.7); }));
  CHECK(throws_invalid([] { StateVariableFilter f(48000.0, 48000.0 * 0.99e-5, 0.7); }));
  CHECK(throws_invalid([] { StateVariableFilter f(48000.0, 1000.0, 0.0); }));
  CHECK(throws_invalid([] { StateVariableFilter f(48000.0, 1000.0, -1.0); }));
  CHECK(throws_invalid([] { StateVariableFilter f(48000.0, 1000.0, 0.049); }));
  CHECK(throws_invalid([] { StateVariableFilter f(48000.0, 1000.0, 50.01); }));
  CHECK(throws_invalid([] { StateVariableFilter f(48000.0, 1000.0, nan); }));
  CHECK(!throws_invalid([] { StateVariableFilter f(48000.0, 48000.0 * 0.49, 0.7); }));
  CHECK(!throws_invalid([] { StateVariableFilter f(48000.0, 48000.0 * 1.0e-5, 0.7); }));
  CHECK(!throws_invalid([] { StateVariableFilter f(48000.0, 1000.0, 0.05); }));
  CHECK(!throws_invalid([] { StateVariableFilter f(48000.0, 1000.0, 50.0); }));
  CHECK(!throws_invalid([] { StateVariableFilter f(8000.0, 1000.0, 0.7); }));
  CHECK(!throws_invalid([] { StateVariableFilter f(768000.0, 1000.0, 0.7); }));
}

void test_failed_prepare_leaves_state_unchanged() {
  StateVariableFilter filter(48000.0, 1000.0, 2.0);
  StateVariableFilter untouched(48000.0, 1000.0, 2.0);
  const std::vector<double> input = noise(64, 1);
  for (double x : input) {
    (void)filter.process(x);
    (void)untouched.process(x);
  }
  CHECK(throws_invalid([&] { filter.prepare(48000.0, 0.0, 0.7); }));
  CHECK(throws_invalid([&] { filter.prepare(96000.0, 1000.0, 0.049); }));
  CHECK(filter.sample_rate() == 48000.0);
  CHECK(filter.cutoff() == 1000.0);
  CHECK(filter.q() == 2.0);
  for (double x : input) {
    check_outputs_match(filter.process(x), untouched.process(x));
  }
}

void test_try_set_rejection_leaves_state_unchanged() {
  StateVariableFilter filter(48000.0, 1000.0, 2.0);
  StateVariableFilter reference(48000.0, 1000.0, 2.0);
  const std::vector<double> input = noise(256, 2);
  const double bad_cutoffs[] = {0.0, -1.0, 48000.0 * 0.5,    48000.0 * 0.4901,
                                nan, inf,  48000.0 * 0.99e-5};
  const double bad_qs[] = {0.0, -1.0, 0.049, 50.01, nan, inf};
  for (std::size_t i = 0; i < input.size(); ++i) {
    CHECK(!filter.try_set_cutoff(bad_cutoffs[i % 7]));
    CHECK(!filter.try_set_q(bad_qs[i % 6]));
    CHECK(!filter.try_set_controls(bad_cutoffs[i % 7], 3.0));
    CHECK(!filter.try_set_controls(500.0, bad_qs[i % 6]));
    CHECK(filter.cutoff() == 1000.0);
    CHECK(filter.q() == 2.0);
    check_outputs_match(filter.process(input[i]), reference.process(input[i]));
  }
  CHECK(filter.try_set_controls(500.0, 3.0));
  CHECK(filter.cutoff() == 500.0);
  CHECK(filter.q() == 3.0);
  CHECK(filter.try_set_cutoff(48000.0 * 0.49));
  CHECK(filter.try_set_q(50.0));
  CHECK(filter.try_set_cutoff(48000.0 * 1.0e-5));
  CHECK(filter.try_set_q(0.05));
}

void test_prepare_clears_history() {
  StateVariableFilter filter(48000.0, 1000.0, 2.0);
  for (double x : noise(512, 3)) {
    (void)filter.process(x);
  }
  filter.prepare(48000.0, 1000.0, 2.0);
  StateVariableFilter fresh(48000.0, 1000.0, 2.0);
  for (double x : noise(64, 4)) {
    check_outputs_match(filter.process(x), fresh.process(x));
  }
}

void test_default_matches_explicit() {
  StateVariableFilter defaulted;
  StateVariableFilter explicit_filter(48000.0, 1000.0, 0.7071067811865476);
  for (double x : noise(256, 5)) {
    check_outputs_match(defaulted.process(x), explicit_filter.process(x));
  }
}

void test_analytic_response_normalization() {
  const double rate = 48000.0;
  for (double cutoff : {20.0, 200.0, 2000.0, 20000.0}) {
    for (double q : {0.05, 0.7071067811865476, 5.0, 50.0}) {
      const Analytic analytic(rate, cutoff, q);
      // Summing the denominator at q = +-1 cancels down to ~4 k^2, so the tolerance grows with
      // 1 / k^2 for very low cutoffs.
      const double edge_tolerance = 1e-12 + 1e-15 / (analytic.k * analytic.k);
      for (Band band : {Band::low, Band::band, Band::high}) {
        CHECK_NEAR(std::abs(analytic.response(band, 0.0)), band == Band::low ? 1.0 : 0.0,
                   edge_tolerance);
        CHECK_NEAR(std::abs(analytic.response(band, 0.5)), band == Band::high ? 1.0 : 0.0,
                   edge_tolerance);
        CHECK_NEAR(std::abs(analytic.response(band, cutoff / rate)), q, 1e-9);
      }
    }
  }
}

void test_impulse_response_matches_analytic_transfer() {
  struct Case {
    double rate;
    double cutoff;
    double q;
  };
  const Case cases[] = {
      {48000.0, 1000.0, 0.7071067811865476},
      {48000.0, 5000.0, 8.0},
      {48000.0, 200.0, 0.7071067811865476},
      {96000.0, 2000.0, 2.0},
      {44100.0, 20000.0, 50.0},
      {8000.0, 50.0, 0.7071067811865476},
      {192000.0, 500.0, 5.0},
      {768000.0, 20000.0, 20.0},
      {48000.0, 23000.0, 0.05},
  };
  for (const Case &test : cases) {
    const std::vector<Output> response = impulse_response(test.rate, test.cutoff, test.q, 32768);
    CHECK(std::abs(response.back().band) < 1e-12);
    const Analytic analytic(test.rate, test.cutoff, test.q);
    for (double normalized : {0.0, 0.001, 0.01, 0.05, 0.1, 0.2, 0.3, 0.45, 0.5}) {
      for (Band band : {Band::low, Band::band, Band::high}) {
        const std::complex<double> measured = dtft(response, band, normalized);
        const std::complex<double> expected = analytic.response(band, normalized);
        CHECK(std::abs(measured - expected) < 1e-9);
      }
    }
  }
}

void test_matches_independently_discretized_reference() {
  struct Case {
    double rate;
    double cutoff;
    double q;
  };
  const Case cases[] = {
      {8000.0, 8000.0 * 1.0e-5, 0.05},
      {8000.0, 800.0, 0.7071067811865476},
      {8000.0, 8000.0 * 0.49, 50.0},
      {48000.0, 48000.0 * 1.0e-5, 50.0},
      {48000.0, 1000.0, 0.7071067811865476},
      {48000.0, 48000.0 * 0.49, 0.05},
      {768000.0, 768000.0 * 1.0e-5, 0.7071067811865476},
      {768000.0, 768000.0 * 0.49, 50.0},
  };
  for (const Case &test : cases) {
    const std::vector<double> input = noise(2048, 7);
    StateVariableFilter filter(test.rate, test.cutoff, test.q);
    Reference reference{test.rate, test.cutoff, test.q};
    for (double x : input) {
      const Output expected = reference.process(x);
      const Output actual = filter.process(x);
      CHECK_NEAR(actual.low, expected.low, 1e-9 * (1.0 + std::abs(expected.low)));
      CHECK_NEAR(actual.band, expected.band, 1e-9 * (1.0 + std::abs(expected.band)));
      CHECK_NEAR(actual.high, expected.high, 1e-9 * (1.0 + std::abs(expected.high)));
    }
  }
}

void test_settled_sine_resonance() {
  struct Case {
    double rate;
    double cutoff;
    double q;
  };
  const Case cases[] = {
      {48000.0, 1000.0, 0.7071067811865476},
      {48000.0, 5000.0, 5.0},
      {96000.0, 2000.0, 20.0},
      {44100.0, 300.0, 0.25},
  };
  constexpr std::size_t settle = 65536;
  constexpr std::size_t measure = 65536;
  for (const Case &test : cases) {
    // Configure the cutoff itself to a whole number of cycles in the measurement window, so
    // the correlation reads the response exactly at the cutoff with no detuning bias.
    const double cutoff = std::round(test.cutoff * static_cast<double>(measure) / test.rate) *
                          test.rate / static_cast<double>(measure);
    StateVariableFilter filter(test.rate, cutoff, test.q);
    std::vector<double> low(measure);
    std::vector<double> band(measure);
    std::vector<double> high(measure);
    for (std::size_t i = 0; i < settle + measure; ++i) {
      const Output output = filter.process(
          std::sin(2.0 * std::numbers::pi * cutoff * static_cast<double>(i) / test.rate));
      if (i >= settle) {
        low[i - settle] = output.low;
        band[i - settle] = output.band;
        high[i - settle] = output.high;
      }
    }
    CHECK_NEAR(coherent_amplitude(low, test.rate, cutoff), test.q, 1e-6);
    CHECK_NEAR(coherent_amplitude(band, test.rate, cutoff), test.q, 1e-6);
    CHECK_NEAR(coherent_amplitude(high, test.rate, cutoff), test.q, 1e-6);
    CHECK_NEAR(coherent_amplitude(band, test.rate, cutoff) / test.q, 1.0, 1e-7);
  }
}

void test_dc_and_nyquist_settling() {
  StateVariableFilter filter(48000.0, 1000.0, 0.7071067811865476);
  for (std::size_t i = 0; i < 65536; ++i) {
    const Output output = filter.process(1.0);
    if (i == 65535) {
      CHECK_NEAR(output.low, 1.0, 1e-9);
      CHECK_NEAR(output.band, 0.0, 1e-9);
      CHECK_NEAR(output.high, 0.0, 1e-9);
    }
  }
  filter.reset();
  for (std::size_t i = 0; i < 65536; ++i) {
    const Output output = filter.process(i % 2 == 0 ? 1.0 : -1.0);
    if (i == 65535) {
      CHECK_NEAR(output.high, i % 2 == 0 ? 1.0 : -1.0, 1e-9);
      CHECK_NEAR(output.low, 0.0, 1e-9);
      CHECK_NEAR(output.band, 0.0, 1e-9);
    }
  }
}

void test_designs_stable_across_range() {
  const std::vector<double> input = noise(2048, 8);
  for (double rate : {8000.0, 44100.0, 48000.0, 96000.0, 768000.0}) {
    for (double normalized : {1.0e-5, 1.0e-3, 0.01, 0.1, 0.25, 0.4, 0.49}) {
      for (double q : {0.05, 0.7071067811865476, 5.0, 50.0}) {
        StateVariableFilter filter(rate, normalized * rate, q);
        double peak = 0.0;
        for (double x : input) {
          const Output output = filter.process(x);
          peak = std::max(peak, std::abs(output.low));
          peak = std::max(peak, std::abs(output.band));
          peak = std::max(peak, std::abs(output.high));
        }
        const bool bounded = std::isfinite(peak) && peak < 1.0e3;
        if (!bounded) {
          std::fprintf(stderr, "%s: unbounded rate=%g f=%g q=%g peak=%g\n", current_test, rate,
                       normalized, q, peak);
        }
        CHECK(bounded);
      }
    }
  }
}

void test_rapid_valid_control_changes() {
  const std::vector<double> input = noise(8192, 9);
  for (std::size_t period : {std::size_t{1}, std::size_t{32}}) {
    StateVariableFilter filter(48000.0, 1000.0, 1.0);
    double peak = 0.0;
    for (std::size_t i = 0; i < input.size(); ++i) {
      if (i % period == 0) {
        const bool extreme = (i / period) % 2 == 0;
        CHECK(filter.try_set_controls(extreme ? 48000.0 * 0.49 : 48000.0 * 1.0e-5,
                                      extreme ? 50.0 : 0.05));
      }
      const Output output = filter.process(input[i]);
      peak = std::max(peak, std::abs(output.low));
      peak = std::max(peak, std::abs(output.band));
      peak = std::max(peak, std::abs(output.high));
    }
    CHECK(std::isfinite(peak));
    CHECK(peak < 1.0e3);
  }

  // Zero state stays exactly zero under any sequence of valid control changes.
  StateVariableFilter silent(48000.0, 1000.0, 1.0);
  for (std::size_t i = 0; i < 4096; ++i) {
    CHECK(
        silent.try_set_controls(48000.0 * (i % 2 == 0 ? 0.49 : 1.0e-5), i % 2 == 0 ? 50.0 : 0.05));
    const Output output = silent.process(0.0);
    CHECK(output.low == 0.0 && output.band == 0.0 && output.high == 0.0);
  }

  // After a mid-stream change the settled response tracks the new design, not the old one.
  const double rate = 48000.0;
  const double new_cutoff = std::round(4000.0 * 65536.0 / rate) * rate / 65536.0;
  const double new_q = 4.0;
  StateVariableFilter moved(rate, 200.0, 0.7071067811865476);
  for (std::size_t i = 0; i < 65536; ++i) {
    (void)moved.process(std::sin(2.0 * std::numbers::pi * 100.0 * static_cast<double>(i) / rate));
  }
  CHECK(moved.try_set_controls(new_cutoff, new_q));
  std::vector<double> band(65536);
  for (std::size_t i = 0; i < 131072; ++i) {
    const Output output = moved.process(
        std::sin(2.0 * std::numbers::pi * new_cutoff * static_cast<double>(i) / rate));
    if (i >= 65536) {
      band[i - 65536] = output.band;
    }
  }
  CHECK_NEAR(coherent_amplitude(band, rate, new_cutoff), new_q, 1e-6);
}

// With zero input the outputs reveal the integrator state the sample started from:
// band = (ic1 - k ic2) / D and low = ic2 + k band, so ic2 = low - k band and ic1 = D band + k ic2.
// The reconstructed state norm must never grow, whatever valid control is installed before each
// sample, including the extremes of cutoff and Q alternating every sample.
void test_zero_input_state_norm_never_grows_under_switching() {
  const double rate = 48000.0;
  std::uint32_t seed = 12345;
  const auto uniform = [&seed] {
    seed = seed * 1664525u + 1013904223u;
    return static_cast<double>(seed >> 8) / static_cast<double>(1u << 24);
  };
  for (int pattern = 0; pattern < 3; ++pattern) {
    StateVariableFilter filter(rate, 1000.0, 1.0);
    for (double x : noise(512, 21 + static_cast<std::uint32_t>(pattern))) {
      (void)filter.process(x);
    }
    double previous_norm = -1.0;
    double initial_norm = 0.0;
    double final_norm = 0.0;
    for (std::size_t i = 0; i < 100000; ++i) {
      double cutoff = 0.0;
      double q = 0.0;
      if (pattern == 0) {
        cutoff = rate * (i % 2 == 0 ? 0.49 : 1.0e-5);
        q = i % 2 == 0 ? 50.0 : 0.05;
      } else if (pattern == 1) {
        cutoff = rate * (i % 2 == 0 ? 0.49 : 1.0e-5);
        q = i % 2 == 0 ? 0.05 : 50.0;
      } else {
        cutoff = rate * 1.0e-5 * std::pow(0.49 / 1.0e-5, uniform());
        q = 0.05 * std::pow(1000.0, uniform());
      }
      CHECK(filter.try_set_controls(cutoff, q));
      const double k = std::tan(std::numbers::pi * cutoff / rate);
      const double d = 1.0 + k * k + k / q;
      const Output output = filter.process(0.0);
      const double ic2 = output.low - k * output.band;
      const double ic1 = d * output.band + k * ic2;
      const double norm = std::hypot(ic1, ic2);
      if (previous_norm < 0.0) {
        initial_norm = norm;
      } else if (!(norm <= previous_norm * (1.0 + 1e-9) + 1e-250)) {
        std::fprintf(stderr, "%s: pattern %d sample %zu norm %.17g after %.17g\n", current_test,
                     pattern, i, norm, previous_norm);
        ++failures;
        break;
      }
      previous_norm = norm;
      final_norm = norm;
    }
    CHECK(initial_norm > 0.0);
    CHECK(final_norm <= initial_norm);
  }
}

void test_long_resonant_decay() {
  const double rate = 48000.0;
  const double cutoff = 1000.0;
  const double q = 50.0;
  const std::vector<Output> response = impulse_response(rate, cutoff, q, 1 << 17);
  CHECK(std::abs(response.back().band) < 1e-12);

  // Exact pole radius of the prewarped design: the analog pole
  // s_p = -1/(2Q) + j sqrt(1 - 1/(4Q^2)) maps to z_p = (1 + k s_p) / (1 - k s_p), so the
  // envelope decays by |z_p| per sample.
  const double k = std::tan(std::numbers::pi * cutoff / rate);
  const std::complex<double> pole(-0.5 / q, std::sqrt(1.0 - 0.25 / (q * q)));
  const double radius = std::abs(1.0 + k * pole) / std::abs(1.0 - k * pole);

  // Peaks of |band| over successive 4096-sample windows decay by radius^4096 per window, within
  // the jitter of where each window's ringing peak lands.
  constexpr std::size_t window = 4096;
  double previous_peak = 0.0;
  for (std::size_t start = 5 * window; start + 2 * window <= (1 << 17); start += window) {
    double peak = 0.0;
    for (std::size_t i = start; i < start + window; ++i) {
      peak = std::max(peak, std::abs(response[i].band));
    }
    if (previous_peak > 0.0) {
      CHECK_NEAR(peak / previous_peak, std::pow(radius, static_cast<double>(window)),
                 0.2 * std::pow(radius, static_cast<double>(window)));
    }
    previous_peak = peak;
  }
  double early_peak = 0.0;
  for (std::size_t i = 0; i < window; ++i) {
    early_peak = std::max(early_peak, std::abs(response[i].band));
  }
  for (std::size_t i = window; i < (1 << 17); ++i) {
    CHECK(std::abs(response[i].band) <=
          2.0 * early_peak *
              std::pow(radius, static_cast<double>(i) - static_cast<double>(window)));
  }
}

void test_reset_clears_history() {
  StateVariableFilter filter(48000.0, 400.0, 8.0);
  StateVariableFilter fresh(48000.0, 400.0, 8.0);
  for (double x : noise(512, 10)) {
    (void)filter.process(x);
  }
  filter.reset();
  for (double x : noise(256, 11)) {
    check_outputs_match(filter.process(x), fresh.process(x));
  }

  for (std::size_t i = 0; i < 4; ++i) {
    (void)filter.process(nan);
  }
  CHECK(std::isnan(filter.process(0.0).low));
  filter.reset();
  CHECK(filter.process(0.0).low == 0.0);
}

void test_stream_position_partition_invariance() {
  const std::vector<double> input = noise(4099, 12);
  const auto control_at = [](StateVariableFilter &filter, std::size_t position) {
    if (position % 1000 == 0) {
      CHECK(filter.try_set_controls(position % 2000 == 0 ? 200.0 : 4000.0,
                                    position % 2000 == 0 ? 0.5 : 8.0));
    }
  };
  // Reference: control changes anchored to absolute stream position.
  StateVariableFilter whole(48000.0, 1000.0, 2.0);
  std::vector<Output> expected(input.size());
  for (std::size_t i = 0; i < input.size(); ++i) {
    control_at(whole, i);
    expected[i] = whole.process(input[i]);
  }
  // The same stream split into blocks of many sizes, including sizes that do and do not
  // coincide with the control-change positions: bit-identical output. The filter has no block
  // size or cadence dependence of its own.
  const std::size_t partitions[] = {1, 2, 3, 7, 64, 127, 999, 1000, 4099};
  for (std::size_t block : partitions) {
    StateVariableFilter filter(48000.0, 1000.0, 2.0);
    for (std::size_t start = 0; start < input.size(); start += block) {
      const std::size_t count = std::min(block, input.size() - start);
      for (std::size_t i = 0; i < count; ++i) {
        control_at(filter, start + i);
        check_outputs_match(filter.process(input[start + i]), expected[start + i]);
      }
    }
  }
}

void test_processing_does_not_allocate() {
  StateVariableFilter filter(48000.0, 1000.0, 2.0);
  const std::vector<double> input = noise(1024, 13);
  allocation_count.store(0);
  counting_allocations.store(true);
  for (double x : input) {
    (void)filter.process(x);
  }
  CHECK(filter.try_set_cutoff(2000.0));
  CHECK(filter.try_set_q(4.0));
  CHECK(filter.try_set_controls(3000.0, 1.0));
  (void)filter.process(0.5f);
  filter.reset();
  counting_allocations.store(false);
  CHECK(allocation_count.load() == 0);
}

}

int main() {
  const std::pair<const char *, void (*)()> tests[] = {
      {"rejects_invalid_controls", test_rejects_invalid_controls},
      {"failed_prepare_leaves_state_unchanged", test_failed_prepare_leaves_state_unchanged},
      {"try_set_rejection_leaves_state_unchanged", test_try_set_rejection_leaves_state_unchanged},
      {"prepare_clears_history", test_prepare_clears_history},
      {"default_matches_explicit", test_default_matches_explicit},
      {"analytic_response_normalization", test_analytic_response_normalization},
      {"impulse_response_matches_analytic_transfer",
       test_impulse_response_matches_analytic_transfer},
      {"matches_independently_discretized_reference",
       test_matches_independently_discretized_reference},
      {"settled_sine_resonance", test_settled_sine_resonance},
      {"dc_and_nyquist_settling", test_dc_and_nyquist_settling},
      {"designs_stable_across_range", test_designs_stable_across_range},
      {"rapid_valid_control_changes", test_rapid_valid_control_changes},
      {"zero_input_state_norm_never_grows_under_switching",
       test_zero_input_state_norm_never_grows_under_switching},
      {"long_resonant_decay", test_long_resonant_decay},
      {"reset_clears_history", test_reset_clears_history},
      {"stream_position_partition_invariance", test_stream_position_partition_invariance},
      {"processing_does_not_allocate", test_processing_does_not_allocate},
  };
  for (const auto &[name, test] : tests) {
    current_test = name;
    test();
  }
  if (failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  std::puts("all DSP state-variable filter tests passed");
  return 0;
}
