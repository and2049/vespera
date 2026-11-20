#include <vespera/dsp/common.hpp>
#include <vespera/dsp/filters.hpp>

#include "support.hpp"

#include <array>
#include <atomic>
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
using vespera::dsp::Biquad;
using vespera::dsp::BiquadCascade;
using vespera::dsp::BiquadCoefficients;
using vespera::dsp::nyquist_safe_frequency;

constexpr double butterworth_q = 0.7071067811865476;

std::array<BiquadCoefficients, 4> pre_coefficients(double rate) {
  const auto frequency = [rate](double hz) { return nyquist_safe_frequency(rate, hz); };
  return {
      BiquadCoefficients::highpass(rate, frequency(25.0), butterworth_q),
      BiquadCoefficients::low_shelf(rate, frequency(160.0), butterworth_q, -2.0),
      BiquadCoefficients::peak(rate, frequency(550.0), 0.65, -3.5),
      BiquadCoefficients::high_shelf(rate, frequency(2800.0), butterworth_q, 2.0),
  };
}

std::array<BiquadCoefficients, 9> tone_coefficients(double rate, double control) {
  const auto frequency = [rate](double hz) { return nyquist_safe_frequency(rate, hz); };
  const double rounded = 1.0 - control;
  return {
      BiquadCoefficients::low_shelf(rate, frequency(110.0), butterworth_q, (control - 0.5) * 16.0),
      BiquadCoefficients::peak(rate, frequency(650.0), 0.7, (control - 0.5) * 16.0),
      BiquadCoefficients::high_shelf(rate, frequency(3200.0), butterworth_q,
                                     (control - 0.5) * 12.0),
      BiquadCoefficients::high_shelf(rate, frequency(8500.0), butterworth_q, control * 4.0),
      BiquadCoefficients::low_shelf(rate, frequency(80.0), butterworth_q, -3.0 * rounded),
      BiquadCoefficients::peak(rate, frequency(130.0), 0.8, 1.5 * rounded),
      BiquadCoefficients::peak(rate, frequency(2800.0), 0.7, 1.5 * rounded),
      BiquadCoefficients::high_shelf(rate, frequency(4800.0), butterworth_q, -12.0 * rounded),
      BiquadCoefficients::high_shelf(rate, frequency(6500.0), butterworth_q, -12.0 * rounded),
  };
}

// Reference recurrence: a hand-walked array of biquads carrying a double sample from stage to
// stage, with stages 2 to 10 of the second array redesigned every 32 samples.
std::vector<double> reference_chain(double rate, const std::vector<double> &input) {
  std::array<Biquad, 4> pre;
  std::array<Biquad, 11> post;
  const auto first = pre_coefficients(rate);
  for (std::size_t i = 0; i < pre.size(); ++i) {
    pre[i].set_coefficients(first[i]);
  }
  post[0].set_coefficients(BiquadCoefficients::low_shelf(rate, 180.0, butterworth_q, 3.0));
  post[1].set_coefficients(BiquadCoefficients::highpass(rate, 40.0, butterworth_q));
  std::vector<double> output;
  for (std::size_t n = 0; n < input.size(); ++n) {
    if (n % 32 == 0) {
      const auto tone = tone_coefficients(rate, static_cast<double>(n % 4096) / 4096.0);
      for (std::size_t i = 0; i < tone.size(); ++i) {
        post[i + 2].set_coefficients(tone[i]);
      }
    }
    if (n == input.size() / 2) {
      for (auto &filter : pre) {
        filter.reset();
      }
      for (auto &filter : post) {
        filter.reset();
      }
    }
    double sample = input[n];
    for (auto &filter : pre) {
      sample = filter.process(sample);
    }
    for (auto &filter : post) {
      sample = filter.process(sample);
    }
    output.push_back(sample);
  }
  return output;
}

std::vector<double> cascade_chain(double rate, const std::vector<double> &input) {
  BiquadCascade<4> pre(pre_coefficients(rate));
  BiquadCascade<11> post;
  post.set_coefficients(0, BiquadCoefficients::low_shelf(rate, 180.0, butterworth_q, 3.0));
  post.set_coefficients(1, BiquadCoefficients::highpass(rate, 40.0, butterworth_q));
  std::vector<double> output;
  for (std::size_t n = 0; n < input.size(); ++n) {
    if (n % 32 == 0) {
      const auto tone = tone_coefficients(rate, static_cast<double>(n % 4096) / 4096.0);
      for (std::size_t i = 0; i < tone.size(); ++i) {
        post.set_coefficients(i + 2, tone[i]);
      }
    }
    if (n == input.size() / 2) {
      pre.reset();
      post.reset();
    }
    output.push_back(post.process(pre.process(input[n])));
  }
  return output;
}

void test_matches_reference_chain() {
  for (double rate : rates) {
    for (const auto &[name, signal] : signals(rate, static_cast<std::size_t>(rate / 5.0))) {
      CHECK(count_mismatches(cascade_chain(rate, signal), reference_chain(rate, signal),
                             name + " at " + std::to_string(rate)) == 0);
    }
  }
}

void test_defaults_and_accessors() {
  static_assert(BiquadCascade<3>::size() == 3);
  BiquadCascade<3> cascade;
  for (double x : noise(256, 3u, 4.0)) {
    CHECK(same_bits(cascade.process(x), x));
  }
  const auto peak = BiquadCoefficients::peak(48000.0, 1000.0, 1.0, 6.0);
  cascade.set_coefficients(1, peak);
  CHECK(cascade.coefficients(1) == peak);
  CHECK(cascade.coefficients(0) == BiquadCoefficients());
  CHECK(cascade.coefficients(2) == BiquadCoefficients());
  const std::array<BiquadCoefficients, 3> all{peak, peak, peak};
  cascade.set_coefficients(all);
  CHECK(cascade.coefficients(0) == peak && cascade.coefficients(2) == peak);
}

void test_reset_and_coefficient_changes_keep_or_clear_history() {
  const auto lowpass = BiquadCoefficients::lowpass(48000.0, 500.0, 0.7);
  BiquadCascade<2> cascade({lowpass, lowpass});
  for (double x : noise(512, 5u)) {
    (void)cascade.process(x);
  }
  BiquadCascade<2> kept = cascade;
  kept.set_coefficients(0, BiquadCoefficients::lowpass(48000.0, 600.0, 0.7));
  CHECK(kept.process(0.0) != 0.0);
  cascade.reset();
  CHECK(same_bits(cascade.process(0.0), 0.0));
  BiquadCascade<2> fresh({lowpass, lowpass});
  const auto signal = noise(512, 6u);
  std::size_t mismatches = 0;
  for (double x : signal) {
    mismatches += same_bits(cascade.process(x), fresh.process(x)) ? 0 : 1;
  }
  CHECK(mismatches == 0);
}

void test_float_samples_round_once() {
  const auto peak = BiquadCoefficients::peak(48000.0, 2000.0, 2.0, 9.0);
  BiquadCascade<3> cascade({peak, peak, peak});
  BiquadCascade<3> reference({peak, peak, peak});
  std::size_t mismatches = 0;
  for (double x : noise(2048, 8u)) {
    const float input = static_cast<float>(x);
    mismatches += same_bits(cascade.process(input),
                            static_cast<float>(reference.process(static_cast<double>(input))))
                      ? 0
                      : 1;
  }
  CHECK(mismatches == 0);
}

void test_block_partitions_match_per_sample() {
  const auto coefficients = pre_coefficients(44100.0);
  const auto signal = noise(1000, 9u, 2.0);
  BiquadCascade<4> sample_wise(coefficients);
  std::vector<double> expected;
  for (double x : signal) {
    expected.push_back(sample_wise.process(x));
  }
  BiquadCascade<4> in_place(coefficients);
  std::vector<double> block = signal;
  std::span<double> all(block);
  in_place.process(all.first(1));
  in_place.process(all.subspan(1, 499));
  in_place.process(all.subspan(500));
  CHECK(count_mismatches(block, expected, "in place") == 0);
  BiquadCascade<4> out_of_place(coefficients);
  std::vector<double> output(signal.size());
  out_of_place.process(std::span<const double>(signal), std::span<double>(output));
  CHECK(count_mismatches(output, expected, "out of place") == 0);
}

void test_processing_does_not_allocate() {
  BiquadCascade<8> cascade;
  const auto peak = BiquadCoefficients::peak(48000.0, 1000.0, 1.0, 3.0);
  std::array<float, 64> block{};
  allocation_count.store(0);
  counting_allocations.store(true);
  for (int i = 0; i < 1000; ++i) {
    cascade.set_coefficients(static_cast<std::size_t>(i % 8), peak);
    block[0] = cascade.process(0.25f);
    cascade.process(std::span<float>(block));
    if (i % 100 == 0) {
      cascade.reset();
    }
  }
  counting_allocations.store(false);
  CHECK(allocation_count.load() == 0);
}

}

int main() {
  run("matches_reference_chain", test_matches_reference_chain);
  run("defaults_and_accessors", test_defaults_and_accessors);
  run("reset_and_coefficient_changes_keep_or_clear_history",
      test_reset_and_coefficient_changes_keep_or_clear_history);
  run("float_samples_round_once", test_float_samples_round_once);
  run("block_partitions_match_per_sample", test_block_partitions_match_per_sample);
  run("processing_does_not_allocate", test_processing_does_not_allocate);
  return finish("cascade");
}
