#include "support.hpp"

#include <vespera/dsp/fft.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>
#include <vector>

namespace {

using namespace test_support;
using vespera::dsp::RealFft;
using vespera::dsp::detail::FftPlan;

constexpr double epsilon = std::numeric_limits<float>::epsilon();

std::vector<float> random_floats(std::size_t count, std::uint32_t seed) {
  const std::vector<double> values = noise(count, seed);
  return {values.begin(), values.end()};
}

double norm(const std::vector<float> &values) {
  double sum = 0.0;
  for (float value : values) {
    sum += static_cast<double>(value) * static_cast<double>(value);
  }
  return std::sqrt(sum);
}

// Bound on the 2-norm error of a single-precision radix-2 transform (Higham, "Accuracy and
// Stability of Numerical Algorithms", 24.1): log2(M) * eta relative error with
// eta = mu + gamma4 * (sqrt(2) + mu), about 6.7 epsilon for correctly rounded twiddles, plus a
// few roundings each for the real-input split and the bit-reversed gather. 8 epsilon per stage
// and two extra stages cover both.
double relative_bound(std::size_t size) {
  return 8.0 * epsilon * (std::log2(static_cast<double>(size)) + 2.0);
}

std::vector<std::complex<double>> reference_dft(const std::vector<float> &input) {
  const std::size_t size = input.size();
  std::vector<std::complex<double>> result(size / 2 + 1);
  for (std::size_t k = 0; k <= size / 2; ++k) {
    std::complex<double> sum = 0.0;
    for (std::size_t n = 0; n < size; ++n) {
      const double angle =
          -2.0 * std::numbers::pi * static_cast<double>((k * n) % size) / static_cast<double>(size);
      sum += static_cast<double>(input[n]) * std::polar(1.0, angle);
    }
    result[k] = sum;
  }
  return result;
}

void test_rejects_invalid_sizes() {
  for (std::size_t size :
       {std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{6},
        std::size_t{12}, std::size_t{1} << 22, (std::size_t{1} << 21) + 2}) {
    CHECK(throws_invalid([&] { RealFft fft(size); }));
  }
}

void test_known_transforms() {
  for (std::size_t size = 4; size <= 1024; size *= 2) {
    RealFft fft(size);
    const std::size_t bins = size / 2 + 1;
    std::vector<float> re(bins);
    std::vector<float> im(bins);
    std::vector<float> input(size, 0.0f);
    input[0] = 1.0f;
    fft.forward(input, re, im);
    for (std::size_t k = 0; k < bins; ++k) {
      CHECK_NEAR(re[k], 1.0, 1e-6);
      CHECK_NEAR(im[k], 0.0, 1e-6);
    }
    std::fill(input.begin(), input.end(), 1.0f);
    fft.forward(input, re, im);
    CHECK(re[0] == static_cast<float>(size));
    for (std::size_t k = 1; k < bins; ++k) {
      CHECK_NEAR(std::hypot(re[k], im[k]), 0.0, 1e-6 * static_cast<double>(size));
    }
    CHECK(im[0] == 0.0f && im[bins - 1] == 0.0f);
    const std::size_t bin = size / 4 + (size > 4 ? 1 : 0);
    for (std::size_t n = 0; n < size; ++n) {
      const double phase =
          2.0 * std::numbers::pi * static_cast<double>(bin * n) / static_cast<double>(size);
      input[n] = static_cast<float>(std::cos(phase) + 0.5 * std::sin(phase));
    }
    fft.forward(input, re, im);
    const double tolerance = relative_bound(size) * static_cast<double>(size) * 4.0;
    for (std::size_t k = 0; k < bins; ++k) {
      const double expected_re = k == bin ? static_cast<double>(size) / 2.0 : 0.0;
      const double expected_im = k == bin ? -0.25 * static_cast<double>(size) : 0.0;
      CHECK_NEAR(re[k], expected_re, tolerance);
      CHECK_NEAR(im[k], expected_im, tolerance);
    }
    std::vector<float> alternating(size);
    for (std::size_t n = 0; n < size; ++n) {
      alternating[n] = n % 2 == 0 ? 1.0f : -1.0f;
    }
    fft.forward(alternating, re, im);
    CHECK(re[bins - 1] == static_cast<float>(size));
    CHECK_NEAR(re[0], 0.0, 1e-6);
  }
}

void test_matches_reference_dft() {
  for (std::size_t size = 4; size <= 4096; size *= 2) {
    RealFft fft(size);
    const std::vector<float> input = random_floats(size, static_cast<std::uint32_t>(size));
    std::vector<float> re(fft.bins());
    std::vector<float> im(fft.bins());
    fft.forward(input, re, im);
    const auto expected = reference_dft(input);
    double error = 0.0;
    double magnitude = 0.0;
    for (std::size_t k = 0; k < fft.bins(); ++k) {
      const double weight = k == 0 || k == size / 2 ? 1.0 : 2.0;
      error += weight * std::norm(std::complex<double>(re[k], im[k]) - expected[k]);
      magnitude += weight * std::norm(expected[k]);
    }
    CHECK(std::sqrt(error / magnitude) <= relative_bound(size));
  }
}

void test_all_sizes_round_trip_and_parseval() {
  for (std::size_t size = vespera::dsp::minimum_fft_size; size <= vespera::dsp::maximum_fft_size;
       size *= 2) {
    RealFft fft(size);
    CHECK(fft.size() == size && fft.bins() == size / 2 + 1);
    const std::vector<float> input = random_floats(size, 7u + static_cast<std::uint32_t>(size));
    std::vector<float> re(fft.bins());
    std::vector<float> im(fft.bins());
    std::vector<float> output(size);
    fft.forward(input, re, im);
    double energy = 0.0;
    for (std::size_t k = 0; k < fft.bins(); ++k) {
      const double weight = k == 0 || k == size / 2 ? 1.0 : 2.0;
      energy += weight * (static_cast<double>(re[k]) * re[k] + static_cast<double>(im[k]) * im[k]);
    }
    const double input_norm = norm(input);
    const double spectral_norm = std::sqrt(energy / static_cast<double>(size));
    CHECK(std::abs(spectral_norm - input_norm) <= relative_bound(size) * input_norm);
    fft.inverse(re, im, output);
    double error = 0.0;
    for (std::size_t n = 0; n < size; ++n) {
      const double difference =
          static_cast<double>(output[n]) / static_cast<double>(size) - input[n];
      error += difference * difference;
    }
    CHECK(std::sqrt(error) <= 2.0 * relative_bound(size) * input_norm);
  }
}

void test_inverse_ignores_edge_imaginary_parts_and_allows_overlap() {
  RealFft fft(64);
  const std::vector<float> input = random_floats(64, 3u);
  std::vector<float> re(fft.bins());
  std::vector<float> im(fft.bins());
  fft.forward(input, re, im);
  std::vector<float> clean(64);
  fft.inverse(re, im, clean);
  im[0] = 5.0f;
  im[fft.bins() - 1] = -3.0f;
  std::vector<float> buffer(64, 0.0f);
  std::copy(re.begin(), re.end(), buffer.begin());
  fft.inverse(std::span<const float>(buffer.data(), fft.bins()), im, buffer);
  CHECK(count_mismatches(buffer, clean, "overlapping inverse") == 0);
}

// The convolver splits transforms into arbitrary element ranges; every split must give the same
// bits as a single call.
void test_split_passes_are_bit_identical() {
  for (std::size_t size :
       {std::size_t{4}, std::size_t{8}, std::size_t{32}, std::size_t{256}, std::size_t{2048}}) {
    FftPlan plan(size);
    RealFft fft(size);
    const std::vector<float> input = random_floats(size, 11u);
    std::vector<float> whole_re(plan.bins());
    std::vector<float> whole_im(plan.bins());
    fft.forward(input, whole_re, whole_im);
    std::vector<float> whole_out(size);
    fft.inverse(whole_re, whole_im, whole_out);

    std::uint32_t state = 5;
    const auto chunked = [&](std::size_t elements, auto &&run) {
      std::size_t begin = 0;
      while (begin < elements) {
        state = state * 1664525u + 1013904223u;
        const std::size_t step = 1 + (state >> 16) % 7;
        const std::size_t end = std::min(elements, begin + step);
        run(begin, end);
        begin = end;
      }
    };
    std::vector<float> re(plan.half());
    std::vector<float> im(plan.half());
    std::vector<float> split_re(plan.bins());
    std::vector<float> split_im(plan.bins());
    chunked(plan.half(), [&](std::size_t b, std::size_t e) {
      plan.gather(input.data(), re.data(), im.data(), b, e);
    });
    for (std::size_t pass = 0; pass < plan.pass_count(); ++pass) {
      chunked(plan.pass_elements(pass),
              [&](std::size_t b, std::size_t e) { plan.pass(pass, re.data(), im.data(), b, e); });
    }
    chunked(plan.half() / 2 + 1, [&](std::size_t b, std::size_t e) {
      plan.split(re.data(), im.data(), split_re.data(), split_im.data(), b, e);
    });
    CHECK(count_mismatches(split_re, whole_re, "split real") == 0);
    CHECK(count_mismatches(split_im, whole_im, "split imaginary") == 0);
    std::vector<float> out(size);
    chunked(plan.half() / 2 + 1, [&](std::size_t b, std::size_t e) {
      plan.merge(split_re.data(), split_im.data(), re.data(), im.data(), b, e);
    });
    for (std::size_t pass = 0; pass < plan.pass_count(); ++pass) {
      chunked(plan.pass_elements(pass),
              [&](std::size_t b, std::size_t e) { plan.pass(pass, re.data(), im.data(), b, e); });
    }
    chunked(plan.half(), [&](std::size_t b, std::size_t e) {
      plan.scatter(re.data(), im.data(), out.data(), b, e);
    });
    CHECK(count_mismatches(out, whole_out, "split inverse") == 0);
  }
}

}

int main() {
  run("rejects invalid sizes", test_rejects_invalid_sizes);
  run("known transforms", test_known_transforms);
  run("matches reference DFT", test_matches_reference_dft);
  run("all sizes round trip and Parseval", test_all_sizes_round_trip_and_parseval);
  run("inverse edge bins and overlap",
      test_inverse_ignores_edge_imaginary_parts_and_allows_overlap);
  run("split passes bit-identical", test_split_passes_are_bit_identical);
  return finish("FFT");
}
