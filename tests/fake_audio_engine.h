#ifndef VESPERA_TESTS_FAKE_AUDIO_ENGINE_H
#define VESPERA_TESTS_FAKE_AUDIO_ENGINE_H

#include <audio_engine/engine.h>

#include <cstdint>
#include <string>
#include <vector>

// Scriptable stand-in for the audio-io-core C API. Callbacks are invoked synchronously by tests.
namespace fake_ae {

struct Control {
  uint32_t input_channels = 4;
  uint32_t output_channels = 4;
  uint32_t negotiated_frames = 128;
  uint32_t negotiated_max_frames = 256;
  uint32_t negotiated_rate = 48000;
  uint32_t negotiated_input_count = 0;
  int32_t output_map_offset = 0;
  ae_result open_result = AE_OK;
  ae_result start_result = AE_OK;
  int stop_failures = 0;
  // Number of upcoming starts that fail with a driver error and leave the stream ready.
  int start_failures = 0;
  int close_failures = 0;
  uint32_t fault_flags = 0;

  ae_stream_request last_request{};
  std::string requested_input_device;
  std::string requested_output_device;
  std::vector<uint32_t> requested_inputs;
  std::vector<uint32_t> requested_outputs;

  int contexts_alive = 0;
  int context_destroy_calls = 0;
  int device_snapshots_alive = 0;
  int capability_snapshots_alive = 0;
  int streams_alive = 0;
  int open_calls = 0;
  int stop_calls = 0;
  int close_calls = 0;

  ae_process_fn process = nullptr;
  void *user = nullptr;
  ae_stream_state state = AE_STREAM_STATE_READY;
  ae_result last_result = AE_OK;
  uint32_t run_generation = 0;
  uint32_t stream_id = 0;
  uint32_t configuration_generation = 0;
};

Control &control();
void reset();

// Invokes the installed callback the same way the upstream dispatcher does, including silencing
// and latching recovery on failure. Returns the callback result.
ae_process_result invoke(const float *input, float *left, float *right, uint32_t frames,
                         uint64_t frame_position);

}

#endif
