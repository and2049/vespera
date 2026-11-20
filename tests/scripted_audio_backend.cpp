#include "scripted_audio_backend.h"

#include "backend.h"
#include "testing_api.h"

#include <algorithm>
#include <memory>
#include <stdexcept>

namespace scripted_ae {

namespace {

Script current_script;
Observed current_observed;
ae::BackendProcessFn installed_process = nullptr;
void *installed_state = nullptr;
uint64_t position = 0;

std::vector<uint32_t> resolve(const ae::ChannelSelection &selection, uint32_t available) {
  if (selection.mode == AE_CHANNEL_MODE_EXPLICIT) {
    return selection.indices;
  }
  std::vector<uint32_t> all;
  if (selection.mode == AE_CHANNEL_MODE_BACKEND_DEFAULT) {
    for (uint32_t index = 0; index < available; ++index) {
      all.push_back(index);
    }
  }
  return all;
}

class ScriptedBackend final : public ae::Backend {
public:
  ScriptedBackend()
      : devices_{{backend_id, device_id, "Scripted Duplex", current_script.input_channels,
                  current_script.output_channels, true, false}} {
    ++current_observed.contexts_alive;
  }
  ~ScriptedBackend() override { --current_observed.contexts_alive; }

  const std::vector<ae::Device> &devices() const override { return devices_; }
  uint32_t faults() const noexcept override { return current_script.faults; }

  ae_result open(const ae::OpenRequest &request, ae::ActualFormat &actual) override {
    ++current_observed.open_calls;
    if (request.device_id != device_id || request.output_device_id != device_id) {
      return AE_ERROR_NOT_FOUND;
    }
    current_observed.requested_inputs = request.inputs.indices;
    current_observed.requested_outputs = request.outputs.indices;
    current_observed.requested_adjustment = request.allow_adjustment;

    actual.sample_rate = current_script.fixed_sample_rate.value_or(request.sample_rate);
    actual.frames_per_block = current_script.fixed_frames.value_or(request.frames_per_block);
    actual.max_frames_per_block = std::max(current_script.max_frames, actual.frames_per_block);
    actual.input_map = resolve(request.inputs, current_script.input_channels);
    actual.output_map = resolve(request.outputs, current_script.output_channels);
    actual.input_channels = static_cast<uint32_t>(actual.input_map.size());
    actual.output_channels = static_cast<uint32_t>(actual.output_map.size());
    current_observed.open = true;
    position = 0;
    return AE_OK;
  }

  ae_result start(ae::BackendProcessFn process, void *engine_state) override {
    ++current_observed.start_calls;
    if (current_script.start_result != AE_OK) {
      return current_script.start_result;
    }
    installed_process = process;
    installed_state = engine_state;
    current_observed.running = true;
    return AE_OK;
  }

  ae_result stop() override {
    ++current_observed.stop_calls;
    if (current_script.stop_failures > 0) {
      --current_script.stop_failures;
      return AE_ERROR_DRIVER_FAILURE;
    }
    installed_process = nullptr;
    installed_state = nullptr;
    current_observed.running = false;
    return AE_OK;
  }

  ae_result close() override {
    ++current_observed.close_calls;
    if (current_script.close_failures > 0) {
      --current_script.close_failures;
      return AE_ERROR_DRIVER_FAILURE;
    }
    current_observed.open = false;
    return AE_OK;
  }

private:
  std::vector<ae::Device> devices_;
};

}

Script &script() { return current_script; }

const Observed &observed() { return current_observed; }

void reset() {
  if (current_observed.contexts_alive != 0) {
    throw std::logic_error("scripted_ae::reset requires every context to be destroyed");
  }
  current_script = {};
  current_observed = {};
  installed_process = nullptr;
  installed_state = nullptr;
  position = 0;
}

namespace {

ae_process_result deliver_channels(const float *const *inputs, uint32_t input_channels, float *left,
                                   float *right, uint32_t frames, float stale,
                                   std::optional<uint64_t> at) {
  if (installed_process == nullptr) {
    throw std::logic_error("scripted_ae::deliver requires a started stream");
  }
  std::fill_n(left, frames, stale);
  std::fill_n(right, frames, stale);
  float *outputs[] = {left, right};
  const uint64_t block_position = at.value_or(position);
  position = block_position + frames;
  return installed_process(installed_state, inputs, input_channels, outputs, 2, frames,
                           block_position);
}

}

ae_process_result deliver(const float *input, float *left, float *right, uint32_t frames,
                          float stale, std::optional<uint64_t> at) {
  const float *inputs[] = {input};
  return deliver_channels(inputs, 1, left, right, frames, stale, at);
}

ae_process_result deliver(const float *left_input, const float *right_input, float *left,
                          float *right, uint32_t frames, float stale, std::optional<uint64_t> at) {
  const float *inputs[] = {left_input, right_input};
  return deliver_channels(inputs, 2, left, right, frames, stale, at);
}

uint64_t next_position() { return position; }

}

extern "C" ae_result ae_context_create(const ae_context_options *options,
                                       ae_context **out_context) {
  if (options == nullptr || out_context == nullptr) {
    return AE_ERROR_INVALID_ARGUMENT;
  }
  *out_context = nullptr;
  if (options->struct_size < sizeof(ae_context_options) ||
      options->api_version_major != AE_API_VERSION_MAJOR ||
      options->api_version_minor != AE_API_VERSION_MINOR) {
    return AE_ERROR_INCOMPATIBLE_ABI;
  }
  return ae::detail::create_context_with_backend(std::make_unique<scripted_ae::ScriptedBackend>(),
                                                 out_context);
}
