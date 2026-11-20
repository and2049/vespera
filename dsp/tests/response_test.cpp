#include "support.hpp"

#include <vespera/dsp/fft.hpp>
#include <vespera/dsp/response.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

namespace {

using namespace test_support;
using vespera::dsp::ResponseNormalisation;

using Signal = std::vector<float>;

const std::pair<double, double> rate_pairs[] = {
    {44100.0, 48000.0}, {48000.0, 44100.0}, {44100.0, 96000.0}, {96000.0, 44100.0},
    {48000.0, 96000.0}, {96000.0, 48000.0}, {88200.0, 48000.0}, {8000.0, 48000.0},
    {48000.0, 47999.5}, {192000.0, 44100.0}};

std::complex<double> dtft(const Signal &signal, double frequency, double rate, double delay) {
  std::complex<double> sum = 0.0;
  for (std::size_t n = 0; n < signal.size(); ++n) {
    const double time = (static_cast<double>(n) - delay) / rate;
    sum += static_cast<double>(signal[n]) *
           std::polar(1.0, -2.0 * std::numbers::pi * frequency * time);
  }
  return sum;
}

double energy(const Signal &signal) {
  double sum = 0.0;
  for (float sample : signal) {
    sum += static_cast<double>(sample) * static_cast<double>(sample);
  }
  return sum;
}

Signal windowed_tone(std::size_t count, double frequency, double rate) {
  Signal result(count);
  for (std::size_t n = 0; n < count; ++n) {
    const double window = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(n) /
                                               static_cast<double>(count - 1));
    result[n] = static_cast<float>(
        window * std::cos(2.0 * std::numbers::pi * frequency * static_cast<double>(n) / rate));
  }
  return result;
}

void test_resampler_rejects_invalid_input() {
  const Signal response{1.0f, 0.5f};
  CHECK(throws_invalid([&] { (void)vespera::dsp::resample_response(response, 7999.0, 48000.0); }));
  CHECK(throws_invalid([&] { (void)vespera::dsp::resample_response(response, 48000.0, qnan); }));
  const Signal bad{1.0f, std::numeric_limits<float>::quiet_NaN()};
  CHECK(throws_invalid([&] { (void)vespera::dsp::resample_response(bad, 44100.0, 48000.0); }));
  const auto same = vespera::dsp::resample_response(response, 48000.0, 48000.0);
  CHECK(same.samples == response && same.delay_frames == 0);
  CHECK(vespera::dsp::resample_response({}, 44100.0, 48000.0).samples.empty());
}

// The resampled response, with its reported delay removed, has the original frequency response
// within +/-0.001 dB in magnitude and the equivalent phase error, up to the passband edge.
void test_resampler_passband_and_delay() {
  const Signal smooth{1.0f, 0.5f, 0.25f};
  const std::vector<double> random = noise(48, 17u);
  const Signal dense(random.begin(), random.end());
  const double allowed = std::pow(10.0, 0.001 / 20.0) - 1.0;
  for (const auto &[source, target] : rate_pairs) {
    const double edge = vespera::dsp::resampler_passband_ratio * std::min(source, target);
    const auto smooth_out = vespera::dsp::resample_response(smooth, source, target);
    const auto dense_out = vespera::dsp::resample_response(dense, source, target);
    const double delay = static_cast<double>(smooth_out.delay_frames);
    CHECK(smooth_out.delay_frames ==
          static_cast<std::size_t>(std::ceil(48.0 * target / std::min(source, target) - 1e-9)));
    double worst_ratio = 0.0;
    double worst_dense = 0.0;
    double dense_scale = 0.0;
    for (float sample : dense) {
      dense_scale += std::abs(static_cast<double>(sample));
    }
    for (int k = 0; k <= 60; ++k) {
      const double frequency = edge * static_cast<double>(k) / 60.0;
      const auto expected = dtft(smooth, frequency, source, 0.0);
      const auto actual = dtft(smooth_out.samples, frequency, target, delay);
      worst_ratio = std::max(worst_ratio, std::abs(actual / expected - 1.0));
      const auto dense_expected = dtft(dense, frequency, source, 0.0);
      const auto dense_actual =
          dtft(dense_out.samples, frequency, target, static_cast<double>(dense_out.delay_frames));
      worst_dense = std::max(worst_dense, std::abs(dense_actual - dense_expected));
    }
    if (!(worst_ratio <= allowed && worst_dense <= allowed * dense_scale)) {
      std::fprintf(stderr, "%s: %g -> %g Hz: passband error %.3g (dense %.3g)\n", current_test,
                   source, target, worst_ratio, worst_dense / dense_scale);
      ++failures;
    }
    double sum_in = 0.0;
    double sum_out = 0.0;
    for (float sample : dense) {
      sum_in += sample;
    }
    for (float sample : dense_out.samples) {
      sum_out += sample;
    }
    CHECK_NEAR(sum_out, sum_in, 1e-5 * dense_scale);
  }
  const Signal impulse{1.0f};
  const auto doubled = vespera::dsp::resample_response(impulse, 48000.0, 96000.0);
  CHECK(doubled.delay_frames == 96);
  CHECK_NEAR(doubled.samples[96], 0.5, 1e-7);
  CHECK(std::max_element(doubled.samples.begin(), doubled.samples.end()) ==
        doubled.samples.begin() + 96);
  CHECK(vespera::dsp::resample_response(impulse, 44100.0, 96000.0).delay_frames == 105);
  CHECK(vespera::dsp::resample_response(impulse, 96000.0, 48000.0).delay_frames == 48);
}

// Content above the stopband edge, aliases when decimating and images when interpolating, is
// attenuated by at least the documented 110 dB.
void test_resampler_stopband() {
  const double limit = std::pow(10.0, -vespera::dsp::resampler_stopband_db / 10.0);
  for (const auto &[source, target] : rate_pairs) {
    const double low = std::min(source, target);
    const double scale = source / target;
    if (target < source) {
      for (double ratio : {0.55, 0.6, 0.75, 0.9}) {
        const double frequency = ratio * low;
        if (frequency >= 0.49 * source) {
          continue;
        }
        const Signal tone =
            windowed_tone(static_cast<std::size_t>(0.1 * source), frequency, source);
        const auto out = vespera::dsp::resample_response(tone, source, target);
        const double attenuation = energy(out.samples) / (scale * energy(tone));
        if (!(attenuation <= limit)) {
          std::fprintf(stderr, "%s: %g -> %g Hz alias at %g Hz: %.1f dB\n", current_test, source,
                       target, frequency, 10.0 * std::log10(attenuation));
          ++failures;
        }
      }
    }
    if (target > source) {
      const Signal tone = windowed_tone(4000, 0.4 * source, source);
      const auto out = vespera::dsp::resample_response(tone, source, target);
      const std::size_t size = std::bit_ceil(out.samples.size());
      vespera::dsp::RealFft fft(size);
      Signal padded(size, 0.0f);
      std::copy(out.samples.begin(), out.samples.end(), padded.begin());
      Signal re(fft.bins());
      Signal im(fft.bins());
      fft.forward(padded, re, im);
      double total = 0.0;
      double images = 0.0;
      for (std::size_t k = 0; k < fft.bins(); ++k) {
        const double power =
            static_cast<double>(re[k]) * re[k] + static_cast<double>(im[k]) * im[k];
        total += power;
        const double frequency = static_cast<double>(k) * target / static_cast<double>(size);
        if (frequency >= vespera::dsp::resampler_stopband_ratio * low) {
          images += power;
        }
      }
      if (!(images / total <= limit)) {
        std::fprintf(stderr, "%s: %g -> %g Hz images: %.1f dB\n", current_test, source, target,
                     10.0 * std::log10(images / total));
        ++failures;
      }
      CHECK_NEAR(energy(out.samples) / (scale * energy(tone)), 1.0, 1e-4);
    }
  }
}

void test_trim_range() {
  const Signal response{0.0f, 1e-7f, 0.0f, 0.5f, -1.0f, 0.2f, 1e-3f, 5e-5f, 1e-6f, 0.0f};
  auto range = vespera::dsp::trim_range(response);
  CHECK(range.offset == 3 && range.frames == 4);
  range = vespera::dsp::trim_range(response, -150.0, -100.0);
  CHECK(range.offset == 1 && range.frames == 7);
  range = vespera::dsp::trim_range(response, -40.0, -50.0);
  CHECK(range.offset == 3 && range.frames == 3);
  range = vespera::dsp::trim_range(response, 0.0, 0.0);
  CHECK(range.offset == 4 && range.frames == 1);
  const Signal left{0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.5f};
  const Signal right{0.0f, 0.0f, 0.25f, 0.5f, 0.0f, 0.0f, 0.0f, 0.1f, 0.0f};
  const std::span<const float> pair[] = {left, right};
  range = vespera::dsp::trim_range(pair, -20.0, -30.0);
  CHECK(range.offset == 2 && range.frames == 6);
  const Signal silent(10, 0.0f);
  range = vespera::dsp::trim_range(silent);
  CHECK(range.offset == 0 && range.frames == 0);
  CHECK(throws_invalid([&] { (void)vespera::dsp::trim_range(response, 1.0, -60.0); }));
  CHECK(throws_invalid([&] { (void)vespera::dsp::trim_range(response, -60.0, qnan); }));
  const Signal bad{1.0f, std::numeric_limits<float>::infinity()};
  CHECK(throws_invalid([&] { (void)vespera::dsp::trim_range(bad); }));
}

void test_normalisation() {
  const Signal three_four{3.0f, 4.0f};
  CHECK_NEAR(vespera::dsp::normalisation_gain(three_four, ResponseNormalisation::energy), 0.2,
             1e-15);
  const Signal one{1.0f};
  const Signal zero{0.0f};
  const std::span<const float> pair[] = {one, zero};
  CHECK_NEAR(vespera::dsp::normalisation_gain(pair, ResponseNormalisation::energy),
             std::numbers::sqrt2, 1e-15);
  const Signal sum{1.0f, 1.0f};
  const Signal difference{1.0f, -1.0f};
  const Signal delayed{0.0f, 0.0f, 0.0f, 0.5f};
  CHECK_NEAR(vespera::dsp::normalisation_gain(sum, ResponseNormalisation::peak_gain), 0.5, 1e-7);
  CHECK_NEAR(vespera::dsp::normalisation_gain(difference, ResponseNormalisation::peak_gain), 0.5,
             1e-7);
  CHECK_NEAR(vespera::dsp::normalisation_gain(delayed, ResponseNormalisation::peak_gain), 2.0,
             1e-6);
  const std::span<const float> both[] = {sum, delayed};
  CHECK_NEAR(vespera::dsp::normalisation_gain(both, ResponseNormalisation::peak_gain), 0.5, 1e-7);

  // A decaying resonance: the peak gain is the largest magnitude on the documented DFT grid.
  Signal resonance(512);
  const double frequency = 37.0 / 2048.0;
  for (std::size_t n = 0; n < resonance.size(); ++n) {
    resonance[n] =
        static_cast<float>(std::pow(0.98, static_cast<double>(n)) *
                           std::cos(2.0 * std::numbers::pi * frequency * static_cast<double>(n)));
  }
  double expected = 0.0;
  for (int k = 20; k < 60; ++k) {
    expected = std::max(expected, std::abs(dtft(resonance, k / 2048.0, 1.0, 0.0)));
  }
  CHECK_NEAR(vespera::dsp::normalisation_gain(resonance, ResponseNormalisation::peak_gain),
             1.0 / expected, 1e-5 / expected);

  CHECK(throws_invalid(
      [&] { (void)vespera::dsp::normalisation_gain(zero, ResponseNormalisation::energy); }));
  CHECK(throws_invalid(
      [&] { (void)vespera::dsp::normalisation_gain(zero, ResponseNormalisation::peak_gain); }));
  const Signal bad{std::numeric_limits<float>::quiet_NaN()};
  CHECK(throws_invalid(
      [&] { (void)vespera::dsp::normalisation_gain(bad, ResponseNormalisation::energy); }));
}

void test_blend() {
  const Signal first{1.0f, 2.0f, 3.0f};
  const Signal second{10.0f};
  const Signal blended = vespera::dsp::blend_responses(first, second, 0.25);
  CHECK(blended.size() == 3);
  CHECK_NEAR(blended[0], 3.25, 1e-6);
  CHECK_NEAR(blended[1], 1.5, 1e-6);
  CHECK_NEAR(blended[2], 2.25, 1e-6);
  CHECK(vespera::dsp::blend_responses(first, second, 0.0) == first);
  CHECK((vespera::dsp::blend_responses(first, second, 1.0) == Signal{10.0f, 0.0f, 0.0f}));
  CHECK(throws_invalid([&] { (void)vespera::dsp::blend_responses(first, second, -0.1); }));
  CHECK(throws_invalid([&] { (void)vespera::dsp::blend_responses(first, second, 1.1); }));
  CHECK(throws_invalid([&] { (void)vespera::dsp::blend_responses(first, second, qnan); }));
}

}

int main() {
  run("resampler rejects invalid input", test_resampler_rejects_invalid_input);
  run("resampler passband and delay", test_resampler_passband_and_delay);
  run("resampler stopband", test_resampler_stopband);
  run("trim range", test_trim_range);
  run("normalisation", test_normalisation);
  run("blend", test_blend);
  return finish("response");
}
