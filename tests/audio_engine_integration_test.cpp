#include <vespera/audio_session.hpp>

#include "fixtures/probe_control.h"
#include "scripted_audio_backend.h"

#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {

using vespera::AudioSession;
using vespera::CloseResult;
using vespera::DeviceStreamState;
using vespera::NodeId;
using vespera::OutputMode;
using vespera::ParameterResult;
using vespera::RecoveryReason;
using vespera::SessionState;

int failures = 0;
const char *current_test = "";
std::filesystem::path probe_module;
std::filesystem::path gain_module;
probe_control *probe = nullptr;

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

template <typename Function> std::optional<int32_t> audio_error(Function &&function) {
  try {
    function();
  } catch (const vespera::AudioError &error) {
    return error.code();
  }
  return std::nullopt;
}

void reset_probe() {
  CHECK(probe->live_instances == 0);
  std::memset(probe, 0, sizeof(*probe));
  probe->fail_prepare_instance = -1;
  probe->fault_instance = -1;
}

vespera::StreamConfig config(OutputMode mode = OutputMode::monitor) {
  vespera::StreamConfig result;
  result.backend_id = scripted_ae::backend_id;
  result.device_id = scripted_ae::device_id;
  result.input_channel = 2;
  result.output_channels = {3, 0};
  result.sample_rate = 48000;
  result.frames_per_block = 64;
  result.output_mode = mode;
  return result;
}

struct Buffers {
  std::vector<float> input;
  std::vector<float> left;
  std::vector<float> right;

  explicit Buffers(uint32_t frames) : input(frames), left(frames), right(frames) {
    for (uint32_t i = 0; i < frames; ++i) {
      input[i] = 0.001f * static_cast<float>(i + 1);
    }
  }

  // Outputs start as NaN, as a driver's uninitialized buffers might, so any sample neither the
  // engine nor the session wrote fails both silent() and scaled().
  ae_process_result deliver(uint32_t frames, std::optional<uint64_t> position = std::nullopt) {
    return scripted_ae::deliver(input.data(), left.data(), right.data(), frames,
                                std::numeric_limits<float>::quiet_NaN(), position);
  }

  bool silent(uint32_t frames) const {
    const auto zero = [](float sample) { return sample == 0.0f; };
    return std::all_of(left.begin(), left.begin() + frames, zero) &&
           std::all_of(right.begin(), right.begin() + frames, zero);
  }

  bool scaled(uint32_t begin, uint32_t end, float gain) const {
    for (uint32_t i = begin; i < end; ++i) {
      if (left[i] != input[i] * gain || right[i] != input[i] * gain) {
        return false;
      }
    }
    return true;
  }
};

vespera::StreamConfig stereo_config(OutputMode mode = OutputMode::monitor) {
  vespera::StreamConfig result = config(mode);
  result.input_mode = vespera::InputMode::stereo;
  result.second_input_channel = 1;
  return result;
}

struct StereoBuffers {
  std::vector<float> left_input;
  std::vector<float> right_input;
  std::vector<float> left;
  std::vector<float> right;

  explicit StereoBuffers(uint32_t frames)
      : left_input(frames), right_input(frames), left(frames), right(frames) {
    for (uint32_t i = 0; i < frames; ++i) {
      left_input[i] = 0.001f * static_cast<float>(i + 1);
      right_input[i] = -0.25f + 0.0005f * static_cast<float>(i);
    }
  }

  ae_process_result deliver(uint32_t frames) {
    return scripted_ae::deliver(left_input.data(), right_input.data(), left.data(), right.data(),
                                frames, std::numeric_limits<float>::quiet_NaN());
  }

  bool scaled(uint32_t begin, uint32_t end, float gain) const {
    for (uint32_t i = begin; i < end; ++i) {
      if (left[i] != left_input[i] * gain || right[i] != right_input[i] * gain) {
        return false;
      }
    }
    return true;
  }
};

void test_engine_lifecycle_round_trip() {
  AudioSession session;
  const auto devices = session.devices();
  CHECK(devices.size() == 1);
  CHECK(devices[0].backend_id == scripted_ae::backend_id);
  CHECK(devices[0].input_channels == 4 && devices[0].output_channels == 4);

  const NodeId node = session.graph().add_module(probe_module);
  session.prepare(config());
  const auto &seen = scripted_ae::observed();
  CHECK(seen.open);
  CHECK(seen.requested_inputs == std::vector<uint32_t>{2});
  CHECK(seen.requested_outputs == (std::vector<uint32_t>{3, 0}));
  CHECK(!seen.requested_adjustment);
  const vespera::StreamInfo info = session.stream_info();
  CHECK(info.physical_input == 2);
  CHECK((info.physical_outputs == std::array<uint32_t, 2>{3, 0}));
  CHECK(info.frames_per_block == 64 && info.max_frames_per_block == 64);
  CHECK(info.stream_id != 0);
  auto status = session.poll();
  CHECK(status.stream_open);
  CHECK(status.stream.state == DeviceStreamState::ready);
  CHECK(status.stream.stream_id == info.stream_id);

  session.start();
  CHECK(seen.running);
  Buffers buffers(64);
  CHECK(buffers.deliver(64) == AE_PROCESS_OK);
  CHECK(buffers.scaled(0, 64, 1.0f));
  CHECK(buffers.deliver(64) == AE_PROCESS_OK);
  status = session.poll();
  CHECK(status.state == SessionState::running);
  CHECK(status.stream.state == DeviceStreamState::running);
  CHECK(status.stream.delivered_frames == 128);
  CHECK(status.telemetry.blocks == 2);
  CHECK(status.telemetry.discontinuities == 1);
  CHECK(status.telemetry.next_frame_position == 128);
  CHECK((probe->blocks[0].flags & VS_PROCESS_DISCONTINUITY) == 0);
  const uint32_t first_run = status.stream.run_generation;

  CHECK(session.set_parameter(session.parameter_token(), node, PROBE_PARAM_LEVEL, 2.0f, 144) ==
        ParameterResult::accepted);
  CHECK(buffers.deliver(64) == AE_PROCESS_OK);
  CHECK(buffers.scaled(0, 16, 1.0f));
  CHECK(buffers.scaled(16, 64, 2.0f));
  CHECK(session.poll().telemetry.parameters.applied == 1);

  session.stop();
  CHECK(session.state() == SessionState::prepared);
  CHECK(!seen.running);
  CHECK(session.poll().stream.state == DeviceStreamState::ready);

  session.start();
  CHECK(buffers.deliver(64) == AE_PROCESS_OK);
  CHECK((probe->blocks[0].flags & VS_PROCESS_DISCONTINUITY) != 0);
  status = session.poll();
  CHECK(status.stream.run_generation != first_run);
  CHECK(status.stream.delivered_frames == 64);
  CHECK(status.telemetry.blocks == 1);
  CHECK(status.telemetry.discontinuities == 1);

  CHECK(session.close() == CloseResult::closed);
  CHECK(!seen.open && !seen.running);
  CHECK(!session.poll().stream_open);
}

void test_silent_output_is_cleared_while_graph_runs() {
  AudioSession session;
  session.graph().add_module(probe_module);
  session.prepare(config(OutputMode::silent));
  session.start();
  Buffers buffers(64);
  CHECK(buffers.deliver(64) == AE_PROCESS_SILENCE);
  CHECK(buffers.silent(64));
  CHECK(probe->blocks[0].calls == 1);
  const auto status = session.poll();
  CHECK(status.state == SessionState::running);
  CHECK(status.stream.delivered_frames == 64);
  CHECK(status.telemetry.output_peak > 0.063f);
  CHECK(session.close() == CloseResult::closed);
}

void test_negotiation_is_validated_by_engine() {
  scripted_ae::script().fixed_sample_rate = 44100;
  scripted_ae::script().fixed_frames = 256;
  scripted_ae::script().max_frames = 512;
  AudioSession session;
  session.graph().add_module(probe_module);

  CHECK(audio_error([&] { session.prepare(config()); }) == AE_ERROR_UNSUPPORTED_CONFIGURATION);
  CHECK(session.state() == SessionState::stopped);
  CHECK(!scripted_ae::observed().open);
  CHECK(probe->last_prepare[0].max_frames == 0);
  CHECK(session.graph().nodes().size() == 1);

  vespera::StreamConfig adjustable = config();
  adjustable.allow_adjustment = true;
  session.prepare(adjustable);
  CHECK(scripted_ae::observed().requested_adjustment);
  const auto &info = session.stream_info();
  CHECK(info.sample_rate == 44100);
  CHECK(info.frames_per_block == 256);
  CHECK(info.max_frames_per_block == 512);
  CHECK(probe->last_prepare[0].sample_rate == 44100.0);
  CHECK(probe->last_prepare[0].max_frames == 512);

  session.start();
  Buffers buffers(512);
  CHECK(buffers.deliver(512) == AE_PROCESS_OK);
  CHECK(buffers.scaled(0, 512, 1.0f));
  CHECK(buffers.deliver(100) == AE_PROCESS_OK);
  const auto status = session.poll();
  CHECK(status.state == SessionState::running);
  CHECK(status.telemetry.frames == 612);
  CHECK(status.telemetry.discontinuities == 1);
  CHECK(session.close() == CloseResult::closed);
}

void test_oversized_block_is_rejected_and_latched() {
  AudioSession session;
  session.graph().add_module(probe_module);
  session.prepare(config());
  session.start();
  Buffers buffers(128);
  CHECK(buffers.deliver(128) == AE_PROCESS_FAILED);
  CHECK(buffers.silent(128));
  CHECK(probe->blocks[0].calls == 0);

  CHECK(buffers.deliver(64) == AE_PROCESS_SILENCE);
  CHECK(buffers.silent(64));
  CHECK(probe->blocks[0].calls == 0);

  const auto status = session.poll();
  CHECK(status.state == SessionState::recovery_required);
  CHECK(status.recovery == RecoveryReason::processing_failure);
  CHECK(status.last_error == AE_ERROR_PROCESSOR_FAILURE);
  CHECK(status.stream.state == DeviceStreamState::recovery_required);
  CHECK(status.stream.delivered_frames == 0);
  CHECK(status.telemetry.contract_violations == 1);
  CHECK(status.telemetry.blocks == 0);
  session.stop();
  CHECK(session.state() == SessionState::recovery_required);
  CHECK(session.close() == CloseResult::closed);
}

void test_varying_blocks_and_scheduled_events() {
  constexpr uint32_t max_frames = 256;
  AudioSession session;
  const NodeId node = session.graph().add_module(probe_module);
  vespera::StreamConfig stream = config();
  stream.frames_per_block = max_frames;
  session.prepare(stream);
  session.start();
  const vespera::ParameterToken token = session.parameter_token();

  std::minstd_rand rng(20261111);
  const auto pick = [&](uint32_t bound) { return static_cast<uint32_t>(rng() % bound); };
  std::multimap<uint64_t, float> pending;
  float level = 1.0f;
  uint64_t position = 0;
  uint64_t scheduled = 0;
  uint64_t applied = 0;
  Buffers buffers(max_frames);
  for (uint32_t block = 0; block < 400; ++block) {
    const uint32_t frames = 1 + pick(max_frames);
    for (uint32_t i = 0; i < frames; ++i) {
      switch (pick(8)) {
      case 0:
        buffers.input[i] = 0.0f;
        break;
      case 1:
        buffers.input[i] = std::numeric_limits<float>::denorm_min() * static_cast<float>(pick(9));
        break;
      case 2:
        buffers.input[i] = 1.0e30f * (pick(2) == 0 ? 1.0f : -1.0f);
        break;
      default:
        buffers.input[i] = (static_cast<float>(pick(2001)) - 1000.0f) / 1000.0f;
        break;
      }
    }
    for (uint32_t request = pick(3); request > 0; --request) {
      const float value = static_cast<float>(pick(17)) / 8.0f;
      const bool immediate = pick(4) == 0;
      const uint64_t at = immediate ? position : position + pick(frames + max_frames);
      const auto result = session.set_parameter(token, node, PROBE_PARAM_LEVEL, value,
                                                immediate ? std::nullopt : std::optional(at));
      CHECK(result == ParameterResult::accepted);
      pending.emplace(at, value);
      ++scheduled;
    }

    CHECK(buffers.deliver(frames) == AE_PROCESS_OK);
    bool exact = true;
    for (uint32_t i = 0; i < frames; ++i) {
      for (auto due = pending.begin(); due != pending.end() && due->first == position + i;
           due = pending.erase(due)) {
        level = due->second;
        ++applied;
      }
      const float expected = buffers.input[i] * level;
      exact = exact && buffers.left[i] == expected && buffers.right[i] == expected;
    }
    CHECK(exact);
    position += frames;
  }

  const auto status = session.poll();
  CHECK(status.state == SessionState::running);
  CHECK(status.stream.delivered_frames == position);
  CHECK(status.telemetry.frames == position);
  CHECK(status.telemetry.blocks == 400);
  CHECK(status.telemetry.discontinuities == 1);
  CHECK(status.telemetry.parameters.applied == applied);
  CHECK(status.telemetry.parameters.late == 0);
  CHECK(applied + pending.size() == scheduled);
  CHECK(session.close() == CloseResult::closed);
}

enum class Fault { module_nan, module_error, module_infinity, input_nan };

void test_dsp_fault_latches_and_recovers(Fault fault) {
  AudioSession session;
  session.graph().add_module(probe_module);
  session.prepare(config());
  const uint32_t faulted_stream = session.stream_info().stream_id;
  session.start();
  Buffers buffers(64);
  const float original = buffers.input[5];
  CHECK(buffers.deliver(64) == AE_PROCESS_OK);

  uint32_t expected_calls = 2;
  switch (fault) {
  case Fault::module_nan:
    probe->fault = PROBE_FAULT_NAN;
    break;
  case Fault::module_error:
    probe->fault = PROBE_FAULT_ERROR;
    break;
  case Fault::module_infinity:
    probe->fault = PROBE_FAULT_INFINITY;
    break;
  case Fault::input_nan:
    buffers.input[5] = std::numeric_limits<float>::quiet_NaN();
    expected_calls = 1;
    break;
  }
  probe->fault_instance = 0;
  CHECK(buffers.deliver(64) == AE_PROCESS_FAILED);
  CHECK(buffers.silent(64));
  CHECK(probe->blocks[0].calls == expected_calls);

  probe->fault = PROBE_FAULT_NONE;
  buffers.input[5] = original;
  CHECK(buffers.deliver(64) == AE_PROCESS_SILENCE);
  CHECK(buffers.silent(64));
  CHECK(probe->blocks[0].calls == expected_calls);

  auto status = session.poll();
  CHECK(status.state == SessionState::recovery_required);
  CHECK(status.recovery == RecoveryReason::processing_failure);
  CHECK(status.last_error == AE_ERROR_PROCESSOR_FAILURE);
  CHECK(status.stream.state == DeviceStreamState::recovery_required);
  CHECK(status.stream.delivered_frames == 64);
  CHECK(status.telemetry.processing_failures == 1);
  CHECK(throws<std::logic_error>([&] { session.start(); }));
  CHECK(throws<std::logic_error>([&] { session.graph(); }));

  session.stop();
  CHECK(!scripted_ae::observed().running);
  CHECK(session.state() == SessionState::recovery_required);
  CHECK(session.close() == CloseResult::closed);
  CHECK(session.state() == SessionState::stopped);
  CHECK(session.recovery_reason() == RecoveryReason::none);

  CHECK(session.graph().nodes().size() == 1);
  session.prepare(config());
  CHECK(session.stream_info().stream_id != faulted_stream);
  session.start();
  CHECK(buffers.deliver(64) == AE_PROCESS_OK);
  CHECK(buffers.scaled(0, 64, 1.0f));
  status = session.poll();
  CHECK(status.state == SessionState::running);
  CHECK(status.stream.state == DeviceStreamState::running);
  CHECK(status.telemetry.processing_failures == 0);
  CHECK(session.close() == CloseResult::closed);
}

void test_device_fault_blocks_dispatch_and_restart() {
  AudioSession session;
  session.graph().add_module(probe_module);
  session.prepare(config());
  session.start();
  Buffers buffers(64);
  CHECK(buffers.deliver(64) == AE_PROCESS_OK);

  scripted_ae::script().faults = AE_FAULT_DEVICE;
  CHECK(buffers.deliver(64) == AE_PROCESS_SILENCE);
  CHECK(buffers.silent(64));
  CHECK(probe->blocks[0].calls == 1);
  auto status = session.poll();
  CHECK(status.state == SessionState::recovery_required);
  CHECK(status.recovery == RecoveryReason::stream_fault);
  CHECK(status.last_error == AE_ERROR_DEVICE_UNAVAILABLE);
  CHECK(status.stream.fault_flags == AE_FAULT_DEVICE);
  CHECK(status.telemetry.blocks == 1);
  session.stop();
  CHECK(session.close() == CloseResult::closed);

  session.prepare(config());
  const int starts = scripted_ae::observed().start_calls;
  CHECK(audio_error([&] { session.start(); }) == AE_ERROR_INVALID_STATE);
  CHECK(session.state() == SessionState::prepared);
  CHECK(scripted_ae::observed().start_calls == starts);

  scripted_ae::script().faults = 0;
  session.start();
  CHECK(buffers.deliver(64) == AE_PROCESS_OK);
  CHECK(buffers.scaled(0, 64, 1.0f));
  CHECK(session.poll().state == SessionState::running);
  CHECK(session.close() == CloseResult::closed);
}

void test_live_replacement_preserves_engine_timeline() {
  AudioSession session;
  const NodeId node = session.graph().add_module(probe_module);
  session.prepare(config());
  const auto stream_id = session.stream_info().stream_id;
  session.start();
  const auto old_token = session.parameter_token();
  CHECK(session.set_parameter(old_token, node, PROBE_PARAM_LEVEL, 2.0f) ==
        ParameterResult::accepted);
  Buffers buffers(64);
  CHECK(buffers.deliver(64) == AE_PROCESS_OK);
  const auto run = session.poll().stream.run_generation;
  CHECK(session.set_parameter(old_token, node, PROBE_PARAM_LEVEL, 1.5f, 1000) ==
        ParameterResult::accepted);
  session.request_graph_replacement(std::make_unique<vespera::Graph>(), 80);
  CHECK(session.parameter_token() == vespera::ParameterToken{});
  CHECK(session.set_parameter(old_token, node, PROBE_PARAM_LEVEL, 4.0f) !=
        ParameterResult::accepted);
  CHECK(buffers.deliver(17) == AE_PROCESS_OK);
  for (uint32_t i = 0; i < 17; ++i) {
    const float weight = static_cast<float>(i + 1) / 80.0f;
    const float expected = buffers.input[i] * 2.0f * (1.0f - weight) + buffers.input[i] * weight;
    CHECK(std::fabs(buffers.left[i] - expected) <= 1e-6f);
    CHECK(buffers.right[i] == buffers.left[i]);
  }
  CHECK(session.poll().state == SessionState::running);
  CHECK(session.nodes().size() == 1);
  CHECK(probe->live_instances == 1);
  CHECK(buffers.deliver(64) == AE_PROCESS_OK);
  for (uint32_t i = 0; i < 64; ++i) {
    const float weight = std::min(1.0f, static_cast<float>(17 + i + 1) / 80.0f);
    const float expected = buffers.input[i] * 2.0f * (1.0f - weight) + buffers.input[i] * weight;
    CHECK(std::fabs(buffers.left[i] - expected) <= 1e-6f);
  }
  CHECK(probe->live_instances == 1);
  const auto status = session.poll();
  CHECK(probe->live_instances == 0);
  CHECK(session.nodes().empty());
  CHECK(status.stream.stream_id == stream_id && status.stream.run_generation == run);
  CHECK(status.stream.delivered_frames == 145);
  CHECK(status.telemetry.frames == 145);
  CHECK(status.telemetry.parameters.stale == 1);
  CHECK(session.parameter_token().graph_generation != old_token.graph_generation);
  CHECK(session.set_parameter(old_token, node, PROBE_PARAM_LEVEL, 1.0f) ==
        ParameterResult::stale_token);
  CHECK(scripted_ae::observed().open_calls == 1);
  CHECK(scripted_ae::observed().start_calls == 1);
  CHECK(scripted_ae::observed().stop_calls == 0);
  CHECK(session.close() == CloseResult::closed);
}

void test_seeded_live_transitions() {
  AudioSession session;
  session.graph().add_module(gain_module);
  session.prepare(config());
  session.start();
  std::minstd_rand random(20261201);
  const auto pick = [&](uint32_t bound) { return static_cast<uint32_t>(random() % bound); };
  Buffers buffers(64);
  float previous = 1.0f;
  uint64_t delivered = 0;
  for (uint32_t transition = 0; transition < 128; ++transition) {
    const auto old_token = session.parameter_token();
    const uint32_t fade_frames = 1 + pick(512);
    const float next = static_cast<float>(pick(257)) / 64.0f;
    auto candidate = std::make_unique<vespera::Graph>();
    const auto node = candidate->add_module(gain_module);
    candidate->set_parameter_value(node, 1, next);
    session.request_graph_replacement(std::move(candidate), fade_frames);
    uint32_t elapsed = 0;
    while (elapsed < fade_frames) {
      const uint32_t frames = 1 + pick(64);
      for (uint32_t f = 0; f < frames; ++f) {
        buffers.input[f] = (static_cast<float>(pick(2049)) - 1024.0f) / 1024.0f;
      }
      CHECK(buffers.deliver(frames) == AE_PROCESS_OK);
      for (uint32_t f = 0; f < frames; ++f) {
        const float weight =
            std::min(1.0f, static_cast<float>(elapsed + f + 1) / static_cast<float>(fade_frames));
        const float expected =
            buffers.input[f] * previous * (1.0f - weight) + buffers.input[f] * next * weight;
        CHECK(std::fabs(buffers.left[f] - expected) <= 1e-6f);
        CHECK(buffers.left[f] == buffers.right[f]);
      }
      elapsed += frames;
      delivered += frames;
      CHECK(session.poll().state == SessionState::running);
    }
    CHECK(session.graph_transition_state() == vespera::GraphTransitionState::idle);
    CHECK(session.parameter_token().graph_generation != old_token.graph_generation);
    CHECK(session.set_parameter(old_token, 0, 1, 0.5f) == ParameterResult::stale_token);
    CHECK(session.nodes().size() == 1);
    previous = next;
  }
  CHECK(session.poll().stream.delivered_frames == delivered);
  CHECK(scripted_ae::observed().open_calls == 1);
  CHECK(scripted_ae::observed().start_calls == 1);
  CHECK(scripted_ae::observed().stop_calls == 0);
  session.stop();
  CHECK(session.parameter_value(0, 1) == previous);
  CHECK(session.close() == CloseResult::closed);
}

void test_replacement_reclaims_queued_automation_capacity() {
  AudioSession session;
  session.graph().add_module(gain_module);
  session.prepare(config());
  session.start();
  const auto old = session.parameter_token();
  for (uint32_t i = 0; i < vespera::ParameterQueue::capacity; ++i) {
    CHECK(session.set_parameter(old, 0, 1, 1.0f, 100000) == ParameterResult::accepted);
  }
  auto candidate = std::make_unique<vespera::Graph>();
  candidate->add_module(gain_module);
  candidate->set_parameter_value(0, 1, 0.5f);
  session.request_graph_replacement(std::move(candidate), 1);
  Buffers buffers(64);
  CHECK(buffers.deliver(64) == AE_PROCESS_OK);
  CHECK(buffers.scaled(0, 64, 0.5f));
  CHECK(session.poll().telemetry.parameters.stale == vespera::ParameterQueue::capacity);
  CHECK(session.set_parameter(session.parameter_token(), 0, 1, 1.5f) == ParameterResult::accepted);
  CHECK(buffers.deliver(64) == AE_PROCESS_OK);
  CHECK(buffers.scaled(0, 64, 1.5f));
  CHECK(session.close() == CloseResult::closed);
}

void test_concurrent_replacement_retirement() {
  AudioSession session;
  session.graph().add_module(gain_module);
  session.prepare(config());
  session.start();
  std::atomic<uint32_t> callback_errors{0};
  std::atomic<uint64_t> callbacks{0};
  std::jthread audio([&](std::stop_token stop) {
    Buffers buffers(64);
    while (!stop.stop_requested()) {
      if (buffers.deliver(64) != AE_PROCESS_OK) {
        callback_errors.fetch_add(1, std::memory_order_relaxed);
      }
      for (uint32_t frame = 0; frame < 64; ++frame) {
        if (!std::isfinite(buffers.left[frame]) || buffers.left[frame] < 0.0f ||
            buffers.left[frame] > buffers.input[frame] * 4.0f + 1e-6f ||
            buffers.left[frame] != buffers.right[frame]) {
          callback_errors.fetch_add(1, std::memory_order_relaxed);
        }
      }
      callbacks.fetch_add(1, std::memory_order_release);
      std::this_thread::yield();
    }
  });
  for (uint32_t replacement = 0; replacement < 100; ++replacement) {
    auto candidate = std::make_unique<vespera::Graph>();
    candidate->add_module(gain_module);
    candidate->set_parameter_value(0, 1, static_cast<float>(replacement % 17) / 4.0f);
    session.request_graph_replacement(std::move(candidate), 257);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (session.graph_transition_state() != vespera::GraphTransitionState::idle &&
           std::chrono::steady_clock::now() < deadline) {
      session.poll();
      const auto meters = session.read_meters();
      CHECK(meters.input_frames >= meters.output_frames && meters.inputs[0].peak <= 1.0f);
      std::this_thread::yield();
    }
    CHECK(session.graph_transition_state() == vespera::GraphTransitionState::idle);
    CHECK(session.nodes().size() == 1);
    if (session.graph_transition_state() != vespera::GraphTransitionState::idle) {
      break;
    }
  }
  audio.request_stop();
  audio.join();
  CHECK(callbacks.load() > 0);
  CHECK(callback_errors.load() == 0);
  CHECK(session.poll().state == SessionState::running);
  CHECK(session.close() == CloseResult::closed);
}

void test_stereo_input_round_trip() {
  AudioSession session;
  const NodeId node = session.graph().add_module(gain_module);
  session.set_parameter_value(node, 1, 2.0f);
  session.prepare(stereo_config());
  CHECK(scripted_ae::observed().requested_inputs == (std::vector<uint32_t>{2, 1}));
  const vespera::StreamInfo info = session.stream_info();
  CHECK(info.input_mode == vespera::InputMode::stereo);
  CHECK(info.input_channels == 2);
  CHECK((info.physical_inputs == std::array<uint32_t, 2>{2, 1}));
  CHECK(info.physical_input == 2);
  CHECK((info.physical_outputs == std::array<uint32_t, 2>{3, 0}));

  session.start();
  StereoBuffers buffers(64);
  CHECK(buffers.deliver(64) == AE_PROCESS_OK);
  CHECK(buffers.scaled(0, 64, 2.0f));
  CHECK(buffers.left[0] != buffers.right[0]);
  CHECK(session.set_parameter(session.parameter_token(), node, 1, 0.5f, 80) ==
        ParameterResult::accepted);
  CHECK(buffers.deliver(32) == AE_PROCESS_OK);
  CHECK(buffers.scaled(0, 16, 2.0f));
  CHECK(buffers.scaled(16, 32, 0.5f));

  const auto meters = session.read_meters();
  CHECK(meters.input_channels == 2 && meters.input_frames == 96 && meters.output_frames == 96);
  CHECK(meters.inputs[0].peak == buffers.left_input[63]);
  CHECK(meters.inputs[1].peak == 0.25f);
  CHECK(meters.outputs[0].peak == buffers.left_input[63] * 2.0f);
  CHECK(meters.outputs[1].peak == 0.5f);
  CHECK(meters.inputs[0].rms != meters.inputs[1].rms);
  CHECK(!meters.outputs[1].clipped());
  auto status = session.poll();
  CHECK(status.state == SessionState::running);
  CHECK(status.stream.delivered_frames == 96);

  Buffers mono(64);
  CHECK(mono.deliver(64) == AE_PROCESS_FAILED);
  CHECK(mono.silent(64));
  status = session.poll();
  CHECK(status.state == SessionState::recovery_required);
  CHECK(status.recovery == RecoveryReason::processing_failure);
  CHECK(status.telemetry.contract_violations == 1);
  CHECK(session.read_meters().input_frames == 0);
  session.stop();
  CHECK(session.close() == CloseResult::closed);

  session.prepare(config());
  CHECK(scripted_ae::observed().requested_inputs == std::vector<uint32_t>{2});
  CHECK(session.stream_info().input_channels == 1);
  session.start();
  CHECK(mono.deliver(64) == AE_PROCESS_OK);
  CHECK(mono.scaled(0, 64, 0.5f));
  CHECK(session.close() == CloseResult::closed);
}

void test_stereo_inputs_validated_by_engine() {
  scripted_ae::script().input_channels = 2;
  AudioSession session;
  auto out_of_range = stereo_config();
  CHECK(audio_error([&] { session.prepare(out_of_range); }) == AE_ERROR_INVALID_ARGUMENT);
  CHECK(session.state() == SessionState::stopped);
  CHECK(scripted_ae::observed().open_calls == 0);

  auto duplicate = stereo_config();
  duplicate.input_channel = 1;
  CHECK(throws<std::invalid_argument>([&] { session.prepare(duplicate); }));
  CHECK(scripted_ae::observed().open_calls == 0);

  auto available = stereo_config();
  available.input_channel = 1;
  available.second_input_channel = 0;
  session.prepare(available);
  CHECK(scripted_ae::observed().requested_inputs == (std::vector<uint32_t>{1, 0}));
  CHECK((session.stream_info().physical_inputs == std::array<uint32_t, 2>{1, 0}));
  session.start();
  StereoBuffers buffers(64);
  CHECK(buffers.deliver(64) == AE_PROCESS_OK);
  CHECK(buffers.scaled(0, 64, 1.0f));
  CHECK(session.close() == CloseResult::closed);
}

void test_stereo_crossfade_feeds_both_graphs() {
  AudioSession session;
  session.graph().add_module(gain_module);
  session.set_parameter_value(0, 1, 2.0f);
  session.prepare(stereo_config());
  session.start();
  StereoBuffers buffers(64);
  CHECK(buffers.deliver(64) == AE_PROCESS_OK);
  auto candidate = std::make_unique<vespera::Graph>();
  candidate->add_module(gain_module);
  candidate->set_parameter_value(0, 1, 0.5f);
  session.request_graph_replacement(std::move(candidate), 40);
  CHECK(buffers.deliver(64) == AE_PROCESS_OK);
  for (uint32_t i = 0; i < 64; ++i) {
    const float weight = std::min(1.0f, static_cast<float>(i + 1) / 40.0f);
    const auto mixed = [&](float sample) {
      return sample * 2.0f * (1.0f - weight) + sample * 0.5f * weight;
    };
    CHECK(std::fabs(buffers.left[i] - mixed(buffers.left_input[i])) <= 1e-6f);
    CHECK(std::fabs(buffers.right[i] - mixed(buffers.right_input[i])) <= 1e-6f);
  }
  CHECK(session.poll().transition.committed == 1);
  CHECK(session.graph_transition_state() == vespera::GraphTransitionState::idle);
  CHECK(buffers.deliver(64) == AE_PROCESS_OK);
  CHECK(buffers.scaled(0, 64, 0.5f));
  CHECK(session.poll().state == SessionState::running);
  CHECK(session.close() == CloseResult::closed);
}

void test_backend_start_stop_close_failures() {
  AudioSession session;
  session.graph().add_module(probe_module);
  session.prepare(config());
  scripted_ae::script().start_result = AE_ERROR_DEVICE_UNAVAILABLE;
  CHECK(audio_error([&] { session.start(); }) == AE_ERROR_DEVICE_UNAVAILABLE);
  CHECK(session.state() == SessionState::prepared);
  auto status = session.poll();
  CHECK(status.state == SessionState::prepared);
  CHECK(status.stream.state == DeviceStreamState::ready);
  CHECK(status.stream.last_result == AE_ERROR_DEVICE_UNAVAILABLE);

  scripted_ae::script().start_result = AE_OK;
  session.start();
  Buffers buffers(64);
  CHECK(buffers.deliver(64) == AE_PROCESS_OK);

  scripted_ae::script().stop_failures = 1;
  CHECK(audio_error([&] { session.stop(); }) == AE_ERROR_DRIVER_FAILURE);
  CHECK(session.state() == SessionState::recovery_required);
  CHECK(session.recovery_reason() == RecoveryReason::quiescence_failure);
  CHECK(buffers.deliver(64) == AE_PROCESS_OK);
  CHECK(buffers.scaled(0, 64, 1.0f));
  status = session.poll();
  CHECK(status.stream.state == DeviceStreamState::running);
  CHECK(status.stream.last_result == AE_ERROR_DRIVER_FAILURE);
  CHECK(status.recovery == RecoveryReason::quiescence_failure);

  scripted_ae::script().close_failures = 1;
  CHECK(session.close() == CloseResult::close_failed);
  CHECK(session.recovery_reason() == RecoveryReason::close_failure);
  CHECK(!scripted_ae::observed().running);
  CHECK(scripted_ae::observed().open);
  CHECK(throws<std::logic_error>([&] { session.prepare(config()); }));
  CHECK(session.close() == CloseResult::closed);
  CHECK(!scripted_ae::observed().open);

  session.prepare(config());
  session.start();
  CHECK(buffers.deliver(64) == AE_PROCESS_OK);
  CHECK(session.close() == CloseResult::closed);
}

}

int main(int argc, char **argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s PROBE_MODULE GAIN_MODULE\n", argv[0]);
    return 2;
  }
  probe_module = std::filesystem::canonical(argv[1]);
  gain_module = std::filesystem::canonical(argv[2]);
  void *library = dlopen(probe_module.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (library == nullptr) {
    std::fprintf(stderr, "cannot open probe fixture: %s\n", dlerror());
    return 2;
  }
  const auto control = reinterpret_cast<probe_control_fn>(dlsym(library, PROBE_CONTROL_SYMBOL));
  if (control == nullptr) {
    std::fprintf(stderr, "probe fixture lacks %s\n", PROBE_CONTROL_SYMBOL);
    return 2;
  }
  probe = control();

  const std::pair<const char *, std::function<void()>> tests[] = {
      {"engine_lifecycle_round_trip", test_engine_lifecycle_round_trip},
      {"silent_output_is_cleared_while_graph_runs", test_silent_output_is_cleared_while_graph_runs},
      {"negotiation_is_validated_by_engine", test_negotiation_is_validated_by_engine},
      {"oversized_block_is_rejected_and_latched", test_oversized_block_is_rejected_and_latched},
      {"varying_blocks_and_scheduled_events", test_varying_blocks_and_scheduled_events},
      {"module_nan_latches_and_recovers",
       [] { test_dsp_fault_latches_and_recovers(Fault::module_nan); }},
      {"module_error_latches_and_recovers",
       [] { test_dsp_fault_latches_and_recovers(Fault::module_error); }},
      {"module_infinity_latches_and_recovers",
       [] { test_dsp_fault_latches_and_recovers(Fault::module_infinity); }},
      {"input_nan_latches_and_recovers",
       [] { test_dsp_fault_latches_and_recovers(Fault::input_nan); }},
      {"device_fault_blocks_dispatch_and_restart", test_device_fault_blocks_dispatch_and_restart},
      {"backend_start_stop_close_failures", test_backend_start_stop_close_failures},
      {"live_replacement_preserves_engine_timeline",
       test_live_replacement_preserves_engine_timeline},
      {"seeded_live_transitions", test_seeded_live_transitions},
      {"replacement_reclaims_queued_automation_capacity",
       test_replacement_reclaims_queued_automation_capacity},
      {"concurrent_replacement_retirement", test_concurrent_replacement_retirement},
      {"stereo_input_round_trip", test_stereo_input_round_trip},
      {"stereo_inputs_validated_by_engine", test_stereo_inputs_validated_by_engine},
      {"stereo_crossfade_feeds_both_graphs", test_stereo_crossfade_feeds_both_graphs},
  };
  for (const auto &[name, test] : tests) {
    current_test = name;
    try {
      scripted_ae::reset();
      reset_probe();
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
  std::printf("all audio engine integration tests passed\n");
  return 0;
}
