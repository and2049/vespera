#ifndef VESPERA_DSP_DELAY_HPP
#define VESPERA_DSP_DELAY_HPP

#include <vespera/dsp/common.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace vespera::dsp {

inline constexpr std::size_t maximum_delay_samples = std::size_t{1} << 22;

struct DelayConfig {
  double sample_rate = 48000.0;
  std::size_t max_delay_samples = 0;
  double delay_samples = 0.0;
};

// Single-channel delay line with linear interpolation between neighbouring samples.
//
// Each process() call first writes the input and then reads, so a delay of D samples yields
// y[n] = x[n - D] for integer D (zero delay passes the input through unchanged), and
// y[n] = (1 - f) * x[n - i] + f * x[n - i - 1] for D = i + f. History before the most recent
// reset or prepare reads as silence. prepare() allocates and must run on a control thread.
// Processing, reset, and try_set_delay_samples() are allocation-free. Non-finite inputs are
// silence.
class FractionalDelay {
public:
  FractionalDelay() = default;

  explicit FractionalDelay(const DelayConfig &config) { prepare(config); }

  // Strong guarantee: validation and allocation happen before any state changes. A successful
  // prepare clears the history.
  void prepare(const DelayConfig &config) {
    validate_sample_rate(config.sample_rate);
    if (config.max_delay_samples > maximum_delay_samples) {
      throw std::invalid_argument("Delay capacity exceeds the maximum delay length");
    }
    validate_delay(config.delay_samples, config.max_delay_samples);
    std::vector<float> buffer(config.max_delay_samples + 2, 0.0f);
    buffer_.swap(buffer);
    sample_rate_ = config.sample_rate;
    max_delay_samples_ = config.max_delay_samples;
    delay_samples_ = config.delay_samples;
    write_index_ = 0;
  }

  void set_delay_samples(double delay_samples) {
    validate_delay(delay_samples, max_delay_samples_);
    delay_samples_ = delay_samples;
  }

  // Real-time variant of set_delay_samples(): rejects invalid values without changing state.
  [[nodiscard]] bool try_set_delay_samples(double delay_samples) noexcept {
    if (!is_valid_delay(delay_samples, max_delay_samples_)) {
      return false;
    }
    delay_samples_ = delay_samples;
    return true;
  }

  // Clears history in O(capacity) time; capacity and delay setting are kept. Large buffers may
  // exceed an audio callback's time budget even though reset does not allocate.
  void reset() noexcept {
    std::fill(buffer_.begin(), buffer_.end(), 0.0f);
    write_index_ = 0;
  }

  float process(float input) noexcept {
    const float sample = std::isfinite(input) ? input : 0.0f;
    if (buffer_.empty()) {
      return sample;
    }
    const std::size_t size = buffer_.size();
    buffer_[write_index_] = sample;

    const auto whole = static_cast<std::size_t>(delay_samples_);
    const double fraction = delay_samples_ - static_cast<double>(whole);
    const std::size_t newer = (write_index_ + size - whole) % size;
    const std::size_t older = newer == 0 ? size - 1 : newer - 1;
    const double output = (1.0 - fraction) * static_cast<double>(buffer_[newer]) +
                          fraction * static_cast<double>(buffer_[older]);

    write_index_ = write_index_ + 1 == size ? 0 : write_index_ + 1;
    return static_cast<float>(output);
  }

  [[nodiscard]] bool prepared() const noexcept { return !buffer_.empty(); }
  [[nodiscard]] double sample_rate() const noexcept { return sample_rate_; }
  [[nodiscard]] std::size_t max_delay_samples() const noexcept { return max_delay_samples_; }
  [[nodiscard]] double delay_samples() const noexcept { return delay_samples_; }

private:
  static bool is_valid_delay(double delay_samples, std::size_t max_delay) noexcept {
    return std::isfinite(delay_samples) && delay_samples >= 0.0 &&
           delay_samples <= static_cast<double>(max_delay);
  }

  static void validate_delay(double delay_samples, std::size_t max_delay) {
    if (!is_valid_delay(delay_samples, max_delay)) {
      throw std::invalid_argument("Delay must be finite and within [0, max_delay_samples]");
    }
  }

  std::vector<float> buffer_;
  double sample_rate_ = 0.0;
  std::size_t max_delay_samples_ = 0;
  double delay_samples_ = 0.0;
  std::size_t write_index_ = 0;
};

}

#endif
