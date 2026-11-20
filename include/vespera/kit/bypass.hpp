#ifndef VESPERA_KIT_BYPASS_HPP
#define VESPERA_KIT_BYPASS_HPP

#include <vespera/dsp/smoothing.hpp>
#include <vespera/kit/audio.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <vector>

namespace vespera::kit {

enum class BypassPolicy {
  // The module applies ExactBypass around the processor.
  exact,
  // The processor receives the bypass parameter like any other and implements bypass itself.
  custom
};

// Selects an ExactBypass whose latency is supplied when it is constructed.
inline constexpr uint32_t prepared_latency = std::numeric_limits<uint32_t>::max();

// Switches between the input, delayed by the processor's latency, and the processor output with a
// linear crossfade. Once settled the output is bit-identical to one of them.
template <uint32_t Latency = prepared_latency> class ExactBypass {
public:
  static constexpr double fade_seconds = 0.01;

  ExactBypass(double sample_rate, float enabled)
    requires(Latency != prepared_latency)
  {
    configure(sample_rate, enabled);
  }

  ExactBypass(double sample_rate, float enabled, uint32_t latency)
    requires(Latency == prepared_latency)
      : delay_(latency) {
    configure(sample_rate, enabled);
  }

  void set_enabled(float enabled) noexcept { mix_.set_target(enabled >= 0.5f ? 1.0 : 0.0); }

  void reset() noexcept {
    mix_.reset(mix_.target());
    std::fill(delay_.begin(), delay_.end(), Frame{});
    cursor_ = 0;
  }

  uint32_t latency() const noexcept { return static_cast<uint32_t>(delay_.size()); }

  Frame process(const Frame &input, const Frame &wet) noexcept {
    Frame dry = input;
    if constexpr (Latency != 0) {
      if (!delay_.empty()) {
        dry = delay_[cursor_];
        delay_[cursor_] = input;
        cursor_ = (cursor_ + 1) % delay_.size();
      }
    }
    const double mix = mix_.process();
    if (mix == 0.0) {
      return dry;
    }
    if (mix == 1.0) {
      return wet;
    }
    return {static_cast<float>(dry[0] * (1.0 - mix) + wet[0] * mix),
            static_cast<float>(dry[1] * (1.0 - mix) + wet[1] * mix)};
  }

private:
  void configure(double sample_rate, float enabled) {
    mix_.configure(sample_rate, fade_seconds);
    set_enabled(enabled);
    reset();
  }

  using Delay = std::conditional_t<Latency == prepared_latency, std::vector<Frame>,
                                   std::array<Frame, Latency == prepared_latency ? 0 : Latency>>;

  dsp::LinearSmoother mix_;
  Delay delay_{};
  size_t cursor_ = 0;
};

}

#endif
