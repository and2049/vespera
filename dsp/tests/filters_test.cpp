#include <vespera/dsp/filters.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
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

using vespera::dsp::Biquad;
using vespera::dsp::BiquadCoefficients;
using vespera::dsp::FirstOrderCoefficients;
using vespera::dsp::FirstOrderFilter;

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

constexpr double rate = 48000.0;
constexpr double nan = std::numeric_limits<double>::quiet_NaN();
constexpr double inf = std::numeric_limits<double>::infinity();

double db(double magnitude) { return 20.0 * std::log10(magnitude); }

std::vector<double> noise(std::size_t count, std::uint32_t seed) {
  std::vector<double> result(count);
  std::uint32_t state = seed;
  for (double &sample : result) {
    state = state * 1664525u + 1013904223u;
    sample = static_cast<double>(state >> 8) / static_cast<double>(1u << 24) * 2.0 - 1.0;
  }
  return result;
}

template <typename Filter> std::vector<double> impulse_response(Filter filter, std::size_t count) {
  filter.reset();
  std::vector<double> result(count);
  for (std::size_t i = 0; i < count; ++i) {
    result[i] = filter.process(i == 0 ? 1.0 : 0.0);
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

double response_db(const BiquadCoefficients &c, double frequency) {
  return db(std::abs(c.response(frequency / rate)));
}

double response_db(const FirstOrderCoefficients &c, double frequency) {
  return db(std::abs(c.response(frequency / rate)));
}

void test_rejects_invalid_controls() {
  CHECK(throws_invalid([] { (void)BiquadCoefficients::lowpass(7999.0, 100.0, 0.7); }));
  CHECK(throws_invalid([] { (void)BiquadCoefficients::lowpass(768001.0, 100.0, 0.7); }));
  CHECK(throws_invalid([] { (void)BiquadCoefficients::lowpass(nan, 100.0, 0.7); }));
  CHECK(throws_invalid([] { (void)BiquadCoefficients::lowpass(rate, 0.0, 0.7); }));
  CHECK(throws_invalid([] { (void)BiquadCoefficients::lowpass(rate, -1.0, 0.7); }));
  CHECK(throws_invalid([] { (void)BiquadCoefficients::lowpass(rate, 24000.0, 0.7); }));
  CHECK(throws_invalid([] { (void)BiquadCoefficients::lowpass(rate, inf, 0.7); }));
  CHECK(throws_invalid([] { (void)BiquadCoefficients::lowpass(rate, rate * 0.4901, 0.7); }));
  CHECK(throws_invalid([] { (void)BiquadCoefficients::lowpass(rate, rate * 0.99e-5, 0.7); }));
  CHECK(!throws_invalid([] { (void)BiquadCoefficients::lowpass(rate, rate * 0.49, 0.7); }));
  CHECK(!throws_invalid([] { (void)BiquadCoefficients::lowpass(rate, rate * 1.0e-5, 0.7); }));
  CHECK(throws_invalid([] { (void)BiquadCoefficients::bandpass(rate, 1000.0, 0.0); }));
  CHECK(throws_invalid([] { (void)BiquadCoefficients::bandpass(rate, 1000.0, -1.0); }));
  CHECK(throws_invalid([] { (void)BiquadCoefficients::bandpass(rate, 1000.0, 0.049); }));
  CHECK(throws_invalid([] { (void)BiquadCoefficients::bandpass(rate, 1000.0, 50.01); }));
  CHECK(throws_invalid([] { (void)BiquadCoefficients::bandpass(rate, 1000.0, nan); }));
  CHECK(!throws_invalid([] { (void)BiquadCoefficients::bandpass(rate, 1000.0, 0.05); }));
  CHECK(!throws_invalid([] { (void)BiquadCoefficients::bandpass(rate, 1000.0, 50.0); }));
  CHECK(throws_invalid([] { (void)BiquadCoefficients::peak(rate, 1000.0, 1.0, 48.01); }));
  CHECK(throws_invalid([] { (void)BiquadCoefficients::low_shelf(rate, 1000.0, 1.0, -48.01); }));
  CHECK(throws_invalid([] { (void)BiquadCoefficients::high_shelf(rate, 1000.0, 1.0, inf); }));
  CHECK(throws_invalid([] { (void)BiquadCoefficients::peak(rate, 1000.0, 1.0, nan); }));
  CHECK(!throws_invalid([] { (void)BiquadCoefficients::peak(rate, 1000.0, 1.0, -48.0); }));
  CHECK(throws_invalid([] { (void)FirstOrderCoefficients::lowpass(rate, 0.0); }));
  CHECK(throws_invalid([] { (void)FirstOrderCoefficients::highpass(rate, 24000.0); }));
  CHECK(throws_invalid([] { (void)FirstOrderCoefficients::dc_blocker(4000.0); }));
  CHECK(!throws_invalid([] { (void)FirstOrderCoefficients::dc_blocker(8000.0); }));
  CHECK(!throws_invalid([] { (void)FirstOrderCoefficients::dc_blocker(768000.0); }));
}

void test_raw_coefficients_require_stability() {
  CHECK(!throws_invalid([] { (void)BiquadCoefficients::from_raw(1.0, 0.0, 0.0, -1.9, 0.95); }));
  CHECK(throws_invalid([] { (void)BiquadCoefficients::from_raw(1.0, 0.0, 0.0, 0.0, 1.0); }));
  CHECK(throws_invalid([] { (void)BiquadCoefficients::from_raw(1.0, 0.0, 0.0, 0.0, -1.0); }));
  CHECK(throws_invalid([] { (void)BiquadCoefficients::from_raw(1.0, 0.0, 0.0, -2.0, 1.0); }));
  CHECK(throws_invalid([] { (void)BiquadCoefficients::from_raw(1.0, 0.0, 0.0, -1.5, 0.5); }));
  CHECK(throws_invalid([] { (void)BiquadCoefficients::from_raw(1.0, 0.0, 0.0, 1.5, 0.5); }));
  CHECK(throws_invalid([] { (void)BiquadCoefficients::from_raw(nan, 0.0, 0.0, 0.0, 0.0); }));
  CHECK(throws_invalid([] { (void)BiquadCoefficients::from_raw(1.0, inf, 0.0, 0.0, 0.0); }));
  CHECK(!throws_invalid([] { (void)FirstOrderCoefficients::from_raw(1.0, 0.0, -0.999); }));
  CHECK(throws_invalid([] { (void)FirstOrderCoefficients::from_raw(1.0, 0.0, -1.0); }));
  CHECK(throws_invalid([] { (void)FirstOrderCoefficients::from_raw(1.0, 0.0, 1.0); }));
  CHECK(throws_invalid([] { (void)FirstOrderCoefficients::from_raw(1.0, nan, 0.0); }));
}

void test_failed_design_leaves_processor_unchanged() {
  Biquad biquad(BiquadCoefficients::lowpass(rate, 1000.0, 0.7071));
  Biquad reference = biquad;
  const std::vector<double> input = noise(64, 1);
  for (std::size_t i = 0; i < 32; ++i) {
    (void)biquad.process(input[i]);
    (void)reference.process(input[i]);
  }
  CHECK(
      throws_invalid([&] { biquad.set_coefficients(BiquadCoefficients::peak(rate, 0.0, 1, 3)); }));
  CHECK(biquad.coefficients() == reference.coefficients());
  for (std::size_t i = 32; i < input.size(); ++i) {
    CHECK(biquad.process(input[i]) == reference.process(input[i]));
  }

  FirstOrderFilter first(FirstOrderCoefficients::lowpass(rate, 500.0));
  const FirstOrderCoefficients before = first.coefficients();
  CHECK(
      throws_invalid([&] { first.set_coefficients(FirstOrderCoefficients::highpass(rate, nan)); }));
  CHECK(first.coefficients() == before);
}

void test_defaults_are_identity() {
  Biquad biquad;
  FirstOrderFilter first;
  const std::vector<double> input = noise(256, 2);
  for (double x : input) {
    CHECK(biquad.process(x) == x);
    CHECK(first.process(x) == x);
    CHECK(biquad.process(static_cast<float>(x)) == static_cast<float>(x));
  }
}

void test_analytic_responses() {
  const double f0 = 1000.0;
  const double q = 2.0;
  const double nyquist = rate / 2.0;
  const double tolerance = 1.0e-9;

  const auto lp = BiquadCoefficients::lowpass(rate, f0, q);
  CHECK_NEAR(response_db(lp, 0.0), 0.0, tolerance);
  CHECK_NEAR(response_db(lp, f0), db(q), tolerance);
  CHECK(std::abs(lp.response(0.5)) < 1.0e-12);

  const auto hp = BiquadCoefficients::highpass(rate, f0, q);
  CHECK(std::abs(hp.response(0.0)) < 1.0e-12);
  CHECK_NEAR(response_db(hp, f0), db(q), tolerance);
  CHECK_NEAR(response_db(hp, nyquist), 0.0, tolerance);

  const auto bp = BiquadCoefficients::bandpass(rate, f0, q);
  CHECK(std::abs(bp.response(0.0)) < 1.0e-12);
  CHECK_NEAR(response_db(bp, f0), 0.0, tolerance);
  CHECK(std::abs(bp.response(0.5)) < 1.0e-12);
  const double bandwidth_edge = f0 * 1.3;
  CHECK(response_db(bp, bandwidth_edge) < -3.0);

  const auto notch = BiquadCoefficients::notch(rate, f0, q);
  CHECK(std::abs(notch.response(f0 / rate)) < 1.0e-9);
  CHECK_NEAR(response_db(notch, 0.0), 0.0, tolerance);
  CHECK_NEAR(response_db(notch, nyquist), 0.0, tolerance);

  for (double gain : {-48.0, -12.0, 6.0, 48.0}) {
    const auto peak = BiquadCoefficients::peak(rate, f0, q, gain);
    CHECK_NEAR(response_db(peak, f0), gain, tolerance);
    CHECK_NEAR(response_db(peak, 0.0), 0.0, tolerance);
    CHECK_NEAR(response_db(peak, nyquist), 0.0, tolerance);

    const auto low = BiquadCoefficients::low_shelf(rate, f0, 0.7071, gain);
    CHECK_NEAR(response_db(low, 0.0), gain, tolerance);
    CHECK_NEAR(response_db(low, f0), gain / 2.0, tolerance);
    CHECK_NEAR(response_db(low, nyquist), 0.0, tolerance);

    const auto high = BiquadCoefficients::high_shelf(rate, f0, 0.7071, gain);
    CHECK_NEAR(response_db(high, 0.0), 0.0, tolerance);
    CHECK_NEAR(response_db(high, f0), gain / 2.0, tolerance);
    CHECK_NEAR(response_db(high, nyquist), gain, tolerance);
  }

  const double half_power = db(std::sqrt(0.5));
  const auto lp1 = FirstOrderCoefficients::lowpass(rate, f0);
  CHECK_NEAR(response_db(lp1, 0.0), 0.0, tolerance);
  CHECK_NEAR(response_db(lp1, f0), half_power, tolerance);
  CHECK(std::abs(lp1.response(0.5)) < 1.0e-12);
  const auto hp1 = FirstOrderCoefficients::highpass(rate, f0);
  CHECK(std::abs(hp1.response(0.0)) < 1.0e-12);
  CHECK_NEAR(response_db(hp1, f0), half_power, tolerance);
  CHECK_NEAR(response_db(hp1, nyquist), 0.0, tolerance);
  const auto dc = FirstOrderCoefficients::dc_blocker(rate);
  CHECK(dc == FirstOrderCoefficients::highpass(rate, 10.0));
  CHECK_NEAR(response_db(dc, 10.0), half_power, tolerance);
  CHECK(response_db(dc, 1.0) < -19.0);
  CHECK_NEAR(response_db(dc, 100.0), 0.0, 0.05);
}

void test_impulse_response_matches_transfer_function() {
  const std::vector<BiquadCoefficients> designs = {
      BiquadCoefficients::lowpass(rate, 3000.0, 0.7071),
      BiquadCoefficients::highpass(rate, 200.0, 4.0),
      BiquadCoefficients::bandpass(rate, 5000.0, 10.0),
      BiquadCoefficients::notch(rate, 60.0, 0.5),
      BiquadCoefficients::peak(rate, 2500.0, 1.5, 9.0),
      BiquadCoefficients::low_shelf(rate, 120.0, 0.7071, -10.0),
      BiquadCoefficients::high_shelf(rate, 8000.0, 0.7071, 12.0),
  };
  for (const auto &coefficients : designs) {
    const std::vector<double> h = impulse_response(Biquad(coefficients), 16384);
    CHECK(std::abs(h.back()) < 1.0e-15);
    for (double f : {0.0, 0.003, 0.02, 0.1, 0.27, 0.45, 0.5}) {
      CHECK(std::abs(dtft(h, f) - coefficients.response(f)) < 1.0e-9);
    }
  }
  const std::vector<double> h1 =
      impulse_response(FirstOrderFilter(FirstOrderCoefficients::lowpass(rate, 700.0)), 4096);
  for (double f : {0.0, 0.01, 0.2, 0.5}) {
    CHECK(std::abs(dtft(h1, f) - FirstOrderCoefficients::lowpass(rate, 700.0).response(f)) <
          1.0e-9);
  }
}

void test_matches_reference_convolution() {
  Biquad biquad(BiquadCoefficients::peak(rate, 1800.0, 3.0, 7.5));
  const std::vector<double> h = impulse_response(biquad, 4096);
  const std::vector<double> input = noise(2048, 3);
  biquad.reset();
  for (std::size_t n = 0; n < input.size(); ++n) {
    double expected = 0.0;
    for (std::size_t k = 0; k <= n; ++k) {
      expected += h[k] * input[n - k];
    }
    CHECK_NEAR(biquad.process(input[n]), expected, 1.0e-12);
  }

  FirstOrderFilter first(FirstOrderCoefficients::highpass(rate, 300.0));
  const std::vector<double> h1 = impulse_response(first, 4096);
  first.reset();
  for (std::size_t n = 0; n < input.size(); ++n) {
    double expected = 0.0;
    for (std::size_t k = 0; k <= n; ++k) {
      expected += h1[k] * input[n - k];
    }
    CHECK_NEAR(first.process(input[n]), expected, 1.0e-12);
  }
}

void test_designs_are_stable_across_range() {
  using Design = std::function<BiquadCoefficients(double, double, double)>;
  const std::vector<std::pair<const char *, Design>> designs = {
      {"lowpass",
       [](double r, double f, double q) { return BiquadCoefficients::lowpass(r, f, q); }},
      {"highpass",
       [](double r, double f, double q) { return BiquadCoefficients::highpass(r, f, q); }},
      {"bandpass",
       [](double r, double f, double q) { return BiquadCoefficients::bandpass(r, f, q); }},
      {"notch", [](double r, double f, double q) { return BiquadCoefficients::notch(r, f, q); }},
      {"peak+", [](double r, double f, double q) { return BiquadCoefficients::peak(r, f, q, 48); }},
      {"peak-",
       [](double r, double f, double q) { return BiquadCoefficients::peak(r, f, q, -48); }},
      {"low_shelf+",
       [](double r, double f, double q) { return BiquadCoefficients::low_shelf(r, f, q, 48); }},
      {"low_shelf-",
       [](double r, double f, double q) { return BiquadCoefficients::low_shelf(r, f, q, -48); }},
      {"high_shelf+",
       [](double r, double f, double q) { return BiquadCoefficients::high_shelf(r, f, q, 48); }},
      {"high_shelf-",
       [](double r, double f, double q) { return BiquadCoefficients::high_shelf(r, f, q, -48); }},
  };
  const std::vector<double> input = noise(2048, 4);
  for (double sample_rate : {8000.0, 44100.0, 768000.0}) {
    for (double normalized : {1.0e-5, 1.0e-4, 1.0e-3, 0.01, 0.1, 0.25, 0.4, 0.49}) {
      for (double q : {0.05, 0.7071, 5.0, 50.0}) {
        for (const auto &[name, design] : designs) {
          BiquadCoefficients c;
          try {
            c = design(sample_rate, normalized * sample_rate, q);
          } catch (const std::exception &) {
            std::fprintf(stderr, "%s: %s rejected rate=%g f=%g q=%g\n", current_test, name,
                         sample_rate, normalized, q);
            ++failures;
            continue;
          }
          Biquad biquad(c);
          double peak = 0.0;
          for (double x : input) {
            peak = std::max(peak, std::abs(biquad.process(x)));
          }
          const bool bounded = std::isfinite(peak) && peak < 1.0e5;
          if (!bounded) {
            std::fprintf(stderr, "%s: %s unbounded rate=%g f=%g q=%g peak=%g\n", current_test, name,
                         sample_rate, normalized, q, peak);
          }
          CHECK(bounded);
        }
      }
    }
    for (double normalized : {1.0e-5, 0.01, 0.49}) {
      CHECK(!throws_invalid(
          [&] { (void)FirstOrderCoefficients::lowpass(sample_rate, normalized * sample_rate); }));
      CHECK(!throws_invalid(
          [&] { (void)FirstOrderCoefficients::highpass(sample_rate, normalized * sample_rate); }));
    }
  }
}

void test_impulse_decays_for_extreme_resonance() {
  Biquad biquad(BiquadCoefficients::bandpass(rate, rate * 0.49, 50.0));
  const std::vector<double> h = impulse_response(biquad, 1 << 16);
  CHECK(std::abs(h.back()) < 1.0e-12);
  Biquad slow(BiquadCoefficients::lowpass(rate, rate * 0.01, 50.0));
  const std::vector<double> hs = impulse_response(slow, 1 << 16);
  CHECK(std::abs(hs.back()) < 1.0e-12);
}

void test_reset_clears_history() {
  Biquad biquad(BiquadCoefficients::lowpass(rate, 400.0, 8.0));
  FirstOrderFilter first(FirstOrderCoefficients::lowpass(rate, 50.0));
  const Biquad fresh = biquad;
  const FirstOrderFilter fresh_first = first;
  for (double x : noise(512, 5)) {
    (void)biquad.process(x);
    (void)first.process(x);
  }
  biquad.reset();
  first.reset();
  CHECK(biquad.coefficients() == fresh.coefficients());
  Biquad expected = fresh;
  FirstOrderFilter expected_first = fresh_first;
  for (double x : noise(256, 6)) {
    CHECK(biquad.process(x) == expected.process(x));
    CHECK(first.process(x) == expected_first.process(x));
  }

  for (std::size_t i = 0; i < 4; ++i) {
    (void)biquad.process(nan);
  }
  CHECK(std::isnan(biquad.process(0.0)));
  biquad.reset();
  CHECK(biquad.process(0.0) == 0.0);
}

template <typename Sample> void check_partition_invariance() {
  const auto coefficients = BiquadCoefficients::high_shelf(rate, 3000.0, 1.2, 6.0);
  const std::vector<double> source = noise(4099, 7);
  std::vector<Sample> input(source.size());
  for (std::size_t i = 0; i < source.size(); ++i) {
    input[i] = static_cast<Sample>(source[i]);
  }

  Biquad per_sample(coefficients);
  FirstOrderFilter per_sample_first(FirstOrderCoefficients::lowpass(rate, 900.0));
  std::vector<Sample> expected(input.size());
  for (std::size_t i = 0; i < input.size(); ++i) {
    expected[i] = per_sample_first.process(per_sample.process(input[i]));
  }

  const std::size_t partitions[] = {1, 2, 3, 7, 64, 127, 1000, 4099};
  for (std::size_t block : partitions) {
    Biquad blocked(coefficients);
    FirstOrderFilter blocked_first(FirstOrderCoefficients::lowpass(rate, 900.0));
    std::vector<Sample> in_place = input;
    std::vector<Sample> separate(input.size());
    Biquad separate_biquad(coefficients);
    FirstOrderFilter separate_first(FirstOrderCoefficients::lowpass(rate, 900.0));
    for (std::size_t start = 0; start < input.size(); start += block) {
      const std::size_t count = std::min(block, input.size() - start);
      std::span<Sample> chunk(in_place.data() + start, count);
      blocked.process(chunk);
      blocked_first.process(chunk);
      std::span<const Sample> source_chunk(input.data() + start, count);
      std::span<Sample> out_chunk(separate.data() + start, count);
      separate_biquad.process(source_chunk, out_chunk);
      separate_first.process(std::span<const Sample>(out_chunk), out_chunk);
    }
    CHECK(in_place == expected);
    CHECK(separate == expected);
  }
}

void test_block_partition_invariance() {
  check_partition_invariance<float>();
  check_partition_invariance<double>();
}

void test_coefficient_change_keeps_state_and_stays_bounded() {
  Biquad biquad(BiquadCoefficients::lowpass(rate, 200.0, 20.0));
  const std::vector<double> input = noise(8192, 8);
  double peak = 0.0;
  for (std::size_t i = 0; i < input.size(); ++i) {
    if (i % 32 == 0) {
      const double frequency = (i / 32) % 2 == 0 ? 200.0 : 12000.0;
      biquad.set_coefficients(BiquadCoefficients::lowpass(rate, frequency, 20.0));
    }
    peak = std::max(peak, std::abs(biquad.process(input[i])));
  }
  CHECK(std::isfinite(peak));
  CHECK(peak < 1.0e3);

  Biquad continuing(BiquadCoefficients::lowpass(rate, 1000.0, 0.7071));
  for (double x : noise(64, 9)) {
    (void)continuing.process(x);
  }
  continuing.set_coefficients(BiquadCoefficients::lowpass(rate, 1000.0, 0.7071));
  CHECK(continuing.process(0.0) != 0.0);
}

void test_processing_does_not_allocate() {
  Biquad biquad(BiquadCoefficients::peak(rate, 1000.0, 1.0, 3.0));
  FirstOrderFilter first(FirstOrderCoefficients::dc_blocker(rate));
  const auto replacement = BiquadCoefficients::notch(rate, 50.0, 2.0);
  std::vector<float> buffer(1024, 0.25f);
  std::vector<double> output(1024);
  const std::vector<double> input = noise(1024, 10);
  allocation_count.store(0);
  counting_allocations.store(true);
  biquad.process(std::span<float>(buffer));
  first.process(std::span<const double>(input), std::span<double>(output));
  biquad.set_coefficients(replacement);
  (void)biquad.process(0.5);
  biquad.reset();
  first.reset();
  counting_allocations.store(false);
  CHECK(allocation_count.load() == 0);
}

}

int main() {
  for (double sample_rate : {124000.0, 48000.0, 768000.0}) {
    try {
      static_cast<void>(BiquadCoefficients::peak(
          sample_rate, sample_rate * vespera::dsp::minimum_normalized_filter_frequency, 0.05,
          -48.0));
    } catch (const std::invalid_argument &) {
      std::fputs("exact minimum filter frequency rejected\n", stderr);
      return 1;
    }
  }
  const std::pair<const char *, void (*)()> tests[] = {
      {"rejects_invalid_controls", test_rejects_invalid_controls},
      {"raw_coefficients_require_stability", test_raw_coefficients_require_stability},
      {"failed_design_leaves_processor_unchanged", test_failed_design_leaves_processor_unchanged},
      {"defaults_are_identity", test_defaults_are_identity},
      {"analytic_responses", test_analytic_responses},
      {"impulse_response_matches_transfer_function",
       test_impulse_response_matches_transfer_function},
      {"matches_reference_convolution", test_matches_reference_convolution},
      {"designs_are_stable_across_range", test_designs_are_stable_across_range},
      {"impulse_decays_for_extreme_resonance", test_impulse_decays_for_extreme_resonance},
      {"reset_clears_history", test_reset_clears_history},
      {"block_partition_invariance", test_block_partition_invariance},
      {"coefficient_change_keeps_state_and_stays_bounded",
       test_coefficient_change_keeps_state_and_stays_bounded},
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
  std::puts("all DSP filter tests passed");
  return 0;
}
