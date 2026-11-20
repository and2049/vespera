#include <vespera/audio_session.hpp>

#include "fake_audio_engine.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::atomic<bool> counting_allocations{false};
std::atomic<uint64_t> allocation_count{0};

}

void *operator new(std::size_t size) {
  if (counting_allocations.load(std::memory_order_relaxed)) {
    allocation_count.fetch_add(1, std::memory_order_relaxed);
  }
  if (void *memory = std::malloc(size == 0 ? 1 : size)) {
    return memory;
  }
  throw std::bad_alloc();
}

void *operator new[](std::size_t size) { return operator new(size); }
void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete[](void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void *memory, std::size_t) noexcept { std::free(memory); }

namespace {

using vespera::AudioSession;
using vespera::CloseResult;
using vespera::NodeId;
using vespera::OutputMode;
using vespera::ParameterResult;
using vespera::ParameterToken;
using vespera::RecoveryReason;
using vespera::SessionState;

int failures = 0;
const char *current_test = "";
std::filesystem::path gain_module;

#define CHECK(condition)                                                                           \
  do {                                                                                             \
    if (!(condition)) {                                                                            \
      std::fprintf(stderr, "%s:%d: %s: CHECK(%s) failed\n", __FILE__, __LINE__, current_test,      \
                   #condition);                                                                    \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (false)

template <typename Exception, typename Function> bool throws(Function &&function) {
  try {
    function();
  } catch (const Exception &) {
    return true;
  } catch (...) {
    return false;
  }
  return false;
}

vespera::StreamConfig config(OutputMode mode = OutputMode::silent) {
  vespera::StreamConfig result;
  result.backend_id = "fake";
  result.device_id = "dev-a";
  result.input_channel = 3;
  result.output_channels = {2, 1};
  result.output_mode = mode;
  return result;
}

struct Buffers {
  std::vector<float> input;
  std::vector<float> left;
  std::vector<float> right;
  explicit Buffers(uint32_t frames) : input(frames), left(frames, 9.0f), right(frames, 9.0f) {
    for (uint32_t i = 0; i < frames; ++i) {
      input[i] = 0.001f * static_cast<float>(i + 1);
    }
  }
  ae_process_result run(uint32_t frames, uint64_t position) {
    return fake_ae::invoke(input.data(), left.data(), right.data(), frames, position);
  }
};

vespera::StreamConfig stereo_config(OutputMode mode = OutputMode::monitor) {
  vespera::StreamConfig result = config(mode);
  result.input_mode = vespera::InputMode::stereo;
  result.second_input_channel = 1;
  return result;
}

// Mirrors fake_ae::invoke for a stream opened with two physical inputs.
ae_process_result invoke_stereo(const float *left_input, const float *right_input, float *left,
                                float *right, uint32_t frames, uint64_t position) {
  auto &state = fake_ae::control();
  const float *inputs[] = {left_input, right_input};
  float *outputs[] = {left, right};
  std::fill_n(left, frames, 0.0f);
  std::fill_n(right, frames, 0.0f);
  ae_audio_block block{};
  block.struct_size = sizeof(block);
  block.inputs = inputs;
  block.input_channels = 2;
  block.outputs = outputs;
  block.output_channels = 2;
  block.frames = frames;
  block.stream_id = state.stream_id;
  block.configuration_generation = state.configuration_generation;
  block.run_generation = state.run_generation;
  block.frame_position = position;
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

struct StereoBuffers {
  std::vector<float> left_input;
  std::vector<float> right_input;
  std::vector<float> left;
  std::vector<float> right;
  explicit StereoBuffers(uint32_t frames)
      : left_input(frames), right_input(frames), left(frames, 9.0f), right(frames, 9.0f) {
    for (uint32_t i = 0; i < frames; ++i) {
      left_input[i] = 0.001f * static_cast<float>(i + 1);
      right_input[i] = -0.0005f * static_cast<float>(i + 1);
    }
  }
  ae_process_result run(uint32_t frames, uint64_t position) {
    return invoke_stereo(left_input.data(), right_input.data(), left.data(), right.data(), frames,
                         position);
  }
};

double rms_of(const std::vector<float> &samples, uint32_t begin, uint32_t end) {
  double sum = 0.0;
  for (uint32_t i = begin; i < end; ++i) {
    sum += static_cast<double>(samples[i]) * samples[i];
  }
  return std::sqrt(sum / (end - begin));
}

bool near(double actual, double expected) {
  return std::fabs(actual - expected) <= 1e-6 * std::max(1.0, std::fabs(expected));
}

bool meter_silent(const vespera::ChannelMeter &meter) {
  return meter.peak == 0.0f && meter.rms == 0.0f && meter.clipped_samples == 0;
}

void test_enumerate_copies_and_releases() {
  AudioSession session;
  const auto devices = session.devices();
  CHECK(devices.size() == 2);
  CHECK(devices[0].device_id == "dev-a");
  CHECK(devices[0].display_name == "Device A");
  CHECK(devices[0].pairable);
  CHECK(!devices[1].pairable);
  CHECK(devices[1].input_channels == 4);
  CHECK(fake_ae::control().device_snapshots_alive == 0);
}

void test_probe() {
  AudioSession session;
  const auto devices = session.devices();
  const auto caps = session.probe(devices[0]);
  CHECK(caps.tested_rates == (std::vector<uint32_t>{44100, 48000, 96000}));
  CHECK(caps.inputs.size() == 2);
  CHECK(caps.outputs.size() == 2);
  CHECK(caps.inputs[1].name == "In 2");
  CHECK(caps.preferred_frames == 128);
  CHECK(fake_ae::control().capability_snapshots_alive == 0);
  CHECK(throws<vespera::AudioError>([&] { session.probe(devices[1]); }));
  CHECK(fake_ae::control().capability_snapshots_alive == 0);

  session.prepare(config());
  CHECK(throws<std::logic_error>([&] { session.probe(devices[0]); }));
  CHECK(session.close() == CloseResult::closed);
}

void test_prepare_uses_explicit_request_and_negotiated_maximum() {
  AudioSession session;
  session.prepare(config());
  const auto &request = fake_ae::control().last_request;
  CHECK(request.input_channels.mode == AE_CHANNEL_MODE_EXPLICIT);
  CHECK(request.output_channels.mode == AE_CHANNEL_MODE_EXPLICIT);
  CHECK(fake_ae::control().requested_inputs == std::vector<uint32_t>{3});
  CHECK(fake_ae::control().requested_outputs == (std::vector<uint32_t>{2, 1}));
  CHECK(request.sample_rate == 48000);
  CHECK(request.frames_per_block == 128);
  CHECK(request.negotiation == AE_NEGOTIATION_EXACT);

  CHECK(session.state() == SessionState::prepared);
  const auto &info = session.stream_info();
  CHECK(info.max_frames_per_block == 256);
  CHECK(info.physical_input == 3);
  CHECK(info.input_mode == vespera::InputMode::mono);
  CHECK(info.input_channels == 1);
  CHECK((info.physical_inputs == std::array<uint32_t, 2>{3, 0}));
  CHECK(info.physical_outputs[0] == 2);
  CHECK(throws<std::logic_error>([&] { session.graph(); }));
  CHECK(throws<std::logic_error>([&] { session.prepare(config()); }));

  session.start();
  Buffers buffers(257);
  CHECK(buffers.run(256, 0) == AE_PROCESS_SILENCE);
  CHECK(buffers.run(257, 256) == AE_PROCESS_FAILED);
  session.stop();
  CHECK(session.state() == SessionState::recovery_required);
  CHECK(session.recovery_reason() == RecoveryReason::processing_failure);
  CHECK(session.poll().telemetry.contract_violations == 1);
  CHECK(session.close() == CloseResult::closed);
  CHECK(session.state() == SessionState::stopped);
  CHECK(fake_ae::control().streams_alive == 0);
  session.graph();
}

void test_prepare_rejects_unusable_layout() {
  AudioSession session;
  fake_ae::control().negotiated_input_count = 2;
  CHECK(throws<vespera::AudioError>([&] { session.prepare(config()); }));
  CHECK(session.state() == SessionState::stopped);
  CHECK(fake_ae::control().streams_alive == 0);

  fake_ae::control().negotiated_input_count = 0;
  fake_ae::control().output_map_offset = 1;
  CHECK(throws<vespera::AudioError>([&] { session.prepare(config()); }));
  CHECK(session.state() == SessionState::stopped);
  CHECK(fake_ae::control().streams_alive == 0);

  fake_ae::control().output_map_offset = 0;
  fake_ae::control().open_result = AE_ERROR_UNSUPPORTED_CONFIGURATION;
  try {
    session.prepare(config());
    CHECK(false);
  } catch (const vespera::AudioError &error) {
    CHECK(error.code() == AE_ERROR_UNSUPPORTED_CONFIGURATION);
  }
  CHECK(session.state() == SessionState::stopped);

  auto same_outputs = config();
  same_outputs.output_channels = {1, 1};
  CHECK(throws<std::invalid_argument>([&] { session.prepare(same_outputs); }));
}

void test_silent_and_monitor_processing() {
  AudioSession session;
  session.prepare(config(OutputMode::silent));
  session.start();
  Buffers buffers(64);
  counting_allocations.store(true);
  allocation_count.store(0);
  const auto silent = buffers.run(64, 0);
  counting_allocations.store(false);
  CHECK(allocation_count.load() == 0);
  CHECK(silent == AE_PROCESS_SILENCE);
  CHECK(buffers.left[10] == 0.0f && buffers.right[63] == 0.0f);
  auto status = session.poll();
  CHECK(status.telemetry.blocks == 1);
  CHECK(status.telemetry.input_peak > 0.063f);
  CHECK(status.telemetry.output_peak > 0.063f);
  session.stop();
  CHECK(session.state() == SessionState::prepared);
  CHECK(session.close() == CloseResult::closed);

  session.prepare(config(OutputMode::monitor));
  session.start();
  CHECK(buffers.run(64, 0) == AE_PROCESS_OK);
  CHECK(buffers.left[10] == buffers.input[10]);
  CHECK(buffers.right[10] == buffers.input[10]);
  CHECK(session.close() == CloseResult::closed);
}

void test_discontinuities() {
  AudioSession session;
  session.prepare(config());
  session.start();
  Buffers buffers(128);
  buffers.run(128, 0);
  buffers.run(128, 128);
  buffers.run(64, 256);
  CHECK(session.poll().telemetry.discontinuities == 1);
  buffers.run(64, 400);
  CHECK(session.poll().telemetry.discontinuities == 2);
  session.stop();
  session.start();
  buffers.run(64, 464);
  const auto status = session.poll();
  CHECK(status.telemetry.discontinuities == 1);
  CHECK(status.telemetry.blocks == 1);

  ++fake_ae::control().stream_id;
  CHECK(buffers.run(64, 528) == AE_PROCESS_FAILED);
  CHECK(session.poll().state == SessionState::recovery_required);
  CHECK(session.recovery_reason() == RecoveryReason::processing_failure);
  CHECK(session.close() == CloseResult::closed);
}

void test_stop_failure_retains_callback_state() {
  AudioSession session;
  session.prepare(config(OutputMode::monitor));
  session.start();
  fake_ae::control().stop_failures = 2;
  CHECK(throws<vespera::AudioError>([&] { session.stop(); }));
  CHECK(session.state() == SessionState::recovery_required);
  CHECK(session.recovery_reason() == RecoveryReason::quiescence_failure);
  CHECK(session.close() == CloseResult::stop_failed);
  CHECK(session.recovery_reason() == RecoveryReason::quiescence_failure);

  Buffers buffers(32);
  CHECK(buffers.run(32, 0) == AE_PROCESS_OK);
  CHECK(buffers.left[5] == buffers.input[5]);

  CHECK(session.close() == CloseResult::closed);
  CHECK(session.state() == SessionState::stopped);
  CHECK(fake_ae::control().streams_alive == 0);
}

void test_destructor_quarantines_unstoppable_callback() {
  {
    auto session = std::make_unique<AudioSession>();
    session->prepare(config(OutputMode::monitor));
    session->start();
    fake_ae::control().stop_failures = 1000;
    session.reset();
  }
  CHECK(fake_ae::control().process != nullptr);
  CHECK(fake_ae::control().context_destroy_calls == 0);
  Buffers buffers(32);
  CHECK(buffers.run(32, 0) == AE_PROCESS_OK);
  CHECK(buffers.right[7] == buffers.input[7]);
}

void test_close_failure_is_retryable() {
  AudioSession session;
  session.prepare(config());
  session.start();
  fake_ae::control().close_failures = 1;
  CHECK(session.close() == CloseResult::close_failed);
  CHECK(session.state() == SessionState::recovery_required);
  CHECK(session.recovery_reason() == RecoveryReason::close_failure);
  CHECK(fake_ae::control().process == nullptr);
  CHECK(throws<std::logic_error>([&] { session.prepare(config()); }));
  CHECK(session.close() == CloseResult::closed);
  session.prepare(config());
  CHECK(session.close() == CloseResult::closed);
}

void test_destructor_after_close_failure_leaks_only_handles() {
  {
    AudioSession session;
    session.prepare(config());
    fake_ae::control().close_failures = 1000;
  }
  CHECK(fake_ae::control().context_destroy_calls == 1);
  CHECK(fake_ae::control().contexts_alive == 1);
  CHECK(fake_ae::control().streams_alive == 1);
}

void test_start_quiescence_failure() {
  AudioSession session;
  session.prepare(config());
  fake_ae::control().start_result = AE_ERROR_QUIESCENCE_FAILURE;
  fake_ae::control().stop_failures = 1;
  CHECK(throws<vespera::AudioError>([&] { session.start(); }));
  CHECK(session.state() == SessionState::recovery_required);
  CHECK(session.recovery_reason() == RecoveryReason::quiescence_failure);
  CHECK(session.close() == CloseResult::stop_failed);
  CHECK(session.close() == CloseResult::closed);
}

void test_start_error_keeps_prepared() {
  AudioSession session;
  session.prepare(config());
  fake_ae::control().start_result = AE_ERROR_DEVICE_UNAVAILABLE;
  CHECK(throws<vespera::AudioError>([&] { session.start(); }));
  CHECK(session.state() == SessionState::prepared);
  fake_ae::control().start_result = AE_OK;
  session.start();
  CHECK(session.state() == SessionState::running);
  CHECK(session.close() == CloseResult::closed);
}

void test_poll_surfaces_stream_fault() {
  AudioSession session;
  session.prepare(config());
  session.start();
  fake_ae::control().fault_flags = AE_FAULT_DEVICE;
  const auto status = session.poll();
  CHECK(status.state == SessionState::recovery_required);
  CHECK(status.recovery == RecoveryReason::stream_fault);
  CHECK(status.stream.fault_flags == AE_FAULT_DEVICE);
  CHECK(status.last_error == AE_ERROR_DEVICE_UNAVAILABLE);
  CHECK(throws<std::logic_error>([&] { session.start(); }));
  session.stop();
  CHECK(session.state() == SessionState::recovery_required);
  CHECK(session.close() == CloseResult::closed);
  CHECK(session.state() == SessionState::stopped);
  CHECK(session.recovery_reason() == RecoveryReason::none);
}

// Prepares and starts a monitored session with one gain node, whose parameter 1 is linear gain.
NodeId start_gain_session(AudioSession &session) {
  const NodeId node = session.graph().add_module(gain_module);
  session.prepare(config(OutputMode::monitor));
  session.start();
  return node;
}

// True when output frames [begin, end) equal the input scaled by gain on both channels.
bool gained(const Buffers &buffers, uint32_t begin, uint32_t end, float gain) {
  for (uint32_t i = begin; i < end; ++i) {
    const float expected = buffers.input[i] * gain;
    if (buffers.left[i] != expected || buffers.right[i] != expected) {
      return false;
    }
  }
  return true;
}

void test_parameter_immediate_and_offsets() {
  AudioSession session;
  const NodeId node = start_gain_session(session);
  const ParameterToken token = session.parameter_token();
  CHECK(token.graph_generation != 0 && token.timeline_generation != 0);
  Buffers buffers(64);
  CHECK(session.set_parameter(token, node, 1, 2.0f) == ParameterResult::accepted);
  CHECK(buffers.run(64, 0) == AE_PROCESS_OK);
  CHECK(gained(buffers, 0, 64, 2.0f));

  CHECK(session.set_parameter(token, node, 1, 3.0f, 64 + 10) == ParameterResult::accepted);
  CHECK(buffers.run(64, 64) == AE_PROCESS_OK);
  CHECK(gained(buffers, 0, 10, 2.0f));
  CHECK(gained(buffers, 10, 64, 3.0f));
  const auto telemetry = session.poll().telemetry;
  CHECK(telemetry.parameters.accepted == 2);
  CHECK(telemetry.parameters.applied == 2);
  CHECK(telemetry.parameters.late == 0);
  CHECK(telemetry.parameters.stale == 0);
  CHECK(telemetry.next_frame_position == 128);
  CHECK(session.close() == CloseResult::closed);
}

void test_parameter_future_does_not_block_and_equal_order() {
  AudioSession session;
  const NodeId node = start_gain_session(session);
  const ParameterToken token = session.parameter_token();
  Buffers buffers(64);
  CHECK(buffers.run(64, 0) == AE_PROCESS_OK);
  CHECK(session.set_parameter(token, node, 1, 0.5f, 1000) == ParameterResult::accepted);
  CHECK(session.set_parameter(token, node, 1, 2.0f) == ParameterResult::accepted);
  CHECK(buffers.run(64, 64) == AE_PROCESS_OK);
  CHECK(gained(buffers, 0, 64, 2.0f));

  CHECK(session.set_parameter(token, node, 1, 3.0f, 128 + 5) == ParameterResult::accepted);
  CHECK(session.set_parameter(token, node, 1, 0.25f, 128 + 5) == ParameterResult::accepted);
  CHECK(buffers.run(64, 128) == AE_PROCESS_OK);
  CHECK(gained(buffers, 0, 5, 2.0f));
  CHECK(gained(buffers, 5, 64, 0.25f));

  for (uint64_t position = 192; position < 960; position += 64) {
    CHECK(buffers.run(64, position) == AE_PROCESS_OK);
  }
  CHECK(gained(buffers, 0, 64, 0.25f));
  CHECK(buffers.run(64, 960) == AE_PROCESS_OK);
  CHECK(gained(buffers, 0, 40, 0.25f));
  CHECK(gained(buffers, 40, 64, 0.5f));
  CHECK(session.poll().telemetry.parameters.applied == 4);
  CHECK(session.close() == CloseResult::closed);
}

void test_parameter_late_and_block_end() {
  AudioSession session;
  const NodeId node = start_gain_session(session);
  const ParameterToken token = session.parameter_token();
  Buffers buffers(64);
  CHECK(buffers.run(64, 0) == AE_PROCESS_OK);
  CHECK(session.set_parameter(token, node, 1, 2.0f, 10) == ParameterResult::accepted);
  CHECK(buffers.run(64, 64) == AE_PROCESS_OK);
  CHECK(gained(buffers, 0, 64, 2.0f));
  CHECK(session.poll().telemetry.parameters.late == 1);

  CHECK(session.set_parameter(token, node, 1, 3.0f, 192) == ParameterResult::accepted);
  CHECK(buffers.run(64, 128) == AE_PROCESS_OK);
  CHECK(gained(buffers, 0, 64, 2.0f));
  CHECK(buffers.run(64, 192) == AE_PROCESS_OK);
  CHECK(gained(buffers, 0, 64, 3.0f));
  const auto telemetry = session.poll().telemetry.parameters;
  CHECK(telemetry.late == 1);
  CHECK(telemetry.applied == 2);
  CHECK(session.close() == CloseResult::closed);
}

void test_parameter_capacity_includes_pending() {
  AudioSession session;
  const NodeId node = start_gain_session(session);
  const ParameterToken token = session.parameter_token();
  Buffers buffers(64);
  for (uint32_t i = 0; i < 200; ++i) {
    CHECK(session.set_parameter(token, node, 1, 1.0f, 100000 + i) == ParameterResult::accepted);
  }
  CHECK(buffers.run(64, 0) == AE_PROCESS_OK);
  for (uint32_t i = 200; i < vespera::ParameterQueue::capacity; ++i) {
    CHECK(session.set_parameter(token, node, 1, 1.0f, 100000) == ParameterResult::accepted);
  }
  CHECK(session.set_parameter(token, node, 1, 2.0f) == ParameterResult::queue_full);
  CHECK(buffers.run(64, 64) == AE_PROCESS_OK);
  CHECK(session.set_parameter(token, node, 1, 2.0f) == ParameterResult::queue_full);
  CHECK(gained(buffers, 0, 64, 1.0f));
  const auto telemetry = session.poll().telemetry.parameters;
  CHECK(telemetry.queue_full == 2);
  CHECK(telemetry.rejected == 2);
  CHECK(telemetry.accepted == vespera::ParameterQueue::capacity);
  CHECK(session.close() == CloseResult::closed);
}

void test_parameter_tokens_expire_on_stop_and_prepare() {
  AudioSession session;
  const NodeId node = start_gain_session(session);
  const ParameterToken first = session.parameter_token();
  Buffers buffers(64);
  CHECK(buffers.run(64, 0) == AE_PROCESS_OK);
  CHECK(session.set_parameter(first, node, 1, 3.0f, 100) == ParameterResult::accepted);
  CHECK(session.set_parameter(first, node, 1, 2.0f) == ParameterResult::accepted);
  session.stop();
  CHECK(session.parameter_token() == ParameterToken{});
  CHECK(session.set_parameter(first, node, 1, 2.0f) == ParameterResult::not_running);

  session.start();
  const ParameterToken second = session.parameter_token();
  CHECK(second.graph_generation == first.graph_generation);
  CHECK(second.timeline_generation != first.timeline_generation);
  CHECK(session.set_parameter(first, node, 1, 2.0f) == ParameterResult::stale_token);
  CHECK(buffers.run(64, 64) == AE_PROCESS_OK);
  CHECK(buffers.run(64, 128) == AE_PROCESS_OK);
  CHECK(gained(buffers, 0, 64, 1.0f));
  CHECK(session.poll().telemetry.parameters.applied == 0);

  CHECK(session.close() == CloseResult::closed);
  session.prepare(config(OutputMode::monitor));
  session.start();
  const ParameterToken third = session.parameter_token();
  CHECK(third.graph_generation != first.graph_generation);
  CHECK(session.set_parameter(second, node, 1, 2.0f) == ParameterResult::stale_token);
  CHECK(session.set_parameter({third.graph_generation, second.timeline_generation}, node, 1,
                              2.0f) == ParameterResult::stale_token);
  CHECK(session.set_parameter(third, node, 1, 2.0f) == ParameterResult::accepted);
  CHECK(buffers.run(64, 0) == AE_PROCESS_OK);
  CHECK(gained(buffers, 0, 64, 2.0f));
  CHECK(session.close() == CloseResult::closed);
}

void test_parameter_discontinuity_invalidates_timeline() {
  AudioSession session;
  const NodeId node = start_gain_session(session);
  const ParameterToken first = session.parameter_token();
  Buffers buffers(64);
  CHECK(session.set_parameter(first, node, 1, 2.0f, 100) == ParameterResult::accepted);
  CHECK(buffers.run(64, 0) == AE_PROCESS_OK);
  CHECK(session.set_parameter(first, node, 1, 3.0f) == ParameterResult::accepted);
  CHECK(buffers.run(64, 500) == AE_PROCESS_OK);
  CHECK(gained(buffers, 0, 64, 1.0f));
  auto telemetry = session.poll().telemetry;
  CHECK(telemetry.discontinuities == 2);
  CHECK(telemetry.parameters.stale == 2);
  CHECK(telemetry.parameters.applied == 0);

  const ParameterToken second = session.parameter_token();
  CHECK(second.timeline_generation != first.timeline_generation);
  CHECK(session.set_parameter(first, node, 1, 2.0f) == ParameterResult::stale_token);
  CHECK(session.set_parameter(second, node, 1, 2.0f) == ParameterResult::accepted);
  CHECK(buffers.run(64, 564) == AE_PROCESS_OK);
  CHECK(gained(buffers, 0, 64, 2.0f));

  CHECK(session.set_parameter(second, node, 1, 3.0f, 700) == ParameterResult::accepted);
  ++fake_ae::control().run_generation;
  CHECK(buffers.run(64, 628) == AE_PROCESS_OK);
  CHECK(buffers.run(64, 692) == AE_PROCESS_OK);
  CHECK(gained(buffers, 0, 64, 2.0f));
  CHECK(session.parameter_token().timeline_generation != second.timeline_generation);
  telemetry = session.poll().telemetry;
  CHECK(telemetry.parameters.stale == 3);
  CHECK(telemetry.parameters.applied == 1);
  CHECK(session.close() == CloseResult::closed);
}

void test_parameter_validation_and_metadata() {
  AudioSession session;
  const NodeId node = session.graph().add_module(gain_module);
  const auto stopped_nodes = session.nodes();
  CHECK(stopped_nodes.size() == 1);
  session.prepare(config(OutputMode::monitor));
  CHECK(session.parameter_token() == ParameterToken{});
  CHECK(session.set_parameter({}, node, 1, 2.0f) == ParameterResult::not_running);
  session.start();

  const auto nodes = session.nodes();
  CHECK(nodes.size() == 1);
  if (nodes.size() == 1) {
    CHECK(nodes[0].instance_id == node);
    CHECK(nodes[0].module.id == "dev.vespera.gain");
    CHECK(nodes[0].module.parameters.size() == 1);
    CHECK(nodes[0].module.parameters[0].id == 1);
    CHECK(nodes[0].module.parameters[0].maximum == 4.0f);
  }

  const ParameterToken token = session.parameter_token();
  constexpr float infinity = std::numeric_limits<float>::infinity();
  CHECK(session.set_parameter({}, node, 1, 2.0f) == ParameterResult::stale_token);
  CHECK(session.set_parameter(token, node + 1, 1, 2.0f) == ParameterResult::unknown_node);
  CHECK(session.set_parameter(token, vespera::invalid_node_id, 1, 2.0f) ==
        ParameterResult::unknown_node);
  CHECK(session.set_parameter(token, node, 2, 2.0f) == ParameterResult::unknown_parameter);
  CHECK(session.set_parameter(token, node, 1, 4.5f) == ParameterResult::invalid_value);
  CHECK(session.set_parameter(token, node, 1, -0.1f) == ParameterResult::invalid_value);
  CHECK(session.set_parameter(token, node, 1, std::nanf("")) == ParameterResult::invalid_value);
  CHECK(session.set_parameter(token, node, 1, infinity) == ParameterResult::invalid_value);
  CHECK(session.set_parameter(token, node, 1, 4.0f) == ParameterResult::accepted);
  CHECK(session.set_parameter(token, node, 1, 0.0f) == ParameterResult::accepted);
  Buffers buffers(64);
  CHECK(buffers.run(64, 0) == AE_PROCESS_OK);
  CHECK(gained(buffers, 0, 64, 0.0f));
  const auto telemetry = session.poll().telemetry.parameters;
  CHECK(telemetry.rejected == 8);
  CHECK(telemetry.accepted == 2);
  CHECK(telemetry.applied == 2);
  CHECK(std::string(vespera::to_string(ParameterResult::queue_full)) == "queue_full");
  CHECK(session.close() == CloseResult::closed);
}

void test_parameter_not_applied_when_processing_fails() {
  AudioSession session;
  const NodeId node = start_gain_session(session);
  const ParameterToken token = session.parameter_token();
  Buffers buffers(64);
  buffers.input[3] = std::numeric_limits<float>::quiet_NaN();
  CHECK(session.set_parameter(token, node, 1, 2.0f) == ParameterResult::accepted);
  CHECK(buffers.run(64, 0) == AE_PROCESS_FAILED);
  const auto telemetry = session.poll().telemetry;
  CHECK(telemetry.processing_failures == 1);
  CHECK(telemetry.parameters.applied == 0);
  CHECK(session.close() == CloseResult::closed);
}

void test_parameter_processing_does_not_allocate() {
  AudioSession session;
  const NodeId node = start_gain_session(session);
  const ParameterToken token = session.parameter_token();
  Buffers buffers(64);
  CHECK(buffers.run(64, 0) == AE_PROCESS_OK);
  counting_allocations.store(true);
  allocation_count.store(0);
  for (uint32_t i = 0; i < vespera::ParameterQueue::capacity; ++i) {
    session.set_parameter(token, node, 1, static_cast<float>(i % 4),
                          i % 2 == 0 ? std::optional<uint64_t>(64 + (i * 7) % 128)
                                     : std::optional<uint64_t>());
  }
  const auto first = buffers.run(64, 64);
  const auto second = buffers.run(64, 128);
  const auto gap = buffers.run(64, 1000);
  counting_allocations.store(false);
  CHECK(allocation_count.load() == 0);
  CHECK(first == AE_PROCESS_OK && second == AE_PROCESS_OK && gap == AE_PROCESS_OK);
  CHECK(session.poll().telemetry.parameters.applied == vespera::ParameterQueue::capacity);
  CHECK(session.close() == CloseResult::closed);
}

vespera::Preset gain_preset(float value) {
  vespera::Graph graph;
  const auto node = graph.add_module(gain_module);
  graph.prepare({48000.0, 1, 2});
  vespera::MonoStereoAdapter adapter(graph, 1);
  const vespera::Event event{node, {1, 0, value}};
  CHECK(adapter.process(nullptr, 1, 0, VS_PROCESS_OFFLINE, {&event, 1}) == VS_OK);
  return graph.capture_preset();
}

void test_preset_capture_and_restore() {
  vespera::ModuleCatalog catalog;
  catalog.add(gain_module);
  AudioSession session;
  const auto node = start_gain_session(session);
  const auto old_token = session.parameter_token();
  CHECK(session.set_parameter(old_token, node, 1, 0.5f) == ParameterResult::accepted);
  Buffers buffers(64);
  CHECK(buffers.run(64, 0) == AE_PROCESS_OK);
  CHECK(session.set_parameter(old_token, node, 1, 3.0f, 1000) == ParameterResult::accepted);
  CHECK(throws<std::logic_error>([&] { session.capture_preset(); }));
  auto replacement = gain_preset(0.25f);
  replacement.nodes[0].instance_id = 12;
  replacement.next_instance_id = 40;
  CHECK(throws<std::logic_error>(
      [&] { session.restore_preset(replacement, catalog, config(OutputMode::monitor)); }));
  session.stop();
  const auto saved = session.capture_preset();
  CHECK(saved.nodes[0].state.bytes == gain_preset(0.5f).nodes[0].state.bytes);
  CHECK(session.close() == CloseResult::closed);
  session.restore_preset(replacement, catalog, config(OutputMode::monitor));
  CHECK(session.state() == SessionState::prepared);
  CHECK(session.nodes()[0].instance_id == 12);
  session.start();
  CHECK(session.set_parameter(old_token, 12, 1, 4.0f) == ParameterResult::stale_token);
  CHECK(buffers.run(64, 0) == AE_PROCESS_OK);
  CHECK(std::fabs(buffers.left[63] - buffers.input[63] * 0.25f) < 1e-7f);
  CHECK(session.poll().telemetry.parameters.applied == 0);
  session.stop();
  CHECK(session.capture_preset().next_instance_id == 40);
  CHECK(session.close() == CloseResult::closed);
  CHECK(session.graph().add_module(gain_module) == 40);
}

void test_preset_restore_rolls_back() {
  vespera::ModuleCatalog catalog;
  catalog.add(gain_module);
  AudioSession session;
  session.graph().add_module(gain_module);
  const auto original = session.capture_preset();
  auto replacement = gain_preset(0.25f);
  replacement.nodes[0].instance_id = 9;
  replacement.next_instance_id = 10;

  vespera::ModuleCatalog missing;
  CHECK(
      throws<std::runtime_error>([&] { session.restore_preset(replacement, missing, config()); }));
  CHECK(fake_ae::control().open_calls == 0);
  auto malformed = replacement;
  malformed.nodes[0].state.bytes.clear();
  CHECK(throws<std::runtime_error>([&] { session.restore_preset(malformed, catalog, config()); }));
  CHECK(fake_ae::control().open_calls == 0);

  fake_ae::control().open_result = AE_ERROR_DEVICE_UNAVAILABLE;
  CHECK(
      throws<vespera::AudioError>([&] { session.restore_preset(replacement, catalog, config()); }));
  CHECK(session.state() == SessionState::stopped);
  fake_ae::control().open_result = AE_OK;
  fake_ae::control().negotiated_rate = 0;
  CHECK(throws<std::invalid_argument>(
      [&] { session.restore_preset(replacement, catalog, config()); }));
  CHECK(session.state() == SessionState::stopped);
  CHECK(fake_ae::control().streams_alive == 0);
  CHECK(session.nodes().size() == 1 && session.nodes()[0].instance_id == 0);
  CHECK(session.capture_preset().nodes[0].state.bytes == original.nodes[0].state.bytes);

  fake_ae::control().close_failures = 1;
  CHECK(throws<std::invalid_argument>(
      [&] { session.restore_preset(replacement, catalog, config()); }));
  CHECK(session.state() == SessionState::recovery_required);
  CHECK(session.nodes()[0].instance_id == 0);
  CHECK(throws<std::logic_error>([&] { session.capture_preset(); }));
  CHECK(session.close() == CloseResult::closed);
  CHECK(session.capture_preset().nodes[0].state.bytes == original.nodes[0].state.bytes);

  fake_ae::control().negotiated_rate = 48000;
  session.prepare(config(OutputMode::monitor));
  session.start();
  Buffers buffers(64);
  CHECK(buffers.run(64, 0) == AE_PROCESS_OK);
  CHECK(buffers.left == buffers.input);
  CHECK(session.close() == CloseResult::closed);
}

void test_capture_requires_quiescence() {
  AudioSession session;
  start_gain_session(session);
  fake_ae::control().stop_failures = 1;
  CHECK(throws<vespera::AudioError>([&] { session.stop(); }));
  CHECK(throws<std::logic_error>([&] { session.capture_preset(); }));
  CHECK(session.close() == CloseResult::closed);
  CHECK(session.capture_preset().nodes.size() == 1);
}

void test_parameter_values_require_quiescence() {
  AudioSession session;
  const NodeId node = session.graph().add_module(gain_module);
  CHECK(session.parameter_value(node, 1) == 1.0f);
  session.set_parameter_value(node, 1, 2.0f);
  session.prepare(config(OutputMode::monitor));
  CHECK(session.parameter_value(node, 1) == 2.0f);
  session.set_parameter_value(node, 1, 0.5f);
  CHECK(throws<std::invalid_argument>([&] { session.set_parameter_value(node, 1, 5.0f); }));
  CHECK(throws<std::invalid_argument>([&] { session.parameter_value(node + 1, 1); }));

  session.start();
  CHECK(throws<std::logic_error>([&] { session.parameter_value(node, 1); }));
  CHECK(throws<std::logic_error>([&] { session.set_parameter_value(node, 1, 1.0f); }));
  Buffers buffers(64);
  CHECK(buffers.run(64, 0) == AE_PROCESS_OK);
  CHECK(gained(buffers, 0, 64, 0.5f));
  const ParameterToken token = session.parameter_token();
  CHECK(session.set_parameter(token, node, 1, 3.0f) == ParameterResult::accepted);
  CHECK(buffers.run(64, 64) == AE_PROCESS_OK);
  CHECK(session.set_parameter(token, node, 1, 1.5f, 1000) == ParameterResult::accepted);
  CHECK(session.set_parameter(token, node, 1, 4.0f) == ParameterResult::accepted);
  session.stop();
  CHECK(session.state() == SessionState::prepared);
  CHECK(session.parameter_value(node, 1) == 3.0f);
  CHECK(session.capture_preset().nodes[0].state.bytes == gain_preset(3.0f).nodes[0].state.bytes);

  session.set_parameter_value(node, 1, 0.25f);
  session.start();
  for (uint64_t position = 0; position < 1088; position += 64) {
    CHECK(buffers.run(64, position) == AE_PROCESS_OK);
  }
  CHECK(gained(buffers, 0, 64, 0.25f));
  fake_ae::control().stop_failures = 1;
  CHECK(throws<vespera::AudioError>([&] { session.stop(); }));
  CHECK(session.state() == SessionState::recovery_required);
  CHECK(throws<std::logic_error>([&] { session.parameter_value(node, 1); }));
  CHECK(throws<std::logic_error>([&] { session.set_parameter_value(node, 1, 1.0f); }));
  CHECK(session.close() == CloseResult::closed);
  CHECK(session.parameter_value(node, 1) == 0.25f);
}

void test_adapter_offline() {
  vespera::Graph graph;
  graph.prepare(vespera::Format{48000.0, 16, 2});
  vespera::MonoStereoAdapter adapter(graph, 16);
  std::vector<float> input(16, 0.5f);
  CHECK(adapter.process(input.data(), 16, 0, VS_PROCESS_OFFLINE | VS_PROCESS_DISCONTINUITY) ==
        VS_OK);
  CHECK(adapter.output(0)[15] == 0.5f && adapter.output(1)[0] == 0.5f);
  CHECK(adapter.process(nullptr, 4, 16, VS_PROCESS_OFFLINE) == VS_OK);
  CHECK(adapter.output(1)[3] == 0.0f);
  CHECK(adapter.process(input.data(), 17, 20, 0) == VS_ERROR_ARGUMENT);
  CHECK(adapter.process(input.data(), 0, 20, 0) == VS_ERROR_ARGUMENT);
  CHECK(adapter.output(2) == nullptr);
  CHECK(throws<std::invalid_argument>([&] { vespera::MonoStereoAdapter(graph, 0); }));
  graph.release();
}

void test_adapter_planar_input() {
  vespera::Graph stereo;
  stereo.prepare(vespera::Format{48000.0, 8, 2});
  vespera::MonoStereoAdapter adapter(stereo, 8);
  std::vector<float> left(8, 0.25f);
  std::vector<float> right(8, -0.5f);
  const std::array<const float *, 2> pair{left.data(), right.data()};
  CHECK(adapter.process(pair, 8, 0, VS_PROCESS_OFFLINE) == VS_OK);
  CHECK(adapter.output(0)[7] == 0.25f && adapter.output(1)[7] == -0.5f);

  const std::array<const float *, 2> right_only{nullptr, right.data()};
  CHECK(adapter.process(right_only, 4, 8, VS_PROCESS_OFFLINE) == VS_OK);
  CHECK(adapter.output(0)[3] == 0.0f && adapter.output(1)[3] == -0.5f);

  const std::array<const float *, 1> mono{left.data()};
  CHECK(adapter.process(mono, 8, 12, VS_PROCESS_OFFLINE) == VS_OK);
  CHECK(adapter.output(0)[0] == 0.25f && adapter.output(1)[0] == 0.25f);
  CHECK(adapter.process(left.data(), 8, 20, VS_PROCESS_OFFLINE) == VS_OK);
  CHECK(adapter.output(1)[5] == 0.25f);

  const std::array<const float *, 3> three{left.data(), right.data(), left.data()};
  CHECK(adapter.process(three, 8, 28, 0) == VS_ERROR_ARGUMENT);
  CHECK(adapter.output(0)[0] == 0.0f && adapter.output(1)[7] == 0.0f);
  CHECK(adapter.process(std::span<const float *const>(), 8, 28, 0) == VS_ERROR_ARGUMENT);
  CHECK(adapter.process(pair, 9, 28, 0) == VS_ERROR_ARGUMENT);
  stereo.release();

  vespera::Graph mono_graph;
  mono_graph.prepare(vespera::Format{48000.0, 8, 1});
  vespera::MonoStereoAdapter mono_adapter(mono_graph, 8, 1);
  CHECK(mono_adapter.process(mono, 8, 0, VS_PROCESS_OFFLINE) == VS_OK);
  CHECK(mono_adapter.output(1)[2] == 0.25f);
  CHECK(mono_adapter.process(pair, 8, 8, VS_PROCESS_OFFLINE) == VS_ERROR_ARGUMENT);
  CHECK(mono_adapter.output(0)[2] == 0.0f && mono_adapter.output(1)[2] == 0.0f);
  mono_graph.release();
}

void test_stereo_prepare_requests_two_inputs() {
  AudioSession session;
  session.prepare(stereo_config());
  CHECK(fake_ae::control().requested_inputs == (std::vector<uint32_t>{3, 1}));
  CHECK(fake_ae::control().last_request.input_channels.count == 2);
  const auto &info = session.stream_info();
  CHECK(info.input_mode == vespera::InputMode::stereo);
  CHECK(info.input_channels == 2);
  CHECK(info.physical_input == 3);
  CHECK((info.physical_inputs == std::array<uint32_t, 2>{3, 1}));
  CHECK(info.graph_channels == 2);
  CHECK(std::string(vespera::to_string(info.input_mode)) == "stereo");
  CHECK(session.close() == CloseResult::closed);
}

void test_stereo_configuration_rejected() {
  AudioSession session;
  auto missing = stereo_config();
  missing.second_input_channel.reset();
  CHECK(throws<std::invalid_argument>([&] { session.prepare(missing); }));
  auto duplicate = stereo_config();
  duplicate.second_input_channel = duplicate.input_channel;
  CHECK(throws<std::invalid_argument>([&] { session.prepare(duplicate); }));
  auto downmix = stereo_config();
  downmix.graph_channels = 1;
  CHECK(throws<std::invalid_argument>([&] { session.prepare(downmix); }));
  auto stale_second = config();
  stale_second.second_input_channel = 1;
  CHECK(throws<std::invalid_argument>([&] { session.prepare(stale_second); }));
  auto unknown = stereo_config();
  unknown.input_mode = static_cast<vespera::InputMode>(7);
  CHECK(throws<std::invalid_argument>([&] { session.prepare(unknown); }));
  CHECK(fake_ae::control().open_calls == 0);
  CHECK(session.state() == SessionState::stopped);

  fake_ae::control().negotiated_input_count = 1;
  try {
    session.prepare(stereo_config());
    CHECK(false);
  } catch (const vespera::AudioError &error) {
    CHECK(error.code() == AE_ERROR_UNSUPPORTED_CONFIGURATION);
  }
  CHECK(session.state() == SessionState::stopped);
  CHECK(fake_ae::control().streams_alive == 0);

  fake_ae::control().negotiated_input_count = 0;
  auto mono_graph = config();
  mono_graph.graph_channels = 1;
  session.prepare(mono_graph);
  CHECK(session.stream_info().input_channels == 1);
  CHECK(session.close() == CloseResult::closed);
}

void test_stereo_processing_keeps_channels_distinct() {
  AudioSession session;
  const NodeId node = session.graph().add_module(gain_module);
  session.set_parameter_value(node, 1, 2.0f);
  session.prepare(stereo_config());
  session.start();
  StereoBuffers buffers(64);
  counting_allocations.store(true);
  allocation_count.store(0);
  const auto first = buffers.run(64, 0);
  const auto second = buffers.run(17, 64);
  counting_allocations.store(false);
  CHECK(allocation_count.load() == 0);
  CHECK(first == AE_PROCESS_OK && second == AE_PROCESS_OK);
  bool exact = true;
  for (uint32_t i = 0; i < 17; ++i) {
    exact = exact && buffers.left[i] == buffers.left_input[i] * 2.0f &&
            buffers.right[i] == buffers.right_input[i] * 2.0f;
  }
  CHECK(exact);
  CHECK(buffers.left[3] != buffers.right[3]);
  auto status = session.poll();
  CHECK(status.state == SessionState::running);
  CHECK(status.telemetry.input_peak == buffers.left_input[63]);

  Buffers mono(64);
  CHECK(mono.run(64, 81) == AE_PROCESS_FAILED);
  CHECK(session.poll().telemetry.contract_violations == 1);
  session.stop();
  CHECK(session.state() == SessionState::recovery_required);
  CHECK(session.close() == CloseResult::closed);

  session.prepare(config(OutputMode::monitor));
  session.start();
  CHECK(mono.run(64, 0) == AE_PROCESS_OK);
  CHECK(mono.left[9] == mono.input[9] * 2.0f && mono.right[9] == mono.input[9] * 2.0f);
  CHECK(buffers.run(64, 64) == AE_PROCESS_FAILED);
  CHECK(session.close() == CloseResult::closed);
}

void test_meters_mono_intervals_and_varying_blocks() {
  AudioSession session;
  const auto empty = session.read_meters();
  CHECK(empty.input_channels == 0 && empty.input_frames == 0 && meter_silent(empty.inputs[0]));
  session.prepare(config(OutputMode::monitor));
  CHECK(session.read_meters().input_channels == 1);
  session.start();
  Buffers buffers(256);
  std::vector<float> history;
  uint64_t position = 0;
  for (const uint32_t frames : {17u, 64u, 5u, 256u}) {
    CHECK(buffers.run(frames, position) == AE_PROCESS_OK);
    history.insert(history.end(), buffers.input.begin(), buffers.input.begin() + frames);
    position += frames;
  }
  const auto reading = session.read_meters();
  CHECK(reading.input_channels == 1);
  CHECK(reading.input_frames == 342 && reading.output_frames == 342);
  CHECK(reading.inputs[0].peak == buffers.input[255]);
  CHECK(near(reading.inputs[0].rms, rms_of(history, 0, 342)));
  CHECK(!reading.inputs[0].clipped());
  CHECK(meter_silent(reading.inputs[1]));
  CHECK(reading.outputs[0].peak == buffers.input[255]);
  CHECK(reading.outputs[1].peak == buffers.input[255]);
  CHECK(reading.outputs[0].rms == reading.inputs[0].rms);
  CHECK(reading.outputs[1].rms == reading.inputs[0].rms);

  const auto reset = session.read_meters();
  CHECK(reset.input_frames == 0 && reset.output_frames == 0);
  CHECK(meter_silent(reset.inputs[0]) && meter_silent(reset.outputs[0]) &&
        meter_silent(reset.outputs[1]));

  CHECK(buffers.run(3, position) == AE_PROCESS_OK);
  const auto small = session.read_meters();
  CHECK(small.input_frames == 3 && small.inputs[0].peak == buffers.input[2]);
  CHECK(near(small.inputs[0].rms, rms_of(buffers.input, 0, 3)));
  CHECK(session.poll().telemetry.input_peak == buffers.input[255]);
  CHECK(session.close() == CloseResult::closed);
}

void test_meters_stereo_separation_clip_and_silence() {
  AudioSession session;
  session.prepare(stereo_config(OutputMode::silent));
  session.start();
  StereoBuffers buffers(32);
  std::fill(buffers.left_input.begin(), buffers.left_input.end(), 0.5f);
  std::fill(buffers.right_input.begin(), buffers.right_input.end(), 0.0f);
  counting_allocations.store(true);
  allocation_count.store(0);
  const auto result = buffers.run(32, 0);
  counting_allocations.store(false);
  CHECK(allocation_count.load() == 0);
  CHECK(result == AE_PROCESS_SILENCE);
  CHECK(buffers.left[0] == 0.0f);
  auto reading = session.read_meters();
  CHECK(reading.input_channels == 2);
  CHECK(reading.inputs[0].peak == 0.5f && near(reading.inputs[0].rms, 0.5));
  CHECK(meter_silent(reading.inputs[1]));
  CHECK(reading.outputs[0].peak == 0.5f && near(reading.outputs[0].rms, 0.5));
  CHECK(meter_silent(reading.outputs[1]));

  buffers.right_input[4] = 1.0f;
  buffers.right_input[5] = -1.5f;
  buffers.right_input[6] = 100.0f;
  CHECK(buffers.run(32, 32) == AE_PROCESS_SILENCE);
  reading = session.read_meters();
  CHECK(!reading.inputs[0].clipped());
  CHECK(reading.inputs[1].clipped_samples == 3);
  CHECK(reading.inputs[1].peak == vespera::meter_ceiling);
  CHECK(near(reading.inputs[1].rms, std::sqrt((1.0 + 2.25 + 16.0) / 32.0)));
  CHECK(reading.outputs[1].clipped_samples == 3);

  buffers.right_input[6] = std::numeric_limits<float>::quiet_NaN();
  CHECK(buffers.run(32, 64) == AE_PROCESS_FAILED);
  reading = session.read_meters();
  CHECK(reading.input_frames == 32 && reading.output_frames == 0);
  CHECK(reading.inputs[1].clipped_samples == 3);
  CHECK(reading.inputs[1].peak == vespera::meter_ceiling);
  CHECK(meter_silent(reading.outputs[0]) && meter_silent(reading.outputs[1]));
  CHECK(session.close() == CloseResult::closed);

  session.prepare(stereo_config());
  const auto prepared = session.read_meters();
  CHECK(prepared.input_frames == 0 && meter_silent(prepared.inputs[1]));
  CHECK(session.close() == CloseResult::closed);
}

}

int main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s GAIN_MODULE\n", argv[0]);
    return 2;
  }
  gain_module = argv[1];
  const std::pair<const char *, std::function<void()>> tests[] = {
      {"enumerate_copies_and_releases", test_enumerate_copies_and_releases},
      {"probe", test_probe},
      {"prepare_uses_explicit_request_and_negotiated_maximum",
       test_prepare_uses_explicit_request_and_negotiated_maximum},
      {"prepare_rejects_unusable_layout", test_prepare_rejects_unusable_layout},
      {"silent_and_monitor_processing", test_silent_and_monitor_processing},
      {"discontinuities", test_discontinuities},
      {"stop_failure_retains_callback_state", test_stop_failure_retains_callback_state},
      {"destructor_quarantines_unstoppable_callback",
       test_destructor_quarantines_unstoppable_callback},
      {"close_failure_is_retryable", test_close_failure_is_retryable},
      {"destructor_after_close_failure_leaks_only_handles",
       test_destructor_after_close_failure_leaks_only_handles},
      {"start_quiescence_failure", test_start_quiescence_failure},
      {"start_error_keeps_prepared", test_start_error_keeps_prepared},
      {"poll_surfaces_stream_fault", test_poll_surfaces_stream_fault},
      {"adapter_offline", test_adapter_offline},
      {"adapter_planar_input", test_adapter_planar_input},
      {"stereo_prepare_requests_two_inputs", test_stereo_prepare_requests_two_inputs},
      {"stereo_configuration_rejected", test_stereo_configuration_rejected},
      {"stereo_processing_keeps_channels_distinct", test_stereo_processing_keeps_channels_distinct},
      {"meters_mono_intervals_and_varying_blocks", test_meters_mono_intervals_and_varying_blocks},
      {"meters_stereo_separation_clip_and_silence", test_meters_stereo_separation_clip_and_silence},
      {"parameter_immediate_and_offsets", test_parameter_immediate_and_offsets},
      {"parameter_future_does_not_block_and_equal_order",
       test_parameter_future_does_not_block_and_equal_order},
      {"parameter_late_and_block_end", test_parameter_late_and_block_end},
      {"parameter_capacity_includes_pending", test_parameter_capacity_includes_pending},
      {"parameter_tokens_expire_on_stop_and_prepare",
       test_parameter_tokens_expire_on_stop_and_prepare},
      {"parameter_discontinuity_invalidates_timeline",
       test_parameter_discontinuity_invalidates_timeline},
      {"parameter_validation_and_metadata", test_parameter_validation_and_metadata},
      {"parameter_not_applied_when_processing_fails",
       test_parameter_not_applied_when_processing_fails},
      {"parameter_processing_does_not_allocate", test_parameter_processing_does_not_allocate},
      {"preset_capture_and_restore", test_preset_capture_and_restore},
      {"preset_restore_rolls_back", test_preset_restore_rolls_back},
      {"capture_requires_quiescence", test_capture_requires_quiescence},
      {"parameter_values_require_quiescence", test_parameter_values_require_quiescence},
  };
  for (const auto &[name, test] : tests) {
    current_test = name;
    fake_ae::reset();
    try {
      test();
    } catch (const std::exception &error) {
      std::fprintf(stderr, "%s: unexpected exception: %s\n", name, error.what());
      ++failures;
    }
  }
  if (failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("all audio session tests passed\n");
  return 0;
}
