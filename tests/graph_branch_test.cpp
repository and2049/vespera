#include <vespera/runtime.hpp>

#include <vespera/instance_cache.hpp>

#include "fixtures/branch_control.h"
#include "fixtures/shared_control.h"

#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <new>
#include <optional>
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

using vespera::Block;
using vespera::Branch;
using vespera::Event;
using vespera::Format;
using vespera::Graph;
using vespera::GraphState;
using vespera::MixLaw;
using vespera::NodeId;

using Signal = std::vector<std::vector<float>>;

int failures = 0;
const char *current_test = "";

#define CHECK(condition)                                                                           \
  do {                                                                                             \
    if (!(condition)) {                                                                            \
      std::fprintf(stderr, "%s:%d: [%s] check failed: %s\n", __FILE__, __LINE__, current_test,     \
                   #condition);                                                                    \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

template <typename Exception>
bool throws(const std::function<void()> &action, const char *contains = nullptr) {
  try {
    action();
  } catch (const Exception &error) {
    return contains == nullptr || std::string(error.what()).find(contains) != std::string::npos;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "[%s] unexpected exception: %s\n", current_test, error.what());
    return false;
  }
  return false;
}

struct Paths {
  std::filesystem::path branch;
  std::filesystem::path no_values;
  std::filesystem::path no_bypass;
  std::filesystem::path shared_a;
  std::filesystem::path shared_b;
  std::filesystem::path shared_c;
  std::filesystem::path shared_retain;
  std::filesystem::path midi;
  std::filesystem::path stepped;
};

struct Spec {
  float gain = 1.0f;
  float offset = 0.0f;
  float latency = 0.0f;
  float tail = 0.0f;
  std::optional<float> mix;
  float power = 1.0f;
  const std::filesystem::path *path = nullptr;
};

Spec module(float gain, float offset, float latency = 0.0f) {
  return Spec{.gain = gain, .offset = offset, .latency = latency};
}

Spec branch(float gain, float offset, float mix, float latency = 0.0f) {
  return Spec{.gain = gain, .offset = offset, .latency = latency, .mix = mix};
}

const Paths *paths = nullptr;

Format format(uint32_t channels = 2, double rate = 48000.0, uint32_t max_frames = 1024) {
  return Format{rate, max_frames, channels};
}

std::vector<NodeId> build(Graph &graph, const std::vector<Spec> &specs) {
  std::vector<NodeId> ids;
  for (const Spec &spec : specs) {
    const NodeId id = graph.add_module(spec.path ? *spec.path : paths->branch);
    graph.set_parameter_value(id, BRANCH_PARAM_GAIN, spec.gain);
    graph.set_parameter_value(id, BRANCH_PARAM_OFFSET, spec.offset);
    graph.set_parameter_value(id, BRANCH_PARAM_LATENCY, spec.latency);
    graph.set_parameter_value(id, BRANCH_PARAM_TAIL, spec.tail);
    graph.set_parameter_value(id, BRANCH_PARAM_POWER, spec.power);
    if (spec.mix) {
      graph.set_parameter_value(id, BRANCH_PARAM_MIX, *spec.mix);
      graph.set_branch(id, Branch{BRANCH_PARAM_MIX, MixLaw::crossfade});
    }
    ids.push_back(id);
  }
  return ids;
}

Signal noise(uint32_t channels, size_t frames, uint32_t seed = 1) {
  Signal signal(channels, std::vector<float>(frames));
  uint32_t state = seed * 2654435761u + 1;
  for (auto &channel : signal) {
    for (float &sample : channel) {
      state = state * 1664525u + 1013904223u;
      sample = static_cast<float>(state >> 8) / static_cast<float>(1u << 24) * 2.0f - 1.0f;
    }
  }
  return signal;
}

Signal zeros(uint32_t channels, size_t frames) {
  return Signal(channels, std::vector<float>(frames, 0.0f));
}

struct Timed {
  uint64_t frame;
  Event event;
};

Timed at(uint64_t frame, NodeId node, uint32_t parameter, float value) {
  Event event;
  event.node = node;
  event.parameter = {parameter, 0, value};
  return {frame, event};
}

Timed midi_at(uint64_t frame, NodeId node, uint8_t status, uint8_t note, uint8_t velocity) {
  Event event;
  event.node = node;
  event.kind = Event::Kind::midi;
  event.midi = {0, status, note, velocity, 0};
  return {frame, event};
}

struct RenderOptions {
  uint32_t block = 64;
  std::vector<Timed> events;
  // Block-start frames flagged as discontinuities, and those preceded by Graph::reset().
  std::vector<uint64_t> discontinuities;
  std::vector<uint64_t> resets;
  std::vector<uint64_t> panics;
  bool in_place = false;
};

Signal render(Graph &graph, const Signal &input, const RenderOptions &options = {}) {
  const auto channels = static_cast<uint32_t>(input.size());
  const size_t length = input[0].size();
  Signal output = zeros(channels, length);
  Signal work = input;
  std::vector<const float *> in(channels);
  std::vector<float *> out(channels);
  std::vector<Event> events;
  size_t next = 0;
  for (size_t start = 0; start < length; start += options.block) {
    const auto frames = static_cast<uint32_t>(std::min<size_t>(options.block, length - start));
    events.clear();
    while (next < options.events.size() && options.events[next].frame < start + frames) {
      Event event = options.events[next].event;
      const auto offset = static_cast<uint32_t>(options.events[next].frame - start);
      (event.kind == Event::Kind::midi ? event.midi.frame_offset : event.parameter.frame_offset) =
          offset;
      events.push_back(event);
      ++next;
    }
    if (std::find(options.resets.begin(), options.resets.end(), start) != options.resets.end()) {
      graph.reset();
    }
    const bool discontinuity =
        std::find(options.discontinuities.begin(), options.discontinuities.end(), start) !=
        options.discontinuities.end();
    for (uint32_t c = 0; c < channels; ++c) {
      if (options.in_place) {
        out[c] = work[c].data() + start;
        in[c] = out[c];
      } else {
        in[c] = input[c].data() + start;
        out[c] = output[c].data() + start;
      }
    }
    Block block;
    block.inputs = in.data();
    block.outputs = out.data();
    block.channels = channels;
    block.frames = frames;
    block.frame_position = start;
    const bool panic =
        std::find(options.panics.begin(), options.panics.end(), start) != options.panics.end();
    block.flags = (discontinuity ? uint32_t(VS_PROCESS_DISCONTINUITY) : 0u) |
                  (panic ? uint32_t(VS_PROCESS_MIDI_PANIC) : 0u);
    const vs_result result = graph.process(block, events);
    CHECK(result == VS_OK);
    if (result != VS_OK) {
      break;
    }
  }
  return options.in_place ? work : output;
}

Signal render(const std::vector<Spec> &specs, const Signal &input, const Format &f,
              const RenderOptions &options = {}) {
  Graph graph;
  build(graph, specs);
  graph.prepare(f);
  return render(graph, input, options);
}

Signal slice_after(const Signal &signal, size_t from) {
  Signal result;
  for (const auto &channel : signal) {
    result.emplace_back(channel.begin() + static_cast<std::ptrdiff_t>(from), channel.end());
  }
  return result;
}

Signal crossfade(const Signal &main, const Signal &wet, float mix) {
  Signal result = main;
  for (size_t c = 0; c < main.size(); ++c) {
    for (size_t i = 0; i < main[c].size(); ++i) {
      result[c][i] = (1.0f - mix) * main[c][i] + mix * wet[c][i];
    }
  }
  return result;
}

Signal shift(const Signal &signal, size_t frames) {
  Signal result = zeros(static_cast<uint32_t>(signal.size()), signal[0].size());
  for (size_t c = 0; c < signal.size(); ++c) {
    for (size_t i = frames; i < signal[c].size(); ++i) {
      result[c][i] = signal[c][i - frames];
    }
  }
  return result;
}

float difference(const Signal &a, const Signal &b, size_t from = 0, size_t to = SIZE_MAX) {
  float largest = 0.0f;
  for (size_t c = 0; c < a.size(); ++c) {
    for (size_t i = from; i < std::min(to, a[c].size()); ++i) {
      largest = std::max(largest, std::fabs(a[c][i] - b[c][i]));
    }
  }
  return largest;
}

bool identical(const Signal &a, const Signal &b, size_t from = 0, size_t to = SIZE_MAX) {
  for (size_t c = 0; c < a.size(); ++c) {
    const size_t end = std::min(to, a[c].size());
    if (from < end &&
        std::memcmp(a[c].data() + from, b[c].data() + from, (end - from) * sizeof(float)) != 0) {
      return false;
    }
  }
  return true;
}

constexpr float tolerance = 2e-6f;

void test_serial_unchanged() {
  current_test = "serial_unchanged";
  const Signal input = noise(2, 2000);
  const std::vector<Spec> chain = {module(2.0f, 0.1f, 3), module(0.5f, -0.3f)};
  Graph plain;
  build(plain, chain);
  plain.prepare(format());
  CHECK(plain.branch_nodes().empty());
  const Signal expected = render(plain, input);

  Graph cleared;
  const auto ids = build(cleared, chain);
  cleared.set_branch(ids[1], Branch{BRANCH_PARAM_MIX});
  CHECK(cleared.branch_nodes() == std::vector<NodeId>{ids[1]});
  cleared.set_branch(ids[1], std::nullopt);
  CHECK(cleared.branch_nodes().empty());
  cleared.prepare(format());
  CHECK(cleared.latency_frames() == 3);
  CHECK(identical(render(cleared, input), expected));
}

void test_crossfade_points() {
  current_test = "crossfade_points";
  const Signal input = noise(2, 3000, 2);
  const Spec a = module(2.0f, 0.1f);
  const Spec b = module(0.5f, -0.3f);
  const Signal main = render({a, b}, input, format());
  const Signal wet = render({module(3.0f, 0.05f)}, input, format());
  for (int step = 0; step <= 8; ++step) {
    const float mix = static_cast<float>(step) / 8.0f;
    const Signal output = render({a, b, branch(3.0f, 0.05f, mix)}, input, format());
    const Signal expected = crossfade(main, wet, mix);
    if (step == 0) {
      CHECK(identical(output, main));
    } else if (step == 8) {
      CHECK(identical(output, wet));
    } else {
      CHECK(difference(output, expected) <= tolerance);
    }
  }
  // Input to the branch is the graph input, not the chain: S(B(A(x))) differs measurably.
  const Signal chained = render({a, b, module(3.0f, 0.05f)}, input, format());
  CHECK(difference(chained, wet) > 0.1f);
}

void test_merge_positions() {
  current_test = "merge_positions";
  const Signal input = noise(2, 2500, 3);
  const Spec a = module(2.0f, 0.1f);
  const Spec b = module(0.5f, -0.3f);
  const Signal s1 = render({module(3.0f, 0.05f)}, input, format());
  const Signal s2 = render({module(-1.5f, 0.2f)}, input, format());

  const Signal first = render({branch(3.0f, 0.05f, 0.25f), a, b}, input, format());
  CHECK(difference(first, render({a, b}, crossfade(input, s1, 0.25f), format())) <= 1e-5f);

  const Signal middle = render({a, branch(3.0f, 0.05f, 0.25f), b}, input, format());
  const Signal after_a = render({a}, input, format());
  CHECK(difference(middle, render({b}, crossfade(after_a, s1, 0.25f), format())) <= 1e-5f);

  const Signal two =
      render({a, branch(3.0f, 0.05f, 0.25f), b, branch(-1.5f, 0.2f, 0.75f)}, input, format());
  const Signal merged = render({b}, crossfade(after_a, s1, 0.25f), format());
  CHECK(difference(two, crossfade(merged, s2, 0.75f)) <= 1e-5f);

  // Adjacent branches each read the graph input; a branch never feeds another.
  const Signal adjacent =
      render({branch(3.0f, 0.05f, 0.25f), branch(-1.5f, 0.2f, 0.75f)}, input, format());
  CHECK(difference(adjacent, crossfade(crossfade(input, s1, 0.25f), s2, 0.75f)) <= 1e-5f);
}

void test_alignment() {
  current_test = "alignment";
  struct Case {
    float main_latency;
    float branch_latency;
    float main_tail;
    float branch_tail;
  };
  for (const Case &c : {Case{10, 3, 100, 98}, Case{5, 5, 20, 30}, Case{3, 10, 100, 1}}) {
    Spec a = module(1.0f, 0.0f, c.main_latency);
    a.tail = c.main_tail;
    Spec s = branch(1.0f, 0.0f, 0.5f, c.branch_latency);
    s.tail = c.branch_tail;
    Spec after = module(1.0f, 0.0f, 2);
    after.tail = 7;
    Graph graph;
    build(graph, {a, s, after});
    graph.prepare(format());
    const auto longer = static_cast<uint32_t>(std::max(c.main_latency, c.branch_latency));
    const auto main_delay =
        static_cast<uint64_t>(std::max(0.0f, c.branch_latency - c.main_latency));
    const auto branch_delay =
        static_cast<uint64_t>(std::max(0.0f, c.main_latency - c.branch_latency));
    CHECK(graph.latency_frames() == longer + 2);
    CHECK(graph.tail_frames() == std::max(static_cast<uint64_t>(c.main_tail) + main_delay,
                                          static_cast<uint64_t>(c.branch_tail) + branch_delay) +
                                     7);
    Signal input = zeros(2, 300);
    input[0][100] = 1.0f;
    input[1][100] = -1.0f;
    const Signal output = render(graph, input, {.block = 7});
    Signal expected = zeros(2, 300);
    expected[0][100 + longer + 2] = 1.0f;
    expected[1][100 + longer + 2] = -1.0f;
    CHECK(identical(output, expected));
  }

  Graph first;
  build(first, {branch(1.0f, 0.0f, 0.5f, 6), module(1.0f, 0.0f, 4)});
  first.prepare(format());
  CHECK(first.latency_frames() == 10);
  Signal impulse = zeros(1, 64);
  impulse[0][3] = 1.0f;
  first.release();
  first.prepare(format(1));
  Signal expected = zeros(1, 64);
  expected[0][13] = 1.0f;
  CHECK(identical(render(first, impulse, {.block = 5}), expected));
}

void test_oversized_delay() {
  current_test = "oversized_delay";
  for (bool branch_longer : {true, false}) {
    Graph graph;
    const auto ids = build(graph, {module(1.0f, 0.0f), branch(1.0f, 0.0f, 0.5f)});
    const NodeId longer = branch_longer ? ids[1] : ids[0];
    graph.set_parameter_value(longer, BRANCH_PARAM_LATENCY, 480001.0f);
    const std::string node = "node " + std::to_string(ids[1]);
    CHECK(throws<std::runtime_error>([&] { graph.prepare(format()); }, node.c_str()));
    CHECK(graph.state() == GraphState::editing);
    CHECK(graph.branch_nodes() == std::vector<NodeId>{ids[1]});
    CHECK(graph.processing_groups().empty());
    graph.set_parameter_value(longer, BRANCH_PARAM_LATENCY, 480000.0f);
    graph.prepare(format());
    CHECK(graph.state() == GraphState::prepared);
    CHECK(graph.latency_frames() == 480000);
  }
}

void test_ramp() {
  current_test = "ramp";
  for (double rate : {44100.0, 48000.0, 96000.0}) {
    const auto length = static_cast<size_t>(std::lround(rate * 0.010));
    for (uint32_t block : {1u, 64u, 128u, 1024u}) {
      Graph graph;
      const auto ids = build(graph, {module(0.0f, 1.0f), branch(0.0f, 0.0f, 0.0f)});
      graph.prepare(format(1, rate));
      const size_t start = 1037;
      const Signal output =
          render(graph, zeros(1, start + length + 300),
                 {.block = block, .events = {at(start, ids[1], BRANCH_PARAM_MIX, 1.0f)}});
      const auto &out = output[0];
      bool before = true;
      for (size_t i = 0; i < start; ++i) {
        before = before && out[i] == 1.0f;
      }
      CHECK(before);
      float worst = 0.0f;
      for (size_t k = 0; k + 1 < length; ++k) {
        const float expected = 1.0f - static_cast<float>(k + 1) / static_cast<float>(length);
        worst = std::max(worst, std::fabs(out[start + k] - expected));
      }
      CHECK(worst <= 1e-6f);
      CHECK(out[start + length - 2] > 0.0f);
      bool settled = true;
      for (size_t i = start + length - 1; i < out.size(); ++i) {
        settled = settled && out[i] == 0.0f;
      }
      CHECK(settled);
    }
  }

  // A retarget mid-ramp starts a new full-length ramp from the value reached.
  Graph graph;
  const auto ids = build(graph, {module(0.0f, 1.0f), branch(0.0f, 0.0f, 0.0f)});
  graph.prepare(format(1));
  const Signal output = render(graph, zeros(1, 2000),
                               {.events = {at(100, ids[1], BRANCH_PARAM_MIX, 1.0f),
                                           at(340, ids[1], BRANCH_PARAM_MIX, 0.0f)}});
  CHECK(std::fabs(output[0][339] - 0.5f) <= 1e-6f);
  CHECK(std::fabs(output[0][340] - (0.5f + 0.5f / 480.0f)) <= 1e-6f);
  CHECK(output[0][340 + 478] < 1.0f && output[0][340 + 479] == 1.0f);
}

void test_bypass_and_extremes() {
  current_test = "bypass_and_extremes";
  const Signal input = noise(2, 4500, 4);
  const Spec a = module(1.5f, 0.1f);
  Graph graph;
  const auto ids = build(graph, {a, branch(-2.0f, 0.2f, 0.5f, 3)});
  graph.prepare(format());
  CHECK(graph.latency_frames() == 3);
  const Signal output = render(graph, input,
                               {.events = {at(500, ids[1], BRANCH_PARAM_POWER, 0.0f),
                                           at(2000, ids[1], BRANCH_PARAM_MIX, 1.0f),
                                           at(3000, ids[1], BRANCH_PARAM_POWER, 1.0f)}});
  const Signal main = shift(render({a}, input, format()), 3);
  const Signal wet = render({module(-2.0f, 0.2f, 3)}, input, format());
  CHECK(difference(output, crossfade(main, wet, 0.5f), 0, 500) <= tolerance);
  CHECK(!identical(output, main, 500, 980));
  CHECK(identical(output, main, 500 + 479, 3000));
  CHECK(!identical(output, wet, 3000, 3479));
  CHECK(identical(output, wet, 3000 + 479));
}

// Settled extremes copy samples: -0.0 from the main path survives, which 1 * m + 0 * w would not.
void test_extremes_are_copies() {
  current_test = "extremes_are_copies";
  Signal input = zeros(1, 1200);
  for (float &sample : input[0]) {
    sample = -0.0f;
  }
  Graph graph;
  const auto ids = build(graph, {module(1.0f, -0.0f), branch(-1.0f, -0.0f, 0.0f)});
  graph.prepare(format(1));
  const Signal output = render(graph, input, {.events = {at(100, ids[1], BRANCH_PARAM_MIX, 1.0f)}});
  bool copies = true;
  for (size_t i = 0; i < output[0].size(); ++i) {
    const bool settled = i < 100 || i >= 100 + 479;
    if (settled) {
      copies = copies && std::signbit(output[0][i]) == (i < 100);
    }
  }
  CHECK(copies);
}

std::vector<uint8_t> state_bytes(const std::vector<float> &values) {
  std::vector<uint8_t> bytes(values.size() * sizeof(float));
  std::memcpy(bytes.data(), values.data(), bytes.size());
  return bytes;
}

void test_canonical_branch_mix() {
  current_test = "canonical_branch_mix";
  struct MixCase {
    float value;
    float stepped;
  };
  constexpr MixCase cases[] = {{0.125f, 0.13f}, {0.375f, 0.38f}, {0.625f, 0.63f},
                               {0.875f, 0.88f}, {0.003f, 0.0f},  {0.997f, 1.0f}};
  const Signal input = zeros(1, 640);
  auto expect_level = [&](const Signal &output, float expected, size_t from, size_t to) {
    const Signal reference(1, std::vector<float>(input[0].size(), expected));
    CHECK(identical(output, reference, from, to));
  };
  for (bool stepped : {true, false}) {
    auto expected = [&](MixCase mix) { return stepped ? mix.stepped : mix.value; };
    Spec wet = branch(0.0f, 1.0f, 0.5f);
    wet.path = stepped ? &paths->stepped : &paths->branch;
    Graph graph;
    const NodeId node = build(graph, {wet})[0];
    // Restore a non-default mix before prepare. With dry = 0 and wet = 1 the output is the gain.
    graph.load_module_state(node, {1, state_bytes({1, 0, 1, 0, 0, 0, cases[1].value, 0})});
    graph.prepare(format(1));
    float previous = expected(cases[1]);
    CHECK(graph.parameter_value(node, BRANCH_PARAM_MIX) == previous);
    expect_level(render(graph, input), previous, 0, input[0].size());

    constexpr size_t offset = 31;
    constexpr size_t ramp = 480;
    for (const MixCase mix : cases) {
      const float target = expected(mix);
      const Signal output =
          render(graph, input, {.events = {at(offset, node, BRANCH_PARAM_MIX, mix.value)}});
      expect_level(output, previous, 0, offset);
      CHECK(std::fabs(output[0][offset] - (previous + (target - previous) / ramp)) < 1e-7f);
      CHECK(output[0][offset + ramp - 2] != target);
      expect_level(output, target, offset + ramp - 1, input[0].size());
      CHECK(graph.parameter_value(node, BRANCH_PARAM_MIX) == target);
      previous = target;
    }

    for (const MixCase mix : cases) {
      graph.set_parameter_value(node, BRANCH_PARAM_MIX, mix.value);
      CHECK(graph.parameter_value(node, BRANCH_PARAM_MIX) == expected(mix));
      expect_level(render(graph, input), expected(mix), 0, input[0].size());
    }
  }
}

void test_start_snap_reset() {
  current_test = "start_snap_reset";
  const Signal input = noise(2, 256, 5);
  const Spec a = module(1.5f, 0.1f);
  Graph graph;
  const auto ids = build(graph, {a, branch(-2.0f, 0.2f, 0.5f, 4)});
  graph.load_module_state(ids[1], {1, state_bytes({0, -2, 0.2f, 4, 0, 0, 1, 0})});
  graph.prepare(format());
  const Signal main = shift(render({a}, input, format()), 4);
  const Signal wet = render({module(-2.0f, 0.2f, 4)}, input, format());
  CHECK(identical(render(graph, input), main));

  graph.set_parameter_value(ids[1], BRANCH_PARAM_POWER, 1.0f);
  Graph reference;
  build(reference, {module(-2.0f, 0.2f, 4)});
  reference.prepare(format());
  render(reference, input);
  CHECK(identical(render(graph, input), render(reference, input)));

  // reset() during a ramp snaps the gains to their targets and clears the alignment delay.
  const Signal after =
      render(graph, input, {.events = {at(0, ids[1], BRANCH_PARAM_MIX, 0.0f)}, .resets = {64}});
  CHECK(!identical(after, main, 0, 64));
  Signal fresh = render({a}, input, format());
  bool cleared = true;
  for (size_t c = 0; c < 2; ++c) {
    for (size_t i = 64; i < 68; ++i) {
      cleared = cleared && after[c][i] == 0.0f;
    }
    for (size_t i = 68; i < 256; ++i) {
      cleared = cleared && after[c][i] == fresh[c][i - 4];
    }
  }
  CHECK(cleared);
  CHECK(graph.parameter_value(ids[1], BRANCH_PARAM_MIX) == 0.0f);

  // A discontinuity flag acts as reset() before the block.
  const std::vector<Spec> specs = {module(1.5f, 0.1f, 2), branch(-2.0f, 0.2f, 0.5f, 7)};
  Graph flagged;
  Graph reset;
  const auto flagged_ids = build(flagged, specs);
  build(reset, specs);
  flagged.prepare(format());
  reset.prepare(format());
  const std::vector<Timed> events = {at(30, flagged_ids[1], BRANCH_PARAM_MIX, 1.0f)};
  const Signal long_input = noise(2, 640, 6);
  const Signal a_out = render(flagged, long_input, {.events = events, .discontinuities = {128}});
  const Signal b_out = render(reset, long_input, {.events = events, .resets = {128}});
  CHECK(identical(a_out, b_out));
}

void test_stages() {
  current_test = "stages";
  auto shared = [](Graph &graph, const std::vector<int> &branches) {
    graph.set_sharing(true);
    std::vector<NodeId> ids;
    for (const auto *path : {&paths->shared_a, &paths->shared_b, &paths->shared_c}) {
      ids.push_back(graph.add_module(*path));
    }
    for (int index : branches) {
      graph.set_branch(ids[static_cast<size_t>(index)], Branch{3});
    }
    graph.prepare(format());
    return ids;
  };
  using Groups = std::vector<std::vector<NodeId>>;
  {
    Graph graph;
    const auto ids = shared(graph, {});
    CHECK(graph.processing_groups() == (Groups{{ids[0], ids[1], ids[2]}}));
  }
  {
    Graph graph;
    const auto ids = shared(graph, {1});
    CHECK(graph.processing_groups() == (Groups{{ids[0]}, {ids[1]}, {ids[2]}}));
    CHECK(graph.latency_frames() == 14);
    const Signal input = noise(2, 512, 7);
    CHECK(identical(render(graph, input), input));
  }
  {
    Graph graph;
    const auto ids = shared(graph, {0});
    CHECK(graph.processing_groups() == (Groups{{ids[0]}, {ids[1], ids[2]}}));
  }
  {
    Graph graph;
    const auto ids = shared(graph, {2});
    CHECK(graph.processing_groups() == (Groups{{ids[0], ids[1]}, {ids[2]}}));
  }
}

SharedFixtureStats &shared_stats(const std::filesystem::path &path) {
  void *handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  auto entry =
      handle ? reinterpret_cast<SharedFixtureStats *(*)()>(dlsym(handle, "shared_fixture_stats"))
             : nullptr;
  if (entry == nullptr) {
    throw std::runtime_error("cannot read shared fixture statistics of " + path.string());
  }
  return *entry();
}

// A shared-capable branch next to compatible shared nodes is its own engine, prepared with the
// instance cache under its own signature, and keeps or retires its pooled instance like any engine.
void test_shared_branch_engine() {
  current_test = "shared_branch_engine";
  SharedFixtureStats &a = shared_stats(paths->shared_a);
  SharedFixtureStats &b = shared_stats(paths->shared_b);
  SharedFixtureStats &c = shared_stats(paths->shared_c);
  SharedFixtureStats &retain = shared_stats(paths->shared_retain);
  for (SharedFixtureStats *s : {&a, &b, &c, &retain}) {
    *s = SharedFixtureStats{};
  }
  constexpr uint32_t gain = 2;
  constexpr uint32_t offset = 3;
  auto cache = std::make_shared<vespera::InstanceCache>(4);
  Graph graph;
  graph.set_sharing(true);
  graph.set_instance_cache(cache);
  const NodeId na = graph.add_module(paths->shared_a);
  const NodeId nb = graph.add_module(paths->shared_b);
  const NodeId nc = graph.add_module(paths->shared_c);
  graph.set_parameter_value(nb, gain, 2.0f);
  graph.set_parameter_value(nb, offset, 0.5f);
  graph.set_branch(nb, Branch{offset});
  graph.prepare(format());
  CHECK(graph.processing_groups() == (std::vector<std::vector<NodeId>>{{na}, {nb}, {nc}}));
  CHECK(a.engine_prepares == 1 && b.engine_prepares == 1 && c.engine_prepares == 1);
  CHECK(a.prepares == 0 && b.prepares == 0 && c.prepares == 0);
  CHECK(std::string(a.last_signature) == "test.shared.a");
  CHECK(std::string(b.last_signature) == "test.shared.b");
  CHECK(std::string(c.last_signature) == "test.shared.c");
  CHECK(b.last_cache != nullptr && b.pool_created == 1);
  CHECK(graph.latency_frames() == 14);

  // The branch engine's own events arrive at their offsets: gain 2 -> 3 at frame 40.
  const Signal input = noise(2, 128, 12);
  const Signal output = render(graph, input, {.events = {at(40, nb, gain, 3.0f)}});
  float worst = 0.0f;
  for (size_t ch = 0; ch < 2; ++ch) {
    for (size_t i = 0; i < 128; ++i) {
      const float wet = (i < 40 ? 2.0f : 3.0f) * input[ch][i] + 0.5f;
      worst = std::max(worst, std::fabs(output[ch][i] - (0.5f * input[ch][i] + 0.5f * wet)));
    }
  }
  CHECK(worst <= 1e-6f);

  graph.release();
  CHECK(a.live == 0 && b.live == 0 && c.live == 0);
  CHECK(cache->idle() == 3 && b.disposed == 0);
  graph.prepare(format());
  CHECK(b.engine_prepares == 2 && b.reused == 1 && b.reused_foreign == 0 && b.pool_created == 1);
  CHECK(a.reused == 1 && c.reused == 1 && cache->idle() == 0);
  graph.release();

  // Without the route the same nodes share one engine.
  graph.set_branch(nb, std::nullopt);
  graph.prepare(format());
  CHECK(graph.processing_groups() == (std::vector<std::vector<NodeId>>{{na, nb, nc}}));
  CHECK(std::string(a.last_signature) == "test.shared.a,test.shared.b,test.shared.c");
  graph.release();

  // A branch whose engine retains audio is retired on release, not returned to the cache.
  auto fresh = std::make_shared<vespera::InstanceCache>(4);
  Graph retiring;
  retiring.set_sharing(true);
  retiring.set_instance_cache(fresh);
  const NodeId first = retiring.add_module(paths->shared_a);
  const NodeId kept = retiring.add_module(paths->shared_retain);
  retiring.set_branch(kept, Branch{offset});
  retiring.prepare(format());
  CHECK(retiring.processing_groups() == (std::vector<std::vector<NodeId>>{{first}, {kept}}));
  CHECK((retain.last_flags & VS_SHARED_RETAINS_AUDIO) != 0 && retain.pool_created == 1);
  retiring.release();
  CHECK(retain.disposed == 1 && retain.live == 0 && fresh->idle() == 1);
}

// A MIDI-input branch receives notes at their offsets and the panic flag; the host mix applies to
// its output alone, and the serial node without MIDI input never sees either.
void test_midi_branch() {
  current_test = "midi_branch";
  Spec synth = branch(0.0f, 0.0f, 0.5f);
  synth.path = &paths->midi;
  Graph graph;
  const auto ids = build(graph, {module(0.0f, 1.0f), synth});
  graph.prepare(format(1));
  CHECK(graph.processing_groups().size() == 2);
  const NodeId s = ids[1];
  const Signal output =
      render(graph, zeros(1, 1400),
             {.events = {midi_at(100, s, 0x90, 60, 127), midi_at(300, s, 0x80, 60, 0),
                         at(500, s, BRANCH_PARAM_MIX, 1.0f), midi_at(1100, s, 0x91, 64, 127)},
              .panics = {1152}});
  auto all = [&](size_t from, size_t to, float value) {
    bool equal = true;
    for (size_t i = from; i < to; ++i) {
      equal = equal && output[0][i] == value;
    }
    return equal;
  };
  CHECK(all(0, 100, 0.5f));
  CHECK(all(100, 300, 1.0f));
  CHECK(all(300, 500, 0.5f));
  CHECK(output[0][500] < 0.5f && output[0][978] > 0.0f);
  CHECK(all(979, 1100, 0.0f));
  CHECK(all(1100, 1152, 1.0f));
  CHECK(all(1152, 1400, 0.0f));

  Event stray;
  stray.node = ids[0];
  stray.kind = Event::Kind::midi;
  stray.midi = {0, 0x90, 60, 100, 0};
  std::vector<float> left(16);
  const float *in[] = {left.data()};
  float *out[] = {left.data()};
  CHECK(graph.process(Block{in, out, 1, 16}, std::span<const Event>(&stray, 1)) ==
        VS_ERROR_ARGUMENT);
}

// Each merge keeps its own gains and delays: snapping one branch leaves another's ramp running,
// and settled merges stay bit-exact after many blocks.
void test_independent_branches() {
  current_test = "independent_branches";
  const Spec a = module(1.5f, 0.1f);
  const Spec b = module(0.5f, 0.0f);
  const std::vector<Spec> specs = {a, branch(-2.0f, 0.2f, 0.25f, 3), b, branch(1.0f, 0.05f, 0.75f)};
  const Signal input = noise(2, 3000, 13);
  const Signal s1 = render({module(-2.0f, 0.2f, 3)}, input, format());
  const Signal a_late = shift(render({a}, input, format()), 3);

  Graph graph;
  const auto ids = build(graph, specs);
  graph.prepare(format());
  const Signal output = render(graph, input,
                               {.events = {at(200, ids[3], BRANCH_PARAM_MIX, 0.0f),
                                           at(1000, ids[1], BRANCH_PARAM_POWER, 0.0f)}});
  CHECK(difference(output, render({b}, crossfade(a_late, s1, 0.25f), format()), 679, 1000) <=
        tolerance);
  CHECK(identical(output, render({b}, a_late, format()), 1479));

  Graph snapped;
  const auto snapped_ids = build(snapped, specs);
  snapped.prepare(format());
  auto slice = [&](size_t from, size_t to) {
    Signal part = zeros(2, to - from);
    for (size_t c = 0; c < 2; ++c) {
      std::copy(input[c].begin() + static_cast<std::ptrdiff_t>(from),
                input[c].begin() + static_cast<std::ptrdiff_t>(to), part[c].begin());
    }
    return part;
  };
  render(snapped, slice(0, 64), {.events = {at(0, snapped_ids[1], BRANCH_PARAM_MIX, 1.0f)}});
  snapped.set_parameter_value(snapped_ids[3], BRANCH_PARAM_MIX, 0.0f);
  const Signal rest = render(snapped, slice(64, 3000));
  const Signal only_s1 = slice_after(render({b}, s1, format()), 64);
  CHECK(!identical(rest, only_s1, 0, 479 - 64));
  CHECK(identical(rest, only_s1, 479 - 64));
}

void test_in_place() {
  current_test = "in_place";
  for (uint32_t channels : {1u, 2u}) {
    const Signal input = noise(channels, 1500, 8);
    const std::vector<Spec> specs = {module(2.0f, 0.1f, 2), branch(3.0f, 0.05f, 0.3f, 5),
                                     module(0.5f, -0.3f)};
    const Signal separate = render(specs, input, format(channels));
    const Signal in_place = render(specs, input, format(channels), {.in_place = true});
    CHECK(identical(separate, in_place));
    const std::vector<Spec> only = {branch(3.0f, 0.05f, 0.3f, 5)};
    CHECK(identical(render(only, input, format(channels)),
                    render(only, input, format(channels), {.in_place = true})));
  }
}

void test_no_bypass_capability() {
  current_test = "no_bypass_capability";
  const Signal input = noise(2, 1000, 9);
  Spec s = branch(3.0f, 0.05f, 1.0f);
  s.path = &paths->no_bypass;
  Graph graph;
  const auto ids = build(graph, {module(2.0f, 0.1f), s});
  graph.prepare(format());
  const Signal output =
      render(graph, input, {.events = {at(10, ids[1], BRANCH_PARAM_POWER, 0.0f)}});
  CHECK(identical(output, render({module(3.0f, 0.05f)}, input, format())));
}

void test_set_branch_rules() {
  current_test = "set_branch_rules";
  Graph graph;
  const NodeId a = graph.add_module(paths->branch);
  const NodeId b = graph.add_module(paths->branch);
  const NodeId plain = graph.add_module(paths->no_values);
  graph.set_parameter_value(b, BRANCH_PARAM_GAIN, 3.0f);
  graph.set_parameter_value(b, BRANCH_PARAM_MIX, 1.0f);
  graph.set_branch(b, Branch{BRANCH_PARAM_MIX});
  CHECK(throws<std::invalid_argument>([&] { graph.set_branch(999, Branch{BRANCH_PARAM_MIX}); }));
  CHECK(throws<std::invalid_argument>([&] { graph.set_branch(b, Branch{77}); }, "no mix"));
  CHECK(throws<std::invalid_argument>([&] { graph.set_branch(b, Branch{BRANCH_PARAM_WIDE}); },
                                      "0 to 1"));
  CHECK(throws<std::invalid_argument>([&] { graph.set_branch(b, Branch{BRANCH_PARAM_GAIN}); },
                                      "0 to 1"));
  CHECK(throws<std::invalid_argument>([&] { graph.set_branch(b, Branch{BRANCH_PARAM_POWER}); },
                                      "bypass"));
  CHECK(throws<std::invalid_argument>(
      [&] { graph.set_branch(b, Branch{BRANCH_PARAM_MIX, static_cast<MixLaw>(7)}); }, "law"));
  CHECK(throws<std::runtime_error>([&] { graph.set_branch(plain, Branch{BRANCH_PARAM_MIX}); },
                                   "parameter value extension"));
  CHECK(graph.branch_nodes() == std::vector<NodeId>{b});
  graph.remove_module(plain);

  // The route moves with the node: [b, a] mixes before a.
  graph.move_module(b, 0);
  CHECK(graph.branch_nodes() == std::vector<NodeId>{b});
  graph.prepare(format());
  CHECK(throws<std::logic_error>([&] { graph.set_branch(a, Branch{BRANCH_PARAM_MIX}); }));
  CHECK(throws<std::logic_error>([&] { graph.set_branch(b, std::nullopt); }));
  const Signal input = noise(2, 300, 10);
  CHECK(identical(render(graph, input), render({module(3.0f, 0.0f)}, input, format())));
  CHECK(graph.processing_groups() == (std::vector<std::vector<NodeId>>{{b}, {a}}));

  // Presets carry no route.
  vespera::ModuleCatalog catalog;
  catalog.add(paths->branch);
  const auto restored = Graph::restore_preset(graph.capture_preset(), catalog);
  CHECK(restored->node_ids() == (std::vector<NodeId>{b, a}));
  CHECK(restored->branch_nodes().empty());

  graph.release();
  graph.remove_module(b);
  CHECK(graph.branch_nodes().empty());
}

void test_failure() {
  current_test = "failure";
  Graph graph;
  const auto ids = build(graph, {module(1.0f, 0.0f), branch(1.0f, 0.0f, 0.5f, 3)});
  graph.prepare(format());
  std::vector<float> left(64, 0.25f), right(64, 0.25f);
  const float *in[] = {left.data(), right.data()};
  std::vector<float> out_left(64, 9.0f), out_right(64, 9.0f);
  float *out[] = {out_left.data(), out_right.data()};
  Block block{in, out, 2, 64};
  Event fault;
  fault.node = ids[1];
  fault.parameter = {BRANCH_PARAM_FAULT, 10, 1.0f};
  CHECK(graph.process(block, std::span<const Event>(&fault, 1)) == VS_ERROR_PROCESS);
  CHECK(graph.state() == GraphState::failed);
  CHECK(std::all_of(out_left.begin(), out_left.end(), [](float v) { return v == 0.0f; }));
  CHECK(graph.process(block) == VS_ERROR_STATE);
  graph.release();
  CHECK(graph.branch_nodes() == std::vector<NodeId>{ids[1]});
  graph.set_parameter_value(ids[1], BRANCH_PARAM_FAULT, 0.0f);
  graph.prepare(format());
  CHECK(graph.process(block) == VS_OK);
}

void test_allocation_free() {
  current_test = "allocation_free";
  Graph graph;
  Spec stepped = branch(-2.0f, 0.2f, 0.5f, 9);
  stepped.path = &paths->stepped;
  const auto ids =
      build(graph, {module(1.5f, 0.1f, 2), stepped, module(0.5f, 0.0f), branch(1.0f, 0.0f, 0.25f)});
  graph.prepare(format(2, 48000.0, 128));
  const Signal input = noise(2, 128, 11);
  std::vector<float> left(128), right(128);
  const float *in[] = {input[0].data(), input[1].data()};
  float *out[] = {left.data(), right.data()};
  std::vector<Event> events;
  for (uint32_t i = 0; i < Graph::max_events_per_block; ++i) {
    Event event;
    event.node = ids[i % 4];
    const uint32_t parameter = i % 3 == 0   ? BRANCH_PARAM_MIX
                               : i % 3 == 1 ? BRANCH_PARAM_POWER
                                            : BRANCH_PARAM_GAIN;
    event.parameter = {parameter, i / 4, static_cast<float>((i * 7) % 9) / 8.0f};
    events.push_back(event);
  }
  Event fault;
  fault.node = ids[3];
  fault.parameter = {BRANCH_PARAM_FAULT, 3, 1.0f};

  allocation_count = 0;
  counting_allocations = true;
  vs_result combined = VS_OK;
  for (uint32_t i = 0; i < 200; ++i) {
    const bool with_events = i % 2 == 1;
    Block block{in,       out,
                2,        with_events ? 128u : 1 + (i * 37) % 128,
                i * 128u, i % 17 == 0 ? uint32_t(VS_PROCESS_DISCONTINUITY) : 0u};
    const std::span<const Event> span =
        with_events ? std::span<const Event>(events) : std::span<const Event>();
    combined = static_cast<vs_result>(combined | graph.process(block, span));
  }
  Block block{in, out, 2, 128};
  graph.reset();
  const vs_result faulted = graph.process(block, std::span<const Event>(&fault, 1));
  counting_allocations = false;
  CHECK(allocation_count == 0);
  CHECK(combined == VS_OK);
  CHECK(faulted == VS_ERROR_PROCESS);
}

}

int main(int argc, char **argv) {
  if (argc != 10) {
    std::fprintf(stderr,
                 "usage: %s <branch> <branch-no-values> <branch-no-bypass> <shared-a> "
                 "<shared-b> <shared-c> <shared-retain> <branch-midi> <branch-stepped>\n",
                 argv[0]);
    return 2;
  }
  const Paths resolved{argv[1], argv[2], argv[3], argv[4], argv[5],
                       argv[6], argv[7], argv[8], argv[9]};
  paths = &resolved;
  for (auto test :
       {test_serial_unchanged, test_crossfade_points, test_merge_positions, test_alignment,
        test_oversized_delay, test_ramp, test_bypass_and_extremes, test_extremes_are_copies,
        test_start_snap_reset, test_stages, test_in_place, test_no_bypass_capability,
        test_set_branch_rules, test_failure, test_allocation_free, test_shared_branch_engine,
        test_midi_branch, test_independent_branches, test_canonical_branch_mix}) {
    try {
      test();
    } catch (const std::exception &error) {
      std::fprintf(stderr, "[%s] unexpected exception: %s\n", current_test, error.what());
      ++failures;
    }
  }
  if (failures != 0) {
    std::fprintf(stderr, "%d graph branch check(s) failed\n", failures);
    return 1;
  }
  std::puts("graph branch tests passed");
  return 0;
}
