#include <vespera/dsp/filters.hpp>
#include <vespera/dsp/oversampling.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
using vespera::dsp::Oversampler;
using vespera::dsp::OversamplingFactor;

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

#define CHECK_BELOW(actual, limit)                                                                 \
  do {                                                                                             \
    const double check_actual = (actual);                                                          \
    if (!(check_actual < (limit))) {                                                               \
      std::fprintf(stderr, "%s:%d: %s: %s = %.6g, expected below %g\n", __FILE__, __LINE__,        \
                   current_test, #actual, check_actual, static_cast<double>(limit));               \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (false)

constexpr OversamplingFactor factors[] = {OversamplingFactor::two, OversamplingFactor::four};
constexpr std::size_t fft_size = 4096;
constexpr std::size_t warmup = 1024;
constexpr double pi = std::numbers::pi;

const auto identity = [](double x) noexcept { return x; };

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

template <typename Function> bool throws_invalid(Function function) {
  try {
    function();
  } catch (const std::invalid_argument &) {
    return true;
  }
  return false;
}

void fft(std::vector<std::complex<double>> &data) {
  const std::size_t n = data.size();
  for (std::size_t i = 1, j = 0; i < n; ++i) {
    std::size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) {
      j ^= bit;
    }
    j ^= bit;
    if (i < j) {
      std::swap(data[i], data[j]);
    }
  }
  for (std::size_t length = 2; length <= n; length <<= 1) {
    const std::complex<double> step = std::polar(1.0, -2.0 * pi / static_cast<double>(length));
    for (std::size_t start = 0; start < n; start += length) {
      std::complex<double> twiddle = 1.0;
      for (std::size_t k = 0; k < length / 2; ++k) {
        const std::complex<double> even = data[start + k];
        const std::complex<double> odd = data[start + k + length / 2] * twiddle;
        data[start + k] = even + odd;
        data[start + k + length / 2] = even - odd;
        twiddle *= step;
      }
    }
  }
}

// Single-sided amplitude spectrum; a bin-aligned sinusoid of amplitude A reads A.
std::vector<double> amplitude_spectrum(const std::vector<double> &signal) {
  std::vector<std::complex<double>> data(signal.begin(), signal.end());
  fft(data);
  std::vector<double> result(signal.size() / 2 + 1);
  for (std::size_t k = 0; k < result.size(); ++k) {
    result[k] = std::abs(data[k]) * 2.0 / static_cast<double>(signal.size());
  }
  return result;
}

double sine(std::size_t bin, std::size_t n, std::size_t period) {
  return std::sin(2.0 * pi * static_cast<double>(bin * n % period) / static_cast<double>(period));
}

int ratio_of(OversamplingFactor factor) { return static_cast<int>(factor); }

void test_factor_selection_and_latency() {
  CHECK(Oversampler::latency(OversamplingFactor::two) == 63);
  CHECK(Oversampler::latency(OversamplingFactor::four) == 71);
  Oversampler oversampler;
  CHECK(oversampler.factor() == OversamplingFactor::two);
  CHECK(oversampler.ratio() == 2);
  CHECK(oversampler.latency_samples() == 63);
  oversampler.prepare(OversamplingFactor::four);
  CHECK(oversampler.ratio() == 4);
  CHECK(oversampler.latency_samples() == 71);
  CHECK(Oversampler::callback_input_bound(OversamplingFactor::two) > 2.7);
  CHECK(Oversampler::callback_input_bound(OversamplingFactor::two) < 2.8);
  CHECK(Oversampler::callback_input_bound(OversamplingFactor::four) < 5.1);

  bool threw = false;
  try {
    Oversampler invalid(static_cast<OversamplingFactor>(3));
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  CHECK(threw);
}

void test_invalid_prepare_leaves_state() {
  Oversampler oversampler(OversamplingFactor::four);
  const std::vector<double> input = noise(300, 1);
  for (std::size_t i = 0; i < 150; ++i) {
    (void)oversampler.process(input[i], identity);
  }
  Oversampler reference = oversampler;
  for (int bad : {0, 1, 3, 8, -2}) {
    bool threw = false;
    try {
      oversampler.prepare(static_cast<OversamplingFactor>(bad));
    } catch (const std::invalid_argument &) {
      threw = true;
    }
    CHECK(threw);
  }
  CHECK(oversampler.factor() == OversamplingFactor::four);
  for (std::size_t i = 150; i < input.size(); ++i) {
    CHECK(oversampler.process(input[i], identity) == reference.process(input[i], identity));
  }
}

void test_dc_is_unity() {
  for (OversamplingFactor factor : factors) {
    Oversampler oversampler(factor);
    double worst = 0.0;
    for (std::size_t n = 0; n < 2000; ++n) {
      const double output = oversampler.process(0.5, identity);
      if (n >= 200) {
        worst = std::max(worst, std::abs(output - 0.5));
      }
    }
    CHECK_BELOW(worst, 1.0e-14);
  }
}

void test_impulse_is_linear_phase_at_latency() {
  for (OversamplingFactor factor : factors) {
    Oversampler oversampler(factor);
    const auto latency = static_cast<std::size_t>(oversampler.latency_samples());
    std::vector<double> h(2 * latency + 32);
    for (std::size_t n = 0; n < h.size(); ++n) {
      h[n] = oversampler.process(n == 0 ? 1.0 : 0.0, identity);
    }
    const auto peak = static_cast<std::size_t>(
        std::max_element(h.begin(), h.end(),
                         [](double a, double b) { return std::abs(a) < std::abs(b); }) -
        h.begin());
    CHECK(peak == latency);
    for (std::size_t k = 1; k <= latency; ++k) {
      CHECK_BELOW(std::abs(h[latency + k] - h[latency - k]), 1.0e-15);
    }
    for (std::size_t n = 2 * latency + 1; n < h.size(); ++n) {
      CHECK(h[n] == 0.0);
    }
  }
}

void test_passband_is_flat_and_latency_aligned() {
  const std::size_t bins[] = {1, 41, 410, 1229, 1638, 1761, 1843};
  for (OversamplingFactor factor : factors) {
    for (std::size_t bin : bins) {
      Oversampler oversampler(factor);
      const auto latency = static_cast<std::size_t>(oversampler.latency_samples());
      double worst = 0.0;
      for (std::size_t n = 0; n < warmup + fft_size; ++n) {
        const double output = oversampler.process(sine(bin, n, fft_size), identity);
        if (n >= warmup) {
          worst = std::max(worst, std::abs(output - sine(bin, n - latency, fft_size)));
        }
      }
      const double normalized = static_cast<double>(bin) / static_cast<double>(fft_size);
      const double limit = normalized <= 0.43 ? 1.2e-4 : 1.2e-3;
      if (!(worst < limit)) {
        std::fprintf(stderr, "%s: ratio %d f=%.4f error %.3g\n", current_test, ratio_of(factor),
                     normalized, worst);
      }
      CHECK(worst < limit);
    }
  }
}

double strongest_spurious_db(const std::vector<double> &spectrum, std::size_t signal_bin,
                             std::size_t harmonic_spacing) {
  double worst = 0.0;
  for (std::size_t k = 1; k < spectrum.size(); ++k) {
    if (k != signal_bin && (harmonic_spacing == 0 || k % harmonic_spacing != 0)) {
      worst = std::max(worst, spectrum[k]);
    }
  }
  return db(worst / spectrum[signal_bin]);
}

void test_interpolation_images_are_rejected() {
  for (OversamplingFactor factor : factors) {
    const auto ratio = static_cast<std::size_t>(ratio_of(factor));
    for (std::size_t bin : {205ul, 1024ul, 1761ul}) {
      Oversampler oversampler(factor);
      std::vector<double> high(ratio * (warmup + fft_size));
      std::size_t index = 0;
      auto capture = [&](double x) noexcept {
        high[index++] = x;
        return x;
      };
      for (std::size_t n = 0; n < warmup + fft_size; ++n) {
        (void)oversampler.process(sine(bin, n, fft_size), capture);
      }
      const std::vector<double> steady(high.begin() + static_cast<long>(ratio * warmup),
                                       high.end());
      const std::vector<double> spectrum = amplitude_spectrum(steady);
      CHECK_BELOW(std::abs(spectrum[bin] - 1.0), 1.2e-4);
      const double spurious = strongest_spurious_db(spectrum, bin, 0);
      if (!(spurious < -99.0)) {
        std::fprintf(stderr, "%s: ratio %zu bin %zu image %.1f dB\n", current_test, ratio, bin,
                     spurious);
      }
      CHECK(spurious < -99.0);
    }
  }
}

void test_documented_fir_response() {
  const auto magnitude = [](const auto &taps, double frequency) {
    double response = 0.5;
    for (std::size_t j = 0; j < taps.size(); ++j) {
      const double offset = 2.0 * static_cast<double>(j) - static_cast<double>(taps.size() - 1);
      response += taps[j] * std::cos(std::numbers::pi * frequency * offset);
    }
    return std::abs(response);
  };
  for (int i = 0; i <= 10000; ++i) {
    const double position = static_cast<double>(i) / 10000.0;
    const double passband = 0.45 * position;
    const double stopband = 0.56 + 0.44 * position;
    CHECK(std::abs(db(magnitude(vespera::dsp::detail::first_stage_taps, passband))) < 0.0003);
    CHECK(std::abs(db(magnitude(vespera::dsp::detail::second_stage_taps, passband / 2.0))) <
          0.0001);
    CHECK(db(magnitude(vespera::dsp::detail::first_stage_taps, stopband)) < -99.0);
  }
}

void test_decimation_aliases_are_rejected() {
  for (OversamplingFactor factor : factors) {
    const auto ratio = static_cast<std::size_t>(ratio_of(factor));
    const std::size_t high_period = ratio * fft_size;
    const std::size_t lowest = static_cast<std::size_t>(0.57 * fft_size) + 1;
    const std::size_t highest = ratio * fft_size / 2 - 1;
    for (std::size_t tone = lowest; tone <= highest; tone += 97) {
      const std::size_t folded_once = tone % fft_size;
      const std::size_t alias = std::min(folded_once, fft_size - folded_once);
      if (alias > static_cast<std::size_t>(0.43 * fft_size)) {
        continue;
      }
      Oversampler oversampler(factor);
      std::size_t index = 0;
      auto generator = [&](double) noexcept { return sine(tone, index++, high_period); };
      std::vector<double> output(fft_size);
      for (std::size_t n = 0; n < warmup + fft_size; ++n) {
        const double y = oversampler.process(0.0, generator);
        if (n >= warmup) {
          output[n - warmup] = y;
        }
      }
      const std::vector<double> spectrum = amplitude_spectrum(output);
      const double level = db(*std::max_element(spectrum.begin(), spectrum.end()));
      if (!(level < -99.0)) {
        std::fprintf(stderr, "%s: ratio %zu tone %.4f alias %.1f dB\n", current_test, ratio,
                     static_cast<double>(tone) / fft_size, level);
      }
      CHECK(level < -99.0);
    }
  }
}

struct TanhResult {
  double alias_db;
  std::vector<double> spectrum;
};

TanhResult run_tanh(int ratio, std::size_t bin, double drive) {
  Oversampler oversampler;
  if (ratio == 4) {
    oversampler.prepare(OversamplingFactor::four);
  }
  const auto shaper = [drive](double x) noexcept { return std::tanh(drive * x); };
  std::vector<double> output(fft_size);
  for (std::size_t n = 0; n < warmup + fft_size; ++n) {
    const double x = 0.9 * sine(bin, n, fft_size);
    const double y = ratio == 1 ? shaper(x) : oversampler.process(x, shaper);
    if (n >= warmup) {
      output[n - warmup] = y;
    }
  }
  std::vector<double> spectrum = amplitude_spectrum(output);
  spectrum.resize(static_cast<std::size_t>(0.43 * fft_size) + 1);
  return {strongest_spurious_db(spectrum, bin, bin), spectrum};
}

std::vector<double> ideal_tanh_harmonics(double drive, std::size_t count) {
  constexpr std::size_t points = 1 << 14;
  std::vector<double> period(points);
  for (std::size_t n = 0; n < points; ++n) {
    period[n] = std::tanh(drive * 0.9 * sine(1, n, points));
  }
  std::vector<double> spectrum = amplitude_spectrum(period);
  spectrum.resize(count + 1);
  return spectrum;
}

void test_tanh_aliasing_is_reduced() {
  constexpr std::size_t bin = 437;
  constexpr double drive = 4.0;
  const TanhResult naive = run_tanh(1, bin, drive);
  const TanhResult twice = run_tanh(2, bin, drive);
  const TanhResult four = run_tanh(4, bin, drive);
  std::printf("tanh drive %.0f at %.4f fs: strongest alias naive %.1f dB, 2x %.1f dB, 4x %.1f dB\n",
              drive, static_cast<double>(bin) / fft_size, naive.alias_db, twice.alias_db,
              four.alias_db);
  CHECK(naive.alias_db > -35.0);
  CHECK_BELOW(twice.alias_db, -50.0);
  CHECK_BELOW(four.alias_db, -120.0);

  const std::size_t in_band = (naive.spectrum.size() - 1) / bin;
  const std::vector<double> ideal = ideal_tanh_harmonics(drive, in_band);
  for (std::size_t h = 1; h <= in_band; h += 2) {
    if (ideal[h] < 1.0e-5) {
      continue;
    }
    const double error = std::abs(db(four.spectrum[h * bin] / ideal[h]));
    if (!(error < 0.01)) {
      std::fprintf(stderr, "%s: harmonic %zu error %.4f dB\n", current_test, h, error);
    }
    CHECK(error < 0.01);
  }
}

void test_callback_order_and_count() {
  for (OversamplingFactor factor : factors) {
    Oversampler oversampler(factor);
    std::size_t calls = 0;
    std::vector<double> seen(static_cast<std::size_t>(oversampler.ratio()) * 512);
    auto record = [&](double x) noexcept {
      seen[calls++] = x;
      return x;
    };
    for (std::size_t n = 0; n < 512; ++n) {
      (void)oversampler.process(sine(3, n, 4096), record);
    }
    CHECK(calls == seen.size());
    const std::size_t up_latency = factor == OversamplingFactor::two ? 63 : 141;
    double worst = 0.0;
    for (std::size_t m = 2 * up_latency; m < calls; ++m) {
      const double t = static_cast<double>(m) - static_cast<double>(up_latency);
      const double expected = std::sin(2.0 * pi * 3.0 * t / (4096.0 * oversampler.ratio()));
      worst = std::max(worst, std::abs(seen[m] - expected));
    }
    CHECK_BELOW(worst, 1.0e-4);
  }
}

void test_reset_and_prepare_clear_history() {
  for (OversamplingFactor factor : factors) {
    Oversampler oversampler(factor);
    const Oversampler fresh(factor);
    for (double x : noise(500, 2)) {
      (void)oversampler.process(x, identity);
    }
    oversampler.reset();
    Oversampler expected = fresh;
    for (double x : noise(300, 3)) {
      CHECK(oversampler.process(x, identity) == expected.process(x, identity));
    }

    for (double x : noise(500, 4)) {
      (void)oversampler.process(x, identity);
    }
    oversampler.prepare(factor);
    expected = fresh;
    for (double x : noise(300, 5)) {
      CHECK(oversampler.process(x, identity) == expected.process(x, identity));
    }
  }

  Oversampler switching;
  for (double x : noise(500, 6)) {
    (void)switching.process(x, identity);
  }
  switching.prepare(OversamplingFactor::four);
  Oversampler four(OversamplingFactor::four);
  for (double x : noise(300, 7)) {
    CHECK(switching.process(x, identity) == four.process(x, identity));
  }
}

template <typename Sample> void check_partition_invariance(OversamplingFactor factor) {
  const std::vector<double> source = noise(2053, 8);
  std::vector<Sample> input(source.size());
  for (std::size_t i = 0; i < source.size(); ++i) {
    input[i] = static_cast<Sample>(source[i]);
  }
  const auto shaper = [](double x) noexcept { return std::tanh(3.0 * x); };
  Oversampler per_sample(factor);
  std::vector<Sample> expected(input.size());
  for (std::size_t i = 0; i < input.size(); ++i) {
    expected[i] = per_sample.process(input[i], shaper);
  }
  for (std::size_t block : {1ul, 2ul, 5ul, 64ul, 333ul, 2053ul}) {
    Oversampler in_place_os(factor);
    Oversampler separate_os(factor);
    std::vector<Sample> in_place = input;
    std::vector<Sample> separate(input.size());
    for (std::size_t start = 0; start < input.size(); start += block) {
      const std::size_t count = std::min(block, input.size() - start);
      in_place_os.process(std::span<Sample>(in_place.data() + start, count), shaper);
      separate_os.process(std::span<const Sample>(input.data() + start, count),
                          std::span<Sample>(separate.data() + start, count), shaper);
    }
    CHECK(in_place == expected);
    CHECK(separate == expected);
  }
}

void test_block_partition_invariance() {
  for (OversamplingFactor factor : factors) {
    check_partition_invariance<float>(factor);
    check_partition_invariance<double>(factor);
  }
}

void test_overshoot_respects_bound() {
  for (OversamplingFactor factor : factors) {
    Oversampler oversampler(factor);
    double square_peak = 0.0;
    auto track = [&](double x) noexcept {
      square_peak = std::max(square_peak, std::abs(x));
      return x;
    };
    for (std::size_t n = 0; n < 4096; ++n) {
      (void)oversampler.process((n / 24) % 2 == 0 ? 1.0 : -1.0, track);
    }
    CHECK(square_peak > 1.0);
    CHECK_BELOW(square_peak, 1.3);

    oversampler.reset();
    double noise_peak = 0.0;
    auto track_noise = [&](double x) noexcept {
      noise_peak = std::max(noise_peak, std::abs(x));
      return x;
    };
    for (double x : noise(1 << 15, 9)) {
      (void)oversampler.process(x, track_noise);
    }
    CHECK_BELOW(noise_peak, Oversampler::callback_input_bound(factor));
  }
}

void test_processing_does_not_allocate() {
  Oversampler oversampler(OversamplingFactor::four);
  std::vector<float> buffer(1024, 0.25f);
  std::vector<double> input = noise(1024, 10);
  std::vector<double> output(1024);
  const auto shaper = [](double x) noexcept { return std::tanh(x); };
  allocation_count.store(0);
  counting_allocations.store(true);
  oversampler.process(std::span<float>(buffer), shaper);
  oversampler.process(std::span<const double>(input), std::span<double>(output), shaper);
  oversampler.prepare(OversamplingFactor::two);
  (void)oversampler.process(0.5f, shaper);
  oversampler.reset();
  counting_allocations.store(false);
  CHECK(allocation_count.load() == 0);
}

// The oversampler's stages are rate-independent fixed FIRs, but a filters.hpp primitive inside
// the callback runs at ratio() times the base rate and must respect maximum_sample_rate: the
// boundary base rates are 192 kHz at 4x and 384 kHz at 2x, both reaching 768 kHz exactly, and
// anything above that must be rejected by the filter factory rather than clamped.
void test_nested_filter_rate_limits() {
  CHECK(!throws_invalid([] { (void)BiquadCoefficients::lowpass(4.0 * 192000.0, 2000.0, 0.7); }));
  CHECK(!throws_invalid([] { (void)BiquadCoefficients::lowpass(2.0 * 384000.0, 2000.0, 0.7); }));
  CHECK(throws_invalid([] { (void)BiquadCoefficients::lowpass(4.0 * 384000.0, 2000.0, 0.7); }));
  CHECK(throws_invalid([] { (void)BiquadCoefficients::lowpass(2.0 * 768000.0, 2000.0, 0.7); }));
  CHECK(
      throws_invalid([] { (void)BiquadCoefficients::lowpass(4.0 * 192000.0 + 1.0, 2000.0, 0.7); }));

  Oversampler oversampler(OversamplingFactor::four);
  const BiquadCoefficients coefficients = BiquadCoefficients::lowpass(4.0 * 192000.0, 2000.0, 0.7);
  Biquad filter(coefficients);
  double sum = 0.0;
  for (double x : noise(2048, 21)) {
    sum += oversampler.process(x, [&](double up) noexcept { return filter.process(up); });
  }
  CHECK(std::isfinite(sum) && std::abs(sum) < 1e4);
  oversampler.reset();
  filter.reset();
}

}

int main() {
  const std::pair<const char *, void (*)()> tests[] = {
      {"factor_selection_and_latency", test_factor_selection_and_latency},
      {"invalid_prepare_leaves_state", test_invalid_prepare_leaves_state},
      {"dc_is_unity", test_dc_is_unity},
      {"impulse_is_linear_phase_at_latency", test_impulse_is_linear_phase_at_latency},
      {"passband_is_flat_and_latency_aligned", test_passband_is_flat_and_latency_aligned},
      {"interpolation_images_are_rejected", test_interpolation_images_are_rejected},
      {"documented_fir_response", test_documented_fir_response},
      {"decimation_aliases_are_rejected", test_decimation_aliases_are_rejected},
      {"tanh_aliasing_is_reduced", test_tanh_aliasing_is_reduced},
      {"callback_order_and_count", test_callback_order_and_count},
      {"reset_and_prepare_clear_history", test_reset_and_prepare_clear_history},
      {"block_partition_invariance", test_block_partition_invariance},
      {"overshoot_respects_bound", test_overshoot_respects_bound},
      {"processing_does_not_allocate", test_processing_does_not_allocate},
      {"nested_filter_rate_limits", test_nested_filter_rate_limits},
  };
  for (const auto &[name, test] : tests) {
    current_test = name;
    test();
  }
  if (failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  std::puts("all DSP oversampling tests passed");
  return 0;
}
