#include <vespera/audio_session.hpp>

#include "scripted_audio_backend.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

using vespera::AudioSession;
using vespera::CloseResult;
using vespera::CoreIoSettings;
using vespera::OutputMode;
using vespera::ParameterResult;
using vespera::SessionState;

int failures = 0;
const char *current_test = "";
std::filesystem::path gain_module;

constexpr uint32_t kBlock = 64;
constexpr uint32_t kRate = 48000;
// 20 ms gain ramp and 6 ms doubler delay at 48 kHz.
constexpr uint32_t kRampFrames = 960;
constexpr uint32_t kDoublerFrames = 288;
// 2 + (window + search) / 2 with a 960-frame window and 480-frame search range at 48 kHz.
constexpr uint32_t kTransposeLatency = 722;

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

float linear(float db) {
  return static_cast<float>(std::pow(10.0, static_cast<double>(db) / 20.0));
}

// Mirrors the processor's linear gain smoothing so ramped samples can be compared exactly.
struct RampModel {
  double current = 1.0;
  double target = 1.0;
  double step = 0.0;
  uint32_t remaining = 0;

  void set_target(double value) {
    if (value == target) {
      return;
    }
    target = value;
    step = (target - current) / kRampFrames;
    remaining = kRampFrames;
  }

  float next() {
    if (remaining != 0) {
      --remaining;
      current = remaining == 0 ? target : current + step;
    }
    return static_cast<float>(current);
  }
};

vespera::StreamConfig config(OutputMode mode = OutputMode::monitor) {
  vespera::StreamConfig result;
  result.backend_id = scripted_ae::backend_id;
  result.device_id = scripted_ae::device_id;
  result.input_channel = 2;
  result.output_channels = {3, 0};
  result.sample_rate = kRate;
  result.frames_per_block = kBlock;
  result.output_mode = mode;
  return result;
}

vespera::StreamConfig stereo_config(OutputMode mode = OutputMode::monitor) {
  vespera::StreamConfig result = config(mode);
  result.input_mode = vespera::InputMode::stereo;
  result.second_input_channel = 1;
  return result;
}

struct Capture {
  std::vector<float> left;
  std::vector<float> right;
  bool all_ok = true;
};

// Delivers the signal in blocks of at most `block` frames, starting the stream timeline at
// `position` when given, and concatenates the presented outputs.
Capture run(const std::vector<float> &left_input, const std::vector<float> *right_input,
            uint32_t block = kBlock, ae_process_result expected = AE_PROCESS_OK,
            std::optional<uint64_t> position = std::nullopt) {
  Capture capture;
  std::vector<float> in_left(block);
  std::vector<float> in_right(block);
  std::vector<float> out_left(block);
  std::vector<float> out_right(block);
  const auto total = static_cast<uint32_t>(left_input.size());
  for (uint32_t offset = 0; offset < total; offset += block) {
    const uint32_t frames = std::min(block, total - offset);
    std::copy_n(left_input.begin() + offset, frames, in_left.begin());
    const float stale = std::numeric_limits<float>::quiet_NaN();
    ae_process_result result;
    if (right_input != nullptr) {
      std::copy_n(right_input->begin() + offset, frames, in_right.begin());
      result = scripted_ae::deliver(in_left.data(), in_right.data(), out_left.data(),
                                    out_right.data(), frames, stale, position);
    } else {
      result = scripted_ae::deliver(in_left.data(), out_left.data(), out_right.data(), frames,
                                    stale, position);
    }
    if (position) {
      *position += frames;
    }
    capture.all_ok = capture.all_ok && result == expected;
    capture.left.insert(capture.left.end(), out_left.begin(), out_left.begin() + frames);
    capture.right.insert(capture.right.end(), out_right.begin(), out_right.begin() + frames);
  }
  return capture;
}

Capture run(const std::vector<float> &input, uint32_t block = kBlock,
            ae_process_result expected = AE_PROCESS_OK) {
  return run(input, nullptr, block, expected);
}

std::vector<float> ramp_signal(uint32_t frames, float scale = 0.001f, float offset = 0.0f) {
  std::vector<float> signal(frames);
  for (uint32_t i = 0; i < frames; ++i) {
    signal[i] = offset + scale * static_cast<float>(i % 400 + 1) * ((i & 1) != 0 ? -1.0f : 1.0f);
  }
  return signal;
}

std::vector<float> impulse(uint32_t frames, uint32_t at, float amplitude) {
  std::vector<float> signal(frames, 0.0f);
  signal[at] = amplitude;
  return signal;
}

bool only_impulse(const std::vector<float> &samples, uint32_t at, float amplitude) {
  for (uint32_t i = 0; i < samples.size(); ++i) {
    if (samples[i] != (i == at ? amplitude : 0.0f)) {
      return false;
    }
  }
  return true;
}

bool all_zero(const std::vector<float> &samples, size_t begin = 0,
              size_t end = std::numeric_limits<size_t>::max()) {
  end = std::min(end, samples.size());
  return std::all_of(samples.begin() + static_cast<std::ptrdiff_t>(begin),
                     samples.begin() + static_cast<std::ptrdiff_t>(end),
                     [](float sample) { return sample == 0.0f; });
}

float peak(const std::vector<float> &samples) {
  float result = 0.0f;
  for (float sample : samples) {
    result = std::max(result, std::fabs(sample));
  }
  return result;
}

double rms(const std::vector<float> &samples) {
  double energy = 0.0;
  for (float sample : samples) {
    energy += static_cast<double>(sample) * sample;
  }
  return std::sqrt(energy / static_cast<double>(samples.size()));
}

void add_gain(AudioSession &session, float value) {
  const vespera::NodeId node = session.graph().add_module(gain_module);
  session.set_parameter_value(node, 1, value);
}

void test_legacy_defaults_are_bit_transparent() {
  CHECK(vespera::StreamConfig{}.core_io == CoreIoSettings{});
  AudioSession session;
  add_gain(session, 1.0f);
  session.prepare(config());
  CHECK(session.core_io_settings() == CoreIoSettings{});
  const auto &info = session.stream_info();
  CHECK(info.core_io_latency_frames == 0);
  CHECK(info.graph_latency_frames == 0);
  session.start();

  std::vector<float> input = ramp_signal(4 * kBlock);
  input[3] = std::numeric_limits<float>::denorm_min();
  input[4] = -std::numeric_limits<float>::denorm_min() * 7.0f;
  input[5] = 1.0e30f;
  input[6] = -3.5f;
  input[7] = -0.0f;
  input[8] = std::numeric_limits<float>::max();
  const Capture out = run(input, 37);
  CHECK(out.all_ok);
  CHECK(out.left == input);
  CHECK(out.right == input);
  CHECK(session.poll().state == SessionState::running);
  CHECK(session.close() == CloseResult::closed);
}

void test_prepare_applies_configured_gains_exactly() {
  AudioSession session;
  add_gain(session, 2.0f);
  vespera::StreamConfig stream = config();
  stream.core_io.input_gain_db = 6.0f;
  stream.core_io.output_gain_db = -9.5f;
  session.prepare(stream);
  CHECK(session.core_io_settings() == stream.core_io);
  CHECK(session.stream_info().core_io_latency_frames == 0);
  session.start();
  const std::vector<float> input = ramp_signal(3 * kBlock);
  const Capture out = run(input);
  CHECK(out.all_ok);
  const float gi = linear(6.0f);
  const float go = linear(-9.5f);
  bool exact = true;
  for (size_t i = 0; i < input.size(); ++i) {
    const float expected = ((input[i] * gi) * 2.0f) * go;
    exact = exact && out.left[i] == expected && out.right[i] == expected;
  }
  CHECK(exact);
  CHECK(session.close() == CloseResult::closed);
}

void test_prepared_update_takes_effect_at_start() {
  AudioSession session;
  CHECK(throws<std::logic_error>([&] { session.set_core_io_settings(CoreIoSettings{}); }));
  session.prepare(config());
  CoreIoSettings settings;
  settings.input_gain_db = -3.0f;
  settings.output_gain_db = 4.0f;
  session.set_core_io_settings(settings);
  CHECK(session.core_io_settings() == settings);
  CHECK(session.stream_info().core_io_latency_frames == 0);
  settings.output_gain_db = 5.0f;
  session.set_core_io_settings(settings);
  CHECK(session.core_io_settings() == settings);
  session.start();
  const std::vector<float> input = ramp_signal(2 * kBlock);
  const Capture out = run(input);
  CHECK(out.all_ok);
  bool exact = true;
  for (size_t i = 0; i < input.size(); ++i) {
    const float expected = (input[i] * linear(-3.0f)) * linear(5.0f);
    exact = exact && out.left[i] == expected && out.right[i] == expected;
  }
  CHECK(exact);
  CHECK(session.close() == CloseResult::closed);
  CHECK(throws<std::logic_error>([&] { session.set_core_io_settings(CoreIoSettings{}); }));
}

void test_running_update_applies_at_block_boundary() {
  AudioSession session;
  add_gain(session, 1.5f);
  session.prepare(config());
  session.start();
  const vespera::ParameterToken token = session.parameter_token();
  const auto nodes = session.nodes();

  const std::vector<float> first = ramp_signal(kBlock);
  Capture out = run(first);
  CHECK(out.all_ok);
  bool exact = true;
  for (size_t i = 0; i < first.size(); ++i) {
    exact = exact && out.left[i] == first[i] * 1.5f;
  }
  CHECK(exact);

  CoreIoSettings settings;
  settings.output_gain_db = -6.0f;
  session.set_core_io_settings(settings);
  CHECK(session.core_io_settings() == settings);
  CoreIoSettings busy = settings;
  busy.input_gain_db = 3.0f;
  CHECK(throws<std::logic_error>([&] { session.set_core_io_settings(busy); }));
  CHECK(session.core_io_settings() == settings);
  CHECK(session.parameter_token() == token);

  RampModel output;
  output.set_target(std::pow(10.0, -6.0 / 20.0));
  const std::vector<float> body = ramp_signal(kRampFrames + 3 * kBlock, 0.0007f);
  out = run(body, 48);
  CHECK(out.all_ok);
  exact = true;
  float worst = 0.0f;
  for (size_t i = 0; i < body.size(); ++i) {
    const float expected = (body[i] * 1.5f) * output.next();
    worst = std::max(worst, std::fabs(out.left[i] - expected));
    exact = exact && out.left[i] == expected && out.right[i] == expected;
  }
  CHECK(exact);
  if (!exact) {
    std::fprintf(stderr, "  ramp worst deviation %.9g\n", static_cast<double>(worst));
  }
  CHECK(out.left[0] != body[0] * 1.5f);
  CHECK(out.left.back() == (body.back() * 1.5f) * linear(-6.0f));

  session.set_core_io_settings(busy);
  CHECK(session.core_io_settings() == busy);
  RampModel input;
  input.set_target(std::pow(10.0, 3.0 / 20.0));
  const std::vector<float> tail = ramp_signal(kRampFrames + kBlock, 0.0003f);
  out = run(tail);
  exact = true;
  for (size_t i = 0; i < tail.size(); ++i) {
    const float expected = ((tail[i] * input.next()) * 1.5f) * linear(-6.0f);
    exact = exact && out.left[i] == expected && out.right[i] == expected;
  }
  CHECK(exact);

  CHECK(session.nodes().size() == nodes.size());
  CHECK(session.parameter_token() == token);
  CHECK(session.set_parameter(token, nodes[0].instance_id, 1, 0.5f) == ParameterResult::accepted);
  out = run(first);
  exact = true;
  for (size_t i = 0; i < first.size(); ++i) {
    const float expected = ((first[i] * linear(3.0f)) * 0.5f) * linear(-6.0f);
    exact = exact && out.left[i] == expected;
  }
  CHECK(exact);
  const auto status = session.poll();
  CHECK(status.state == SessionState::running);
  CHECK(status.telemetry.parameters.applied == 1);
  CHECK(status.telemetry.discontinuities == 1);
  CHECK(session.close() == CloseResult::closed);
}

std::vector<CoreIoSettings> malformed_settings() {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  std::vector<CoreIoSettings> result;
  const auto with = [&](auto mutate) {
    CoreIoSettings settings;
    mutate(settings);
    result.push_back(settings);
  };
  with([&](CoreIoSettings &s) { s.input_gain_db = nan; });
  with([&](CoreIoSettings &s) { s.input_gain_db = 24.01f; });
  with([&](CoreIoSettings &s) { s.input_gain_db = -60.5f; });
  with([&](CoreIoSettings &s) { s.output_gain_db = inf; });
  with([&](CoreIoSettings &s) { s.output_gain_db = -inf; });
  with([&](CoreIoSettings &s) { s.output_gain_db = 30.0f; });
  with([&](CoreIoSettings &s) { s.gate_threshold_db = 0.5f; });
  with([&](CoreIoSettings &s) { s.gate_threshold_db = -121.0f; });
  with([&](CoreIoSettings &s) { s.gate_threshold_db = nan; });
  with([&](CoreIoSettings &s) { s.transpose_semitones = 12.5f; });
  with([&](CoreIoSettings &s) { s.transpose_semitones = -13.0f; });
  with([&](CoreIoSettings &s) { s.transpose_semitones = nan; });
  with([&](CoreIoSettings &s) { s.doubler_delay_ms = 0.5f; });
  with([&](CoreIoSettings &s) { s.doubler_delay_ms = 40.5f; });
  with([&](CoreIoSettings &s) {
    s.doubler_enabled = true;
    s.doubler_delay_ms = nan;
  });
  return result;
}

void test_malformed_settings_rejected_without_mutation() {
  AudioSession session;
  add_gain(session, 1.0f);
  const auto settings = malformed_settings();
  for (const CoreIoSettings &bad : settings) {
    vespera::StreamConfig stream = config();
    stream.core_io = bad;
    CHECK(throws<std::invalid_argument>([&] { session.prepare(stream); }));
    CHECK(session.state() == SessionState::stopped);
  }
  CHECK(scripted_ae::observed().open_calls == 0);

  vespera::StreamConfig stream = config();
  stream.core_io.output_gain_db = -6.0f;
  stream.core_io.transpose_semitones = 0.0f;
  session.prepare(stream);
  for (const CoreIoSettings &bad : settings) {
    CHECK(throws<std::invalid_argument>([&] { session.set_core_io_settings(bad); }));
    CHECK(session.core_io_settings() == stream.core_io);
  }
  CoreIoSettings transposed = stream.core_io;
  transposed.transpose_semitones = 2.0f;
  CHECK(throws<std::invalid_argument>([&] { session.set_core_io_settings(transposed); }));
  CHECK(session.core_io_settings() == stream.core_io);

  session.start();
  const std::vector<float> input = ramp_signal(kBlock);
  for (const CoreIoSettings &bad : settings) {
    CHECK(throws<std::invalid_argument>([&] { session.set_core_io_settings(bad); }));
  }
  CHECK(throws<std::invalid_argument>([&] { session.set_core_io_settings(transposed); }));
  CHECK(session.core_io_settings() == stream.core_io);
  Capture out = run(input);
  bool exact = true;
  for (size_t i = 0; i < input.size(); ++i) {
    exact = exact && out.left[i] == input[i] * linear(-6.0f) &&
            out.right[i] == input[i] * linear(-6.0f);
  }
  CHECK(exact);

  // A rejected update must not leave a queued change that would make the next one busy.
  CoreIoSettings boundary;
  boundary.input_gain_db = vespera::core_io_max_gain_db;
  boundary.output_gain_db = vespera::core_io_min_gain_db;
  boundary.gate_threshold_db = vespera::core_io_min_gate_threshold_db;
  boundary.doubler_delay_ms = vespera::core_io_max_doubler_delay_ms;
  session.set_core_io_settings(boundary);
  CHECK(session.core_io_settings() == boundary);
  out = run(input);
  CHECK(out.all_ok);
  boundary.gate_threshold_db = vespera::core_io_max_gate_threshold_db;
  boundary.doubler_delay_ms = vespera::core_io_min_doubler_delay_ms;
  boundary.input_gain_db = vespera::core_io_min_gain_db;
  boundary.output_gain_db = vespera::core_io_max_gain_db;
  session.set_core_io_settings(boundary);
  CHECK(session.core_io_settings() == boundary);
  CHECK(session.poll().state == SessionState::running);
  CHECK(session.close() == CloseResult::closed);
}

void test_post_core_output_meters() {
  AudioSession session;
  add_gain(session, 1.0f);
  vespera::StreamConfig stream = config(OutputMode::silent);
  stream.core_io.input_gain_db = 6.0f;
  stream.core_io.output_gain_db = 6.0f;
  stream.core_io.doubler_enabled = true;
  session.prepare(stream);
  session.start();
  const uint32_t frames = 8 * kBlock;
  std::vector<float> input = ramp_signal(frames, 0.0f);
  for (uint32_t i = 0; i < frames; ++i) {
    input[i] = i < 400 ? 0.3f * std::sin(0.05f * static_cast<float>(i)) : 0.0f;
  }
  const Capture silent = run(input, kBlock, AE_PROCESS_SILENCE);
  CHECK(silent.all_ok);
  CHECK(all_zero(silent.left) && all_zero(silent.right));

  const float gi = linear(6.0f);
  const float go = linear(6.0f);
  std::vector<float> left(frames);
  std::vector<float> right(frames, 0.0f);
  for (uint32_t i = 0; i < frames; ++i) {
    left[i] = (input[i] * gi) * go;
    if (i >= kDoublerFrames) {
      right[i] = (input[i - kDoublerFrames] * gi) * go;
    }
  }
  const auto meters = session.read_meters();
  CHECK(meters.input_channels == 1);
  CHECK(meters.input_frames == frames && meters.output_frames == frames);
  CHECK(meters.inputs[0].peak == peak(input));
  CHECK(meters.inputs[0].clipped_samples == 0);
  CHECK(meters.inputs[1].peak == 0.0f && meters.inputs[1].rms == 0.0f);
  CHECK(meters.outputs[0].peak == peak(left));
  CHECK(meters.outputs[1].peak == peak(right));
  CHECK(meters.outputs[0].peak > 1.0f);
  CHECK(std::fabs(static_cast<double>(meters.inputs[0].rms) - rms(input)) < 1e-6);
  CHECK(std::fabs(static_cast<double>(meters.outputs[0].rms) - rms(left)) < 1e-6);
  CHECK(std::fabs(static_cast<double>(meters.outputs[1].rms) - rms(right)) < 1e-6);
  const auto clipped = [](const std::vector<float> &samples) {
    return static_cast<uint64_t>(std::count_if(samples.begin(), samples.end(),
                                               [](float s) { return std::fabs(s) >= 1.0f; }));
  };
  CHECK(meters.outputs[0].clipped_samples == clipped(left));
  CHECK(meters.outputs[1].clipped_samples == clipped(right));
  CHECK(meters.outputs[0].clipped_samples != 0);
  const auto status = session.poll();
  CHECK(status.telemetry.output_peak == std::max(peak(left), peak(right)));
  CHECK(status.telemetry.input_peak == peak(input));
  CHECK(session.close() == CloseResult::closed);
}

void test_stereo_input_mode_is_per_channel() {
  AudioSession session;
  add_gain(session, 1.0f);
  vespera::StreamConfig stream = stereo_config();
  stream.core_io.input_gain_db = -4.0f;
  stream.core_io.gate_enabled = true;
  stream.core_io.gate_threshold_db = -40.0f;
  session.prepare(stream);
  CHECK(session.stream_info().input_channels == 2);
  session.start();
  const uint32_t frames = kRate + 4 * kBlock;
  std::vector<float> loud(frames);
  std::vector<float> quiet(frames);
  for (uint32_t i = 0; i < frames; ++i) {
    loud[i] = 0.5f * std::sin(0.01f * static_cast<float>(i)) + ((i & 1) != 0 ? 0.25f : -0.25f);
    quiet[i] = (i & 1) != 0 ? 0.001f : -0.001f;
  }
  const Capture out = run(loud, &quiet, kBlock);
  CHECK(out.all_ok);
  const float gi = linear(-4.0f);
  bool left_exact = true;
  for (uint32_t i = 0; i < frames; ++i) {
    left_exact = left_exact && out.left[i] == loud[i] * gi;
  }
  CHECK(left_exact);
  CHECK(std::fabs(out.right[0]) <= 0.001f * gi);
  CHECK(std::fabs(out.right[0]) > 0.0f);
  CHECK(all_zero(out.right, frames - 4 * kBlock));
  const auto meters = session.read_meters();
  CHECK(meters.input_channels == 2);
  CHECK(meters.inputs[1].peak == 0.001f);
  CHECK(meters.outputs[1].peak <= 0.001f * gi);

  // Swap which side is quiet: each channel's gate opens independently.
  const Capture swapped = run(quiet, &loud, kBlock);
  CHECK(swapped.all_ok);
  CHECK(all_zero(swapped.left, frames - 4 * kBlock));
  bool right_exact = true;
  for (uint32_t i = 512; i < frames; ++i) {
    right_exact = right_exact && swapped.right[i] == loud[i] * gi;
  }
  CHECK(right_exact);
  CHECK(session.close() == CloseResult::closed);
}

void test_gate_attenuates_quiet_signal() {
  AudioSession session;
  vespera::StreamConfig stream = config();
  session.prepare(stream);
  session.start();
  const uint32_t frames = kRate;
  std::vector<float> quiet(frames);
  for (uint32_t i = 0; i < frames; ++i) {
    quiet[i] = (i & 1) != 0 ? 0.002f : -0.002f;
  }
  Capture out = run(quiet);
  CHECK(out.left == quiet && out.right == quiet);

  CoreIoSettings gated;
  gated.gate_enabled = true;
  gated.gate_threshold_db = -40.0f;
  session.set_core_io_settings(gated);
  out = run(quiet);
  CHECK(out.all_ok);
  CHECK(std::fabs(out.left[0]) <= 0.002f);
  bool decaying = true;
  for (uint32_t i = 2; i < frames; ++i) {
    decaying = decaying && std::fabs(out.left[i]) <= std::fabs(out.left[i - 2]);
  }
  CHECK(decaying);
  CHECK(std::fabs(out.left[kRate / 10]) < 0.002f * 0.5f);
  CHECK(all_zero(out.left, frames - 2 * kBlock));
  CHECK(all_zero(out.right, frames - 2 * kBlock));

  std::vector<float> loud(8 * kBlock);
  for (uint32_t i = 0; i < loud.size(); ++i) {
    loud[i] = (i & 1) != 0 ? 0.4f : -0.4f;
  }
  out = run(loud);
  CHECK(std::fabs(out.left[0]) < 0.4f);
  bool reopened = true;
  // The 0.5 ms attack reaches unity gain within about 280 frames.
  for (uint32_t i = 5 * kBlock; i < loud.size(); ++i) {
    reopened = reopened && out.left[i] == loud[i] && out.right[i] == loud[i];
  }
  CHECK(reopened);
  CHECK(session.close() == CloseResult::closed);
}

void check_doubler_impulse(uint32_t graph_channels, bool stereo_input) {
  AudioSession session;
  add_gain(session, 1.0f);
  vespera::StreamConfig stream = stereo_input ? stereo_config() : config();
  stream.graph_channels = graph_channels;
  stream.core_io.doubler_enabled = true;
  stream.core_io.doubler_delay_ms = 6.0f;
  session.prepare(stream);
  CHECK(session.stream_info().core_io_latency_frames == 0);
  session.start();
  const uint32_t frames = 6 * kBlock;
  const std::vector<float> left = impulse(frames, 10, 0.5f);
  const std::vector<float> right = impulse(frames, 20, -0.75f);
  const Capture out = stereo_input ? run(left, &right) : run(left);
  CHECK(out.all_ok);
  CHECK(only_impulse(out.left, 10, 0.5f));
  if (stereo_input) {
    CHECK(only_impulse(out.right, 20 + kDoublerFrames, -0.75f));
  } else {
    CHECK(only_impulse(out.right, 10 + kDoublerFrames, 0.5f));
  }
  CHECK(session.close() == CloseResult::closed);
}

void test_doubler_six_millisecond_impulse() {
  check_doubler_impulse(2, false);
  check_doubler_impulse(1, false);
  check_doubler_impulse(2, true);
}

void test_doubler_enabled_while_running_settles_on_delay() {
  AudioSession session;
  session.prepare(config());
  session.start();
  CoreIoSettings settings;
  settings.doubler_enabled = true;
  session.set_core_io_settings(settings);
  Capture out = run(std::vector<float>(2 * kRampFrames, 0.0f));
  CHECK(out.all_ok && all_zero(out.left) && all_zero(out.right));
  out = run(impulse(8 * kBlock, 5, 0.25f));
  CHECK(only_impulse(out.left, 5, 0.25f));
  CHECK(only_impulse(out.right, 5 + kDoublerFrames, 0.25f));
  CHECK(session.close() == CloseResult::closed);
}

void test_timeline_discontinuity_resets_core_state() {
  AudioSession session;
  vespera::StreamConfig stream = config();
  stream.core_io.doubler_enabled = true;
  session.prepare(stream);
  session.start();

  Capture out = run(impulse(kBlock, 10, 0.5f));
  CHECK(only_impulse(out.left, 10, 0.5f) && all_zero(out.right));
  out = run(std::vector<float>(6 * kBlock, 0.0f), nullptr, kBlock, AE_PROCESS_OK, 100000);
  CHECK(all_zero(out.left) && all_zero(out.right));
  auto status = session.poll();
  CHECK(status.telemetry.discontinuities == 2);
  CHECK(status.state == SessionState::running);

  // A gain ramp still in flight snaps to its target when the timeline breaks.
  CoreIoSettings settings = stream.core_io;
  settings.output_gain_db = -12.0f;
  session.set_core_io_settings(settings);
  const std::vector<float> input = ramp_signal(kBlock);
  out = run(input);
  CHECK(out.left[kBlock - 1] != input[kBlock - 1] * linear(-12.0f));
  out = run(input, nullptr, kBlock, AE_PROCESS_OK, 7);
  bool exact = true;
  for (uint32_t i = 0; i < kBlock; ++i) {
    exact = exact && out.left[i] == input[i] * linear(-12.0f);
  }
  CHECK(exact);
  CHECK(session.poll().telemetry.discontinuities == 3);
  CHECK(session.close() == CloseResult::closed);
}

void test_stop_start_resets_and_applies_queued_update() {
  AudioSession session;
  vespera::StreamConfig stream = config();
  stream.core_io.doubler_enabled = true;
  session.prepare(stream);
  session.start();
  Capture out = run(impulse(kBlock, 30, 0.5f));
  CHECK(only_impulse(out.left, 30, 0.5f));

  CoreIoSettings settings = stream.core_io;
  settings.input_gain_db = 12.0f;
  session.set_core_io_settings(settings);
  session.stop();
  CHECK(session.state() == SessionState::prepared);
  CHECK(session.core_io_settings() == settings);
  session.start();
  const std::vector<float> input = ramp_signal(6 * kBlock, 0.0001f);
  out = run(input);
  CHECK(out.all_ok);
  bool exact = true;
  for (uint32_t i = 0; i < input.size(); ++i) {
    const float expected = input[i] * linear(12.0f);
    const float delayed = i >= kDoublerFrames ? input[i - kDoublerFrames] * linear(12.0f) : 0.0f;
    exact = exact && out.left[i] == expected && out.right[i] == delayed;
  }
  CHECK(exact);
  settings.input_gain_db = 0.0f;
  session.set_core_io_settings(settings);
  CHECK(session.close() == CloseResult::closed);
}

void test_transpose_requires_fixed_latency_from_prepare() {
  AudioSession session;
  vespera::StreamConfig stream = config();
  stream.core_io.transpose_semitones = 3.0f;
  session.prepare(stream);
  CHECK(session.stream_info().core_io_latency_frames == kTransposeLatency);
  CHECK(session.stream_info().graph_latency_frames == 0);
  session.start();

  std::vector<float> tone(kRate / 4);
  for (uint32_t i = 0; i < tone.size(); ++i) {
    tone[i] = 0.5f * std::sin(2.0f * 3.14159265f * 220.0f * static_cast<float>(i) / kRate);
  }
  Capture out = run(tone);
  CHECK(out.all_ok);
  CHECK(all_zero(out.left, 0, 512));
  CHECK(out.left == out.right);
  bool finite = true;
  for (float sample : out.left) {
    finite = finite && std::isfinite(sample) && std::fabs(sample) <= 1.0f;
  }
  CHECK(finite);
  CHECK(rms(std::vector<float>(out.left.begin() + kRate / 8, out.left.end())) > 0.2);

  CoreIoSettings settings = stream.core_io;
  settings.transpose_semitones = 0.0f;
  session.set_core_io_settings(settings);
  run(std::vector<float>(4 * kRampFrames, 0.0f));
  out = run(impulse(16 * kBlock, 3, 0.625f));
  CHECK(only_impulse(out.left, 3 + kTransposeLatency, 0.625f));
  CHECK(only_impulse(out.right, 3 + kTransposeLatency, 0.625f));

  settings.transpose_semitones = -vespera::core_io_max_transpose_semitones;
  session.set_core_io_settings(settings);
  out = run(tone);
  CHECK(out.all_ok);
  CHECK(session.stream_info().core_io_latency_frames == kTransposeLatency);
  CHECK(session.close() == CloseResult::closed);

  vespera::StreamConfig plain = config();
  session.prepare(plain);
  CHECK(session.stream_info().core_io_latency_frames == 0);
  CHECK(session.close() == CloseResult::closed);
}

void check_transparent_none_stream(AudioSession &session) {
  CHECK(session.stream_info().core_io_latency_frames == 0);
  CoreIoSettings transposed = session.core_io_settings();
  transposed.transpose_semitones = 1.0f;
  CHECK(throws<std::invalid_argument>([&] { session.set_core_io_settings(transposed); }));
  session.start();
  const std::vector<float> input = ramp_signal(4 * kBlock);
  const Capture out = run(input);
  CHECK(out.all_ok);
  CHECK(out.left == input && out.right == input);
  CHECK(throws<std::invalid_argument>([&] { session.set_core_io_settings(transposed); }));
  CHECK(session.close() == CloseResult::closed);
}

// The processor keeps its latency policy across close(), so each prepare must choose the policy
// from the new configuration rather than the previous stream's.
void test_reprepare_switches_transpose_latency_policy() {
  AudioSession session;
  session.prepare(config());
  check_transparent_none_stream(session);

  vespera::StreamConfig stream = config();
  stream.core_io.transpose_semitones = -5.0f;
  stream.core_io.output_gain_db = -3.0f;
  try {
    session.prepare(stream);
  } catch (const std::exception &error) {
    std::fprintf(stderr, "  none -> fixed re-prepare threw: %s\n", error.what());
    CHECK(false && "transposing prepare after a zero-transpose prepare");
    return;
  }
  CHECK(session.stream_info().core_io_latency_frames == kTransposeLatency);
  CHECK(session.core_io_settings() == stream.core_io);
  session.start();
  CoreIoSettings settings = stream.core_io;
  settings.transpose_semitones = 0.0f;
  session.set_core_io_settings(settings);
  run(std::vector<float>(4 * kRampFrames, 0.0f));
  const Capture out = run(impulse(16 * kBlock, 9, 0.5f));
  CHECK(only_impulse(out.left, 9 + kTransposeLatency, 0.5f * linear(-3.0f)));
  CHECK(only_impulse(out.right, 9 + kTransposeLatency, 0.5f * linear(-3.0f)));
  CHECK(session.close() == CloseResult::closed);

  session.prepare(config());
  CHECK(session.core_io_settings() == CoreIoSettings{});
  check_transparent_none_stream(session);

  stream.core_io.transpose_semitones = 7.0f;
  session.prepare(stream);
  CHECK(session.stream_info().core_io_latency_frames == kTransposeLatency);
  CHECK(session.close() == CloseResult::closed);
}

void test_graph_replacement_keeps_core_controls() {
  AudioSession session;
  add_gain(session, 2.0f);
  vespera::StreamConfig stream = config();
  stream.core_io.input_gain_db = 2.0f;
  stream.core_io.output_gain_db = -6.0f;
  session.prepare(stream);
  session.start();
  const auto old_token = session.parameter_token();
  const std::vector<float> input = ramp_signal(kBlock);
  Capture out = run(input);
  CHECK(out.all_ok);

  auto candidate = std::make_unique<vespera::Graph>();
  candidate->add_module(gain_module);
  candidate->set_parameter_value(0, 1, 0.5f);
  constexpr uint32_t fade = 100;
  session.request_graph_replacement(std::move(candidate), fade);
  CHECK(session.core_io_settings() == stream.core_io);
  const float gi = linear(2.0f);
  const float go = linear(-6.0f);
  out = run(std::vector<float>(input.begin(), input.begin() + 40));
  bool exact = true;
  for (uint32_t i = 0; i < 40; ++i) {
    const float x = input[i] * gi;
    const double weight = static_cast<double>(i + 1) / fade;
    const auto mixed = static_cast<float>(static_cast<double>(x * 2.0f) * (1.0 - weight) +
                                          static_cast<double>(x * 0.5f) * weight);
    exact = exact && out.left[i] == mixed * go && out.right[i] == mixed * go;
  }
  CHECK(exact);

  CoreIoSettings settings = stream.core_io;
  settings.output_gain_db = 0.0f;
  settings.input_gain_db = 0.0f;
  session.set_core_io_settings(settings);
  CHECK(session.graph_transition_state() == vespera::GraphTransitionState::crossfading);
  out = run(ramp_signal(kRampFrames + kBlock));
  CHECK(out.all_ok);
  const auto status = session.poll();
  CHECK(status.state == SessionState::running);
  CHECK(status.transition.committed == 1);
  CHECK(session.graph_transition_state() == vespera::GraphTransitionState::idle);
  CHECK(session.core_io_settings() == settings);
  const auto token = session.parameter_token();
  CHECK(token.graph_generation != old_token.graph_generation);

  out = run(input);
  exact = true;
  for (uint32_t i = 0; i < kBlock; ++i) {
    exact = exact && out.left[i] == input[i] * 0.5f && out.right[i] == input[i] * 0.5f;
  }
  CHECK(exact);
  settings.output_gain_db = -20.0f;
  session.set_core_io_settings(settings);
  CHECK(session.parameter_token() == token);
  CHECK(session.set_parameter(token, 0, 1, 3.0f) == ParameterResult::accepted);
  run(std::vector<float>(kRampFrames + kBlock, 0.0f));
  out = run(input);
  exact = true;
  for (uint32_t i = 0; i < kBlock; ++i) {
    exact = exact && out.left[i] == (input[i] * 3.0f) * linear(-20.0f);
  }
  CHECK(exact);
  session.stop();
  CHECK(session.parameter_value(0, 1) == 3.0f);
  CHECK(session.core_io_settings() == settings);
  CHECK(session.close() == CloseResult::closed);
}

void test_core_updates_preserve_module_parity() {
  AudioSession session;
  add_gain(session, 1.25f);
  add_gain(session, 2.0f);
  session.prepare(config());
  const auto nodes = session.nodes();
  CoreIoSettings settings;
  settings.gate_enabled = true;
  settings.doubler_enabled = true;
  session.set_core_io_settings(settings);
  CHECK(session.parameter_value(nodes[0].instance_id, 1) == 1.25f);
  CHECK(session.parameter_value(nodes[1].instance_id, 1) == 2.0f);
  session.start();
  const auto token = session.parameter_token();
  for (int i = 0; i < 8; ++i) {
    settings.output_gain_db = static_cast<float>(-i);
    session.set_core_io_settings(settings);
    CHECK(session.parameter_token() == token);
    run(ramp_signal(kBlock));
  }
  CHECK(session.nodes().size() == nodes.size());
  CHECK(session.set_parameter(token, nodes[1].instance_id, 1, 0.25f) == ParameterResult::accepted);
  run(ramp_signal(kBlock));
  const auto status = session.poll();
  CHECK(status.telemetry.parameters.applied == 1);
  CHECK(status.telemetry.parameters.stale == 0);
  CHECK(status.telemetry.processing_failures == 0);
  session.stop();
  CHECK(session.parameter_value(nodes[0].instance_id, 1) == 1.25f);
  CHECK(session.parameter_value(nodes[1].instance_id, 1) == 0.25f);
  CHECK(session.close() == CloseResult::closed);
  CHECK(session.graph().nodes().size() == 2);
}

void test_quiet_meter_rms() {
  AudioSession session;
  session.prepare(config());
  session.start();
  const std::vector<float> quiet(4800, 1.0e-6f);
  CHECK(run(quiet).all_ok);
  const auto reading = session.read_meters();
  CHECK(reading.input_frames == quiet.size());
  CHECK(reading.inputs[0].peak == quiet[0]);
  CHECK(reading.inputs[0].rms > 8.0e-7f);
  CHECK(reading.inputs[0].rms < 1.2e-6f);
  CHECK(reading.outputs[0].rms > 8.0e-7f);
  session.stop();
  CHECK(session.close() == CloseResult::closed);
}

void test_output_gain_overflow_fails_safely() {
  AudioSession session;
  auto settings = config();
  settings.core_io.output_gain_db = 24.0f;
  session.prepare(settings);
  session.start();
  const Capture result = run(std::vector<float>(64, 1.0e38f), nullptr, 64, AE_PROCESS_FAILED);
  CHECK(result.all_ok);
  CHECK(std::all_of(result.left.begin(), result.left.end(),
                    [](float value) { return value == 0.0f; }));
  CHECK(session.poll().telemetry.processing_failures == 1);
  session.stop();
  CHECK(session.close() == CloseResult::closed);
}

}

int main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s GAIN_MODULE\n", argv[0]);
    return 2;
  }
  gain_module = std::filesystem::canonical(argv[1]);

  const std::pair<const char *, std::function<void()>> tests[] = {
      {"legacy_defaults_are_bit_transparent", test_legacy_defaults_are_bit_transparent},
      {"prepare_applies_configured_gains_exactly", test_prepare_applies_configured_gains_exactly},
      {"prepared_update_takes_effect_at_start", test_prepared_update_takes_effect_at_start},
      {"running_update_applies_at_block_boundary", test_running_update_applies_at_block_boundary},
      {"malformed_settings_rejected_without_mutation",
       test_malformed_settings_rejected_without_mutation},
      {"post_core_output_meters", test_post_core_output_meters},
      {"quiet_meter_rms", test_quiet_meter_rms},
      {"output_gain_overflow_fails_safely", test_output_gain_overflow_fails_safely},
      {"stereo_input_mode_is_per_channel", test_stereo_input_mode_is_per_channel},
      {"gate_attenuates_quiet_signal", test_gate_attenuates_quiet_signal},
      {"doubler_six_millisecond_impulse", test_doubler_six_millisecond_impulse},
      {"doubler_enabled_while_running_settles_on_delay",
       test_doubler_enabled_while_running_settles_on_delay},
      {"timeline_discontinuity_resets_core_state", test_timeline_discontinuity_resets_core_state},
      {"stop_start_resets_and_applies_queued_update",
       test_stop_start_resets_and_applies_queued_update},
      {"transpose_requires_fixed_latency_from_prepare",
       test_transpose_requires_fixed_latency_from_prepare},
      {"reprepare_switches_transpose_latency_policy",
       test_reprepare_switches_transpose_latency_policy},
      {"graph_replacement_keeps_core_controls", test_graph_replacement_keeps_core_controls},
      {"core_updates_preserve_module_parity", test_core_updates_preserve_module_parity},
  };
  for (const auto &[name, test] : tests) {
    current_test = name;
    try {
      scripted_ae::reset();
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
  std::printf("all core IO session tests passed\n");
  return 0;
}
