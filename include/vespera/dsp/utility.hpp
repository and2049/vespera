#ifndef VESPERA_DSP_UTILITY_HPP
#define VESPERA_DSP_UTILITY_HPP

#include <bit>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <limits>

#if !defined(__aarch64__) && (defined(__SSE2__) || defined(_M_X64))
#include <xmmintrin.h>
#endif

namespace vespera::dsp {

// Makes the calling thread treat subnormal floating-point operands and results as zero for the
// guard's lifetime, then restores the previous mode. On AArch64 it sets FPCR.FZ; on x86-64 it
// sets MXCSR.FTZ and MXCSR.DAZ, which affect SSE arithmetic only, not x87. Elsewhere it does
// nothing. Create it on the audio thread at the top of a processing callback; guards may nest.
class DenormalGuard {
public:
  DenormalGuard() noexcept {
#if defined(__aarch64__)
    __asm__ volatile("mrs %0, fpcr" : "=r"(previous_) : : "memory");
    const std::uint64_t flushing = previous_ | (std::uint64_t{1} << 24);
    __asm__ volatile("msr fpcr, %0" : : "r"(flushing) : "memory");
#elif defined(__SSE2__) || defined(_M_X64)
    previous_ = _mm_getcsr();
    _mm_setcsr(previous_ | 0x8040u);
#endif
  }

  ~DenormalGuard() {
#if defined(__aarch64__)
    __asm__ volatile("msr fpcr, %0" : : "r"(previous_) : "memory");
#elif defined(__SSE2__) || defined(_M_X64)
    _mm_setcsr(previous_);
#endif
  }

  DenormalGuard(const DenormalGuard &) = delete;
  DenormalGuard &operator=(const DenormalGuard &) = delete;

private:
#if defined(__aarch64__)
  std::uint64_t previous_ = 0;
#elif defined(__SSE2__) || defined(_M_X64)
  unsigned int previous_ = 0;
#endif
};

template <std::floating_point T> T flush_subnormal(T value) noexcept {
  return std::abs(value) < std::numeric_limits<T>::min() ? T{0} : value;
}

inline double decibels_to_gain(double decibels) noexcept { return std::pow(10.0, decibels / 20.0); }

inline double gain_to_decibels(double gain) noexcept {
  return gain > 0.0 ? 20.0 * std::log10(gain) : -std::numeric_limits<double>::infinity();
}

// The weight is in [0, 1]; double intermediates preserve finite float endpoints.
inline float crossfade(float from, float to, double weight) noexcept {
  return static_cast<float>(static_cast<double>(from) * (1.0 - weight) +
                            static_cast<double>(to) * weight);
}

// Preserve this order and separate binary32 rounding; reassociation changes the frame count.
// The result is fractional, with NaN products mapped to the one-frame minimum.
inline float fade_frames(float rate_hz, float milliseconds) noexcept {
  static_assert(std::bit_cast<std::uint32_t>(0.001f) == 0x3A83126Fu,
                "the fade-length contract requires the binary32 seconds-per-millisecond unit");
  const float frames_per_millisecond = rate_hz * 0.001f;
  return std::fmax(milliseconds * frames_per_millisecond, 1.0f);
}

}

#endif
