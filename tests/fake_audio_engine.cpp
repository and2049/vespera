#include "fake_audio_engine.h"

#include <algorithm>
#include <cstring>
#include <string>

struct ae_context {
  int unused = 0;
};

struct ae_stream {
  std::vector<uint32_t> inputs;
  std::vector<uint32_t> outputs;
};

struct ae_device_snapshot {
  std::vector<std::string> strings;
  std::vector<ae_device_info> infos;
};

struct ae_capability_snapshot {
  std::vector<uint32_t> rates;
  ae_device_capabilities info{};
};

namespace fake_ae {

namespace {

Control state;

}

Control &control() { return state; }

void reset() { state = Control{}; }

ae_process_result invoke(const float *input, float *left, float *right, uint32_t frames,
                         uint64_t frame_position) {
  const float *inputs[] = {input};
  float *outputs[] = {left, right};
  std::fill_n(left, frames, 0.0f);
  std::fill_n(right, frames, 0.0f);
  ae_audio_block block{};
  block.struct_size = sizeof(block);
  block.inputs = inputs;
  block.input_channels = 1;
  block.outputs = outputs;
  block.output_channels = 2;
  block.frames = frames;
  block.stream_id = state.stream_id;
  block.configuration_generation = state.configuration_generation;
  block.run_generation = state.run_generation;
  block.frame_position = frame_position;
  const ae_process_result result = state.process(state.user, &block);
  if (result != AE_PROCESS_OK) {
    std::fill_n(left, frames, 0.0f);
    std::fill_n(right, frames, 0.0f);
  }
  if (result == AE_PROCESS_FAILED) {
    state.last_result = AE_ERROR_PROCESSOR_FAILURE;
    state.state = AE_STREAM_STATE_RECOVERY_REQUIRED;
  }
  return result;
}

}

using fake_ae::state;

extern "C" {

ae_result ae_get_api_version(uint32_t *out_major, uint32_t *out_minor) {
  *out_major = AE_API_VERSION_MAJOR;
  *out_minor = AE_API_VERSION_MINOR;
  return AE_OK;
}

ae_result ae_context_create(const ae_context_options *options, ae_context **out_context) {
  if (options == nullptr || options->api_version_major != AE_API_VERSION_MAJOR ||
      options->api_version_minor != AE_API_VERSION_MINOR) {
    return AE_ERROR_INCOMPATIBLE_ABI;
  }
  *out_context = new ae_context();
  ++state.contexts_alive;
  return AE_OK;
}

ae_result ae_context_destroy(ae_context *context) {
  ++state.context_destroy_calls;
  if (state.streams_alive != 0 || state.device_snapshots_alive != 0 ||
      state.capability_snapshots_alive != 0) {
    return AE_ERROR_BUSY;
  }
  delete context;
  --state.contexts_alive;
  return AE_OK;
}

ae_result ae_context_refresh_devices(ae_context *) { return AE_OK; }

ae_result ae_context_get_diagnostic(const ae_context *, ae_diagnostic *out) {
  *out = {};
  out->struct_size = sizeof(*out);
  out->result = AE_ERROR_DRIVER_FAILURE;
  std::strcpy(out->operation, "fake");
  std::strcpy(out->message, "scripted failure");
  return AE_OK;
}

ae_result ae_context_probe_device(ae_context *, const char *, const char *device_id,
                                  ae_capability_snapshot **out) {
  *out = nullptr;
  if (state.streams_alive != 0) {
    return AE_ERROR_BUSY;
  }
  if (std::strcmp(device_id, "dev-a") != 0) {
    return AE_ERROR_NOT_FOUND;
  }
  auto *snapshot = new ae_capability_snapshot();
  snapshot->rates = {44100, 48000, 96000};
  snapshot->info.input_channels = 2;
  snapshot->info.output_channels = 2;
  snapshot->info.current_sample_rate = 48000;
  snapshot->info.min_frames = 16;
  snapshot->info.max_frames = 1024;
  snapshot->info.preferred_frames = 128;
  ++state.capability_snapshots_alive;
  *out = snapshot;
  return AE_OK;
}

ae_result ae_capability_snapshot_get(const ae_capability_snapshot *snapshot,
                                     ae_device_capabilities *out) {
  *out = snapshot->info;
  out->struct_size = sizeof(*out);
  out->tested_rate_count = static_cast<uint32_t>(snapshot->rates.size());
  out->tested_rates = snapshot->rates.data();
  return AE_OK;
}

ae_result ae_capability_snapshot_get_channel(const ae_capability_snapshot *snapshot,
                                             ae_direction direction, uint32_t index,
                                             ae_channel_info *out) {
  const uint32_t count = direction == AE_DIRECTION_INPUT ? snapshot->info.input_channels
                                                         : snapshot->info.output_channels;
  if (index >= count) {
    return AE_ERROR_INVALID_ARGUMENT;
  }
  *out = {};
  out->struct_size = sizeof(*out);
  out->index = index;
  out->supported = 1;
  const std::string name =
      std::string(direction == AE_DIRECTION_INPUT ? "In " : "Out ") + std::to_string(index + 1);
  std::memset(out->name, 'x', sizeof(out->name));
  std::memcpy(out->name, name.c_str(), name.size() + 1);
  return AE_OK;
}

ae_result ae_capability_snapshot_release(ae_capability_snapshot *snapshot) {
  delete snapshot;
  --state.capability_snapshots_alive;
  return AE_OK;
}

ae_result ae_context_enumerate_devices(const ae_context *, ae_device_snapshot **out) {
  auto *snapshot = new ae_device_snapshot();
  snapshot->strings = {"fake", "dev-a", "Device A", "fake", "dev-b", "Device B"};
  for (size_t i = 0; i < 2; ++i) {
    ae_device_info info{};
    info.struct_size = sizeof(info);
    info.backend_id = snapshot->strings[i * 3].c_str();
    info.device_id = snapshot->strings[i * 3 + 1].c_str();
    info.display_name = snapshot->strings[i * 3 + 2].c_str();
    info.input_channels = state.input_channels;
    info.output_channels = state.output_channels;
    info.channel_counts_known = 1;
    info.flags = i == 0 ? AE_DEVICE_PAIRABLE : 0;
    snapshot->infos.push_back(info);
  }
  ++state.device_snapshots_alive;
  *out = snapshot;
  return AE_OK;
}

uint32_t ae_device_snapshot_count(const ae_device_snapshot *snapshot) {
  return static_cast<uint32_t>(snapshot->infos.size());
}

ae_result ae_device_snapshot_get(const ae_device_snapshot *snapshot, uint32_t index,
                                 ae_device_info *out) {
  if (index >= snapshot->infos.size()) {
    return AE_ERROR_INVALID_ARGUMENT;
  }
  *out = snapshot->infos[index];
  return AE_OK;
}

ae_result ae_device_snapshot_release(ae_device_snapshot *snapshot) {
  delete snapshot;
  --state.device_snapshots_alive;
  return AE_OK;
}

ae_result ae_stream_open(ae_context *, const ae_stream_request *request, ae_stream **out_stream,
                         ae_stream_format *out_format) {
  ++state.open_calls;
  *out_stream = nullptr;
  state.last_request = *request;
  state.requested_input_device = request->input_device_id;
  state.requested_output_device = request->output_device_id;
  state.requested_inputs.assign(request->input_channels.indices,
                                request->input_channels.indices + request->input_channels.count);
  state.requested_outputs.assign(request->output_channels.indices,
                                 request->output_channels.indices + request->output_channels.count);
  if (state.open_result != AE_OK) {
    return state.open_result;
  }
  if (state.streams_alive != 0) {
    return AE_ERROR_BUSY;
  }
  auto *stream = new ae_stream();
  stream->inputs = state.requested_inputs;
  stream->outputs = state.requested_outputs;
  for (uint32_t &output : stream->outputs) {
    output = static_cast<uint32_t>(static_cast<int32_t>(output) + state.output_map_offset);
  }
  const uint32_t input_count = state.negotiated_input_count != 0
                                   ? state.negotiated_input_count
                                   : static_cast<uint32_t>(stream->inputs.size());
  ++state.stream_id;
  ++state.configuration_generation;
  *out_format = {};
  out_format->struct_size = sizeof(*out_format);
  out_format->sample_rate = state.negotiated_rate;
  out_format->frames_per_block = state.negotiated_frames;
  out_format->max_frames_per_block = state.negotiated_max_frames;
  out_format->input_channels = input_count;
  out_format->output_channels = static_cast<uint32_t>(stream->outputs.size());
  out_format->stream_id = state.stream_id;
  out_format->configuration_generation = state.configuration_generation;
  state.state = AE_STREAM_STATE_READY;
  ++state.streams_alive;
  *out_stream = stream;
  return AE_OK;
}

ae_result ae_stream_get_format(const ae_stream *, ae_stream_format *) {
  return AE_ERROR_UNSUPPORTED;
}

ae_result ae_stream_get_channel(const ae_stream *stream, ae_direction direction, uint32_t index,
                                uint32_t *out) {
  const auto &channels = direction == AE_DIRECTION_INPUT ? stream->inputs : stream->outputs;
  if (index >= channels.size()) {
    return AE_ERROR_INVALID_ARGUMENT;
  }
  *out = channels[index];
  return AE_OK;
}

ae_result ae_stream_start(ae_stream *, ae_process_fn process, void *user) {
  if (state.state != AE_STREAM_STATE_READY || state.fault_flags != 0) {
    return AE_ERROR_INVALID_STATE;
  }
  if (state.start_failures > 0) {
    --state.start_failures;
    return AE_ERROR_DRIVER_FAILURE;
  }
  ++state.run_generation;
  state.process = process;
  state.user = user;
  if (state.start_result == AE_ERROR_QUIESCENCE_FAILURE) {
    state.state = AE_STREAM_STATE_RECOVERY_REQUIRED;
    state.last_result = state.start_result;
    return state.start_result;
  }
  if (state.start_result != AE_OK) {
    state.process = nullptr;
    state.user = nullptr;
    return state.start_result;
  }
  state.state = AE_STREAM_STATE_RUNNING;
  return AE_OK;
}

ae_result ae_stream_stop(ae_stream *) {
  ++state.stop_calls;
  if (state.state == AE_STREAM_STATE_READY) {
    return AE_OK;
  }
  if (state.stop_failures > 0) {
    --state.stop_failures;
    state.last_result = AE_ERROR_QUIESCENCE_FAILURE;
    return AE_ERROR_QUIESCENCE_FAILURE;
  }
  state.process = nullptr;
  state.user = nullptr;
  state.state = state.fault_flags == 0 ? AE_STREAM_STATE_READY : AE_STREAM_STATE_RECOVERY_REQUIRED;
  return AE_OK;
}

ae_result ae_stream_get_status(const ae_stream *, ae_stream_status *out) {
  *out = {};
  out->struct_size = sizeof(*out);
  out->state = state.fault_flags != 0 ? AE_STREAM_STATE_RECOVERY_REQUIRED : state.state;
  out->last_result = state.fault_flags != 0 ? AE_ERROR_DEVICE_UNAVAILABLE : state.last_result;
  out->stream_id = state.stream_id;
  out->configuration_generation = state.configuration_generation;
  out->run_generation = state.run_generation;
  out->fault_flags = state.fault_flags;
  return AE_OK;
}

ae_result ae_stream_close(ae_stream *stream) {
  ++state.close_calls;
  if (ae_stream_stop(stream) != AE_OK) {
    return AE_ERROR_QUIESCENCE_FAILURE;
  }
  if (state.close_failures > 0) {
    --state.close_failures;
    return AE_ERROR_DRIVER_FAILURE;
  }
  delete stream;
  --state.streams_alive;
  state.state = AE_STREAM_STATE_READY;
  state.fault_flags = 0;
  state.last_result = AE_OK;
  return AE_OK;
}
}
