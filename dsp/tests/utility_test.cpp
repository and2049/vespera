#include <vespera/dsp/common.hpp>
#include <vespera/dsp/filters.hpp>
#include <vespera/dsp/utility.hpp>

#include "support.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

using namespace test_support;
using vespera::dsp::BiquadCoefficients;
using vespera::dsp::DenormalGuard;
using vespera::dsp::nyquist_safe_frequency;

constexpr bool flushes_denormals =
#if defined(__aarch64__) || defined(__SSE2__) || defined(_M_X64)
    true;
#else
    false;
#endif

double halve(double value) {
  volatile double operand = value;
  return operand * 0.5;
}

float halve(float value) {
  volatile float operand = value;
  return operand * 0.5f;
}

void test_denormal_guard_flushes_and_restores() {
  const double smallest = std::numeric_limits<double>::min();
  const float smallest_float = std::numeric_limits<float>::min();
  CHECK(halve(smallest) != 0.0);
  {
    const DenormalGuard guard;
    if (flushes_denormals) {
      CHECK(halve(smallest) == 0.0);
      CHECK(halve(smallest_float) == 0.0f);
      CHECK(halve(-smallest) == 0.0);
    }
    CHECK(halve(4.0) == 2.0);
    {
      const DenormalGuard nested;
      if (flushes_denormals) {
        CHECK(halve(smallest) == 0.0);
      }
    }
    if (flushes_denormals) {
      CHECK(halve(smallest) == 0.0);
    }
  }
  CHECK(halve(smallest) != 0.0);
  CHECK(halve(smallest_float) != 0.0f);
  CHECK(std::fpclassify(halve(smallest)) == FP_SUBNORMAL);
}

// Reference recurrence: the inline frequency limit written as std::min(hz, sample_rate * 0.45).
double reference_frequency(double sample_rate, double hz) {
  return std::min(hz, sample_rate * 0.45);
}

void test_nyquist_safe_frequency_matches_reference() {
  constexpr double frequencies[] = {20.0,   25.0,   40.0,    80.0,    110.0,   160.0,
                                    550.0,  2800.0, 3200.0,  4800.0,  6500.0,  8500.0,
                                    3600.0, 3599.9, 19845.0, 21600.0, 43200.0, 1.0e9};
  for (double rate : {8000.0, 32000.0, 44100.0, 48000.0, 96000.0, 768000.0}) {
    for (double hz : frequencies) {
      CHECK(same_bits(nyquist_safe_frequency(rate, hz), reference_frequency(rate, hz)));
    }
  }
}

void test_nyquist_safe_frequency_boundaries() {
  CHECK(nyquist_safe_frequency(8000.0, 3600.0) == 3600.0);
  CHECK(nyquist_safe_frequency(8000.0, 3600.5) == 3600.0);
  CHECK(nyquist_safe_frequency(8000.0, 3599.5) == 3599.5);
  CHECK(nyquist_safe_frequency(48000.0, inf) == 21600.0);
  CHECK(nyquist_safe_frequency(48000.0, -5.0) == -5.0);
  CHECK(std::isnan(nyquist_safe_frequency(48000.0, qnan)));
  CHECK(vespera::dsp::nyquist_safe_frequency_ratio <
        vespera::dsp::maximum_normalized_filter_frequency);
  CHECK(throws_invalid([] { (void)BiquadCoefficients::high_shelf(8000.0, 8500.0, 0.7, -12.0); }));
  CHECK(!throws_invalid([] {
    (void)BiquadCoefficients::high_shelf(8000.0, nyquist_safe_frequency(8000.0, 8500.0), 0.7,
                                         -12.0);
  }));
}

}

int main() {
  run("denormal_guard_flushes_and_restores", test_denormal_guard_flushes_and_restores);
  run("nyquist_safe_frequency_matches_reference", test_nyquist_safe_frequency_matches_reference);
  run("nyquist_safe_frequency_boundaries", test_nyquist_safe_frequency_boundaries);
  return finish("utility");
}
