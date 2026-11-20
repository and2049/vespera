#ifndef VESPERA_AUDIO_FUZZ_CASE_HPP
#define VESPERA_AUDIO_FUZZ_CASE_HPP

#include <vespera/runtime.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <span>
#include <vector>

namespace audio_fuzz {

inline void require(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "audio fuzz invariant: %s\n", message);
    std::abort();
  }
}

class Bytes {
public:
  explicit Bytes(std::span<const uint8_t> input) : input_(input) {}
  uint8_t byte() { return cursor_ < input_.size() ? input_[cursor_++] : 0; }
  uint32_t word() {
    uint32_t value = 0;
    for (uint32_t i = 0; i < 4; ++i) {
      value |= static_cast<uint32_t>(byte()) << (8 * i);
    }
    return value;
  }

private:
  std::span<const uint8_t> input_;
  size_t cursor_ = 0;
};

inline vs_result run(std::span<const uint8_t> input, const std::filesystem::path &module) {
  Bytes bytes(input);
  const uint32_t channels = 1 + bytes.byte() % 2;
  const uint32_t frames = 1 + bytes.byte();
  const uint32_t nodes = bytes.byte() % 5;
  const uint32_t signal = bytes.byte() % 8;
  const uint32_t alias = bytes.byte() % 3;
  const uint32_t invalid_event = bytes.byte() % 8;
  const uint32_t count_selector = bytes.byte();
  const uint32_t event_count = nodes == 0              ? 0
                               : count_selector >= 254 ? count_selector + 2
                                                       : count_selector % 33;
  constexpr uint32_t capacity = 256;
  constexpr float guard = 12345.0f;
  vespera::Graph graph;
  for (uint32_t i = 0; i < nodes; ++i) {
    graph.add_module(module);
  }
  constexpr double rates[] = {44100.0, 48000.0, 96000.0, 192000.0};
  const vespera::Format format{rates[bytes.byte() % 4], capacity, channels};
  graph.prepare(format);

  std::array<std::array<float, capacity + 3>, 2> source{}, destination{};
  std::array<std::array<float, capacity>, 2> expected{};
  std::array<std::array<uint32_t, capacity>, 2> original{};
  std::array<const float *, 2> inputs{};
  std::array<float *, 2> outputs{};
  bool finite_input = true;
  for (uint32_t c = 0; c < channels; ++c) {
    source[c].fill(guard);
    destination[c].fill(guard);
    inputs[c] = source[c].data() + 1;
    outputs[c] = alias == 0 ? destination[c].data() + 1 : source[c].data() + alias;
    for (uint32_t f = 0; f < frames; ++f) {
      const float phase = static_cast<float>(f) * 0.13f + static_cast<float>(c);
      float sample = 0.0f;
      switch (signal) {
      case 0:
        break;
      case 1:
        sample = f == 0 ? 1.0f : 0.0f;
        break;
      case 2:
        sample = std::sin(phase);
        break;
      case 3:
        sample = std::sin(phase * phase * 0.1f);
        break;
      case 4:
        sample = std::exp(-static_cast<float>(f) * 0.02f) *
                 (std::sin(phase) + 0.3f * std::sin(phase * 3.0f));
        break;
      case 5:
        sample = (static_cast<float>(bytes.byte()) - 128.0f) / 128.0f;
        break;
      case 6:
        sample = std::numeric_limits<float>::denorm_min() * static_cast<float>(bytes.byte());
        break;
      case 7:
        sample = std::bit_cast<float>(bytes.word());
        break;
      }
      source[c][f + 1] = sample;
      expected[c][f] = sample;
      original[c][f] = std::bit_cast<uint32_t>(sample);
      finite_input = finite_input && std::isfinite(sample);
    }
  }
  std::vector<vespera::Event> events;
  for (uint32_t e = 0; e < event_count; ++e) {
    const auto node = static_cast<vespera::NodeId>(bytes.byte() % nodes);
    const uint32_t offset = bytes.byte() % frames;
    const float gain = static_cast<float>(bytes.byte()) / 64.0f;
    events.push_back({node, {1, offset, gain}});
  }
  std::stable_sort(events.begin(), events.end(), [](const auto &a, const auto &b) {
    return a.parameter.frame_offset < b.parameter.frame_offset;
  });
  bool valid_events = event_count <= vespera::Graph::max_events_per_block;
  if (!events.empty() && invalid_event < 4) {
    valid_events = false;
    switch (invalid_event) {
    case 0:
      events.back().node = vespera::invalid_node_id;
      break;
    case 1:
      events.back().parameter.parameter_id = 999;
      break;
    case 2:
      events.back().parameter.frame_offset = frames;
      break;
    case 3:
      events.back().parameter.value = std::numeric_limits<float>::quiet_NaN();
      break;
    }
  }
  bool finite_output = true;
  std::array<float, 4> gains{1.0f, 1.0f, 1.0f, 1.0f};
  if (finite_input && valid_events) {
    for (uint32_t n = 0; n < nodes; ++n) {
      for (uint32_t f = 0; f < frames; ++f) {
        for (const auto &event : events) {
          if (event.node == n && event.parameter.frame_offset == f) {
            gains[n] = event.parameter.value;
          }
        }
        for (uint32_t c = 0; c < channels; ++c) {
          expected[c][f] *= gains[n];
          finite_output = finite_output && std::isfinite(expected[c][f]);
        }
      }
    }
  }
  const vs_result wanted = !finite_input || !valid_events ? VS_ERROR_ARGUMENT
                           : !finite_output               ? VS_ERROR_PROCESS
                                                          : VS_OK;
  const vespera::Block block{inputs.data(), outputs.data(),
                             channels,      frames,
                             bytes.word(),  VS_PROCESS_OFFLINE | VS_PROCESS_DISCONTINUITY};
  require(graph.process(block, events) == wanted, "processing result differs from reference");
  for (uint32_t c = 0; c < channels; ++c) {
    for (uint32_t f = 0; f < frames; ++f) {
      require(outputs[c][f] == (wanted == VS_OK ? expected[c][f] : 0.0f),
              "sample differs from reference or failure did not silence output");
      if (alias == 0) {
        require(std::bit_cast<uint32_t>(source[c][f + 1]) == original[c][f],
                "out-of-place processing modified its input");
      }
    }
    require(source[c][0] == guard && source[c][frames + 2] == guard,
            "input/aliased buffer guard changed");
    require(destination[c][0] == guard && destination[c][frames + 1] == guard,
            "output buffer guard changed");
  }
  require(graph.state() == (wanted == VS_ERROR_PROCESS ? vespera::GraphState::failed
                                                       : vespera::GraphState::prepared),
          "unexpected graph lifecycle state");
  if (wanted == VS_ERROR_PROCESS) {
    require(graph.process(block) == VS_ERROR_STATE, "processing failure did not latch");
    graph.release();
    graph.prepare(format);
  }
  graph.reset();
  for (uint32_t c = 0; c < channels; ++c) {
    source[c].fill(0.25f);
  }
  events.clear();
  for (uint32_t n = 0; n < nodes; ++n) {
    events.push_back({n, {1, 0, 1.0f}});
  }
  require(graph.process(block, events) == VS_OK,
          "valid processing after rejection/recovery failed");
  for (uint32_t c = 0; c < channels; ++c) {
    for (uint32_t f = 0; f < frames; ++f) {
      require(outputs[c][f] == 0.25f, "recovered processing has incorrect samples");
    }
  }
  return wanted;
}

}

#endif
