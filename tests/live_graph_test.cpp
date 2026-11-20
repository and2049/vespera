#include <vespera/audio_session.hpp>

#include "fake_audio_engine.h"
#include "fixtures/probe_control.h"

#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <new>
#include <stdexcept>
#include <vector>

namespace {

std::atomic<bool> counting_heap{false};
std::atomic<uint64_t> allocation_count{0};
std::atomic<uint64_t> deallocation_count{0};

void count_deallocation(void *memory) noexcept {
  if (memory != nullptr && counting_heap.load(std::memory_order_relaxed)) {
    deallocation_count.fetch_add(1, std::memory_order_relaxed);
  }
}

}

void *operator new(std::size_t size) {
  if (counting_heap.load(std::memory_order_relaxed)) {
    allocation_count.fetch_add(1, std::memory_order_relaxed);
  }
  if (void *memory = std::malloc(size == 0 ? 1 : size)) {
    return memory;
  }
  throw std::bad_alloc();
}

void *operator new[](std::size_t size) { return operator new(size); }
void operator delete(void *memory) noexcept {
  count_deallocation(memory);
  std::free(memory);
}
void operator delete[](void *memory) noexcept {
  count_deallocation(memory);
  std::free(memory);
}
void operator delete(void *memory, std::size_t) noexcept {
  count_deallocation(memory);
  std::free(memory);
}
void operator delete[](void *memory, std::size_t) noexcept {
  count_deallocation(memory);
  std::free(memory);
}

namespace {

using vespera::AudioSession;
using vespera::CloseResult;
using vespera::Graph;
using vespera::GraphTransitionState;
using vespera::NodeId;
using vespera::OutputMode;
using vespera::ParameterResult;
using vespera::ParameterToken;
using vespera::RecoveryReason;
using vespera::SessionState;

int failures = 0;
const char *current_test = "";
std::filesystem::path gain_module;
std::filesystem::path probe_module;
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

void reset_probe() {
  CHECK(probe->live_instances == 0);
  std::memset(probe, 0, sizeof(*probe));
  probe->fail_prepare_instance = -1;
  probe->fault_instance = -1;
}

int probe_ops(int32_t op, int32_t instance) {
  int count = 0;
  for (uint32_t i = 0; i < std::min<uint32_t>(probe->op_count, PROBE_MAX_OPS); ++i) {
    if (probe->ops[i].op == op && probe->ops[i].instance == instance) {
      ++count;
    }
  }
  return count;
}

vespera::StreamConfig config(OutputMode mode = OutputMode::monitor, uint32_t graph_channels = 2) {
  vespera::StreamConfig result;
  result.backend_id = "fake";
  result.device_id = "dev-a";
  result.input_channel = 3;
  result.output_channels = {2, 1};
  result.output_mode = mode;
  result.graph_channels = graph_channels;
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

// A graph of `count` gain nodes, each applying `gain`.
std::unique_ptr<Graph> gain_graph(float gain, uint32_t count = 1) {
  auto graph = std::make_unique<Graph>();
  for (uint32_t i = 0; i < count; ++i) {
    graph->set_parameter_value(graph->add_module(gain_module), 1, gain);
  }
  return graph;
}

// The documented crossfade: incoming weight min(1, (faded + frame + 1) / length).
float expected_mix(float from, float to, uint64_t elapsed, uint32_t length) {
  const double weight =
      elapsed >= length ? 1.0 : static_cast<double>(elapsed) / static_cast<double>(length);
  return static_cast<float>(static_cast<double>(from) * (1.0 - weight) +
                            static_cast<double>(to) * weight);
}

// True when frames [0, frames) on both outputs follow the crossfade from gain `from` to gain
// `to`, given `faded` frames already crossfaded before this block.
bool crossfaded(const Buffers &buffers, uint32_t frames, uint32_t faded, uint32_t length,
                float from, float to) {
  for (uint32_t i = 0; i < frames; ++i) {
    const float input = buffers.input[i];
    const float expected = expected_mix(input * from, input * to, faded + i + 1, length);
    if (std::fabs(buffers.left[i] - expected) > 1e-7f || buffers.right[i] != buffers.left[i]) {
      return false;
    }
  }
  return true;
}

bool gained(const Buffers &buffers, uint32_t frames, float gain) {
  for (uint32_t i = 0; i < frames; ++i) {
    const float expected = buffers.input[i] * gain;
    if (buffers.left[i] != expected || buffers.right[i] != expected) {
      return false;
    }
  }
  return true;
}

void test_publication_adoption_fade_and_retirement() {
  AudioSession session;
  session.graph().add_module(gain_module);
  session.prepare(config());
  session.start();
  const auto stream_id = session.stream_info().stream_id;
  const ParameterToken old_token = session.parameter_token();
  Buffers buffers(256);
  CHECK(buffers.run(64, 0) == AE_PROCESS_OK);
  CHECK(gained(buffers, 64, 1.0f));

  session.request_graph_replacement(gain_graph(2.0f, 2), 100);
  CHECK(session.graph_transition_state() == GraphTransitionState::queued);
  CHECK(session.parameter_token() == ParameterToken{});
  CHECK(session.nodes().size() == 1);
  auto status = session.poll();
  CHECK(status.transition.state == GraphTransitionState::queued);
  CHECK(status.transition.transition_frames == 100);
  CHECK(status.transition.faded_frames == 0);

  CHECK(buffers.run(40, 64) == AE_PROCESS_OK);
  CHECK(crossfaded(buffers, 40, 0, 100, 1.0f, 4.0f));
  status = session.poll();
  CHECK(status.transition.state == GraphTransitionState::crossfading);
  CHECK(status.transition.faded_frames == 40);
  CHECK(session.nodes().size() == 1);

  CHECK(buffers.run(1, 104) == AE_PROCESS_OK);
  CHECK(crossfaded(buffers, 1, 40, 100, 1.0f, 4.0f));
  CHECK(buffers.run(80, 105) == AE_PROCESS_OK);
  CHECK(crossfaded(buffers, 80, 41, 100, 1.0f, 4.0f));
  CHECK(buffers.left[58] == buffers.input[58] * 4.0f);
  CHECK(buffers.left[59] == buffers.input[59] * 4.0f);
  CHECK(buffers.left[57] != buffers.input[57] * 4.0f);
  CHECK(session.graph_transition_state() == GraphTransitionState::awaiting_retirement);
  CHECK(session.nodes().size() == 1);
  CHECK(session.parameter_token() == ParameterToken{});

  CHECK(buffers.run(64, 185) == AE_PROCESS_OK);
  CHECK(gained(buffers, 64, 4.0f));

  status = session.poll();
  CHECK(status.transition.state == GraphTransitionState::idle);
  CHECK(status.transition.committed == 1);
  CHECK(status.transition.cancelled == 0);
  CHECK(status.state == SessionState::running);
  CHECK(status.telemetry.discontinuities == 1);
  CHECK(status.telemetry.frames == 249);
  CHECK(session.stream_info().stream_id == stream_id);
  CHECK(fake_ae::control().open_calls == 1 && fake_ae::control().stop_calls == 0);
  const auto nodes = session.nodes();
  CHECK(nodes.size() == 2);

  const ParameterToken token = session.parameter_token();
  CHECK(token.graph_generation != 0 && token.graph_generation != old_token.graph_generation);
  CHECK(session.set_parameter(old_token, nodes[0].instance_id, 1, 0.5f) ==
        ParameterResult::stale_token);
  CHECK(session.set_parameter(token, nodes[1].instance_id, 1, 0.5f) == ParameterResult::accepted);
  CHECK(buffers.run(64, 249) == AE_PROCESS_OK);
  CHECK(gained(buffers, 64, 1.0f));
  CHECK(session.poll().telemetry.parameters.applied == 1);

  session.request_graph_replacement(gain_graph(0.5f), 1);
  CHECK(buffers.run(16, 313) == AE_PROCESS_OK);
  CHECK(gained(buffers, 16, 0.5f));
  CHECK(session.poll().transition.committed == 2);
  CHECK(session.nodes().size() == 1);
  CHECK(session.close() == CloseResult::closed);
}

void test_discontinuity_and_automation_during_fade() {
  AudioSession session;
  const NodeId old_node = session.graph().add_module(probe_module);
  session.prepare(config());
  session.start();
  const ParameterToken old_token = session.parameter_token();
  Buffers buffers(128);
  CHECK(buffers.run(32, 0) == AE_PROCESS_OK);
  CHECK(session.set_parameter(old_token, old_node, PROBE_PARAM_OFFSET, 0.5f, 1000) ==
        ParameterResult::accepted);
  CHECK(buffers.run(32, 32) == AE_PROCESS_OK);
  CHECK(session.set_parameter(old_token, old_node, PROBE_PARAM_OFFSET, 0.25f) ==
        ParameterResult::accepted);

  auto candidate = std::make_unique<Graph>();
  const NodeId new_node = candidate->add_module(probe_module);
  session.request_graph_replacement(std::move(candidate), 48);
  CHECK(session.set_parameter(old_token, old_node, PROBE_PARAM_LEVEL, 0.5f) ==
        ParameterResult::stale_token);
  CHECK(session.set_parameter(session.parameter_token(), new_node, PROBE_PARAM_LEVEL, 0.5f) ==
        ParameterResult::stale_token);

  CHECK(buffers.run(32, 64) == AE_PROCESS_OK);
  CHECK((probe->blocks[0].flags & VS_PROCESS_DISCONTINUITY) == 0);
  CHECK((probe->blocks[1].flags & VS_PROCESS_DISCONTINUITY) != 0);
  CHECK(probe->blocks[0].event_count == 0 && probe->blocks[1].event_count == 0);
  CHECK(buffers.run(16, 500) == AE_PROCESS_OK);
  CHECK((probe->blocks[0].flags & VS_PROCESS_DISCONTINUITY) != 0);
  CHECK((probe->blocks[1].flags & VS_PROCESS_DISCONTINUITY) != 0);
  CHECK(probe->blocks[1].calls == 2);
  CHECK(session.graph_transition_state() == GraphTransitionState::awaiting_retirement);
  CHECK(probe->live_instances == 2);

  CHECK(buffers.run(32, 516) == AE_PROCESS_OK);
  CHECK(probe->blocks[0].calls == 4);
  CHECK(probe->blocks[1].calls == 3);
  CHECK((probe->blocks[1].flags & VS_PROCESS_DISCONTINUITY) == 0);
  CHECK(probe->event_count == 0);
  CHECK(gained(buffers, 32, 1.0f));

  const auto status = session.poll();
  CHECK(probe->live_instances == 1);
  CHECK(probe_ops(PROBE_OP_RELEASE, 0) == 1 && probe_ops(PROBE_OP_DESTROY, 0) == 1);
  CHECK(status.telemetry.parameters.accepted == 2);
  CHECK(status.telemetry.parameters.stale == 2);
  CHECK(status.telemetry.parameters.applied == 0);
  CHECK(status.telemetry.parameters.rejected == 2);

  const ParameterToken token = session.parameter_token();
  CHECK(session.set_parameter(token, new_node, PROBE_PARAM_LEVEL, 2.0f, 548 + 4) ==
        ParameterResult::accepted);
  CHECK(buffers.run(8, 548) == AE_PROCESS_OK);
  CHECK(buffers.left[3] == buffers.input[3] && buffers.left[4] == buffers.input[4] * 2.0f);
  CHECK(session.poll().telemetry.parameters.applied == 1);
  CHECK(session.close() == CloseResult::closed);
  CHECK(probe->live_instances == 1);
  session.graph().remove_module(new_node);
  CHECK(probe->live_instances == 0);
}

void test_rejections_leave_active_graph() {
  AudioSession session;
  session.graph().add_module(gain_module);
  CHECK(throws<std::logic_error>([&] { session.request_graph_replacement(gain_graph(2.0f), 8); }));
  session.prepare(config());
  CHECK(throws<std::logic_error>([&] { session.request_graph_replacement(gain_graph(2.0f), 8); }));
  session.start();
  const ParameterToken token = session.parameter_token();
  CHECK(session.max_transition_frames() == 48000);
  CHECK(throws<std::invalid_argument>([&] { session.request_graph_replacement(nullptr, 8); }));
  CHECK(throws<std::invalid_argument>(
      [&] { session.request_graph_replacement(gain_graph(2.0f), 0); }));
  CHECK(throws<std::invalid_argument>(
      [&] { session.request_graph_replacement(gain_graph(2.0f), 48001); }));

  auto prepared = gain_graph(2.0f);
  prepared->prepare({48000.0, 256, 2});
  CHECK(
      throws<std::logic_error>([&] { session.request_graph_replacement(std::move(prepared), 8); }));

  CHECK(session.graph_transition_state() == GraphTransitionState::idle);
  CHECK(session.parameter_token() == token);
  Buffers buffers(64);
  CHECK(buffers.run(64, 0) == AE_PROCESS_OK);
  CHECK(gained(buffers, 64, 1.0f));

  auto probe_candidate = std::make_unique<Graph>();
  probe_candidate->add_module(probe_module);
  session.request_graph_replacement(gain_graph(2.0f), 48000);
  CHECK(probe->live_instances == 1);
  CHECK(throws<std::logic_error>(
      [&] { session.request_graph_replacement(std::move(probe_candidate), 8); }));
  CHECK(probe->live_instances == 0);
  CHECK(session.graph_transition_state() == GraphTransitionState::queued);
  CHECK(session.poll().transition.transition_frames == 48000);
  CHECK(session.close() == CloseResult::closed);
}

void test_prepare_failure_leaves_active_graph() {
  AudioSession session;
  const NodeId node = session.graph().add_module(gain_module);
  session.prepare(config());
  session.start();
  const ParameterToken token = session.parameter_token();

  auto candidate = std::make_unique<Graph>();
  candidate->add_module(probe_module);
  candidate->add_module(probe_module);
  probe->fail_prepare_instance = 1;
  CHECK(throws<std::runtime_error>(
      [&] { session.request_graph_replacement(std::move(candidate), 32); }));
  CHECK(probe->live_instances == 0);
  CHECK(probe_ops(PROBE_OP_RELEASE, 0) == 1);
  CHECK(session.graph_transition_state() == GraphTransitionState::idle);
  CHECK(session.parameter_token() == token);
  CHECK(session.nodes().size() == 1);
  CHECK(session.set_parameter(token, node, 1, 3.0f) == ParameterResult::accepted);
  Buffers buffers(64);
  CHECK(buffers.run(64, 0) == AE_PROCESS_OK);
  CHECK(gained(buffers, 64, 3.0f));
  const auto status = session.poll();
  CHECK(status.state == SessionState::running);
  CHECK(status.transition.committed == 0 && status.transition.cancelled == 0);
  CHECK(session.close() == CloseResult::closed);
}

void test_latency_must_match() {
  AudioSession session;
  session.graph().add_module(probe_module);
  probe->latency_frames[0] = 5;
  session.prepare(config());
  CHECK(session.stream_info().graph_latency_frames == 5);
  session.start();

  CHECK(throws<std::invalid_argument>(
      [&] { session.request_graph_replacement(std::make_unique<Graph>(), 16); }));
  auto mismatched = std::make_unique<Graph>();
  mismatched->add_module(probe_module);
  probe->latency_frames[1] = 6;
  CHECK(throws<std::invalid_argument>(
      [&] { session.request_graph_replacement(std::move(mismatched), 16); }));
  CHECK(probe_ops(PROBE_OP_PREPARE, 1) == 1 && probe_ops(PROBE_OP_RELEASE, 1) == 1);
  CHECK(probe_ops(PROBE_OP_DESTROY, 1) == 1);
  CHECK(session.graph_transition_state() == GraphTransitionState::idle);

  auto matching = std::make_unique<Graph>();
  matching->add_module(probe_module);
  probe->latency_frames[2] = 5;
  session.request_graph_replacement(std::move(matching), 16);
  Buffers buffers(32);
  CHECK(buffers.run(32, 0) == AE_PROCESS_OK);
  CHECK(session.poll().transition.committed == 1);
  CHECK(session.stream_info().graph_latency_frames == 5);
  CHECK(session.close() == CloseResult::closed);
  session.graph().remove_module(session.nodes()[0].instance_id);
}

void test_empty_replacement_is_dry_bypass() {
  AudioSession session;
  session.graph().set_parameter_value(session.graph().add_module(gain_module), 1, 2.0f);
  session.prepare(config());
  session.start();
  session.request_graph_replacement(std::make_unique<Graph>(), 80);
  Buffers buffers(128);
  CHECK(buffers.run(64, 0) == AE_PROCESS_OK);
  CHECK(crossfaded(buffers, 64, 0, 80, 2.0f, 1.0f));
  CHECK(buffers.run(64, 64) == AE_PROCESS_OK);
  CHECK(crossfaded(buffers, 64, 64, 80, 2.0f, 1.0f));
  CHECK(buffers.left[15] == buffers.input[15] && buffers.left[16] == buffers.input[16]);
  CHECK(session.poll().transition.committed == 1);
  CHECK(session.nodes().empty());
  CHECK(buffers.run(64, 128) == AE_PROCESS_OK);
  CHECK(buffers.left[0] == buffers.input[0] && buffers.right[63] == buffers.input[63]);
  CHECK(session.close() == CloseResult::closed);
}

void test_variable_blocks_are_sample_continuous() {
  AudioSession session;
  session.graph().add_module(gain_module);
  session.prepare(config(OutputMode::monitor));
  session.start();
  session.request_graph_replacement(gain_graph(3.0f), 211);
  Buffers buffers(256);
  const uint32_t sizes[] = {1, 7, 33, 2, 128, 64};
  uint64_t position = 0;
  uint32_t faded = 0;
  for (uint32_t frames : sizes) {
    CHECK(buffers.run(frames, position) == AE_PROCESS_OK);
    CHECK(crossfaded(buffers, frames, faded, 211, 1.0f, 3.0f));
    position += frames;
    faded = std::min<uint32_t>(faded + frames, 211);
  }
  CHECK(session.poll().transition.committed == 1);
  CHECK(session.close() == CloseResult::closed);
}

void test_silent_output_still_fades_and_retires() {
  AudioSession session;
  session.graph().add_module(gain_module);
  session.prepare(config(OutputMode::silent));
  session.start();
  session.request_graph_replacement(gain_graph(4.0f), 64);
  Buffers buffers(64);
  CHECK(buffers.run(64, 0) == AE_PROCESS_SILENCE);
  CHECK(buffers.left[63] == 0.0f && buffers.right[0] == 0.0f);
  const auto status = session.poll();
  CHECK(status.transition.committed == 1);
  CHECK(std::fabs(status.telemetry.output_peak - buffers.input[63] * 4.0f) < 1e-6f);
  CHECK(session.close() == CloseResult::closed);
}

void test_incoming_failure_mid_fade_enters_recovery() {
  AudioSession session;
  session.graph().add_module(probe_module);
  session.prepare(config());
  session.start();
  auto candidate = std::make_unique<Graph>();
  candidate->add_module(probe_module);
  session.request_graph_replacement(std::move(candidate), 64);
  Buffers buffers(32);
  CHECK(buffers.run(16, 0) == AE_PROCESS_OK);
  probe->fault_instance = 1;
  probe->fault = PROBE_FAULT_NAN;
  CHECK(buffers.run(16, 16) == AE_PROCESS_FAILED);
  CHECK(buffers.left[0] == 0.0f && buffers.right[15] == 0.0f);
  auto status = session.poll();
  CHECK(status.state == SessionState::recovery_required);
  CHECK(status.recovery == RecoveryReason::processing_failure);
  CHECK(status.telemetry.processing_failures == 1);
  CHECK(status.transition.state == GraphTransitionState::crossfading);
  CHECK(probe->live_instances == 2);

  session.stop();
  status = session.poll();
  CHECK(status.transition.state == GraphTransitionState::idle);
  CHECK(status.transition.committed == 1);
  CHECK(probe->live_instances == 1);
  CHECK(probe_ops(PROBE_OP_DESTROY, 0) == 1);
  CHECK(session.close() == CloseResult::closed);
  probe->fault_instance = -1;
  session.prepare(config());
  session.start();
  CHECK(buffers.run(32, 0) == AE_PROCESS_OK);
  CHECK(buffers.left[31] == buffers.input[31]);
  CHECK(session.close() == CloseResult::closed);
  session.graph().remove_module(session.nodes().at(0).instance_id);
}

void test_outgoing_failure_mid_fade_enters_recovery() {
  AudioSession session;
  session.graph().add_module(probe_module);
  session.prepare(config());
  session.start();
  session.request_graph_replacement(gain_graph(2.0f), 64);
  Buffers buffers(32);
  CHECK(buffers.run(16, 0) == AE_PROCESS_OK);
  probe->fault_instance = 0;
  probe->fault = PROBE_FAULT_ERROR;
  CHECK(buffers.run(16, 16) == AE_PROCESS_FAILED);
  CHECK(session.poll().recovery == RecoveryReason::processing_failure);
  CHECK(session.close() == CloseResult::closed);
  CHECK(probe->live_instances == 0);
  CHECK(session.graph().node_count() == 1);
  CHECK(session.graph().nodes()[0].module.id == "dev.vespera.gain");
}

void test_stop_while_queued_cancels() {
  AudioSession session;
  const NodeId node = session.graph().add_module(gain_module);
  session.prepare(config());
  session.start();
  auto candidate = std::make_unique<Graph>();
  candidate->add_module(probe_module);
  session.request_graph_replacement(std::move(candidate), 64);
  session.stop();
  CHECK(session.state() == SessionState::prepared);
  CHECK(session.graph_transition_state() == GraphTransitionState::idle);
  CHECK(probe->live_instances == 0);
  CHECK(probe_ops(PROBE_OP_RELEASE, 0) == 1);
  const auto status = session.poll();
  CHECK(status.transition.cancelled == 1 && status.transition.committed == 0);
  CHECK(session.nodes().size() == 1 && session.nodes()[0].instance_id == node);
  CHECK(session.parameter_value(node, 1) == 1.0f);

  session.start();
  const ParameterToken token = session.parameter_token();
  CHECK(session.set_parameter(token, node, 1, 2.0f) == ParameterResult::accepted);
  Buffers buffers(32);
  CHECK(buffers.run(32, 0) == AE_PROCESS_OK);
  CHECK(gained(buffers, 32, 2.0f));
  CHECK(session.close() == CloseResult::closed);
}

void test_stop_mid_fade_commits() {
  AudioSession session;
  session.graph().add_module(probe_module);
  session.prepare(config());
  session.start();
  session.request_graph_replacement(gain_graph(2.0f, 3), 64);
  Buffers buffers(32);
  CHECK(buffers.run(32, 0) == AE_PROCESS_OK);
  session.stop();
  CHECK(session.state() == SessionState::prepared);
  CHECK(probe->live_instances == 0);
  const auto status = session.poll();
  CHECK(status.transition.state == GraphTransitionState::idle);
  CHECK(status.transition.committed == 1);
  CHECK(session.nodes().size() == 3);
  CHECK(session.parameter_value(session.nodes()[0].instance_id, 1) == 2.0f);
  CHECK(session.capture_preset().nodes.size() == 3);

  session.start();
  CHECK(session.parameter_token().graph_generation != 0);
  CHECK(buffers.run(32, 32) == AE_PROCESS_OK);
  CHECK(gained(buffers, 32, 8.0f));
  CHECK(session.close() == CloseResult::closed);
}

void test_close_mid_fade_commits_and_releases() {
  AudioSession session;
  session.graph().add_module(probe_module);
  session.prepare(config());
  session.start();
  auto candidate = std::make_unique<Graph>();
  candidate->add_module(probe_module);
  candidate->add_module(gain_module);
  session.request_graph_replacement(std::move(candidate), 64);
  Buffers buffers(16);
  CHECK(buffers.run(16, 0) == AE_PROCESS_OK);
  CHECK(session.close() == CloseResult::closed);
  CHECK(session.state() == SessionState::stopped);
  CHECK(fake_ae::control().streams_alive == 0);
  CHECK(probe->live_instances == 1);
  CHECK(probe_ops(PROBE_OP_DESTROY, 0) == 1);
  CHECK(probe_ops(PROBE_OP_RELEASE, 1) == 1);
  CHECK(session.graph().node_count() == 2);
  CHECK(session.graph().state() == vespera::GraphState::editing);
  session.graph().remove_module(session.graph().nodes()[0].instance_id);
  CHECK(probe->live_instances == 0);
  CHECK(session.poll().transition.committed == 1);
}

void test_stop_failure_mid_fade_retains_both_graphs() {
  AudioSession session;
  session.graph().add_module(probe_module);
  session.prepare(config());
  session.start();
  auto candidate = std::make_unique<Graph>();
  candidate->add_module(probe_module);
  session.request_graph_replacement(std::move(candidate), 64);
  Buffers buffers(32);
  CHECK(buffers.run(16, 0) == AE_PROCESS_OK);
  fake_ae::control().stop_failures = 2;
  CHECK(throws<vespera::AudioError>([&] { session.stop(); }));
  CHECK(session.recovery_reason() == RecoveryReason::quiescence_failure);
  CHECK(session.close() == CloseResult::stop_failed);
  CHECK(probe->live_instances == 2);
  CHECK(probe_ops(PROBE_OP_RELEASE, 0) == 0 && probe_ops(PROBE_OP_RELEASE, 1) == 0);
  CHECK(session.poll().transition.state == GraphTransitionState::crossfading);

  CHECK(buffers.run(16, 16) == AE_PROCESS_OK);
  CHECK(crossfaded(buffers, 16, 16, 64, 1.0f, 1.0f));
  CHECK(probe->blocks[0].calls == 2 && probe->blocks[1].calls == 2);

  CHECK(session.close() == CloseResult::closed);
  CHECK(session.poll().transition.committed == 1);
  CHECK(probe->live_instances == 1);
  CHECK(probe_ops(PROBE_OP_DESTROY, 0) == 1);
  session.graph().remove_module(session.graph().nodes()[0].instance_id);
}

void test_mono_graph_duplicates_to_both_outputs() {
  AudioSession session;
  session.graph().set_parameter_value(session.graph().add_module(gain_module), 1, 2.0f);
  CHECK(throws<std::invalid_argument>([&] { session.prepare(config(OutputMode::monitor, 0)); }));
  CHECK(throws<std::invalid_argument>([&] { session.prepare(config(OutputMode::monitor, 3)); }));
  CHECK(fake_ae::control().open_calls == 0);
  session.prepare(config(OutputMode::monitor, 1));
  CHECK(session.stream_info().graph_channels == 1);
  CHECK(fake_ae::control().requested_outputs == (std::vector<uint32_t>{2, 1}));
  session.start();
  Buffers buffers(64);
  CHECK(buffers.run(64, 0) == AE_PROCESS_OK);
  CHECK(gained(buffers, 64, 2.0f));

  auto candidate = std::make_unique<Graph>();
  candidate->add_module(probe_module);
  session.request_graph_replacement(std::move(candidate), 32);
  CHECK(probe->last_prepare[0].channels == 1);
  CHECK(probe->last_prepare[0].max_frames == 256);
  CHECK(buffers.run(64, 64) == AE_PROCESS_OK);
  CHECK(probe->blocks[0].channels == 1);
  CHECK(crossfaded(buffers, 64, 0, 32, 2.0f, 1.0f));
  CHECK(session.close() == CloseResult::closed);
  session.graph().remove_module(session.graph().nodes()[0].instance_id);

  session.prepare(config());
  CHECK(session.stream_info().graph_channels == 2);
  CHECK(session.close() == CloseResult::closed);
}

void test_adapter_graph_channels() {
  Graph graph;
  graph.prepare({48000.0, 8, 1});
  vespera::MonoStereoAdapter adapter(graph, 8, 1);
  CHECK(adapter.graph_channels() == 1);
  std::vector<float> input(8, 0.25f);
  CHECK(adapter.process(input.data(), 8, 0, VS_PROCESS_OFFLINE) == VS_OK);
  CHECK(adapter.output(0) == adapter.output(1));
  CHECK(adapter.output(1)[7] == 0.25f);
  CHECK(adapter.output(2) == nullptr);
  CHECK(throws<std::invalid_argument>([&] { vespera::MonoStereoAdapter(graph, 8, 0); }));
  CHECK(throws<std::invalid_argument>([&] { vespera::MonoStereoAdapter(graph, 8, 3); }));
  graph.release();
  graph.prepare({48000.0, 8, 2});
  vespera::MonoStereoAdapter stereo(graph, 8);
  CHECK(stereo.graph_channels() == 2);
  CHECK(stereo.process(input.data(), 8, 0, VS_PROCESS_OFFLINE) == VS_OK);
  CHECK(stereo.output(0) != stereo.output(1));
  CHECK(stereo.output(1)[3] == 0.25f);
  graph.release();
}

void test_callback_never_touches_heap() {
  AudioSession session;
  const NodeId node = session.graph().add_module(gain_module);
  session.prepare(config());
  session.start();
  Buffers buffers(256);
  CHECK(buffers.run(64, 0) == AE_PROCESS_OK);
  CHECK(session.set_parameter(session.parameter_token(), node, 1, 2.0f, 5000) ==
        ParameterResult::accepted);
  session.request_graph_replacement(gain_graph(3.0f, 2), 300);

  counting_heap.store(true);
  allocation_count.store(0);
  deallocation_count.store(0);
  const uint32_t sizes[] = {1, 63, 128, 256, 64, 64};
  uint64_t position = 64;
  bool ok = true;
  for (uint32_t frames : sizes) {
    ok = ok && buffers.run(frames, position) == AE_PROCESS_OK;
    position += frames;
  }
  ok = ok && buffers.run(32, position + 100) == AE_PROCESS_OK;
  counting_heap.store(false);
  CHECK(ok);
  CHECK(allocation_count.load() == 0);
  CHECK(deallocation_count.load() == 0);
  CHECK(session.graph_transition_state() == GraphTransitionState::awaiting_retirement);
  CHECK(session.poll().transition.committed == 1);
  CHECK(session.close() == CloseResult::closed);
}

void test_destructor_mid_fade_quarantines_both_graphs() {
  {
    auto session = std::make_unique<AudioSession>();
    session->graph().add_module(gain_module);
    session->prepare(config());
    session->start();
    session->request_graph_replacement(gain_graph(3.0f), 64);
    Buffers buffers(16);
    CHECK(buffers.run(16, 0) == AE_PROCESS_OK);
    fake_ae::control().stop_failures = 1000;
    session.reset();
  }
  CHECK(fake_ae::control().process != nullptr);
  Buffers buffers(32);
  CHECK(buffers.run(32, 16) == AE_PROCESS_OK);
  CHECK(crossfaded(buffers, 32, 16, 64, 1.0f, 3.0f));
  CHECK(buffers.run(32, 48) == AE_PROCESS_OK);
  CHECK(crossfaded(buffers, 32, 48, 64, 1.0f, 3.0f));
}

}

int main(int argc, char **argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s GAIN_MODULE PROBE_MODULE\n", argv[0]);
    return 2;
  }
  gain_module = std::filesystem::canonical(argv[1]);
  probe_module = std::filesystem::canonical(argv[2]);
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

  // The destructor test deliberately leaks its callback state, so it runs last.
  const std::pair<const char *, std::function<void()>> tests[] = {
      {"publication_adoption_fade_and_retirement", test_publication_adoption_fade_and_retirement},
      {"discontinuity_and_automation_during_fade", test_discontinuity_and_automation_during_fade},
      {"rejections_leave_active_graph", test_rejections_leave_active_graph},
      {"prepare_failure_leaves_active_graph", test_prepare_failure_leaves_active_graph},
      {"latency_must_match", test_latency_must_match},
      {"empty_replacement_is_dry_bypass", test_empty_replacement_is_dry_bypass},
      {"variable_blocks_are_sample_continuous", test_variable_blocks_are_sample_continuous},
      {"silent_output_still_fades_and_retires", test_silent_output_still_fades_and_retires},
      {"incoming_failure_mid_fade_enters_recovery", test_incoming_failure_mid_fade_enters_recovery},
      {"outgoing_failure_mid_fade_enters_recovery", test_outgoing_failure_mid_fade_enters_recovery},
      {"stop_while_queued_cancels", test_stop_while_queued_cancels},
      {"stop_mid_fade_commits", test_stop_mid_fade_commits},
      {"close_mid_fade_commits_and_releases", test_close_mid_fade_commits_and_releases},
      {"stop_failure_mid_fade_retains_both_graphs", test_stop_failure_mid_fade_retains_both_graphs},
      {"mono_graph_duplicates_to_both_outputs", test_mono_graph_duplicates_to_both_outputs},
      {"adapter_graph_channels", test_adapter_graph_channels},
      {"callback_never_touches_heap", test_callback_never_touches_heap},
      {"destructor_mid_fade_quarantines_both_graphs",
       test_destructor_mid_fade_quarantines_both_graphs},
  };
  for (const auto &[name, test] : tests) {
    current_test = name;
    fake_ae::reset();
    reset_probe();
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
  std::printf("all live graph tests passed\n");
  return 0;
}
