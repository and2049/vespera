#include <vespera/dsp/dynamics.hpp>

#include "support.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>

namespace {

using namespace test_support;
using vespera::dsp::decibels_to_gain;
using vespera::dsp::EnvelopeFollower;
using vespera::dsp::SoftKneeConfig;
using vespera::dsp::SoftKneeGainComputer;

// Reference recurrence: soft-knee gain with a -24 dB threshold, 6 dB knee, 1.6:1 ratio and at most
// 6 dB of reduction, written with compile-time constants.
double reference_gain(double envelope) noexcept {
  constexpr double threshold_db = -24.0;
  constexpr double knee_db = 6.0;
  constexpr double slope = 1.0 / 1.6 - 1.0;
  const double over = vespera::dsp::gain_to_decibels(envelope) - threshold_db;
  double reduction = 0.0;
  if (over >= knee_db / 2.0) {
    reduction = slope * over;
  } else if (over > -knee_db / 2.0) {
    const double knee = over + knee_db / 2.0;
    reduction = slope * knee * knee / (2.0 * knee_db);
  }
  return vespera::dsp::decibels_to_gain(std::max(reduction, -6.0));
}

constexpr SoftKneeConfig reference_config{-24.0, 6.0, 1.6, 6.0};

void test_matches_reference_on_level_grid() {
  const SoftKneeGainComputer computer(reference_config);
  std::size_t mismatches = 0;
  for (int step = -4000; step <= 2000; ++step) {
    const double level = std::pow(10.0, static_cast<double>(step) / 1000.0);
    for (double envelope : {level, std::nextafter(level, 0.0), std::nextafter(level, inf)}) {
      mismatches += same_bits(computer.gain(envelope), reference_gain(envelope)) ? 0 : 1;
    }
  }
  for (double edge_db : {-27.0, -24.0, -21.0, -14.0, -8.0}) {
    const double level = decibels_to_gain(edge_db);
    for (double envelope = std::nextafter(level, 0.0), i = 0; i < 64.0; ++i) {
      mismatches += same_bits(computer.gain(envelope), reference_gain(envelope)) ? 0 : 1;
      envelope = std::nextafter(envelope, inf);
    }
  }
  constexpr double specials[] = {
      0.0, -0.0, -1.0, 4.9e-324, 1e-300, std::numeric_limits<double>::max(),
      inf, -inf, qnan, 1.0,      16.0};
  for (double envelope : specials) {
    mismatches += same_bits(computer.gain(envelope), reference_gain(envelope)) ? 0 : 1;
  }
  CHECK(mismatches == 0);
}

void test_matches_reference_on_detected_signals() {
  for (double rate : rates) {
    const SoftKneeGainComputer computer(reference_config);
    for (const auto &[name, signal] : signals(rate, static_cast<std::size_t>(rate / 5.0))) {
      EnvelopeFollower detector({rate, 0.012, 0.12});
      std::vector<double> actual;
      std::vector<double> expected;
      for (double sample : signal) {
        const double envelope = detector.process(static_cast<float>(std::abs(sample)));
        actual.push_back(computer.gain(envelope));
        expected.push_back(reference_gain(envelope));
      }
      CHECK(count_mismatches(actual, expected, name + " at " + std::to_string(rate)) == 0);
    }
  }
}

void test_curve_shape() {
  const SoftKneeGainComputer computer({-20.0, 10.0, 4.0, 30.0});
  CHECK(computer.reduction_db(decibels_to_gain(-40.0)) == 0.0);
  CHECK(computer.reduction_db(decibels_to_gain(-25.0)) == 0.0);
  CHECK_NEAR(computer.reduction_db(decibels_to_gain(-20.0)), -0.75 * 25.0 / 20.0, 1e-12);
  CHECK_NEAR(computer.reduction_db(decibels_to_gain(-15.0)), -0.75 * 5.0, 1e-12);
  CHECK_NEAR(computer.reduction_db(decibels_to_gain(0.0)), -15.0, 1e-12);
  CHECK(computer.reduction_db(decibels_to_gain(40.0)) == -30.0);
  double previous = 1.0;
  for (int step = -600; step <= 400; ++step) {
    const double gain = computer.gain(decibels_to_gain(static_cast<double>(step) / 10.0));
    CHECK(gain <= previous);
    CHECK(gain <= 1.0 && gain >= decibels_to_gain(-30.0));
    previous = gain;
  }
  for (double edge_db : {-25.0, -15.0}) {
    const double below = computer.reduction_db(decibels_to_gain(edge_db - 1e-7));
    const double above = computer.reduction_db(decibels_to_gain(edge_db + 1e-7));
    CHECK_NEAR(below, above, 1e-6);
  }
}

void test_hard_knee_and_unity_ratio() {
  const SoftKneeGainComputer hard({-10.0, 0.0, 2.0, 100.0});
  CHECK(hard.reduction_db(decibels_to_gain(-10.5)) == 0.0);
  CHECK_NEAR(hard.reduction_db(decibels_to_gain(-6.0)), -2.0, 1e-12);
  CHECK(hard.reduction_db(decibels_to_gain(-10.0)) <= 0.0);
  CHECK(std::isfinite(hard.reduction_db(decibels_to_gain(-10.0))));
  const SoftKneeGainComputer unity({-10.0, 6.0, 1.0, 20.0});
  for (double envelope : {0.0, 0.5, 1.0, 1e300, inf, qnan}) {
    CHECK(unity.gain(envelope) == 1.0);
  }
}

void test_non_finite_and_degenerate_envelopes() {
  const SoftKneeGainComputer computer({-30.0, 6.0, 8.0, 12.0});
  CHECK(computer.gain(0.0) == 1.0);
  CHECK(computer.gain(-1.0) == 1.0);
  CHECK(computer.gain(qnan) == 1.0);
  CHECK(computer.gain(-inf) == 1.0);
  CHECK(computer.reduction_db(inf) == -12.0);
  CHECK(computer.gain(inf) == decibels_to_gain(-12.0));
}

void test_rejects_invalid_configuration() {
  SoftKneeGainComputer computer({-12.0, 4.0, 3.0, 9.0});
  const SoftKneeConfig invalid[] = {
      {qnan, 4.0, 3.0, 9.0},   {inf, 4.0, 3.0, 9.0},   {-12.0, -0.1, 3.0, 9.0},
      {-12.0, qnan, 3.0, 9.0}, {-12.0, inf, 3.0, 9.0}, {-12.0, 4.0, 0.99, 9.0},
      {-12.0, 4.0, qnan, 9.0}, {-12.0, 4.0, inf, 9.0}, {-12.0, 4.0, 3.0, -1.0},
      {-12.0, 4.0, 3.0, qnan}, {-12.0, 4.0, 3.0, inf},
  };
  const double before = computer.reduction_db(1.0);
  for (const SoftKneeConfig &config : invalid) {
    CHECK(throws_invalid([&] { computer.configure(config); }));
    CHECK(throws_invalid([&] { (void)SoftKneeGainComputer(config); }));
  }
  CHECK(computer.config().threshold_db == -12.0 && computer.config().ratio == 3.0);
  CHECK(same_bits(computer.reduction_db(1.0), before));
  CHECK(!throws_invalid([&] { computer.configure({0.0, 0.0, 1.0, 0.0}); }));
  CHECK(computer.gain(10.0) == 1.0);
}

}

int main() {
  run("matches_reference_on_level_grid", test_matches_reference_on_level_grid);
  run("matches_reference_on_detected_signals", test_matches_reference_on_detected_signals);
  run("curve_shape", test_curve_shape);
  run("hard_knee_and_unity_ratio", test_hard_knee_and_unity_ratio);
  run("non_finite_and_degenerate_envelopes", test_non_finite_and_degenerate_envelopes);
  run("rejects_invalid_configuration", test_rejects_invalid_configuration);
  return finish("dynamics");
}
