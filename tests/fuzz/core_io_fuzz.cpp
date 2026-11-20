#include <vespera/core_io.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size < 9) {
    return 0;
  }
  constexpr uint32_t capacity = 64;
  vespera::CoreIoProcessor processor;
  vespera::CoreIoSettings settings;
  const double rate = 8000.0 + data[0] * 2000.0;
  settings.input_gain_db = -30.0f + data[1] * 0.2f;
  settings.output_gain_db = -30.0f + data[2] * 0.2f;
  settings.gate_enabled = (data[3] & 1) != 0;
  settings.gate_threshold_db = std::min(0.0f, -100.0f + data[4] * (100.0f / 255.0f));
  settings.transpose_semitones = std::clamp(-12.0f + data[5] * (24.0f / 255.0f), -12.0f, 12.0f);
  settings.doubler_enabled = (data[6] & 1) != 0;
  settings.doubler_delay_ms = std::min(40.0f, 1.0f + data[7] * (39.0f / 255.0f));
  if (!processor.set_settings(settings)) {
    std::abort();
  }
  processor.prepare(rate, capacity, vespera::TransposeLatency::fixed);
  std::array<float, capacity> left{}, right{}, pre_left{}, pre_right{}, post_left{}, post_right{};
  const std::array<const float *, 2> inputs{left.data(), right.data()};
  const std::array<float *, 2> pre{pre_left.data(), pre_right.data()};
  const std::array<const float *, 2> pre_inputs{pre_left.data(), pre_right.data()};
  const std::array<float *, 2> post{post_left.data(), post_right.data()};
  for (size_t offset = 8; offset < size;) {
    const uint32_t frames = static_cast<uint32_t>(std::min<size_t>(capacity, size - offset));
    for (uint32_t i = 0; i < frames; ++i) {
      left[i] = (static_cast<float>(data[offset + i]) - 128.0f) / 128.0f;
      right[i] = -left[i];
    }
    if (!processor.process_input(inputs.data(), pre.data(), 2, frames) ||
        !processor.process_output(pre_inputs.data(), 2, post.data(), 2, frames)) {
      std::abort();
    }
    for (uint32_t i = 0; i < frames; ++i) {
      if (!std::isfinite(post_left[i]) || !std::isfinite(post_right[i])) {
        std::abort();
      }
    }
    if ((data[offset] & 15) == 0) {
      processor.reset();
    }
    if ((data[offset] & 31) == 1) {
      settings.transpose_semitones =
          std::clamp(-12.0f + data[offset] * (24.0f / 255.0f), -12.0f, 12.0f);
      settings.doubler_enabled = !settings.doubler_enabled;
      if (!processor.set_settings(settings)) {
        std::abort();
      }
    }
    offset += frames;
  }
  return 0;
}
