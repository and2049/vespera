#include <vespera/dsp/smoothing.hpp>
#include <vespera/dsp/utility.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
void check(bool condition) {
  if (!condition) {
    std::fputs("smoothing check failed\n", stderr);
    std::abort();
  }
}
void check_bits(float value, std::uint32_t expected) {
  if (std::bit_cast<std::uint32_t>(value) != expected) {
    std::fprintf(stderr, "smoothing check failed: %08x != %08x\n",
                 static_cast<unsigned>(std::bit_cast<std::uint32_t>(value)),
                 static_cast<unsigned>(expected));
    std::abort();
  }
}
template <typename F> void rejects(F operation) {
  try {
    operation();
  } catch (const std::invalid_argument &) {
    return;
  }
  check(false);
}
}

int main() {
  using namespace vespera::dsp;
  LinearSmoother ramp;
  ramp.configure(8000, 0.001);
  ramp.set_target(1.0);
  for (int i = 1; i <= 8; ++i) {
    check(ramp.process() == static_cast<double>(i) / 8);
    ramp.set_target(1.0);
  }
  check(!ramp.active());
  ramp.set_target(-1.0);
  check(ramp.process() == 0.75);
  ramp.set_target(0.0);
  check(ramp.process() == 0.65625);
  rejects([&] { ramp.configure(0.0, 1.0); });
  rejects([&] { ramp.configure(48000, -1.0); });
  rejects([&] { ramp.configure(48000, std::numeric_limits<double>::quiet_NaN()); });
  check(ramp.process() == 0.5625);
  ramp.reset(-0.5);
  check(ramp.process() == -0.5 && !ramp.active());
  ramp.configure(48000, 0.0);
  ramp.set_target(0.25);
  check(ramp.current() == 0.25);
  ramp.configure(768000, 60.0);
  ramp.set_target(1.0);
  check(ramp.process() > 0.25);

  ExponentialSmoother smooth;
  smooth.configure(48000, 0.01);
  smooth.set_target(1.0);
  for (int i = 0; i < 480; ++i) {
    smooth.process();
  }
  check(std::abs(smooth.current() - (1.0 - std::exp(-1.0))) < 1e-13);
  rejects([&] { smooth.configure(48000, 61.0); });
  smooth.reset(1.0);
  smooth.set_target(0.0);
  for (int i = 0; i < 480; ++i) {
    smooth.process();
  }
  check(std::abs(smooth.current() - std::exp(-1.0)) < 1e-13);
  smooth.configure(48000, 0.0);
  check(smooth.process() == 0.0);
  check(flush_subnormal(std::numeric_limits<float>::denorm_min()) == 0.0f);
  check(flush_subnormal(std::numeric_limits<float>::min()) == std::numeric_limits<float>::min());
  check(std::abs(gain_to_decibels(decibels_to_gain(-60.0)) + 60.0) < 1e-12);
  check(std::isinf(gain_to_decibels(0.0)) && gain_to_decibels(0.0) < 0.0);
  const float maximum = std::numeric_limits<float>::max();
  check(crossfade(maximum, -maximum, 0.5) == 0.0f);
  check(crossfade(maximum, maximum, 0.5) == maximum);

  // Duration to frames. The expected bits are the retained 1.0.1 instruction oracle's output for
  // the same three durations; the oracle also prints the reordered evaluation, which is one ULP
  // lower at 50 and 100 ms and must not be what this primitive produces.
  check_bits(fade_frames(48000.0f, 50.0f), 0x45160001u);
  check_bits(fade_frames(48000.0f, 20.0f), 0x44700001u);
  check_bits(fade_frames(48000.0f, 100.0f), 0x45960001u);
  check(fade_frames(48000.0f, 50.0f) != std::bit_cast<float>(0x45160000u));
  check(fade_frames(48000.0f, 100.0f) != std::bit_cast<float>(0x45960000u));
  check(std::lround(fade_frames(48000.0f, 50.0f)) == 2400);
  check(std::lround(fade_frames(48000.0f, 20.0f)) == 960);
  // The 44.1 kHz values are the oracle's predictions for the same formula, not captures.
  check_bits(fade_frames(44100.0f, 20.0f), 0x445C8001u);
  check_bits(fade_frames(44100.0f, 50.0f), 0x4509D000u);
  check_bits(fade_frames(44100.0f, 100.0f), 0x4589D000u);

  // One-frame clamp: an under-one-frame product, the exact zero case and the negative case all
  // collapse onto 1.0f, and a product just above the boundary does not.
  check_bits(fade_frames(48000.0f, 0.0f), 0x3F800000u);
  check_bits(fade_frames(48000.0f, 0.0208f), 0x3F800000u);
  check(fade_frames(48000.0f, 0.021f) > 1.0f);
  check(fade_frames(48000.0f, -5.0f) == 1.0f);

  // NaN is forgiven by the clamp from either argument; an infinite rate or duration is not.
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float infinity = std::numeric_limits<float>::infinity();
  check_bits(fade_frames(nan, 50.0f), 0x3F800000u);
  check_bits(fade_frames(48000.0f, nan), 0x3F800000u);
  check(fade_frames(48000.0f, infinity) == infinity);
  check(fade_frames(infinity, 50.0f) == infinity);
  check_bits(fade_frames(48000.0f, -infinity), 0x3F800000u);
}
