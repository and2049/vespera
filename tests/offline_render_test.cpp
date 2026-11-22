#include "offline_render.hpp"

#include "fake_audio_engine.h"

#include <vespera/host_services.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using vespera::CoreIoSettings;
using vespera::Graph;
using vespera::OfflineRenderer;
using vespera::OfflineRenderSettings;

using Channels = std::vector<std::vector<float>>;

int failures = 0;
const char *current_test = "";
std::filesystem::path gain_module;
std::filesystem::path delay_module;
std::filesystem::path unbounded_module;
std::filesystem::path echo_module;

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

// Alternating loud and near-silent tone bursts with a little noise, so the gate opens and closes.
std::vector<float> take(size_t frames, double rate, double hz, uint32_t seed) {
  std::vector<float> result(frames);
  uint32_t state = seed;
  for (size_t i = 0; i < frames; ++i) {
    state = state * 1664525u + 1013904223u;
    const double noise = (static_cast<double>(state >> 8) / 16777216.0 - 0.5) * 0.002;
    const double level = (i / 2400) % 2 == 0 ? 0.5 : 0.0004;
    result[i] = static_cast<float>(
        level * std::sin(2.0 * 3.141592653589793 * hz * static_cast<double>(i) / rate) +
        noise * level);
  }
  return result;
}

std::unique_ptr<Graph> graph_of(const std::filesystem::path &module, float level) {
  auto graph = std::make_unique<Graph>();
  graph->set_host_services(
      vespera::HostServices::create({vespera::HostWorkMode::synchronous, 0, false}));
  const auto node = graph->add_module(module);
  graph->set_parameter_value(node, 1, level);
  return graph;
}

// Streams `input` through a fresh renderer in aligned mode and returns its output channels.
Channels render(Graph &graph, const OfflineRenderSettings &settings, const Channels &input,
                std::vector<uint64_t> *progress = nullptr,
                std::function<bool()> cancelled = nullptr,
                vespera::OfflineRenderOutcome *outcome = nullptr) {
  OfflineRenderer renderer(graph, settings);
  Channels output(renderer.output_channels());
  size_t position = 0;
  vespera::OfflineStream stream;
  stream.read = [&](float *const *channels, uint64_t frames) {
    const auto count = std::min<uint64_t>(frames, input[0].size() - position);
    for (size_t c = 0; c < input.size(); ++c) {
      std::copy_n(input[c].begin() + static_cast<std::ptrdiff_t>(position), count, channels[c]);
    }
    position += count;
    return count;
  };
  stream.write = [&](const float *const *channels, uint64_t frames) {
    for (size_t c = 0; c < output.size(); ++c) {
      output[c].insert(output[c].end(), channels[c], channels[c] + frames);
    }
  };
  stream.cancelled = std::move(cancelled);
  if (progress) {
    stream.progress = [&](uint64_t written) { progress->push_back(written); };
  }
  const auto result = vespera::render_aligned(renderer, stream);
  if (outcome) {
    *outcome = result;
  }
  return output;
}

OfflineRenderSettings live(uint32_t input_channels, CoreIoSettings core_io = {},
                           uint32_t block = 128, double rate = 48000.0) {
  return {rate, block, input_channels, 2, 120.0, core_io};
}

CoreIoSettings conditioned() {
  CoreIoSettings settings;
  settings.input_gain_db = 6.0f;
  settings.output_gain_db = -3.0f;
  settings.gate_enabled = true;
  settings.gate_threshold_db = -30.0f;
  settings.doubler_enabled = true;
  settings.doubler_delay_ms = 8.0f;
  return settings;
}

bool same(const std::vector<float> &a, const std::vector<float> &b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

// Runs the core IO stages and a stereo graph by hand, block by block, over the input followed
// by `extra` frames of silence.
Channels by_hand(Graph &graph, const CoreIoSettings &settings, const Channels &input, size_t extra,
                 uint32_t block, double rate) {
  vespera::CoreIoProcessor core_io;
  core_io.prepare(rate, block,
                  settings.transpose_semitones == 0.0f ? vespera::TransposeLatency::none
                                                       : vespera::TransposeLatency::fixed,
                  settings);
  graph.prepare({rate, block, 2});
  const size_t total = input[0].size() + extra;
  const auto channels = static_cast<uint32_t>(input.size());
  Channels output(2, std::vector<float>(total));
  std::vector<float> source(block), conditioned_left(block), conditioned_right(block);
  std::vector<float> source_right(block), graph_left(block), graph_right(block);
  for (size_t offset = 0; offset < total; offset += block) {
    const auto frames = static_cast<uint32_t>(std::min<size_t>(block, total - offset));
    for (uint32_t i = 0; i < frames; ++i) {
      const size_t at = offset + i;
      source[i] = at < input[0].size() ? input[0][at] : 0.0f;
      source_right[i] = at < input[0].size() ? input[channels - 1][at] : 0.0f;
    }
    const float *in[] = {source.data(), source_right.data()};
    float *conditioned_channels[] = {conditioned_left.data(), conditioned_right.data()};
    CHECK(core_io.process_input(in, conditioned_channels, channels, frames));
    const float *graph_in[] = {conditioned_left.data(),
                               channels == 2 ? conditioned_right.data() : conditioned_left.data()};
    float *graph_out[] = {graph_left.data(), graph_right.data()};
    const vs_tempo_context tempo{sizeof(vs_tempo_context),
                                 VS_TEMPO_VALID | (offset == 0 ? VS_TEMPO_DISCONTINUITY : 0u),
                                 120.0, static_cast<double>(offset) * 120.0 / (60.0 * rate)};
    const vespera::Block process{
        graph_in, graph_out,
        2,        frames,
        offset,   VS_PROCESS_OFFLINE | (offset == 0 ? uint32_t{VS_PROCESS_DISCONTINUITY} : 0u),
        &tempo};
    CHECK(graph.process(process) == VS_OK);
    const float *processed[] = {graph_left.data(), graph_right.data()};
    float *out[] = {output[0].data() + offset, output[1].data() + offset};
    CHECK(core_io.process_output(processed, 2, out, 2, frames));
  }
  graph.release();
  return output;
}

void test_transparent_core_io_matches_graph_alone() {
  const auto input = take(9000, 48000.0, 220.0, 1);
  auto mono = graph_of(gain_module, 0.5f);
  const auto alone = render(*mono, {48000.0, 128, 1, 1, 120.0, std::nullopt}, {input});
  CHECK(alone.size() == 1);
  auto stereo = graph_of(gain_module, 0.5f);
  const auto through = render(*stereo, live(1), {input});
  CHECK(through.size() == 2);
  CHECK(same(alone[0], through[0]));
  CHECK(same(alone[0], through[1]));
  CHECK(alone[0].size() == input.size());
  CHECK(alone[0][100] == input[100] * 0.5f);
}

void test_core_io_matches_processor_and_graph_by_hand() {
  for (const float transpose : {0.0f, 5.0f, -7.0f}) {
    auto settings = conditioned();
    settings.transpose_semitones = transpose;
    const auto input = take(9000, 48000.0, 196.0, 2);
    auto graph = graph_of(gain_module, 0.75f);
    uint64_t latency = 0;
    uint64_t tail = 0;
    {
      OfflineRenderer probe(*graph, live(1, settings));
      latency = probe.latency_frames();
      tail = probe.tail_frames();
    }
    CHECK(latency == (transpose == 0.0f ? 0u : 722u));
    CHECK(tail == 384u + (transpose == 0.0f ? 0u : 722u));
    graph = graph_of(gain_module, 0.75f);
    const auto rendered = render(*graph, live(1, settings), {input});
    auto reference_graph = graph_of(gain_module, 0.75f);
    const auto reference =
        by_hand(*reference_graph, settings, {input}, latency + tail, 128, 48000.0);
    CHECK(rendered[0].size() == input.size() + tail);
    for (size_t c = 0; c < 2; ++c) {
      const std::vector<float> aligned(reference[c].begin() + static_cast<std::ptrdiff_t>(latency),
                                       reference[c].end());
      CHECK(same(rendered[c], aligned));
    }
    CHECK(rendered[0] != rendered[1]);
  }
}

// The live session itself, driven through the fake device, is the reference: aligned offline
// output equals the session's output shifted by the reported latency.
void test_matches_live_session() {
  struct Case {
    uint32_t channels;
    float transpose;
  };
  for (const auto [channels, transpose] : {Case{1, 0.0f}, Case{1, 3.0f}, Case{2, 0.0f}}) {
    auto settings = conditioned();
    settings.transpose_semitones = transpose;
    const size_t frames = 12800;
    Channels input{take(frames, 48000.0, 233.0, 3)};
    if (channels == 2) {
      input.push_back(take(frames, 48000.0, 311.0, 4));
    }
    fake_ae::reset();
    vespera::AudioSession session;
    const auto node = session.graph().add_module(gain_module);
    session.graph().set_parameter_value(node, 1, 0.6f);
    vespera::StreamConfig config;
    config.backend_id = "fake";
    config.device_id = "dev-a";
    config.input_channel = 3;
    if (channels == 2) {
      config.input_mode = vespera::InputMode::stereo;
      config.second_input_channel = 1;
    }
    config.output_channels = {0, 1};
    config.output_mode = vespera::OutputMode::monitor;
    config.core_io = settings;
    session.prepare(config);
    session.start();
    Channels heard(2, std::vector<float>(frames));
    auto &state = fake_ae::control();
    for (size_t offset = 0; offset < frames; offset += 128) {
      const float *inputs[] = {input[0].data() + offset, input[channels - 1].data() + offset};
      float *outputs[] = {heard[0].data() + offset, heard[1].data() + offset};
      ae_audio_block block{};
      block.struct_size = sizeof(block);
      block.inputs = inputs;
      block.input_channels = channels;
      block.outputs = outputs;
      block.output_channels = 2;
      block.frames = 128;
      block.stream_id = state.stream_id;
      block.configuration_generation = state.configuration_generation;
      block.run_generation = state.run_generation;
      block.frame_position = offset;
      CHECK(state.process(state.user, &block) == AE_PROCESS_OK);
    }
    const auto latency = session.stream_info().core_io_latency_frames;
    CHECK(session.close() == vespera::CloseResult::closed);

    auto graph = graph_of(gain_module, 0.6f);
    const auto rendered = render(*graph, live(channels, settings), input);
    for (size_t c = 0; c < 2; ++c) {
      bool equal = true;
      for (size_t i = 0; i + latency < frames; ++i) {
        equal = equal && rendered[c][i] == heard[c][i + latency];
      }
      CHECK(equal);
    }
  }
}

void test_block_size_independence() {
  auto settings = conditioned();
  settings.transpose_semitones = 4.0f;
  const auto input = take(7001, 44100.0, 150.0, 5);
  std::optional<Channels> first;
  for (const uint32_t block : {1u, 7u, 128u, 1000u, 4096u}) {
    auto graph = graph_of(gain_module, 1.5f);
    const auto rendered = render(*graph, live(1, settings, block, 44100.0), {input});
    if (!first) {
      first = rendered;
      CHECK(rendered[0].size() > input.size());
      continue;
    }
    CHECK(same(rendered[0], (*first)[0]));
    CHECK(same(rendered[1], (*first)[1]));
  }
}

void test_latency_alignment_and_tail() {
  const auto input = take(5000, 48000.0, 440.0, 6);
  for (const uint32_t block : {64u, 100u, 128u}) {
    auto graph = graph_of(delay_module, 1.0f);
    {
      OfflineRenderer probe(*graph, live(1, {}, block));
      CHECK(probe.latency_frames() == 100);
      CHECK(probe.tail_frames() == 64);
    }
    graph = graph_of(delay_module, 1.0f);
    const auto rendered = render(*graph, live(1, {}, block), {input});
    CHECK(rendered[0].size() == input.size() + 64);
    bool aligned = true;
    for (size_t i = 0; i < rendered[0].size(); ++i) {
      const float direct = i < input.size() ? input[i] : 0.0f;
      const float echo = i >= 32 && i - 32 < input.size() ? input[i - 32] : 0.0f;
      aligned =
          aligned && rendered[0][i] == direct + 0.5f * echo && rendered[1][i] == rendered[0][i];
    }
    CHECK(aligned);
    CHECK(rendered[0][input.size() + 31] == 0.5f * input.back());
    CHECK(rendered[0][input.size() + 31] != 0.0f);
  }

  CoreIoSettings transposed;
  transposed.transpose_semitones = 2.0f;
  auto graph = graph_of(delay_module, 1.0f);
  const auto rendered = render(*graph, live(1, transposed), {input});
  CHECK(rendered[0].size() == input.size() + 64 + 722);
}

void test_unbounded_tail_is_capped() {
  const auto input = take(1000, 8000.0, 100.0, 7);
  auto graph = graph_of(unbounded_module, 1.0f);
  {
    OfflineRenderer probe(*graph, live(1, {}, 128, 8000.0));
    CHECK(probe.tail_frames() == 80000);
  }
  graph = graph_of(unbounded_module, 1.0f);
  const auto rendered = render(*graph, live(1, {}, 128, 8000.0), {input});
  CHECK(rendered[0].size() == input.size() + 80000);
}

void test_stereo_channels_stay_distinct() {
  const Channels input{take(3000, 48000.0, 220.0, 8), take(3000, 48000.0, 330.0, 9)};
  auto graph = graph_of(gain_module, 2.0f);
  const auto rendered = render(*graph, live(2), input);
  bool distinct = true;
  for (size_t i = 0; i < input[0].size(); ++i) {
    distinct =
        distinct && rendered[0][i] == input[0][i] * 2.0f && rendered[1][i] == input[1][i] * 2.0f;
  }
  CHECK(distinct);
  CHECK(rendered[0].size() == input[0].size());
}

void test_progress_and_cancellation() {
  const auto input = take(4000, 48000.0, 220.0, 10);
  auto graph = graph_of(gain_module, 1.0f);
  std::vector<uint64_t> progress;
  const auto rendered = render(*graph, live(1), {input}, &progress);
  CHECK(!progress.empty() && progress.back() == input.size());
  CHECK(std::is_sorted(progress.begin(), progress.end()));

  graph = graph_of(gain_module, 1.0f);
  size_t polls = 0;
  vespera::OfflineRenderOutcome outcome = vespera::OfflineRenderOutcome::finished;
  const auto partial =
      render(*graph, live(1), {input}, nullptr, [&] { return ++polls > 3; }, &outcome);
  CHECK(outcome == vespera::OfflineRenderOutcome::cancelled);
  CHECK(partial[0].size() == 3 * 128);
}

constexpr double echo_rate = 8000.0;
constexpr size_t echo_input_frames = 1000;
constexpr size_t echo_hold = 16000;
constexpr size_t echo_cap = 80000;

// A 200-frame tone burst at the start of a 1000-frame input.
std::vector<float> burst() {
  std::vector<float> result(echo_input_frames, 0.0f);
  for (size_t i = 0; i < 200; ++i) {
    result[i] = static_cast<float>(
        0.5 * std::sin(2.0 * 3.141592653589793 * 0.0371 * static_cast<double>(i)));
  }
  return result;
}

std::unique_ptr<Graph> echo_graph(float interval, float repeats, float hum) {
  auto graph = std::make_unique<Graph>();
  graph->set_host_services(
      vespera::HostServices::create({vespera::HostWorkMode::synchronous, 0, false}));
  const auto node = graph->add_module(echo_module);
  graph->set_parameter_value(node, 1, interval);
  graph->set_parameter_value(node, 2, repeats);
  graph->set_parameter_value(node, 3, hum);
  return graph;
}

// The echo fixture's output for `input` followed by silence, computed as the fixture does.
std::vector<float> echoes(const std::vector<float> &input, size_t interval, size_t repeats,
                          size_t frames) {
  std::vector<float> result(frames);
  for (size_t n = 0; n < frames; ++n) {
    float out = n < input.size() ? input[n] : 0.0f;
    float gain = 1.0f;
    for (size_t k = 1; k <= repeats; ++k) {
      gain *= 0.5f;
      const size_t at = n >= k * interval ? n - k * interval : input.size();
      out += gain * (at < input.size() ? input[at] : 0.0f);
    }
    result[n] = out;
  }
  return result;
}

size_t last_audible_end(const std::vector<float> &signal) {
  size_t end = 0;
  for (size_t i = 0; i < signal.size(); ++i) {
    if (std::abs(signal[i]) > vespera::offline_render_audible_level) {
      end = i + 1;
    }
  }
  return end;
}

void test_undeclared_tail_ends_at_last_audible_frame() {
  CHECK(echo_hold == static_cast<size_t>(echo_rate * vespera::offline_render_tail_hold_seconds));
  CHECK(echo_cap == static_cast<size_t>(echo_rate * vespera::offline_render_max_tail_seconds));
  const auto input = burst();
  const auto expected = echoes(input, 4000, 3, 40000);
  const size_t end = last_audible_end(expected);
  CHECK(end > 12190 && end <= 12200);
  for (const uint32_t block : {1u, 7u, 128u, 4096u}) {
    auto graph = echo_graph(4000, 3, 0);
    const auto rendered = render(*graph, live(1, {}, block, echo_rate), {input});
    CHECK(rendered[0].size() == end);
    CHECK(
        same(rendered[0], std::vector<float>(expected.begin(),
                                             expected.begin() + static_cast<std::ptrdiff_t>(end))));
    CHECK(same(rendered[1], rendered[0]));
  }
}

void test_gaps_shorter_than_hold_keep_later_echoes() {
  const auto input = burst();
  const size_t interval = 12000;
  CHECK(interval - 200 < echo_hold);
  const auto expected = echoes(input, interval, 2, 60000);
  auto graph = echo_graph(static_cast<float>(interval), 2, 0);
  const auto rendered = render(*graph, live(1, {}, 128, echo_rate), {input});
  CHECK(rendered[0].size() == last_audible_end(expected));
  CHECK(rendered[0].size() > 2 * interval);
}

void test_gap_longer_than_hold_ends_render() {
  const auto input = burst();
  const size_t interval = 24000;
  CHECK(interval - echo_input_frames > echo_hold);
  auto graph = echo_graph(static_cast<float>(interval), 2, 0);
  const auto rendered = render(*graph, live(1, {}, 128, echo_rate), {input});
  CHECK(rendered[0].size() == echo_input_frames);
}

void test_never_decaying_output_reaches_cap() {
  const auto input = burst();
  for (const uint32_t block : {7u, 4096u}) {
    auto graph = echo_graph(4000, 0, 0.01f);
    std::vector<uint64_t> progress;
    const auto rendered = render(*graph, live(1, {}, block, echo_rate), {input}, &progress);
    CHECK(rendered[0].size() == echo_input_frames + echo_cap);
    CHECK(rendered[0].back() == 0.01f);
    CHECK(std::is_sorted(progress.begin(), progress.end()));
    CHECK(!progress.empty() && progress.back() == rendered[0].size());
  }
}

void test_cancel_during_extension() {
  const auto input = burst();
  auto graph = echo_graph(4000, 3, 0);
  size_t polls = 0;
  std::vector<uint64_t> progress;
  vespera::OfflineRenderOutcome outcome = vespera::OfflineRenderOutcome::finished;
  const auto partial = render(
      *graph, live(1, {}, 128, echo_rate), {input}, &progress, [&] { return ++polls > 40; },
      &outcome);
  CHECK(outcome == vespera::OfflineRenderOutcome::cancelled);
  CHECK(partial[0].size() >= echo_input_frames && partial[0].size() < 40 * 128);
  CHECK(!progress.empty() && progress.back() == partial[0].size());
}

// Non-finite input samples are processed as zero on both paths, as live input is, without
// touching the caller's buffer.
void test_non_finite_input_processes_as_zero() {
  const Channels clean{take(9000, 48000.0, 220.0, 2), take(9000, 48000.0, 330.0, 3)};
  auto poisoned = clean;
  const std::array<size_t, 4> at{0, 127, 128, 4001};
  for (auto &channel : poisoned) {
    channel[at[0]] = std::numeric_limits<float>::quiet_NaN();
    channel[at[1]] = std::numeric_limits<float>::infinity();
    channel[at[2]] = -std::numeric_limits<float>::infinity();
  }
  poisoned[1][at[3]] = std::numeric_limits<float>::signaling_NaN();
  auto zeroed = clean;
  for (auto &channel : zeroed) {
    channel[at[0]] = channel[at[1]] = channel[at[2]] = 0.0f;
  }
  zeroed[1][at[3]] = 0.0f;
  for (const auto &settings :
       {live(2, conditioned()), OfflineRenderSettings{48000.0, 128, 2, 2, 120.0, std::nullopt}}) {
    auto graph = graph_of(gain_module, 0.5f);
    const auto rendered = render(*graph, settings, poisoned);
    auto reference_graph = graph_of(gain_module, 0.5f);
    CHECK(rendered == render(*reference_graph, settings, zeroed));
    CHECK(std::isnan(poisoned[0][at[0]]) && std::isinf(poisoned[1][at[2]]));
  }
}

void test_rejects_invalid_settings_and_samples() {
  const auto invalid = [](OfflineRenderSettings settings) {
    auto graph = graph_of(gain_module, 1.0f);
    return throws<std::invalid_argument>([&] { OfflineRenderer renderer(*graph, settings); });
  };
  CHECK(invalid(live(1, {}, 0)));
  CHECK(invalid(live(1, {}, vespera::offline_render_max_block_size + 1)));
  CHECK(invalid(live(3)));
  CHECK(invalid({48000.0, 128, 2, 1, 120.0, std::nullopt}));
  CHECK(invalid({48000.0, 128, 1, 2, 300.0, std::nullopt}));
  CHECK(invalid({std::numeric_limits<double>::quiet_NaN(), 128, 1, 2, 120.0, std::nullopt}));
  CoreIoSettings out_of_range;
  out_of_range.input_gain_db = 100.0f;
  CHECK(invalid(live(1, out_of_range)));

  auto graph = graph_of(gain_module, 1.0f);
  OfflineRenderer renderer(*graph, live(1));
  std::vector<float> samples(128, 0.25f), left(128), right(128);
  const float *in[] = {samples.data()};
  float *out[] = {left.data(), right.data()};
  CHECK(throws<std::invalid_argument>([&] { renderer.process(in, out, 129); }));
  renderer.process(in, out, 128);
  CHECK(left[3] == 0.25f && right[3] == 0.25f);

  auto overflowing = graph_of(gain_module, 1.0f);
  CoreIoSettings loud;
  loud.input_gain_db = vespera::core_io_max_gain_db;
  OfflineRenderer conditioning(*overflowing, live(1, loud));
  samples[5] = std::numeric_limits<float>::max();
  CHECK(throws<std::runtime_error>([&] { conditioning.process(in, out, 128); }));
  samples[5] = 0.25f;
  vespera::OfflineStream stream;
  stream.read = [](float *const *, uint64_t) -> uint64_t { return 0; };
  stream.write = [](const float *const *, uint64_t) {};
  CHECK(throws<std::logic_error>([&] { vespera::render_aligned(renderer, stream); }));

  auto overreading = graph_of(gain_module, 1.0f);
  OfflineRenderer fresh(*overreading, live(1));
  stream.read = [](float *const *, uint64_t frames) -> uint64_t { return frames + 1; };
  CHECK(throws<std::logic_error>([&] { vespera::render_aligned(fresh, stream); }));
}

}

int main(int argc, char **argv) {
  if (argc != 5) {
    std::fprintf(stderr, "usage: %s GAIN_MODULE DELAY_MODULE UNBOUNDED_DELAY_MODULE ECHO_MODULE\n",
                 argv[0]);
    return 2;
  }
  gain_module = argv[1];
  delay_module = argv[2];
  unbounded_module = argv[3];
  echo_module = argv[4];
  const std::pair<const char *, void (*)()> tests[] = {
      {"transparent_core_io_matches_graph_alone", test_transparent_core_io_matches_graph_alone},
      {"core_io_matches_processor_and_graph_by_hand",
       test_core_io_matches_processor_and_graph_by_hand},
      {"matches_live_session", test_matches_live_session},
      {"block_size_independence", test_block_size_independence},
      {"latency_alignment_and_tail", test_latency_alignment_and_tail},
      {"unbounded_tail_is_capped", test_unbounded_tail_is_capped},
      {"stereo_channels_stay_distinct", test_stereo_channels_stay_distinct},
      {"progress_and_cancellation", test_progress_and_cancellation},
      {"undeclared_tail_ends_at_last_audible_frame",
       test_undeclared_tail_ends_at_last_audible_frame},
      {"gaps_shorter_than_hold_keep_later_echoes", test_gaps_shorter_than_hold_keep_later_echoes},
      {"gap_longer_than_hold_ends_render", test_gap_longer_than_hold_ends_render},
      {"never_decaying_output_reaches_cap", test_never_decaying_output_reaches_cap},
      {"cancel_during_extension", test_cancel_during_extension},
      {"non_finite_input_processes_as_zero", test_non_finite_input_processes_as_zero},
      {"rejects_invalid_settings_and_samples", test_rejects_invalid_settings_and_samples},
  };
  for (const auto &[name, test] : tests) {
    current_test = name;
    test();
  }
  if (failures != 0) {
    std::fprintf(stderr, "%d offline render check(s) failed\n", failures);
    return 1;
  }
  std::puts("offline render tests passed");
  return 0;
}
