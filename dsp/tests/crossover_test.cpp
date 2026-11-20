#include <vespera/dsp/crossover.hpp>

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

using vespera::dsp::BiquadCascade;
using vespera::dsp::BiquadCoefficients;
using vespera::dsp::LinkwitzRileyCrossover;
using Output = LinkwitzRileyCrossover::Output;

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
constexpr double section_q = 0.7071067811865476;

std::vector<double> noise(std::size_t count, std::uint32_t seed) {
  std::vector<double> result(count);
  std::uint32_t state = seed;
  for (double &sample : result) {
    state = state * 1664525u + 1013904223u;
    sample = static_cast<double>(state >> 8) / static_cast<double>(1u << 24) * 2.0 - 1.0;
  }
  return result;
}

struct Branches {
  std::vector<double> low;
  std::vector<double> high;
};

Branches impulse_response(double sample_rate, double cutoff, std::size_t count) {
  LinkwitzRileyCrossover crossover(sample_rate, cutoff);
  Branches result{{std::vector<double>(count)}, {std::vector<double>(count)}};
  for (std::size_t i = 0; i < count; ++i) {
    const Output output = crossover.process(i == 0 ? 1.0 : 0.0);
    result.low[i] = output.low;
    result.high[i] = output.high;
  }
  return result;
}

std::complex<double> dtft(const std::vector<double> &signal, double normalized_frequency) {
  std::complex<double> sum = 0.0;
  for (std::size_t n = 0; n < signal.size(); ++n) {
    sum += signal[n] *
           std::polar(1.0, -2.0 * std::numbers::pi * normalized_frequency * static_cast<double>(n));
  }
  return sum;
}

// Independent analytic model. Each RBJ biquad section at (fs, fc, 1/sqrt(2)) is exactly the
// bilinear image of the normalized analog Butterworth section 1 / (s^2 + sqrt(2) s + 1)
// evaluated at s = j tan(pi f / fs) / tan(pi fc / fs), so the two cascaded sections per branch
// reproduce the analog fourth-order Linkwitz-Riley responses:
//   low4  = 1 / D(s)^2,   high4 = s^4 / D(s)^2,   D(s) = s^2 + sqrt(2) s + 1,
// and their in-phase sum is the second-order allpass
//   low4 + high4 = (s^4 + 1) / D(s)^2 = (s^2 - sqrt(2) s + 1) / D(s).
enum class Branch { low, high, sum };

std::complex<double> analytic(double sample_rate, double cutoff, double frequency, Branch branch) {
  const std::complex<double> j(0.0, 1.0);
  const double u = std::tan(std::numbers::pi * frequency / sample_rate) /
                   std::tan(std::numbers::pi * cutoff / sample_rate);
  const std::complex<double> s = j * u;
  const std::complex<double> d = s * s + std::numbers::sqrt2 * s + 1.0;
  switch (branch) {
  case Branch::low:
    return 1.0 / (d * d);
  case Branch::high:
    return s * s * s * s / (d * d);
  case Branch::sum:
    return (s * s - std::numbers::sqrt2 * s + 1.0) / d;
  }
  return 0.0;
}

// Complex correlation of a whole-cycle coherent tone; magnitude is its amplitude and its phase
// carries the branch polarity.
std::complex<double> coherent_tone(std::span<const double> signal, double sample_rate,
                                   double frequency) {
  const double cycles = std::round(frequency * static_cast<double>(signal.size()) / sample_rate);
  const double step = 2.0 * std::numbers::pi * cycles / static_cast<double>(signal.size());
  std::complex<double> sum = 0.0;
  for (std::size_t n = 0; n < signal.size(); ++n) {
    sum += signal[n] * std::polar(1.0, -step * static_cast<double>(n));
  }
  return 2.0 * sum / static_cast<double>(signal.size());
}

void test_rejects_invalid_controls() {
  CHECK(throws_invalid([] { LinkwitzRileyCrossover c(7999.0, 100.0); }));
  CHECK(throws_invalid([] { LinkwitzRileyCrossover c(768001.0, 100.0); }));
  CHECK(throws_invalid([] { LinkwitzRileyCrossover c(nan, 100.0); }));
  CHECK(throws_invalid([] { LinkwitzRileyCrossover c(48000.0, 0.0); }));
  CHECK(throws_invalid([] { LinkwitzRileyCrossover c(48000.0, -1.0); }));
  CHECK(throws_invalid([] { LinkwitzRileyCrossover c(48000.0, 24000.0); }));
  CHECK(throws_invalid([] { LinkwitzRileyCrossover c(48000.0, inf); }));
  CHECK(throws_invalid([] { LinkwitzRileyCrossover c(48000.0, nan); }));
  CHECK(throws_invalid([] { LinkwitzRileyCrossover c(48000.0, 48000.0 * 0.4901); }));
  CHECK(throws_invalid([] { LinkwitzRileyCrossover c(48000.0, 48000.0 * 0.99e-5); }));
  CHECK(!throws_invalid([] { LinkwitzRileyCrossover c(48000.0, 48000.0 * 0.49); }));
  CHECK(!throws_invalid([] { LinkwitzRileyCrossover c(48000.0, 48000.0 * 1.0e-5); }));
  CHECK(!throws_invalid([] { LinkwitzRileyCrossover c(8000.0, 100.0); }));
  CHECK(!throws_invalid([] { LinkwitzRileyCrossover c(768000.0, 100.0); }));
}

void test_failed_configure_leaves_state_unchanged() {
  LinkwitzRileyCrossover crossover(48000.0, 1000.0);
  LinkwitzRileyCrossover untouched(48000.0, 1000.0);
  const std::vector<double> input = noise(64, 1);
  for (double x : input) {
    (void)crossover.process(x);
    (void)untouched.process(x);
  }
  CHECK(throws_invalid([&] { crossover.configure(48000.0, 0.0); }));
  CHECK(throws_invalid([&] { crossover.configure(96000.0, 96000.0 * 0.4901); }));
  CHECK(crossover.sample_rate() == 48000.0);
  CHECK(crossover.cutoff() == 1000.0);
  for (double x : input) {
    const Output output = crossover.process(x);
    const Output expected = untouched.process(x);
    CHECK(output.low == expected.low);
    CHECK(output.high == expected.high);
  }
}

void test_configure_keeps_state() {
  const double rate = 48000.0;
  const double first_cutoff = 800.0;
  const double second_cutoff = 3000.0;
  LinkwitzRileyCrossover crossover(rate, first_cutoff);
  BiquadCascade<2> reference_low;
  BiquadCascade<2> reference_high;
  const auto install = [&](BiquadCascade<2> &cascade, bool lowpass, double cutoff) {
    const auto coefficients = lowpass ? BiquadCoefficients::lowpass(rate, cutoff, section_q)
                                      : BiquadCoefficients::highpass(rate, cutoff, section_q);
    cascade.set_coefficients({coefficients, coefficients});
  };
  install(reference_low, true, first_cutoff);
  install(reference_high, false, first_cutoff);
  const std::vector<double> input = noise(256, 2);
  for (std::size_t i = 0; i < input.size(); ++i) {
    if (i == 128) {
      crossover.configure(rate, second_cutoff);
      install(reference_low, true, second_cutoff);
      install(reference_high, false, second_cutoff);
    }
    const Output output = crossover.process(input[i]);
    CHECK(output.low == reference_low.process(input[i]));
    CHECK(output.high == reference_high.process(input[i]));
  }
}

void test_default_matches_explicit() {
  LinkwitzRileyCrossover defaulted;
  LinkwitzRileyCrossover explicit_filter(48000.0, 1000.0);
  for (double x : noise(256, 3)) {
    const Output a = defaulted.process(x);
    const Output b = explicit_filter.process(x);
    CHECK(a.low == b.low);
    CHECK(a.high == b.high);
  }
}

void test_matches_hand_built_biquad_cascades() {
  const double rate = 48000.0;
  for (double cutoff : {100.0, 1000.0, 12000.0}) {
    LinkwitzRileyCrossover crossover(rate, cutoff);
    BiquadCascade<2> low;
    BiquadCascade<2> high;
    const auto low_section = BiquadCoefficients::lowpass(rate, cutoff, section_q);
    const auto high_section = BiquadCoefficients::highpass(rate, cutoff, section_q);
    low.set_coefficients({low_section, low_section});
    high.set_coefficients({high_section, high_section});
    CHECK(low_section.a1() == high_section.a1());
    CHECK(low_section.a2() == high_section.a2());
    for (double x : noise(512, 4)) {
      const Output output = crossover.process(x);
      CHECK(output.low == low.process(x));
      CHECK(output.high == high.process(x));
    }
  }
}

void test_branches_match_analytic_fourth_order_responses() {
  struct Case {
    double rate;
    double cutoff;
  };
  const Case cases[] = {
      {48000.0, 100.0},  {48000.0, 1000.0}, {48000.0, 8000.0},  {44100.0, 300.0},
      {96000.0, 5000.0}, {8000.0, 50.0},    {192000.0, 2000.0}, {768000.0, 200000.0},
  };
  for (const Case &test : cases) {
    const Branches branches = impulse_response(test.rate, test.cutoff, 16384);
    CHECK(std::abs(branches.low.back()) < 1e-12);
    CHECK(std::abs(branches.high.back()) < 1e-12);
    std::vector<double> sum(branches.low.size());
    for (std::size_t i = 0; i < sum.size(); ++i) {
      sum[i] = branches.low[i] + branches.high[i];
    }
    for (double normalized : {0.0, 0.001, 0.01, 0.05, 0.1, 0.2, 0.3, 0.45, 0.4999}) {
      const double frequency = normalized * test.rate;
      const std::complex<double> measured_low = dtft(branches.low, normalized);
      const std::complex<double> measured_high = dtft(branches.high, normalized);
      const std::complex<double> measured_sum = dtft(sum, normalized);
      CHECK(std::abs(measured_low - analytic(test.rate, test.cutoff, frequency, Branch::low)) <
            1e-9);
      CHECK(std::abs(measured_high - analytic(test.rate, test.cutoff, frequency, Branch::high)) <
            1e-9);
      CHECK(std::abs(measured_sum - analytic(test.rate, test.cutoff, frequency, Branch::sum)) <
            1e-9);
      CHECK_NEAR(std::abs(measured_sum), 1.0, 1e-9);
    }
  }
}

void test_crossover_point_and_polarity() {
  const double rate = 48000.0;
  const double cutoff = 1000.0;
  const Branches branches = impulse_response(rate, cutoff, 16384);
  const std::complex<double> low = dtft(branches.low, cutoff / rate);
  const std::complex<double> high = dtft(branches.high, cutoff / rate);
  CHECK_NEAR(std::abs(low), 0.5, 1e-9);
  CHECK_NEAR(std::abs(high), 0.5, 1e-9);
  // Both branches are exactly in phase at the crossover, so the reconstruction sum is -1 there:
  // the polarity convention is low + high with no inversion.
  CHECK_NEAR(std::abs(low + high), 1.0, 1e-9);
  CHECK(std::abs((low + high) + 1.0) < 1e-9);
  CHECK((low / high).imag() < 1e-12);
  CHECK_NEAR((low / high).real(), 1.0, 1e-9);

  // The summed impulse is not a delayed or scaled copy of the dry impulse: the reconstruction is
  // an allpass with its own phase, not a sample-null against dry.
  double summed_first = branches.low[0] + branches.high[0];
  CHECK(std::abs(summed_first - 1.0) > 1e-3);
  CHECK(std::abs(branches.low[1] + branches.high[1]) > 1e-6);

  for (double normalized : {0.0, 0.01, 0.05, 0.15, 0.3, 0.45, 0.4999}) {
    const std::complex<double> l = dtft(branches.low, normalized);
    const std::complex<double> h = dtft(branches.high, normalized);
    CHECK_NEAR(std::abs(l + h), 1.0, 1e-9);
  }
}

void test_section_poles_match_bilinear_butterworth() {
  struct Case {
    double rate;
    double cutoff;
  };
  const Case cases[] = {
      {8000.0, 8000.0 * 1.0e-5}, {48000.0, 20.0},   {48000.0, 1000.0},
      {48000.0, 48000.0 * 0.49}, {96000.0, 5000.0}, {768000.0, 200000.0},
  };
  for (const Case &test : cases) {
    const auto section = BiquadCoefficients::lowpass(test.rate, test.cutoff, section_q);
    // Roots of the section denominator 1 + a1 q + a2 q^2.
    const double discriminant = section.a1() * section.a1() - 4.0 * section.a2();
    CHECK(discriminant < 0.0);
    const std::complex<double> root(-section.a1() / (2.0 * section.a2()),
                                    std::sqrt(-discriminant) / (2.0 * section.a2()));
    // The analog Butterworth pole s_p = (-1 + j) / sqrt(2) maps to z_p = (1 + k s_p) / (1 - k s_p)
    // under the section's prewarping; the two section poles are the conjugate pair.
    const double k = std::tan(std::numbers::pi * test.cutoff / test.rate);
    const std::complex<double> analog_pole(-1.0 / std::numbers::sqrt2, 1.0 / std::numbers::sqrt2);
    const std::complex<double> expected_pole = (1.0 + k * analog_pole) / (1.0 - k * analog_pole);
    CHECK(std::abs(1.0 / root - std::conj(expected_pole)) < 1e-9);
    CHECK(std::abs(1.0 / std::conj(root) - expected_pole) < 1e-9);
    CHECK(std::abs(expected_pole) < 1.0);
  }
}

void test_settled_sine_crossover() {
  const double rate = 48000.0;
  const double cutoff = std::round(1000.0 * 65536.0 / rate) * rate / 65536.0;
  LinkwitzRileyCrossover crossover(rate, cutoff);
  constexpr std::size_t settle = 65536;
  constexpr std::size_t measure = 65536;
  std::vector<double> low(measure);
  std::vector<double> high(measure);
  std::vector<double> sum(measure);
  for (std::size_t i = 0; i < settle + measure; ++i) {
    const double x = std::sin(2.0 * std::numbers::pi * cutoff * static_cast<double>(i) / rate);
    const Output output = crossover.process(x);
    if (i >= settle) {
      low[i - settle] = output.low;
      high[i - settle] = output.high;
      sum[i - settle] = output.low + output.high;
    }
  }
  CHECK_NEAR(std::abs(coherent_tone(low, rate, cutoff)), 0.5, 1e-6);
  CHECK_NEAR(std::abs(coherent_tone(high, rate, cutoff)), 0.5, 1e-6);
  // The summed settled tone has unity amplitude and the allpass phase: at the crossover it is
  // the exact polarity inversion of the input tone.
  CHECK_NEAR(std::abs(coherent_tone(sum, rate, cutoff)), 1.0, 1e-6);
  std::vector<double> input_measure(measure);
  for (std::size_t i = 0; i < measure; ++i) {
    input_measure[i] =
        std::sin(2.0 * std::numbers::pi * cutoff * static_cast<double>(settle + i) / rate);
  }
  const std::complex<double> input = coherent_tone(input_measure, rate, cutoff);
  const std::complex<double> summed = coherent_tone(sum, rate, cutoff);
  CHECK_NEAR(std::abs(summed / input + 1.0), 0.0, 1e-6);
}

void test_designs_stable_across_range() {
  const std::vector<double> input = noise(2048, 5);
  for (double rate : {8000.0, 44100.0, 48000.0, 96000.0, 768000.0}) {
    for (double normalized : {1.0e-5, 1.0e-3, 0.01, 0.1, 0.25, 0.4, 0.49}) {
      LinkwitzRileyCrossover crossover(rate, normalized * rate);
      double peak = 0.0;
      for (double x : input) {
        const Output output = crossover.process(x);
        peak = std::max(peak, std::abs(output.low));
        peak = std::max(peak, std::abs(output.high));
      }
      const bool bounded = std::isfinite(peak) && peak < 1.0e3;
      if (!bounded) {
        std::fprintf(stderr, "%s: unbounded rate=%g f=%g peak=%g\n", current_test, rate, normalized,
                     peak);
      }
      CHECK(bounded);
    }
  }
}

void test_reset_clears_history() {
  LinkwitzRileyCrossover crossover(48000.0, 800.0);
  LinkwitzRileyCrossover fresh(48000.0, 800.0);
  for (double x : noise(512, 6)) {
    (void)crossover.process(x);
  }
  crossover.reset();
  for (double x : noise(256, 7)) {
    const Output output = crossover.process(x);
    const Output expected = fresh.process(x);
    CHECK(output.low == expected.low);
    CHECK(output.high == expected.high);
  }

  for (std::size_t i = 0; i < 4; ++i) {
    (void)crossover.process(nan);
  }
  CHECK(std::isnan(crossover.process(0.0).low));
  crossover.reset();
  CHECK(crossover.process(0.0).low == 0.0);
}

void test_stream_position_partition_invariance() {
  const std::vector<double> input = noise(4099, 8);
  LinkwitzRileyCrossover whole(48000.0, 1000.0);
  std::vector<Output> expected(input.size());
  for (std::size_t i = 0; i < input.size(); ++i) {
    expected[i] = whole.process(input[i]);
  }
  const std::size_t partitions[] = {1, 2, 3, 7, 64, 127, 999, 1000, 4099};
  for (std::size_t block : partitions) {
    LinkwitzRileyCrossover crossover(48000.0, 1000.0);
    for (std::size_t start = 0; start < input.size(); start += block) {
      const std::size_t count = std::min(block, input.size() - start);
      for (std::size_t i = 0; i < count; ++i) {
        const Output output = crossover.process(input[start + i]);
        CHECK(output.low == expected[start + i].low);
        CHECK(output.high == expected[start + i].high);
      }
    }
  }
}

void test_processing_does_not_allocate() {
  LinkwitzRileyCrossover crossover(48000.0, 1000.0);
  const std::vector<double> input = noise(1024, 9);
  allocation_count.store(0);
  counting_allocations.store(true);
  for (double x : input) {
    (void)crossover.process(x);
  }
  crossover.configure(48000.0, 2000.0);
  (void)crossover.process(0.5f);
  crossover.reset();
  counting_allocations.store(false);
  CHECK(allocation_count.load() == 0);
}

}

int main() {
  const std::pair<const char *, void (*)()> tests[] = {
      {"rejects_invalid_controls", test_rejects_invalid_controls},
      {"failed_configure_leaves_state_unchanged", test_failed_configure_leaves_state_unchanged},
      {"configure_keeps_state", test_configure_keeps_state},
      {"default_matches_explicit", test_default_matches_explicit},
      {"matches_hand_built_biquad_cascades", test_matches_hand_built_biquad_cascades},
      {"branches_match_analytic_fourth_order_responses",
       test_branches_match_analytic_fourth_order_responses},
      {"crossover_point_and_polarity", test_crossover_point_and_polarity},
      {"section_poles_match_bilinear_butterworth", test_section_poles_match_bilinear_butterworth},
      {"settled_sine_crossover", test_settled_sine_crossover},
      {"designs_stable_across_range", test_designs_stable_across_range},
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
  std::puts("all DSP crossover tests passed");
  return 0;
}
