#ifndef VESPERA_KIT_AUDIO_HPP
#define VESPERA_KIT_AUDIO_HPP

#include <array>
#include <cstdint>
#include <span>

namespace vespera::kit {

// The module ABI defines mono and stereo layouts only.
inline constexpr uint32_t max_channels = 2;

// One sample per channel. Channels beyond the prepared count are zero on input and ignored on
// output.
using Frame = std::array<float, max_channels>;

struct Setup {
  double sample_rate = 0.0;
  uint32_t channels = 0;
  uint32_t max_frames = 0;
  // Current parameter values in table order. Valid only while the processor is being constructed.
  std::span<const float> values;
};

// Planar audio for one span of frames. Inputs are finite and never alias outputs; outputs for
// every prepared channel must be written in full.
struct Block {
  std::array<const float *, max_channels> inputs{};
  std::array<float *, max_channels> outputs{};
  uint32_t channels = 0;
  uint32_t frames = 0;

  std::span<const float> input(uint32_t channel) const noexcept {
    return {inputs[channel], frames};
  }
  std::span<float> output(uint32_t channel) const noexcept { return {outputs[channel], frames}; }
};

}

#endif
