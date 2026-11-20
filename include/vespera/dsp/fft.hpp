#ifndef VESPERA_DSP_FFT_HPP
#define VESPERA_DSP_FFT_HPP

#include <bit>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <stdexcept>
#include <vector>

namespace vespera::dsp {

inline constexpr std::size_t minimum_fft_size = 4;
inline constexpr std::size_t maximum_fft_size = std::size_t{1} << 21;

namespace detail {

// A real transform of size N runs as a complex radix-2 transform of size M = N / 2 on split real
// and imaginary arrays: z[n] = x[2n] + i x[2n + 1] is stored in bit-reversed order, transformed
// in place, then split into the N / 2 + 1 bins of the real spectrum. The inverse merges the
// spectrum back into M complex values with real and imaginary arrays exchanged, which turns the
// same forward passes into an inverse transform.
//
// Every pass is a loop over independent elements and accepts an element range, so a transform
// can be divided between calls in any way without changing a single bit of its result.
class FftPlan {
public:
  explicit FftPlan(std::size_t size) {
    if (size < minimum_fft_size || size > maximum_fft_size || !std::has_single_bit(size)) {
      throw std::invalid_argument("FFT size must be a power of two within [4, 2097152]");
    }
    size_ = size;
    half_ = size / 2;
    stage_count_ = static_cast<std::size_t>(std::countr_zero(half_));
    reversed_.resize(half_);
    for (std::size_t i = 0; i < half_; ++i) {
      std::size_t reversed = 0;
      for (std::size_t bit = 0; bit < stage_count_; ++bit) {
        reversed |= ((i >> bit) & 1u) << (stage_count_ - 1 - bit);
      }
      reversed_[i] = static_cast<std::uint32_t>(reversed);
    }
    stage_cos_.assign(half_, 0.0f);
    stage_sin_.assign(half_, 0.0f);
    for (std::size_t span = 1; span < half_; span *= 2) {
      for (std::size_t j = 0; j < span; ++j) {
        const double angle = -std::numbers::pi * static_cast<double>(j) / static_cast<double>(span);
        stage_cos_[span + j] = twiddle(std::cos(angle));
        stage_sin_[span + j] = twiddle(std::sin(angle));
      }
    }
    split_cos_.resize(half_ / 2 + 1);
    split_sin_.resize(half_ / 2 + 1);
    for (std::size_t k = 0; k <= half_ / 2; ++k) {
      const double angle =
          -2.0 * std::numbers::pi * static_cast<double>(k) / static_cast<double>(size_);
      split_cos_[k] = twiddle(std::cos(angle));
      split_sin_[k] = twiddle(std::sin(angle));
    }
  }

  std::size_t size() const noexcept { return size_; }
  std::size_t half() const noexcept { return half_; }
  std::size_t bins() const noexcept { return half_ + 1; }

  // Butterfly passes after gather or merge. Pass 0 fuses the first two radix-2 stages.
  std::size_t pass_count() const noexcept { return stage_count_ == 1 ? 1 : stage_count_ - 1; }
  std::size_t pass_elements(std::size_t pass) const noexcept {
    return pass == 0 && stage_count_ > 1 ? half_ / 4 : half_ / 2;
  }

  // half() elements: re[rev(n)] + i im[rev(n)] = input[2n] + i input[2n + 1].
  void gather(const float *input, float *re, float *im, std::size_t begin,
              std::size_t end) const noexcept {
    for (std::size_t n = begin; n < end; ++n) {
      const std::uint32_t target = reversed_[n];
      re[target] = input[2 * n];
      im[target] = input[2 * n + 1];
    }
  }

  void pass(std::size_t index, float *re, float *im, std::size_t begin,
            std::size_t end) const noexcept {
    if (stage_count_ == 1) {
      first_stage(re, im, begin, end);
    } else if (index == 0) {
      first_two_stages(re, im, begin, end);
    } else {
      stage(index + 1, re, im, begin, end);
    }
  }

  // half() / 2 + 1 elements: bin k and bin half() - k of the real spectrum.
  void split(const float *re, const float *im, float *out_re, float *out_im, std::size_t begin,
             std::size_t end) const noexcept {
    for (std::size_t k = begin; k < end; ++k) {
      if (k == 0) {
        out_re[0] = re[0] + im[0];
        out_im[0] = 0.0f;
        out_re[half_] = re[0] - im[0];
        out_im[half_] = 0.0f;
        continue;
      }
      const std::size_t mirror = half_ - k;
      const float even_re = 0.5f * (re[k] + re[mirror]);
      const float even_im = 0.5f * (im[k] - im[mirror]);
      const float odd_re = 0.5f * (im[k] + im[mirror]);
      const float odd_im = 0.5f * (re[mirror] - re[k]);
      const float t_re = split_cos_[k] * odd_re - split_sin_[k] * odd_im;
      const float t_im = split_cos_[k] * odd_im + split_sin_[k] * odd_re;
      out_re[k] = even_re + t_re;
      out_im[k] = even_im + t_im;
      out_re[mirror] = even_re - t_re;
      out_im[mirror] = t_im - even_im;
    }
  }

  // half() / 2 + 1 elements: the inverse of split() scaled by 2, written bit-reversed with real
  // and imaginary arrays exchanged. The imaginary parts of bins 0 and half() are ignored.
  void merge(const float *in_re, const float *in_im, float *re, float *im, std::size_t begin,
             std::size_t end) const noexcept {
    for (std::size_t k = begin; k < end; ++k) {
      if (k == 0) {
        re[0] = in_re[0] - in_re[half_];
        im[0] = in_re[0] + in_re[half_];
        continue;
      }
      const std::size_t mirror = half_ - k;
      const float even_re = in_re[k] + in_re[mirror];
      const float even_im = in_im[k] - in_im[mirror];
      const float diff_re = in_re[k] - in_re[mirror];
      const float diff_im = in_im[k] + in_im[mirror];
      const float odd_re = split_cos_[k] * diff_re + split_sin_[k] * diff_im;
      const float odd_im = split_cos_[k] * diff_im - split_sin_[k] * diff_re;
      const std::uint32_t low = reversed_[k];
      const std::uint32_t high = reversed_[mirror];
      im[low] = even_re - odd_im;
      re[low] = even_im + odd_re;
      im[high] = even_re + odd_im;
      re[high] = odd_re - even_im;
    }
  }

  // half() elements after merge() and the passes: output[2n] + i output[2n + 1] = im[n] + i re[n].
  void scatter(const float *re, const float *im, float *output, std::size_t begin,
               std::size_t end) const noexcept {
    for (std::size_t n = begin; n < end; ++n) {
      output[2 * n] = im[n];
      output[2 * n + 1] = re[n];
    }
  }

private:
  // Quarter-turn cosines and sines are exactly zero, which keeps the two writes of the middle bin
  // in split() and merge() identical.
  static float twiddle(double value) noexcept {
    return static_cast<float>(std::abs(value) < 1e-12 ? 0.0 : value);
  }

  static void first_stage(float *re, float *im, std::size_t begin, std::size_t end) noexcept {
    for (std::size_t b = begin; b < end; ++b) {
      const std::size_t i = 2 * b;
      const float a_re = re[i];
      const float a_im = im[i];
      re[i] = a_re + re[i + 1];
      im[i] = a_im + im[i + 1];
      re[i + 1] = a_re - re[i + 1];
      im[i + 1] = a_im - im[i + 1];
    }
  }

  static void first_two_stages(float *re, float *im, std::size_t begin, std::size_t end) noexcept {
    for (std::size_t g = begin; g < end; ++g) {
      float *r = re + 4 * g;
      float *m = im + 4 * g;
      const float s0_re = r[0] + r[1];
      const float s0_im = m[0] + m[1];
      const float d0_re = r[0] - r[1];
      const float d0_im = m[0] - m[1];
      const float s1_re = r[2] + r[3];
      const float s1_im = m[2] + m[3];
      const float d1_re = r[2] - r[3];
      const float d1_im = m[2] - m[3];
      r[0] = s0_re + s1_re;
      m[0] = s0_im + s1_im;
      r[2] = s0_re - s1_re;
      m[2] = s0_im - s1_im;
      r[1] = d0_re + d1_im;
      m[1] = d0_im - d1_re;
      r[3] = d0_re - d1_im;
      m[3] = d0_im + d1_re;
    }
  }

  void stage(std::size_t stage_index, float *re, float *im, std::size_t begin,
             std::size_t end) const noexcept {
    const std::size_t span = std::size_t{1} << stage_index;
    const float *cos_table = stage_cos_.data() + span;
    const float *sin_table = stage_sin_.data() + span;
    std::size_t b = begin;
    while (b < end) {
      const std::size_t j = b & (span - 1);
      const std::size_t run = std::min(span - j, end - b);
      const std::size_t base = ((b >> stage_index) << (stage_index + 1)) + j;
      float *__restrict top_re = re + base;
      float *__restrict top_im = im + base;
      float *__restrict bottom_re = top_re + span;
      float *__restrict bottom_im = top_im + span;
      const float *__restrict c = cos_table + j;
      const float *__restrict s = sin_table + j;
      for (std::size_t q = 0; q < run; ++q) {
        const float t_re = c[q] * bottom_re[q] - s[q] * bottom_im[q];
        const float t_im = c[q] * bottom_im[q] + s[q] * bottom_re[q];
        bottom_re[q] = top_re[q] - t_re;
        bottom_im[q] = top_im[q] - t_im;
        top_re[q] = top_re[q] + t_re;
        top_im[q] = top_im[q] + t_im;
      }
      b += run;
    }
  }

  std::size_t size_ = 0;
  std::size_t half_ = 0;
  std::size_t stage_count_ = 0;
  std::vector<std::uint32_t> reversed_;
  std::vector<float> stage_cos_;
  std::vector<float> stage_sin_;
  std::vector<float> split_cos_;
  std::vector<float> split_sin_;
};

}

// Real-input FFT of a power-of-two size in [4, 2^21], single precision.
//
// forward() produces the size() / 2 + 1 non-negative-frequency bins
// X[k] = sum_n x[n] exp(-2 pi i k n / size()) as separate real and imaginary arrays; the
// imaginary parts of bins 0 and size() / 2 are exactly zero. inverse() is unscaled: it returns
// size() times the original signal, using only the real parts of bins 0 and size() / 2.
//
// Construction allocates tables and work buffers; the transforms never allocate, lock or throw.
// The work buffers make an instance usable by one thread at a time. Input and output spans may
// overlap. The radix-2 algorithm's rounding error grows with log2(size()); results are
// deterministic for a given compiler and target but are not bit-identical to other FFT
// libraries.
class RealFft {
public:
  explicit RealFft(std::size_t size) : plan_(size), re_(plan_.half()), im_(plan_.half()) {}

  std::size_t size() const noexcept { return plan_.size(); }
  std::size_t bins() const noexcept { return plan_.bins(); }

  void forward(std::span<const float> input, std::span<float> real,
               std::span<float> imaginary) noexcept {
    assert(input.size() >= size() && real.size() >= bins() && imaginary.size() >= bins());
    plan_.gather(input.data(), re_.data(), im_.data(), 0, plan_.half());
    run_passes();
    plan_.split(re_.data(), im_.data(), real.data(), imaginary.data(), 0, plan_.half() / 2 + 1);
  }

  void inverse(std::span<const float> real, std::span<const float> imaginary,
               std::span<float> output) noexcept {
    assert(output.size() >= size() && real.size() >= bins() && imaginary.size() >= bins());
    plan_.merge(real.data(), imaginary.data(), re_.data(), im_.data(), 0, plan_.half() / 2 + 1);
    run_passes();
    plan_.scatter(re_.data(), im_.data(), output.data(), 0, plan_.half());
  }

private:
  void run_passes() noexcept {
    for (std::size_t pass = 0; pass < plan_.pass_count(); ++pass) {
      plan_.pass(pass, re_.data(), im_.data(), 0, plan_.pass_elements(pass));
    }
  }

  detail::FftPlan plan_;
  std::vector<float> re_;
  std::vector<float> im_;
};

}

#endif
