#include <vespera/dsp/oversampling.hpp>
#include <vespera/dsp/shaping.hpp>

#include "support.hpp"

#include <cmath>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

namespace {

using namespace test_support;
using vespera::dsp::Oversampler;
using vespera::dsp::OversamplingFactor;
using vespera::dsp::soft_saturate;
using vespera::dsp::SoftSaturator;

static_assert(std::is_nothrow_invocable_r_v<double, SoftSaturator &, double>);
static_assert(std::is_nothrow_invocable_r_v<double, const SoftSaturator &, double>);

// Reference recurrence: algebraic saturation applied at the oversampled rate.
double reference_shape(double x, double drive) noexcept { return x / std::hypot(1.0, x * drive); }

constexpr double drives[] = {0.0, 1.0, 1.0 + 3.0 * 0.25 * 0.25, 1.75, 2.5, 4.0, 1.0e-3, 37.0};

void test_matches_reference_pointwise() {
  std::size_t mismatches = 0;
  std::vector<double> inputs = noise(4096, 7u, 24.0);
  for (double x : {0.0, -0.0, 4.9e-324, -1e-310, 1e-300, 1.0, -1.0, 16.0, -16.0, 20.4, 1e150,
                   -1e300, std::numeric_limits<double>::max(), inf, -inf, qnan}) {
    inputs.push_back(x);
  }
  for (double drive : drives) {
    const SoftSaturator saturator(drive);
    for (double x : inputs) {
      const double expected = reference_shape(x, drive);
      mismatches += same_bits(soft_saturate(x, drive), expected) ? 0 : 1;
      mismatches += same_bits(saturator(x), expected) ? 0 : 1;
    }
  }
  CHECK(mismatches == 0);
}

void test_matches_reference_inside_oversampler() {
  for (double rate : rates) {
    for (const auto &[name, signal] : signals(rate, static_cast<std::size_t>(rate / 10.0))) {
      for (double drive : {1.0, 1.75, 4.0}) {
        for (OversamplingFactor factor : {OversamplingFactor::two, OversamplingFactor::four}) {
          Oversampler reference(factor);
          Oversampler shaped(factor);
          Oversampler functor(factor);
          const SoftSaturator saturator(drive);
          std::vector<double> expected;
          std::vector<double> actual;
          std::vector<double> through_functor;
          for (double sample : signal) {
            expected.push_back(reference.process(
                sample, [drive](double x) noexcept { return x / std::hypot(1.0, x * drive); }));
            actual.push_back(shaped.process(
                sample, [drive](double x) noexcept { return soft_saturate(x, drive); }));
            through_functor.push_back(functor.process(sample, saturator));
          }
          const std::string label = name + " at " + std::to_string(rate);
          CHECK(count_mismatches(actual, expected, label) == 0);
          CHECK(count_mismatches(through_functor, expected, label) == 0);
        }
      }
    }
  }
}

void test_bounds_symmetry_and_slope() {
  constexpr double epsilon = std::numeric_limits<double>::epsilon();
  for (double drive : {1.0e-3, 0.5, 1.0, 1.75, 4.0, 37.0}) {
    const double bound = 1.0 / drive;
    double previous = -inf;
    for (int step = -3000; step <= 3000; ++step) {
      const double x = std::sinh(static_cast<double>(step) / 100.0);
      const double y = soft_saturate(x, drive);
      CHECK(std::abs(y) <= bound * (1.0 + 4.0 * epsilon));
      CHECK(same_bits(soft_saturate(-x, drive), -y));
      CHECK(y >= previous - bound * 4.0 * epsilon);
      previous = y;
    }
    CHECK_NEAR(soft_saturate(1e-9, drive) / 1e-9, 1.0, 1e-12);
    CHECK_NEAR(soft_saturate(1e300, drive), bound, bound * 4.0 * epsilon);
    CHECK(same_bits(soft_saturate(1e-300, drive), 1e-300));
    CHECK(same_bits(soft_saturate(0.0, drive), 0.0));
    CHECK(same_bits(soft_saturate(-0.0, drive), -0.0));
    CHECK(same_bits(soft_saturate(0.5, -drive), soft_saturate(0.5, drive)));
  }
  CHECK_NEAR(soft_saturate(1.0, 1.0), 1.0 / std::sqrt(2.0), 1e-16);
}

void test_zero_drive_is_identity() {
  for (double x : noise(2048, 11u, 16.0)) {
    CHECK(same_bits(soft_saturate(x, 0.0), x));
  }
  CHECK(same_bits(soft_saturate(std::numeric_limits<double>::max(), 0.0),
                  std::numeric_limits<double>::max()));
}

void test_non_finite_and_overflowing_input() {
  for (double drive : {0.0, 1.0, 4.0}) {
    CHECK(std::isnan(soft_saturate(qnan, drive)));
    CHECK(std::isnan(soft_saturate(inf, drive)));
    CHECK(std::isnan(soft_saturate(-inf, drive)));
  }
  const double big = std::numeric_limits<double>::max();
  CHECK(same_bits(soft_saturate(big, 4.0), 0.0));
  CHECK(same_bits(soft_saturate(-big, 4.0), -0.0));
}

void test_saturator_validation() {
  SoftSaturator saturator(2.0);
  CHECK(saturator.drive() == 2.0);
  CHECK(SoftSaturator().drive() == 1.0);
  for (double drive : {-0.5, qnan, inf, -inf}) {
    CHECK(throws_invalid([&] { saturator.set_drive(drive); }));
    CHECK(throws_invalid([&] { (void)SoftSaturator(drive); }));
  }
  CHECK(saturator.drive() == 2.0);
  CHECK(!throws_invalid([&] { saturator.set_drive(0.0); }));
  CHECK(saturator(0.75) == 0.75);
}

}

int main() {
  run("matches_reference_pointwise", test_matches_reference_pointwise);
  run("matches_reference_inside_oversampler", test_matches_reference_inside_oversampler);
  run("bounds_symmetry_and_slope", test_bounds_symmetry_and_slope);
  run("zero_drive_is_identity", test_zero_drive_is_identity);
  run("non_finite_and_overflowing_input", test_non_finite_and_overflowing_input);
  run("saturator_validation", test_saturator_validation);
  return finish("shaping");
}
