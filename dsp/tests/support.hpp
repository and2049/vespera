#ifndef VESPERA_DSP_TESTS_SUPPORT_HPP
#define VESPERA_DSP_TESTS_SUPPORT_HPP

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace test_support {

inline int failures = 0;
inline const char *current_test = "";

#define CHECK(condition)                                                                           \
  do {                                                                                             \
    if (!(condition)) {                                                                            \
      std::fprintf(stderr, "%s:%d: %s: CHECK(%s) failed\n", __FILE__, __LINE__,                    \
                   test_support::current_test, #condition);                                        \
      ++test_support::failures;                                                                    \
    }                                                                                              \
  } while (false)

#define CHECK_NEAR(actual, expected, tolerance)                                                    \
  do {                                                                                             \
    const double check_actual = (actual);                                                          \
    const double check_expected = (expected);                                                      \
    if (!(std::abs(check_actual - check_expected) <= (tolerance))) {                               \
      std::fprintf(stderr, "%s:%d: %s: %s = %.17g, expected %.17g +/- %g\n", __FILE__, __LINE__,   \
                   test_support::current_test, #actual, check_actual, check_expected,              \
                   static_cast<double>(tolerance));                                                \
      ++test_support::failures;                                                                    \
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

inline constexpr double qnan = std::numeric_limits<double>::quiet_NaN();
inline constexpr double inf = std::numeric_limits<double>::infinity();
inline constexpr double rates[] = {44100.0, 48000.0, 96000.0};

inline bool same_bits(double a, double b) noexcept {
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

inline bool same_bits(float a, float b) noexcept {
  return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
}

// Counts sample positions whose bit patterns differ and reports the first one.
template <typename Sample>
std::size_t count_mismatches(const std::vector<Sample> &actual, const std::vector<Sample> &expected,
                             const std::string &label) {
  std::size_t mismatches = actual.size() == expected.size() ? 0 : 1;
  for (std::size_t i = 0; i < actual.size() && i < expected.size(); ++i) {
    if (!same_bits(actual[i], expected[i])) {
      if (mismatches == 0) {
        std::fprintf(stderr, "%s: %s: first mismatch at %zu: %.17g vs %.17g\n", current_test,
                     label.c_str(), i, static_cast<double>(actual[i]),
                     static_cast<double>(expected[i]));
      }
      ++mismatches;
    }
  }
  return mismatches;
}

inline std::vector<double> noise(std::size_t count, std::uint32_t seed, double amplitude = 1.0) {
  std::vector<double> result(count);
  std::uint32_t state = seed;
  for (double &sample : result) {
    state = state * 1664525u + 1013904223u;
    sample =
        amplitude * (static_cast<double>(state >> 8) / static_cast<double>(1u << 24) * 2.0 - 1.0);
  }
  return result;
}

inline std::vector<double> sine(std::size_t count, double rate, double frequency,
                                double amplitude) {
  std::vector<double> result(count);
  for (std::size_t i = 0; i < count; ++i) {
    result[i] =
        amplitude * std::sin(2.0 * std::numbers::pi * frequency * static_cast<double>(i) / rate);
  }
  return result;
}

inline std::vector<double> impulse(std::size_t count, double amplitude) {
  std::vector<double> result(count, 0.0);
  result[0] = amplitude;
  return result;
}

// Repeats full-scale, out-of-range, signed-zero, subnormal and near-zero values; finite only.
inline std::vector<double> extremes(std::size_t count) {
  constexpr double pattern[] = {16.0, -16.0, 0.0,     -0.0,    4.9e-324, -4.9e-324, 1e-310, -1e-300,
                                1.0,  -1.0,  15.999,  -15.999, 1e-6,     0.5,       -0.25,  16.0,
                                16.0, -16.0, 1.0e-30, 2.0,     -2.0e-20, 1.5,       -16.0,  0.0};
  std::vector<double> result(count);
  for (std::size_t i = 0; i < count; ++i) {
    result[i] = pattern[(i * 7 + i / 24) % std::size(pattern)];
  }
  return result;
}

// Deterministic named test signals of `count` samples for one sample rate.
inline std::vector<std::pair<std::string, std::vector<double>>> signals(double rate,
                                                                        std::size_t count) {
  std::vector<std::pair<std::string, std::vector<double>>> result;
  result.emplace_back("sine 110 Hz", sine(count, rate, 110.0, 0.5));
  result.emplace_back("sine 1 kHz", sine(count, rate, 1000.0, 1.0));
  result.emplace_back("sine 7 kHz hot", sine(count, rate, 7000.0, 4.0));
  result.emplace_back("noise", noise(count, 1u));
  result.emplace_back("noise hot", noise(count, 2u, 16.0));
  result.emplace_back("impulse", impulse(count, 1.0));
  result.emplace_back("impulse hot", impulse(count, 16.0));
  result.emplace_back("extremes", extremes(count));
  return result;
}

template <typename Test> void run(const char *name, Test &&test) {
  current_test = name;
  test();
}

inline int finish(const char *suite) {
  if (failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  std::printf("all DSP %s tests passed\n", suite);
  return 0;
}

}

#endif
