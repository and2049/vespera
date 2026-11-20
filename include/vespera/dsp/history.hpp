#ifndef VESPERA_DSP_HISTORY_HPP
#define VESPERA_DSP_HISTORY_HPP

#include <vespera/dsp/common.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace vespera::dsp {

inline constexpr std::size_t maximum_history_samples = std::size_t{1} << 22;

enum class Interpolation { linear, lagrange3 };

struct HistoryConfig {
  double sample_rate = 48000.0;
  std::size_t capacity_samples = 0;
};

// Prepared bounded mono history buffer shared by any number of read taps.
//
// The sample-time semantics are explicit push/read/advance: push() writes the current sample
// at the cursor without moving it, read() returns the tap a delay behind the cursor, and
// advance() steps the cursor to the next slot. The intended per-sample order is
//
//   push(x[n]); y1 = read(D1); y2 = read(D2); ...; advance();
//
// so read(0) returns the sample just pushed (zero delay is passthrough), an integer D returns
// x[n - D] exactly, and a fractional D interpolates between neighbouring samples. A feedback
// loop reads before pushing instead:
//
//   s = f(read(D)); push(s); advance();
//
// where read(D) returns the sample written D pushes earlier, so the loop delay is exactly D
// samples. In this order the cursor slot still holds the sample written capacity pushes
// earlier, so a read must not touch delay 0: the smallest loop is D = 1 for linear and D = 2
// for lagrange3, whose window reaches one sample newer than D (minimum_feedback_delay()).
// Lagrange reads with D in [1, 2) before a push silently mix in that stale slot; the buffer
// cannot detect call order, so the loop owner must respect this bound. The delay is always
// measured from the
// cursor; the caller chooses write-then-read or read-then-write by call order, and the buffer
// keeps no hidden sample-time of its own. Pushing twice without an intervening advance
// overwrites the same slot, and advancing without pushing leaves stale history in the
// window; both are caller errors, not buffer states.
//
// The linear kernel uses the same convention and evaluation order as FractionalDelay:
// D = i + f returns (1 - f) * x[n - i] + f * x[n - i - 1], exact for integer D, and covers
// D in [0, capacity - 1]. The third-order Lagrange kernel evaluates the four-point Lagrange
// polynomial through the samples at delays i - 1, i, i + 1 and i + 2 (for D = i + f), so its
// support reaches one sample further back and one further forward: D must be at least 1 and
// at most capacity - 3. It reproduces polynomials through degree three exactly; it is Lagrange
// interpolation, not cubic Hermite or spline interpolation, and it is not band-limited.
// Moving the delay resamples the history (Doppler); it does not crossfade two frozen taps.
//
// read() is a const, allocation-free query, so any number of taps can read one pushed sample.
// A finite delay outside the kernel's range is clamped into it and a non-finite delay
// returns silence; both are defensive fallbacks, not audio features. Non-finite pushed
// samples are stored as silence. History before the most recent reset or prepare reads as
// silence. prepare() allocates and must run while quiescent; a rejected configuration leaves
// the previous buffer intact. reset() clears in O(capacity) without allocating; processing
// work per sample is one push, one advance and the reads the caller makes. Storage is
// capacity_samples floats, about 16 MiB at the 2^22-sample maximum.
class HistoryBuffer {
public:
  HistoryBuffer() = default;

  explicit HistoryBuffer(const HistoryConfig &config) { prepare(config); }

  // Strong guarantee: validation and allocation happen before any state changes. A successful
  // prepare clears the history and resets the cursor. Capacity must be at least 1; zero
  // capacity cannot serve any tap.
  void prepare(const HistoryConfig &config) {
    validate_sample_rate(config.sample_rate);
    if (config.capacity_samples > maximum_history_samples) {
      throw std::invalid_argument("History capacity exceeds the maximum history length");
    }
    if (config.capacity_samples == 0) {
      throw std::invalid_argument("History capacity must be at least one sample");
    }
    std::vector<float> buffer(config.capacity_samples, 0.0f);
    buffer_.swap(buffer);
    sample_rate_ = config.sample_rate;
    capacity_samples_ = config.capacity_samples;
    cursor_ = 0;
  }

  // Clears history in O(capacity) time; capacity and sample rate are kept. Large buffers may
  // exceed an audio callback's time budget even though reset does not allocate.
  void reset() noexcept {
    std::fill(buffer_.begin(), buffer_.end(), 0.0f);
    cursor_ = 0;
  }

  // Writes the sample at the cursor without moving the cursor. Before preparation it is a
  // no-op and every read returns silence.
  void push(float sample) noexcept {
    if (buffer_.empty()) {
      return;
    }
    buffer_[cursor_] = std::isfinite(sample) ? sample : 0.0f;
  }

  // Moves the cursor to the slot that held the oldest sample.
  void advance() noexcept {
    if (buffer_.empty()) {
      return;
    }
    cursor_ = cursor_ + 1 == buffer_.size() ? 0 : cursor_ + 1;
  }

  // Returns the tap `delay_samples` behind the cursor, interpolated by `kernel`.
  [[nodiscard]] float read(double delay_samples,
                           Interpolation kernel = Interpolation::linear) const noexcept {
    const std::size_t size = buffer_.size();
    if (size == 0 || !std::isfinite(delay_samples)) {
      return 0.0f;
    }
    if (kernel == Interpolation::lagrange3) {
      if (size < 4) {
        return 0.0f;
      }
      const double delay = std::clamp(delay_samples, 1.0, static_cast<double>(size - 3));
      const auto whole = static_cast<std::size_t>(delay);
      const double u = delay - static_cast<double>(whole) + 1.0;
      const double c0 = -(u - 1.0) * (u - 2.0) * (u - 3.0) / 6.0;
      const double c1 = u * (u - 2.0) * (u - 3.0) / 2.0;
      const double c2 = -u * (u - 1.0) * (u - 3.0) / 2.0;
      const double c3 = u * (u - 1.0) * (u - 2.0) / 6.0;
      const double output = c0 * static_cast<double>(sample_at(whole - 1)) +
                            c1 * static_cast<double>(sample_at(whole)) +
                            c2 * static_cast<double>(sample_at(whole + 1)) +
                            c3 * static_cast<double>(sample_at(whole + 2));
      return static_cast<float>(output);
    }
    const double delay = std::clamp(delay_samples, 0.0, static_cast<double>(size - 1));
    const auto whole = static_cast<std::size_t>(delay);
    const double fraction = delay - static_cast<double>(whole);
    const double output = (1.0 - fraction) * static_cast<double>(sample_at(whole)) +
                          fraction * static_cast<double>(sample_at(whole + 1));
    return static_cast<float>(output);
  }

  [[nodiscard]] bool prepared() const noexcept { return !buffer_.empty(); }
  [[nodiscard]] double sample_rate() const noexcept { return sample_rate_; }
  [[nodiscard]] std::size_t capacity_samples() const noexcept { return capacity_samples_; }

  // Smallest delay the kernel can read without clamping, for reads between push and advance.
  [[nodiscard]] static constexpr double minimum_delay(Interpolation kernel) noexcept {
    return kernel == Interpolation::linear ? 0.0 : 1.0;
  }

  // Smallest loop delay for reads made before the push of the same sample (feedback order),
  // where the cursor slot is not yet the current sample.
  [[nodiscard]] static constexpr double minimum_feedback_delay(Interpolation kernel) noexcept {
    return kernel == Interpolation::linear ? 1.0 : 2.0;
  }

  // Largest delay the kernel can read from `capacity` samples without clamping. Lagrange3
  // returns 0 when the capacity cannot hold its four-point window.
  [[nodiscard]] static constexpr double maximum_delay(std::size_t capacity,
                                                      Interpolation kernel) noexcept {
    if (kernel == Interpolation::linear) {
      return capacity == 0 ? 0.0 : static_cast<double>(capacity - 1);
    }
    return capacity < 4 ? 0.0 : static_cast<double>(capacity - 3);
  }

private:
  // Requires delay <= buffer_.size(); delays beyond the capacity alias the newest slots.
  [[nodiscard]] float sample_at(std::size_t delay) const noexcept {
    return buffer_[(cursor_ + buffer_.size() - delay) % buffer_.size()];
  }

  std::vector<float> buffer_;
  double sample_rate_ = 0.0;
  std::size_t capacity_samples_ = 0;
  std::size_t cursor_ = 0;
};

} // namespace vespera

#endif
