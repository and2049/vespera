#ifndef VESPERA_TESTS_SCRIPTED_AUDIO_BACKEND_H
#define VESPERA_TESTS_SCRIPTED_AUDIO_BACKEND_H

#include <audio_engine/engine.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Hardware-free device behind the real audio-io-core engine. Every context created through
// ae_context_create receives a backend driven by this script. Blocks are delivered synchronously
// on the calling thread, so tests decide exactly when each callback runs.
namespace scripted_ae {

inline constexpr const char *backend_id = "scripted";
inline constexpr const char *device_id = "scripted-duplex";

struct Script {
  uint32_t input_channels = 4;
  uint32_t output_channels = 4;
  // When set, the device ignores the requested value and offers only this one.
  std::optional<uint32_t> fixed_sample_rate;
  std::optional<uint32_t> fixed_frames;
  // Zero reports the negotiated block size as the maximum.
  uint32_t max_frames = 0;
  ae_result start_result = AE_OK;
  int stop_failures = 0;
  int close_failures = 0;
  uint32_t faults = 0;
};

struct Observed {
  int contexts_alive = 0;
  int open_calls = 0;
  int start_calls = 0;
  int stop_calls = 0;
  int close_calls = 0;
  bool open = false;
  bool running = false;
  std::vector<uint32_t> requested_inputs;
  std::vector<uint32_t> requested_outputs;
  bool requested_adjustment = false;
};

Script &script();
const Observed &observed();
// Requires that no context is alive.
void reset();

// Runs one block through the installed engine dispatcher with a mono input and stereo output.
// Outputs are filled with `stale` first so tests can tell whether the engine or session wrote
// them. The frame position advances by `frames` unless overridden with `position`.
ae_process_result deliver(const float *input, float *left, float *right, uint32_t frames,
                          float stale = 7.0f, std::optional<uint64_t> position = std::nullopt);
// The same with two planar input channels, as a stream opened with two inputs receives them.
ae_process_result deliver(const float *left_input, const float *right_input, float *left,
                          float *right, uint32_t frames, float stale = 7.0f,
                          std::optional<uint64_t> position = std::nullopt);
uint64_t next_position();

}

#endif
