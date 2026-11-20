#include <vespera/runtime.hpp>

#include "fixtures/probe_control.h"

#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iterator>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
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

// ABI version 1 parameter and event arrays have no element size, so their layout must not change.
static_assert(sizeof(vs_parameter_event) == 12);
static_assert(offsetof(vs_parameter_event, parameter_id) == 0 &&
              offsetof(vs_parameter_event, frame_offset) == 4 &&
              offsetof(vs_parameter_event, value) == 8);
static_assert(offsetof(vs_parameter_descriptor, id) == 0 &&
              offsetof(vs_parameter_descriptor, name) == sizeof(void *) &&
              offsetof(vs_parameter_descriptor, unit) == 2 * sizeof(void *) &&
              offsetof(vs_parameter_descriptor, minimum) == 3 * sizeof(void *) &&
              offsetof(vs_parameter_descriptor, maximum) == 3 * sizeof(void *) + 4 &&
              offsetof(vs_parameter_descriptor, default_value) == 3 * sizeof(void *) + 8);
static_assert(sizeof(vs_parameter_descriptor) ==
              (3 * sizeof(void *) + 12 + alignof(void *) - 1) / alignof(void *) * alignof(void *));

namespace fs = std::filesystem;
using vespera::Block;
using vespera::Event;
using vespera::Format;
using vespera::Graph;
using vespera::GraphState;
using vespera::NodeId;
using vespera::NodeMetadata;

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

template <typename Exception> bool throws(const std::function<void()> &action) {
  try {
    action();
  } catch (const Exception &) {
    return true;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "[%s] unexpected exception type: %s\n", current_test, error.what());
    return false;
  }
  return false;
}

struct Paths {
  fs::path gain;
  fs::path probe;
  fs::path no_entry;
  fs::path temp;
};

probe_control *probe = nullptr;

void reset_probe() {
  CHECK(probe->live_instances == 0);
  std::memset(probe, 0, sizeof(*probe));
  probe->fail_prepare_instance = -1;
  probe->fault_instance = -1;
}

bool near(float a, float b) { return std::fabs(a - b) <= 1e-6f; }

struct Buffers {
  Buffers(uint32_t channels, uint32_t frames)
      : input(channels, std::vector<float>(frames)), output(channels, std::vector<float>(frames)) {
    for (uint32_t c = 0; c < channels; ++c) {
      in_ptrs.push_back(input[c].data());
      out_ptrs.push_back(output[c].data());
    }
  }

  Block block(uint32_t frames, uint64_t position = 0, uint32_t flags = 0) const {
    return Block{in_ptrs.data(), out_ptrs.data(), static_cast<uint32_t>(in_ptrs.size()),
                 frames,         position,        flags};
  }

  void fill_input(float value) {
    for (auto &channel : input) {
      std::fill(channel.begin(), channel.end(), value);
    }
  }

  void fill_output(float value) {
    for (auto &channel : output) {
      std::fill(channel.begin(), channel.end(), value);
    }
  }

  bool output_is(float value, uint32_t frames) const {
    for (const auto &channel : output) {
      for (uint32_t i = 0; i < frames; ++i) {
        if (channel[i] != value) {
          return false;
        }
      }
    }
    return true;
  }

  std::vector<std::vector<float>> input;
  std::vector<std::vector<float>> output;
  std::vector<const float *> in_ptrs;
  std::vector<float *> out_ptrs;
};

Event event(NodeId node, uint32_t parameter, uint32_t offset, float value) {
  return Event{node, vs_parameter_event{parameter, offset, value}};
}

Format stereo(uint32_t max_frames = 64) { return Format{48000.0, max_frames, 2}; }

void test_gain_processing(const Paths &paths) {
  current_test = "gain_processing";
  Graph graph;
  graph.add_module(paths.gain);
  CHECK(graph.node_count() == 1);
  CHECK(graph.state() == GraphState::editing);
  graph.prepare(stereo());
  CHECK(graph.state() == GraphState::prepared);
  CHECK(graph.latency_frames() == 0);

  Buffers io(2, 64);
  io.fill_input(1.0f);
  std::fill(io.input[1].begin(), io.input[1].end(), -0.5f);
  const Event change[] = {event(0, 1, 4, 2.0f)};
  CHECK(graph.process(io.block(16), change) == VS_OK);
  for (uint32_t i = 0; i < 16; ++i) {
    const float gain = i < 4 ? 1.0f : 2.0f;
    CHECK(near(io.output[0][i], gain));
    CHECK(near(io.output[1][i], -0.5f * gain));
  }

  CHECK(graph.process(io.block(8)) == VS_OK);
  CHECK(near(io.output[0][0], 2.0f) && near(io.output[0][7], 2.0f));

  const Event at_max[] = {event(0, 1, 0, 4.0f)};
  CHECK(graph.process(io.block(8), at_max) == VS_OK);
  CHECK(near(io.output[0][0], 4.0f));

  const Event above_max[] = {event(0, 1, 0, 4.5f)};
  io.fill_output(9.0f);
  CHECK(graph.process(io.block(8), above_max) == VS_ERROR_ARGUMENT);
  CHECK(io.output_is(0.0f, 8));
  CHECK(graph.state() == GraphState::prepared);

  const Event unknown[] = {event(0, 2, 0, 1.0f)};
  CHECK(graph.process(io.block(8), unknown) == VS_ERROR_ARGUMENT);

  const Event mute[] = {event(0, 1, 0, 0.0f)};
  CHECK(graph.process(io.block(8), mute) == VS_OK);
  CHECK(io.output_is(0.0f, 8));
}

void test_mono_gain(const Paths &paths) {
  current_test = "mono_gain";
  Graph graph;
  graph.add_module(paths.gain);
  graph.prepare(Format{44100.0, 32, 1});
  Buffers io(1, 32);
  io.fill_input(0.25f);
  const Event change[] = {event(0, 1, 31, 4.0f)};
  CHECK(graph.process(io.block(32), change) == VS_OK);
  CHECK(near(io.output[0][30], 0.25f));
  CHECK(near(io.output[0][31], 1.0f));

  Buffers wrong(2, 32);
  CHECK(graph.process(wrong.block(32)) == VS_ERROR_ARGUMENT);
}

void test_empty_graph() {
  current_test = "empty_graph";
  Graph graph;
  Buffers io(2, 16);
  io.fill_input(0.5f);
  io.fill_output(3.0f);
  CHECK(graph.process(io.block(16)) == VS_ERROR_STATE);
  CHECK(io.output_is(0.0f, 16));

  graph.prepare(stereo(16));
  CHECK(graph.node_count() == 0);
  CHECK(graph.latency_frames() == 0 && graph.tail_frames() == 0);
  for (uint32_t i = 0; i < 16; ++i) {
    io.input[0][i] = static_cast<float>(i);
    io.input[1][i] = -static_cast<float>(i);
  }
  CHECK(graph.process(io.block(16)) == VS_OK);
  CHECK(io.output[0] == io.input[0] && io.output[1] == io.input[1]);

  std::vector<float> left{1, 2, 3, 4}, right{5, 6, 7, 8};
  float *channels[] = {left.data(), right.data()};
  const Block in_place{channels, channels, 2, 4, 0, 0};
  CHECK(graph.process(in_place) == VS_OK);
  CHECK((left == std::vector<float>{1, 2, 3, 4}) && (right == std::vector<float>{5, 6, 7, 8}));

  float *swapped_out[] = {right.data(), left.data()};
  const Block swapped{channels, swapped_out, 2, 4, 0, 0};
  CHECK(graph.process(swapped) == VS_OK);
  CHECK((left == std::vector<float>{5, 6, 7, 8}) && (right == std::vector<float>{1, 2, 3, 4}));

  const Event targeted[] = {event(0, 1, 0, 1.0f)};
  CHECK(graph.process(io.block(16), targeted) == VS_ERROR_ARGUMENT);
}

void test_in_place_single_node(const Paths &paths) {
  current_test = "in_place_single_node";
  reset_probe();
  {
    Graph graph;
    graph.add_module(paths.probe);
    graph.prepare(stereo(8));
    std::vector<float> left(8, 1.0f), right(8, 2.0f);
    float *channels[] = {left.data(), right.data()};
    const Event level[] = {event(0, PROBE_PARAM_LEVEL, 0, 0.5f)};
    CHECK(graph.process(Block{channels, channels, 2, 8, 0, 0}, level) == VS_OK);
    CHECK(probe->blocks[0].aliased == 0);
    CHECK(near(left[7], 0.5f) && near(right[0], 1.0f));
  }
  {
    Graph graph;
    graph.add_module(paths.gain);
    graph.prepare(stereo(8));
    std::vector<float> left(8, 1.0f), right(8, 2.0f);
    float *channels[] = {left.data(), right.data()};
    const Event gain[] = {event(0, 1, 0, 3.0f)};
    CHECK(graph.process(Block{channels, channels, 2, 8, 0, 0}, gain) == VS_OK);
    CHECK(near(left[0], 3.0f) && near(right[7], 6.0f));
  }
}

void test_multiple_instances(const Paths &paths) {
  current_test = "multiple_instances";
  const fs::path link = paths.temp / "gain-link.module";
  fs::create_symlink(paths.gain, link);
  {
    Graph graph;
    graph.add_module(paths.gain);
    graph.add_module(link);
    graph.add_module(paths.gain);
    CHECK(graph.node_count() == 3);
    graph.prepare(stereo(16));
    Buffers io(2, 16);
    io.fill_input(1.0f);
    const Event gains[] = {event(1, 1, 0, 2.0f), event(2, 1, 0, 3.0f)};
    CHECK(graph.process(io.block(16), gains) == VS_OK);
    CHECK(near(io.output[0][0], 6.0f) && near(io.output[1][15], 6.0f));
    const Event first[] = {event(0, 1, 0, 0.5f)};
    CHECK(graph.process(io.block(16), first) == VS_OK);
    CHECK(near(io.output[0][3], 3.0f));
  }

  reset_probe();
  {
    Graph graph;
    for (int i = 0; i < 4; ++i) {
      graph.add_module(paths.probe);
    }
    graph.prepare(stereo(16));
    Buffers io(2, 16);
    io.fill_input(1.0f);
    const Event offsets[] = {event(0, PROBE_PARAM_OFFSET, 0, 0.5f),
                             event(3, PROBE_PARAM_LEVEL, 0, 2.0f)};
    CHECK(graph.process(io.block(16), offsets) == VS_OK);
    CHECK(near(io.output[0][0], 3.0f));
    for (int i = 0; i < 4; ++i) {
      CHECK(probe->blocks[i].calls == 1);
      CHECK(probe->blocks[i].aliased == 0);
    }
  }
  CHECK(probe->live_instances == 0);
}

void test_duplicate_identity(const Paths &paths) {
  current_test = "duplicate_identity";
  const fs::path copy = paths.temp / "gain-copy.module";
  fs::copy_file(paths.gain, copy);
  {
    Graph graph;
    graph.add_module(paths.gain);
    CHECK(throws<std::runtime_error>([&] { graph.add_module(copy); }));
    CHECK(graph.node_count() == 1);
    graph.add_module(paths.gain);
    CHECK(graph.node_count() == 2);
  }
  {
    Graph graph;
    graph.add_module(copy);
    CHECK(graph.node_count() == 1);
  }

  reset_probe();
  probe->descriptor = PROBE_DESCRIPTOR_GAIN_IMPOSTOR;
  {
    Graph graph;
    graph.add_module(paths.gain);
    CHECK(throws<std::runtime_error>([&] { graph.add_module(paths.probe); }));
    CHECK(graph.node_count() == 1);
  }
  {
    Graph graph;
    graph.add_module(paths.probe);
    CHECK(throws<std::runtime_error>([&] { graph.add_module(paths.gain); }));
    CHECK(graph.node_count() == 1);
  }
  CHECK(probe->live_instances == 0);
}

void test_load_failures(const Paths &paths) {
  current_test = "load_failures";
  const fs::path garbage = paths.temp / "garbage.module";
  {
    FILE *file = std::fopen(garbage.c_str(), "wb");
    std::fputs("not a module", file);
    std::fclose(file);
  }
  for (const fs::path &path :
       {paths.temp / "missing.module", paths.temp, garbage, paths.no_entry}) {
    Graph graph;
    CHECK(throws<std::runtime_error>([&] { graph.add_module(path); }));
    CHECK(graph.node_count() == 0);
  }

  for (int variant = PROBE_DESCRIPTOR_NULL; variant <= PROBE_DESCRIPTOR_TOO_MANY_PARAMETERS;
       ++variant) {
    reset_probe();
    probe->descriptor = variant;
    Graph graph;
    if (!throws<std::runtime_error>([&] { graph.add_module(paths.probe); })) {
      std::fprintf(stderr, "[%s] descriptor variant %d was accepted\n", current_test, variant);
      ++failures;
    }
    CHECK(graph.node_count() == 0);
    CHECK(probe->next_instance == 0);
  }

  for (int variant :
       {PROBE_DESCRIPTOR_OVERLONG_NAME_UTF8, PROBE_DESCRIPTOR_SURROGATE_VERSION_UTF8,
        PROBE_DESCRIPTOR_TRUNCATED_PARAMETER_UTF8, PROBE_DESCRIPTOR_OUT_OF_RANGE_UNIT_UTF8}) {
    reset_probe();
    probe->descriptor = variant;
    Graph graph;
    if (!throws<std::runtime_error>([&] { graph.add_module(paths.probe); })) {
      std::fprintf(stderr, "[%s] invalid UTF-8 variant %d was accepted\n", current_test, variant);
      ++failures;
    }
    CHECK(graph.node_count() == 0);
    CHECK(probe->next_instance == 0);
  }

  reset_probe();
  probe->fail_create = 1;
  {
    Graph graph;
    CHECK(throws<std::runtime_error>([&] { graph.add_module(paths.probe); }));
    CHECK(graph.node_count() == 0);
    probe->fail_create = 0;
    graph.add_module(paths.probe);
    CHECK(graph.node_count() == 1);
  }
  CHECK(probe->live_instances == 0);
}

void test_unload_after_destroy(const Paths &paths) {
  current_test = "unload_after_destroy";
  const std::string path = fs::canonical(paths.gain).string();
  if (void *already = dlopen(path.c_str(), RTLD_NOW | RTLD_NOLOAD)) {
    dlclose(already);
    std::fprintf(stderr, "[%s] skipped: gain module is resident before the test\n", current_test);
    return;
  }
  {
    Graph graph;
    graph.add_module(paths.gain);
    graph.add_module(paths.gain);
    graph.prepare(stereo());
    void *resident = dlopen(path.c_str(), RTLD_NOW | RTLD_NOLOAD);
    CHECK(resident != nullptr);
    if (resident != nullptr) {
      dlclose(resident);
    }
  }
  CHECK(dlopen(path.c_str(), RTLD_NOW | RTLD_NOLOAD) == nullptr);
}

bool ops_are(std::initializer_list<probe_op_record> expected) {
  if (probe->op_count != expected.size()) {
    return false;
  }
  uint32_t i = 0;
  for (const probe_op_record &op : expected) {
    if (probe->ops[i].op != op.op || probe->ops[i].instance != op.instance) {
      return false;
    }
    ++i;
  }
  return true;
}

void test_lifecycle_order(const Paths &paths) {
  current_test = "lifecycle_order";
  reset_probe();
  {
    Graph graph;
    graph.add_module(paths.probe);
    graph.add_module(paths.probe);
    graph.prepare(Format{96000.0, 32, 2});
    CHECK(probe->last_prepare[1].sample_rate == 96000.0);
    CHECK(probe->last_prepare[1].max_frames == 32);
    CHECK(probe->last_prepare[1].channels == 2);
    CHECK(probe->last_prepare[1].struct_size == sizeof(vs_prepare_info));
    graph.reset();
  }
  CHECK(ops_are({{PROBE_OP_CREATE, 0},
                 {PROBE_OP_CREATE, 1},
                 {PROBE_OP_PREPARE, 0},
                 {PROBE_OP_PREPARE, 1},
                 {PROBE_OP_RESET, 0},
                 {PROBE_OP_RESET, 1},
                 {PROBE_OP_RELEASE, 1},
                 {PROBE_OP_RELEASE, 0},
                 {PROBE_OP_DESTROY, 1},
                 {PROBE_OP_DESTROY, 0}}));
  CHECK(probe->live_instances == 0);
}

void test_partial_prepare_failure(const Paths &paths) {
  current_test = "partial_prepare_failure";
  reset_probe();
  probe->fail_prepare_instance = 1;
  {
    Graph graph;
    for (int i = 0; i < 3; ++i) {
      graph.add_module(paths.probe);
    }
    CHECK(throws<std::runtime_error>([&] { graph.prepare(stereo()); }));
    CHECK(graph.state() == GraphState::editing);
    CHECK(ops_are({{PROBE_OP_CREATE, 0},
                   {PROBE_OP_CREATE, 1},
                   {PROBE_OP_CREATE, 2},
                   {PROBE_OP_PREPARE, 0},
                   {PROBE_OP_PREPARE, 1},
                   {PROBE_OP_RELEASE, 1},
                   {PROBE_OP_RELEASE, 0}}));
    Buffers io(2, 16);
    CHECK(graph.process(io.block(16)) == VS_ERROR_STATE);

    probe->fail_prepare_instance = -1;
    graph.prepare(stereo());
    CHECK(graph.state() == GraphState::prepared);
    io.fill_input(0.5f);
    CHECK(graph.process(io.block(16)) == VS_OK);
    CHECK(near(io.output[1][15], 0.5f));
  }
  CHECK(probe->live_instances == 0);
}

void test_event_routing(const Paths &paths) {
  current_test = "event_routing";
  reset_probe();
  Graph graph;
  graph.add_module(paths.probe);
  graph.add_module(paths.probe);
  graph.prepare(stereo(16));
  Buffers io(2, 16);
  io.fill_input(1.0f);
  const Event events[] = {
      event(1, PROBE_PARAM_LEVEL, 0, 0.5f),  event(0, PROBE_PARAM_LEVEL, 2, 2.0f),
      event(0, PROBE_PARAM_LEVEL, 2, 1.5f),  event(1, PROBE_PARAM_OFFSET, 5, 0.25f),
      event(0, PROBE_PARAM_LEVEL, 15, 0.0f),
  };
  CHECK(graph.process(io.block(16), events) == VS_OK);
  CHECK(probe->event_count == 5);
  CHECK(probe->blocks[0].event_count == 3 && probe->blocks[1].event_count == 2);
  CHECK(probe->events[0].instance == 0 && probe->events[0].event.value == 2.0f);
  CHECK(probe->events[1].instance == 0 && probe->events[1].event.value == 1.5f);
  CHECK(probe->events[2].instance == 0 && probe->events[2].event.frame_offset == 15);
  CHECK(probe->events[3].instance == 1 && probe->events[3].event.value == 0.5f);
  CHECK(probe->events[4].instance == 1 &&
        probe->events[4].event.parameter_id == PROBE_PARAM_OFFSET);
  CHECK(near(io.output[0][0], 0.5f) && near(io.output[0][1], 0.5f));
  CHECK(near(io.output[0][2], 0.75f) && near(io.output[0][4], 0.75f));
  CHECK(near(io.output[0][5], 1.0f) && near(io.output[1][14], 1.0f));
  CHECK(near(io.output[0][15], 0.25f));

  CHECK(graph.process(io.block(4)) == VS_OK);
  CHECK(near(io.output[0][0], 0.25f));
}

void test_invalid_events(const Paths &paths) {
  current_test = "invalid_events";
  reset_probe();
  Graph graph;
  graph.add_module(paths.probe);
  graph.add_module(paths.probe);
  graph.prepare(stereo(16));
  Buffers io(2, 16);
  io.fill_input(1.0f);

  const std::vector<std::vector<Event>> rejected = {
      {event(0, PROBE_PARAM_LEVEL, 8, 1.0f)},
      {event(0, PROBE_PARAM_LEVEL, 5, 1.0f), event(1, PROBE_PARAM_LEVEL, 4, 1.0f)},
      {event(2, PROBE_PARAM_LEVEL, 0, 1.0f)},
      {event(0, 99, 0, 1.0f)},
      {event(0, PROBE_PARAM_LEVEL, 0, std::numeric_limits<float>::quiet_NaN())},
      {event(0, PROBE_PARAM_LEVEL, 0, std::numeric_limits<float>::infinity())},
      {event(0, PROBE_PARAM_LEVEL, 0, 2.0001f)},
      {event(0, PROBE_PARAM_OFFSET, 0, -1.5f)},
      {event(0, PROBE_PARAM_LEVEL, 0, 1.0f), event(1, PROBE_PARAM_LEVEL, 8, 1.0f)},
  };
  for (const auto &events : rejected) {
    io.fill_output(5.0f);
    CHECK(graph.process(io.block(8), events) == VS_ERROR_ARGUMENT);
    CHECK(io.output_is(0.0f, 8));
    CHECK(io.output[0][8] == 5.0f);
  }
  CHECK(probe->blocks[0].calls == 0 && probe->blocks[1].calls == 0);
  CHECK(probe->event_count == 0);
  CHECK(graph.state() == GraphState::prepared);

  const Event boundary[] = {event(0, PROBE_PARAM_LEVEL, 7, 2.0f),
                            event(1, PROBE_PARAM_OFFSET, 7, -1.0f)};
  CHECK(graph.process(io.block(8), boundary) == VS_OK);
  CHECK(near(io.output[0][6], 1.0f) && near(io.output[0][7], 1.0f));

  std::vector<Event> full;
  for (uint32_t i = 0; i < Graph::max_events_per_block; ++i) {
    full.push_back(event(i % 2, PROBE_PARAM_LEVEL, i / 32, 1.0f + static_cast<float>(i) / 512.0f));
  }
  const uint32_t before = probe->event_count;
  CHECK(graph.process(io.block(16), full) == VS_OK);
  CHECK(probe->event_count - before == Graph::max_events_per_block);
  constexpr uint32_t half = Graph::max_events_per_block / 2;
  CHECK(probe->blocks[0].event_count == half && probe->blocks[1].event_count == half);
  bool ordered = true;
  for (uint32_t i = 0; i < Graph::max_events_per_block; ++i) {
    const probe_event_record &record = probe->events[before + i];
    const uint32_t source = record.instance == 0 ? 2 * (i % half) : 2 * (i % half) + 1;
    ordered = ordered && record.instance == (i < half ? 0 : 1) &&
              record.event.value == full[source].parameter.value;
  }
  CHECK(ordered);

  full.push_back(event(0, PROBE_PARAM_LEVEL, 15, 1.0f));
  const uint32_t calls = probe->blocks[0].calls;
  CHECK(graph.process(io.block(16), full) == VS_ERROR_ARGUMENT);
  CHECK(probe->blocks[0].calls == calls);
}

void test_block_validation(const Paths &paths) {
  current_test = "block_validation";
  reset_probe();
  Graph graph;
  graph.add_module(paths.probe);
  graph.prepare(stereo(16));
  Buffers io(2, 32);
  io.fill_input(1.0f);

  const auto rejects = [&](const Block &block, bool silenced) {
    io.fill_output(4.0f);
    CHECK(graph.process(block) == VS_ERROR_ARGUMENT);
    if (silenced) {
      CHECK(io.output_is(0.0f, std::min<uint32_t>(block.frames, 16)));
    }
  };
  rejects(io.block(0), false);
  rejects(io.block(17), true);
  CHECK(io.output[0][16] == 4.0f);
  Block block = io.block(8);
  block.channels = 1;
  rejects(block, false);
  block = io.block(8);
  block.inputs = nullptr;
  rejects(block, true);
  block = io.block(8);
  block.outputs = nullptr;
  CHECK(graph.process(block) == VS_ERROR_ARGUMENT);
  block = io.block(8, 0, 8u);
  rejects(block, true);

  const float *missing_in[] = {io.input[0].data(), nullptr};
  block = io.block(8);
  block.inputs = missing_in;
  rejects(block, true);
  float *missing_out[] = {nullptr, io.output[1].data()};
  block = io.block(8);
  block.outputs = missing_out;
  io.fill_output(4.0f);
  CHECK(graph.process(block) == VS_ERROR_ARGUMENT);
  CHECK(io.output[1][0] == 0.0f && io.output[0][0] == 4.0f);

  float *shared_out[] = {io.output[0].data(), io.output[0].data() + 4};
  block = io.block(8);
  block.outputs = shared_out;
  CHECK(graph.process(block) == VS_ERROR_ARGUMENT);

  io.input[1][3] = std::numeric_limits<float>::quiet_NaN();
  rejects(io.block(8), true);
  io.input[1][3] = 1.0f;

  CHECK(probe->blocks[0].calls == 0);
  CHECK(graph.state() == GraphState::prepared);

  CHECK(graph.process(io.block(16, 12345, VS_PROCESS_DISCONTINUITY | VS_PROCESS_OFFLINE)) == VS_OK);
  CHECK(probe->blocks[0].frames == 16);
  CHECK(probe->blocks[0].frame_position == 12345);
  CHECK(probe->blocks[0].flags == (VS_PROCESS_DISCONTINUITY | VS_PROCESS_OFFLINE));
  CHECK(probe->blocks[0].channels == 2);
}

void test_block_segmentation(const Paths &paths) {
  current_test = "block_segmentation";
  constexpr uint32_t total = 200;
  struct Change {
    uint64_t at;
    uint32_t node;
    uint32_t parameter;
    float value;
  };
  const Change changes[] = {{0, 1, PROBE_PARAM_LEVEL, 0.5f},    {3, 0, PROBE_PARAM_LEVEL, 2.0f},
                            {63, 1, PROBE_PARAM_OFFSET, 0.1f},  {64, 0, PROBE_PARAM_LEVEL, 1.25f},
                            {64, 0, PROBE_PARAM_LEVEL, 0.75f},  {150, 1, PROBE_PARAM_LEVEL, 2.0f},
                            {199, 0, PROBE_PARAM_OFFSET, -0.5f}};

  std::vector<float> source(total);
  for (uint32_t i = 0; i < total; ++i) {
    source[i] = std::sin(static_cast<float>(i) * 0.1f) * 0.5f;
  }

  const auto render = [&](const std::vector<uint32_t> &sizes) {
    reset_probe();
    std::vector<float> rendered;
    Graph graph;
    graph.add_module(paths.probe);
    graph.add_module(paths.probe);
    graph.prepare(Format{48000.0, 64, 1});
    uint64_t position = 0;
    size_t next = 0;
    for (size_t s = 0; position < total; ++s) {
      const uint32_t frames =
          std::min<uint32_t>(sizes[s % sizes.size()], static_cast<uint32_t>(total - position));
      std::vector<Event> events;
      while (next < std::size(changes) && changes[next].at < position + frames) {
        events.push_back(event(changes[next].node, changes[next].parameter,
                               static_cast<uint32_t>(changes[next].at - position),
                               changes[next].value));
        ++next;
      }
      std::vector<float> out(frames);
      const float *in_ptr[] = {source.data() + position};
      float *out_ptr[] = {out.data()};
      CHECK(graph.process(Block{in_ptr, out_ptr, 1, frames, position, 0}, events) == VS_OK);
      CHECK(probe->blocks[0].frame_position == position);
      rendered.insert(rendered.end(), out.begin(), out.end());
      position += frames;
    }
    return rendered;
  };

  const std::vector<float> reference = render({64});
  CHECK(reference.size() == total);
  CHECK(render({1}) == reference);
  CHECK(render({7, 13, 64, 1, 33}) == reference);
  CHECK(render({63, 2}) == reference);
  CHECK(near(reference[2], source[2] * 0.5f));
  CHECK(near(reference[3], source[3] * 2.0f * 0.5f));
  CHECK(near(reference[64], source[64] * 0.75f * 0.5f + 0.1f));
}

void test_process_failure(const Paths &paths) {
  current_test = "process_failure";
  for (int fault : {PROBE_FAULT_NAN, PROBE_FAULT_ERROR, PROBE_FAULT_INFINITY}) {
    for (int faulty : {0, 1}) {
      reset_probe();
      Graph graph;
      graph.add_module(paths.probe);
      graph.add_module(paths.probe);
      graph.prepare(stereo(16));
      Buffers io(2, 16);
      io.fill_input(1.0f);
      CHECK(graph.process(io.block(16)) == VS_OK);

      probe->fault = fault;
      probe->fault_instance = faulty;
      io.fill_output(2.0f);
      CHECK(graph.process(io.block(16)) == VS_ERROR_PROCESS);
      CHECK(io.output_is(0.0f, 16));
      CHECK(graph.state() == GraphState::failed);
      CHECK(probe->blocks[1].calls == (faulty == 0 ? 1u : 2u));

      probe->fault = PROBE_FAULT_NONE;
      io.fill_output(2.0f);
      CHECK(graph.process(io.block(16)) == VS_ERROR_STATE);
      CHECK(io.output_is(0.0f, 16));
      CHECK(throws<std::logic_error>([&] { graph.reset(); }));
      CHECK(throws<std::logic_error>([&] { graph.prepare(stereo(16)); }));
      CHECK(throws<std::logic_error>([&] { graph.add_module(paths.probe); }));

      graph.release();
      CHECK(graph.state() == GraphState::editing);
      graph.prepare(stereo(16));
      CHECK(graph.process(io.block(16)) == VS_OK);
      CHECK(near(io.output[0][0], 1.0f));
    }
  }
}

void test_timing(const Paths &paths) {
  current_test = "timing";
  reset_probe();
  {
    Graph graph;
    graph.add_module(paths.probe);
    graph.add_module(paths.probe);
    probe->latency_frames[0] = 3;
    probe->latency_frames[1] = 5;
    probe->tail_frames[0] = 10;
    probe->tail_frames[1] = 20;
    graph.prepare(stereo());
    CHECK(graph.latency_frames() == 8);
    CHECK(graph.tail_frames() == 30);
    graph.release();
    CHECK(graph.latency_frames() == 0 && graph.tail_frames() == 0);

    probe->tail_frames[1] = std::numeric_limits<uint64_t>::max();
    graph.prepare(stereo());
    CHECK(graph.tail_frames() == std::numeric_limits<uint64_t>::max());
    graph.release();

    probe->latency_frames[0] = std::numeric_limits<uint32_t>::max();
    probe->op_count = 0;
    CHECK(throws<std::overflow_error>([&] { graph.prepare(stereo()); }));
    CHECK(graph.state() == GraphState::editing);
    CHECK(graph.latency_frames() == 0);
    CHECK(ops_are({{PROBE_OP_PREPARE, 0},
                   {PROBE_OP_PREPARE, 1},
                   {PROBE_OP_RELEASE, 1},
                   {PROBE_OP_RELEASE, 0}}));
  }
  CHECK(probe->live_instances == 0);
}

void test_state_control(const Paths &paths) {
  current_test = "state_control";
  reset_probe();
  {
    Graph graph;
    CHECK(throws<std::logic_error>([&] { graph.reset(); }));
    graph.release();
    CHECK(graph.state() == GraphState::editing);
    CHECK(throws<std::invalid_argument>([&] { graph.prepare(Format{48000.0, 64, 3}); }));
    CHECK(throws<std::invalid_argument>([&] { graph.prepare(Format{48000.0, 64, 0}); }));
    CHECK(throws<std::invalid_argument>([&] { graph.prepare(Format{48000.0, 0, 2}); }));
    CHECK(throws<std::invalid_argument>([&] { graph.prepare(Format{48000.0, 1u << 20, 2}); }));
    CHECK(throws<std::invalid_argument>(
        [&] { graph.prepare(Format{std::numeric_limits<double>::quiet_NaN(), 64, 2}); }));
    CHECK(throws<std::invalid_argument>([&] { graph.prepare(Format{0.0, 64, 2}); }));
    CHECK(graph.state() == GraphState::editing);

    graph.add_module(paths.probe);
    graph.prepare(stereo());
    CHECK(throws<std::logic_error>([&] { graph.prepare(stereo()); }));
    CHECK(throws<std::logic_error>([&] { graph.add_module(paths.probe); }));
    CHECK(graph.node_count() == 1);
    graph.reset();
    graph.release();
    graph.release();
    CHECK(graph.state() == GraphState::editing);
  }

  reset_probe();
  probe->descriptor = PROBE_DESCRIPTOR_MONO_ONLY;
  Graph mono;
  mono.add_module(paths.probe);
  CHECK(throws<std::runtime_error>([&] { mono.prepare(stereo()); }));
  CHECK(mono.state() == GraphState::editing);
  mono.prepare(Format{48000.0, 64, 1});
  CHECK(mono.state() == GraphState::prepared);
}

void test_reset_and_reprepare(const Paths &paths) {
  current_test = "reset_and_reprepare";
  reset_probe();
  Graph graph;
  graph.add_module(paths.gain);
  graph.add_module(paths.probe);
  graph.prepare(stereo(16));
  Buffers io(2, 64);
  io.fill_input(1.0f);
  const Event gain[] = {event(0, 1, 0, 2.0f)};
  CHECK(graph.process(io.block(16), gain) == VS_OK);
  graph.reset();
  CHECK(graph.process(io.block(16)) == VS_OK);
  CHECK(near(io.output[0][0], 2.0f));

  graph.release();
  graph.prepare(Format{96000.0, 64, 2});
  CHECK(probe->last_prepare[0].max_frames == 64);
  CHECK(graph.process(io.block(64), gain) == VS_OK);
  CHECK(near(io.output[1][63], 2.0f));
  graph.release();
  graph.add_module(paths.gain);
  graph.prepare(Format{48000.0, 8, 1});
  Buffers mono(1, 8);
  mono.fill_input(1.0f);
  const Event third[] = {event(0, 1, 0, 2.0f), event(2, 1, 0, 0.5f)};
  CHECK(graph.process(mono.block(8), third) == VS_OK);
  CHECK(near(mono.output[0][7], 1.0f));
}

std::vector<NodeId> node_ids(const Graph &graph) {
  std::vector<NodeId> ids;
  for (const NodeMetadata &node : graph.nodes()) {
    ids.push_back(node.instance_id);
  }
  return ids;
}

void test_node_identity(const Paths &paths) {
  current_test = "node_identity";
  reset_probe();
  {
    Graph graph;
    CHECK(graph.nodes().empty());
    CHECK(graph.add_module(paths.probe) == 0);
    CHECK(graph.add_module(paths.probe) == 1);
    CHECK(graph.add_module(paths.probe) == 2);
    CHECK((node_ids(graph) == std::vector<NodeId>{0, 1, 2}));

    graph.move_module(2, 0);
    CHECK((node_ids(graph) == std::vector<NodeId>{2, 0, 1}));
    graph.move_module(2, 2);
    CHECK((node_ids(graph) == std::vector<NodeId>{0, 1, 2}));
    graph.move_module(1, 1);
    CHECK((node_ids(graph) == std::vector<NodeId>{0, 1, 2}));
    graph.move_module(0, 1);
    CHECK((node_ids(graph) == std::vector<NodeId>{1, 0, 2}));

    probe->op_count = 0;
    graph.remove_module(0);
    CHECK(ops_are({{PROBE_OP_DESTROY, 0}}));
    CHECK(probe->live_instances == 2);
    CHECK((node_ids(graph) == std::vector<NodeId>{1, 2}));
    CHECK(graph.node_count() == 2);

    CHECK(graph.add_module(paths.probe) == 3);
    CHECK((node_ids(graph) == std::vector<NodeId>{1, 2, 3}));
    graph.remove_module(3);
    CHECK(graph.add_module(paths.probe) == 4);
    CHECK((node_ids(graph) == std::vector<NodeId>{1, 2, 4}));

    probe->fail_create = 1;
    CHECK(throws<std::runtime_error>([&] { graph.add_module(paths.probe); }));
    probe->fail_create = 0;
    CHECK(graph.add_module(paths.probe) == 5);
  }
  CHECK(probe->live_instances == 0);
}

void test_invalid_edits(const Paths &paths) {
  current_test = "invalid_edits";
  reset_probe();
  {
    Graph graph;
    CHECK(throws<std::invalid_argument>([&] { graph.remove_module(0); }));
    CHECK(throws<std::invalid_argument>([&] { graph.move_module(0, 0); }));
    graph.add_module(paths.probe);
    graph.add_module(paths.probe);
    graph.remove_module(0);
    graph.add_module(paths.probe);

    probe->op_count = 0;
    const std::vector<NodeId> before = node_ids(graph);
    CHECK(throws<std::invalid_argument>([&] { graph.remove_module(0); }));
    CHECK(throws<std::invalid_argument>([&] { graph.remove_module(vespera::invalid_node_id); }));
    CHECK(throws<std::invalid_argument>([&] { graph.remove_module(7); }));
    CHECK(throws<std::invalid_argument>([&] { graph.move_module(0, 0); }));
    CHECK(throws<std::invalid_argument>([&] { graph.move_module(vespera::invalid_node_id, 0); }));
    CHECK(throws<std::out_of_range>([&] { graph.move_module(1, 2); }));
    CHECK(throws<std::out_of_range>([&] { graph.move_module(2, UINT32_MAX); }));
    CHECK(node_ids(graph) == before);
    CHECK(probe->op_count == 0);

    graph.prepare(stereo(16));
    CHECK(throws<std::logic_error>([&] { graph.remove_module(1); }));
    CHECK(throws<std::logic_error>([&] { graph.move_module(2, 0); }));
    CHECK(node_ids(graph) == before);
    CHECK(graph.state() == GraphState::prepared);
    Buffers io(2, 16);
    io.fill_input(1.0f);
    const Event events[] = {event(1, PROBE_PARAM_LEVEL, 0, 2.0f),
                            event(2, PROBE_PARAM_OFFSET, 0, 0.5f)};
    CHECK(graph.process(io.block(16), events) == VS_OK);
    CHECK(near(io.output[0][0], 2.5f));
    CHECK(probe->op_count == 2);
  }
  CHECK(probe->live_instances == 0);
}

void test_routing_after_reorder(const Paths &paths) {
  current_test = "routing_after_reorder";
  reset_probe();
  {
    Graph graph;
    const NodeId a = graph.add_module(paths.probe);
    const NodeId b = graph.add_module(paths.probe);
    graph.move_module(b, 0);
    probe->op_count = 0;
    graph.prepare(stereo(16));
    CHECK(ops_are({{PROBE_OP_PREPARE, 1}, {PROBE_OP_PREPARE, 0}}));

    Buffers io(2, 16);
    io.fill_input(1.0f);
    const Event events[] = {event(a, PROBE_PARAM_LEVEL, 0, 2.0f),
                            event(b, PROBE_PARAM_OFFSET, 0, 0.5f)};
    CHECK(graph.process(io.block(16), events) == VS_OK);
    CHECK(near(io.output[0][0], 3.0f) && near(io.output[1][15], 3.0f));
    CHECK(probe->event_count == 2);
    CHECK(probe->events[0].instance == 1 &&
          probe->events[0].event.parameter_id == PROBE_PARAM_OFFSET);
    CHECK(probe->events[1].instance == 0 &&
          probe->events[1].event.parameter_id == PROBE_PARAM_LEVEL);
    CHECK(probe->blocks[0].event_count == 1 && probe->blocks[1].event_count == 1);

    const Event invalid_offset[] = {event(a, PROBE_PARAM_OFFSET, 0, 2.0f)};
    CHECK(graph.process(io.block(16), invalid_offset) == VS_ERROR_ARGUMENT);
    const Event stale_position[] = {event(b, PROBE_PARAM_LEVEL, 0, 1.0f),
                                    event(2, PROBE_PARAM_LEVEL, 0, 1.0f)};
    CHECK(graph.process(io.block(16), stale_position) == VS_ERROR_ARGUMENT);
    CHECK(probe->blocks[0].calls == 1 && probe->blocks[1].calls == 1);

    graph.release();
    CHECK(ops_are({{PROBE_OP_PREPARE, 1},
                   {PROBE_OP_PREPARE, 0},
                   {PROBE_OP_RELEASE, 0},
                   {PROBE_OP_RELEASE, 1}}));

    graph.remove_module(a);
    const NodeId c = graph.add_module(paths.probe);
    CHECK(c == 2);
    graph.move_module(c, 0);
    graph.prepare(stereo(16));
    const Event removed[] = {event(a, PROBE_PARAM_LEVEL, 0, 1.0f)};
    CHECK(graph.process(io.block(16), removed) == VS_ERROR_ARGUMENT);
    const Event reordered[] = {event(b, PROBE_PARAM_LEVEL, 0, 2.0f),
                               event(c, PROBE_PARAM_OFFSET, 0, -0.25f)};
    const uint32_t before = probe->event_count;
    CHECK(graph.process(io.block(16), reordered) == VS_OK);
    CHECK(near(io.output[0][0], 2.0f));
    CHECK(probe->events[before].instance == 2 &&
          probe->events[before].event.parameter_id == PROBE_PARAM_OFFSET);
    CHECK(probe->events[before + 1].instance == 1 &&
          probe->events[before + 1].event.parameter_id == PROBE_PARAM_LEVEL);
    probe->op_count = 0;
  }
  CHECK(ops_are({{PROBE_OP_RELEASE, 1},
                 {PROBE_OP_RELEASE, 2},
                 {PROBE_OP_DESTROY, 1},
                 {PROBE_OP_DESTROY, 2}}));
  CHECK(probe->live_instances == 0);
}

bool gain_resident(const std::string &path) {
  void *handle = dlopen(path.c_str(), RTLD_NOW | RTLD_NOLOAD);
  if (handle != nullptr) {
    dlclose(handle);
  }
  return handle != nullptr;
}

void test_metadata(const Paths &paths) {
  current_test = "metadata";
  reset_probe();
  const std::string gain_path = fs::canonical(paths.gain).string();
  const bool track_residency = !gain_resident(gain_path);
  std::vector<NodeMetadata> snapshot;
  std::vector<NodeMetadata> removed;
  {
    Graph graph;
    const NodeId first = graph.add_module(paths.gain);
    const NodeId probe_node = graph.add_module(paths.probe);
    const NodeId second = graph.add_module(paths.gain);
    graph.move_module(probe_node, 0);
    snapshot = graph.nodes();
    CHECK(snapshot.size() == 3);
    CHECK(snapshot[0].instance_id == probe_node && snapshot[1].instance_id == first &&
          snapshot[2].instance_id == second);

    const vespera::ModuleMetadata &probe_module = snapshot[0].module;
    CHECK(probe_module.id == "dev.vespera.test.probe");
    CHECK(probe_module.name == "Probe" && probe_module.version == "0.0.1");
    CHECK(probe_module.supported_layouts == (VS_LAYOUT_MONO | VS_LAYOUT_STEREO));
    CHECK(probe_module.parameters.size() == 2);
    if (probe_module.parameters.size() == 2) {
      const vespera::ParameterMetadata &level = probe_module.parameters[0];
      const vespera::ParameterMetadata &offset = probe_module.parameters[1];
      CHECK(level.id == PROBE_PARAM_LEVEL && level.name == "Level" && level.unit.empty());
      CHECK(level.minimum == 0.0f && level.maximum == 2.0f && level.default_value == 1.0f);
      CHECK(offset.id == PROBE_PARAM_OFFSET && offset.name == "Offset");
      CHECK(offset.minimum == -1.0f && offset.maximum == 1.0f && offset.default_value == 0.0f);
    }

    for (size_t i : {size_t{1}, size_t{2}}) {
      const vespera::ModuleMetadata &gain = snapshot[i].module;
      CHECK(gain.id == "dev.vespera.gain" && gain.name == "Gain" && gain.version == "0.1.0");
      CHECK(gain.parameters.size() == 1);
      if (gain.parameters.size() == 1) {
        CHECK(gain.parameters[0].id == 1 && gain.parameters[0].name == "Gain" &&
              gain.parameters[0].unit == "linear");
        CHECK(gain.parameters[0].minimum == 0.0f && gain.parameters[0].maximum == 4.0f &&
              gain.parameters[0].default_value == 1.0f);
      }
    }

    graph.prepare(stereo(16));
    CHECK(graph.nodes().size() == 3 && graph.nodes()[2].instance_id == second);
    graph.release();

    graph.remove_module(first);
    CHECK(track_residency == false || gain_resident(gain_path));
    removed = graph.nodes();
    graph.remove_module(second);
    if (track_residency) {
      CHECK(!gain_resident(gain_path));
    }
    CHECK((node_ids(graph) == std::vector<NodeId>{probe_node}));
  }
  CHECK(removed.size() == 2 && removed[1].module.name == "Gain");
  CHECK(snapshot[1].module.id == "dev.vespera.gain" && snapshot[2].module.version == "0.1.0");
  CHECK(snapshot[2].module.parameters.size() == 1 &&
        snapshot[2].module.parameters[0].unit == "linear");
  CHECK(probe->live_instances == 0);
}

void test_generation(const Paths &paths) {
  current_test = "generation";
  reset_probe();
  Graph first;
  Graph second;
  CHECK(first.generation() == 0 && second.generation() == 0);
  first.add_module(paths.probe);
  second.add_module(paths.probe);

  first.prepare(stereo(16));
  const uint64_t g1 = first.generation();
  CHECK(g1 != 0);
  first.reset();
  CHECK(first.generation() == g1);
  Buffers io(2, 16);
  io.fill_input(1.0f);
  CHECK(first.process(io.block(16)) == VS_OK);
  CHECK(first.generation() == g1);

  second.prepare(stereo(16));
  const uint64_t g2 = second.generation();
  CHECK(g2 != 0 && g2 != g1);

  first.release();
  CHECK(first.generation() == 0);
  first.prepare(stereo(16));
  const uint64_t g3 = first.generation();
  CHECK(g3 != 0 && g3 != g1 && g3 != g2);

  probe->fault = PROBE_FAULT_ERROR;
  probe->fault_instance = 0;
  CHECK(first.process(io.block(16)) == VS_ERROR_PROCESS);
  CHECK(first.state() == GraphState::failed);
  CHECK(first.generation() == g3);
  probe->fault = PROBE_FAULT_NONE;
  first.release();
  CHECK(first.generation() == 0);

  probe->fail_prepare_instance = 0;
  CHECK(throws<std::runtime_error>([&] { first.prepare(stereo(16)); }));
  CHECK(first.generation() == 0);
  CHECK(throws<std::invalid_argument>([&] { first.prepare(Format{48000.0, 0, 2}); }));
  CHECK(first.generation() == 0);
  probe->fail_prepare_instance = -1;
  first.prepare(stereo(16));
  const uint64_t g4 = first.generation();
  CHECK(g4 != 0 && g4 != g1 && g4 != g2 && g4 != g3);
  CHECK(second.generation() == g2);
  first.release();
  second.release();
}

void test_metadata_during_processing(const Paths &paths) {
  current_test = "metadata_during_processing";
  Graph graph;
  const NodeId id = graph.add_module(paths.gain);
  graph.prepare(stereo(16));
  const uint64_t generation = graph.generation();
  Buffers io(2, 16);
  io.fill_input(0.5f);
  bool process_ok = true;
  vs_result failure_result = VS_OK;
  std::atomic<bool> start{false};
  std::thread audio([&] {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    for (uint32_t i = 0; i < 10000; ++i) {
      const Event change[] = {event(id, 1, 0, i % 2 == 0 ? 0.5f : 2.0f)};
      process_ok &= graph.process(io.block(16, i * 16), change) == VS_OK;
    }
    io.fill_input(std::numeric_limits<float>::max());
    const Event change[] = {event(id, 1, 0, 4.0f)};
    failure_result = graph.process(io.block(16), change);
  });
  start.store(true, std::memory_order_release);
  for (uint32_t i = 0; i < 10000; ++i) {
    const auto metadata = graph.nodes();
    CHECK(metadata.size() == 1 && metadata[0].instance_id == id);
    CHECK(metadata[0].module.parameters[0].default_value == 1.0f);
    CHECK(graph.generation() == generation);
  }
  audio.join();
  CHECK(process_ok);
  CHECK(failure_result == VS_ERROR_PROCESS);
  CHECK(graph.state() == GraphState::failed);
}

void test_duplicate_library_removal(const Paths &paths) {
  current_test = "duplicate_library_removal";
  const fs::path copy = paths.temp / "gain-removal-copy.module";
  fs::copy_file(paths.gain, copy);
  {
    Graph graph;
    const NodeId first = graph.add_module(paths.gain);
    const NodeId second = graph.add_module(paths.gain);
    graph.remove_module(first);
    CHECK(throws<std::runtime_error>([&] { graph.add_module(copy); }));
    CHECK((node_ids(graph) == std::vector<NodeId>{second}));
    graph.remove_module(second);
    const NodeId replacement = graph.add_module(copy);
    CHECK(replacement == 2);
    CHECK(throws<std::runtime_error>([&] { graph.add_module(paths.gain); }));
    CHECK((node_ids(graph) == std::vector<NodeId>{replacement}));
    graph.prepare(stereo(16));
    Buffers io(2, 16);
    io.fill_input(1.0f);
    const Event gain[] = {event(replacement, 1, 0, 3.0f)};
    CHECK(graph.process(io.block(16), gain) == VS_OK);
    CHECK(near(io.output[1][15], 3.0f));
  }

  reset_probe();
  probe->descriptor = PROBE_DESCRIPTOR_GAIN_IMPOSTOR;
  {
    Graph graph;
    const NodeId gain = graph.add_module(paths.gain);
    CHECK(throws<std::runtime_error>([&] { graph.add_module(paths.probe); }));
    graph.remove_module(gain);
    CHECK(graph.add_module(paths.probe) == 1);
    CHECK(graph.nodes().size() == 1 && graph.nodes()[0].module.name == "Probe");
  }
  CHECK(probe->live_instances == 0);
}

void test_capabilities(const Paths &paths) {
  current_test = "capabilities";
  reset_probe();
  {
    Graph graph;
    graph.add_module(paths.probe);
    graph.add_module(paths.gain);
    for (const auto &node : graph.nodes()) {
      CHECK(!node.module.bypass_parameter && !node.module.host_tempo && !node.module.midi_input);
    }
  }

  reset_probe();
  probe->descriptor = PROBE_DESCRIPTOR_BYPASS_OFFSET;
  {
    Graph graph;
    graph.add_module(paths.probe);
    const vespera::ModuleMetadata module = graph.nodes()[0].module;
    CHECK(module.bypass_parameter == PROBE_PARAM_OFFSET);
    CHECK(!module.host_tempo && !module.midi_input);
  }

  reset_probe();
  probe->descriptor = PROBE_DESCRIPTOR_TEMPO_AND_MIDI;
  {
    Graph graph;
    graph.add_module(paths.probe);
    const vespera::ModuleMetadata module = graph.nodes()[0].module;
    CHECK(!module.bypass_parameter && module.host_tempo && module.midi_input);
  }

  for (int variant :
       {PROBE_DESCRIPTOR_BYPASS_UNDECLARED_PARAMETER, PROBE_DESCRIPTOR_BYPASS_WRONG_RANGE,
        PROBE_DESCRIPTOR_BYPASS_WITHOUT_CAPABILITY, PROBE_DESCRIPTOR_UNKNOWN_CAPABILITY}) {
    reset_probe();
    probe->descriptor = variant;
    Graph graph;
    if (!throws<std::runtime_error>([&] { graph.add_module(paths.probe); })) {
      std::fprintf(stderr, "[%s] capability variant %d was accepted\n", current_test, variant);
      ++failures;
    }
    CHECK(graph.node_count() == 0 && probe->next_instance == 0);
  }

  reset_probe();
  probe->descriptor = PROBE_DESCRIPTOR_ABI_1_ONLY;
  {
    Graph graph;
    std::string message;
    try {
      graph.add_module(paths.probe);
    } catch (const std::runtime_error &error) {
      message = error.what();
    }
    CHECK(message.find("built for module ABI version 1") != std::string::npos);
    CHECK(message.find("loads only ABI version " + std::to_string(VS_MODULE_ABI_VERSION)) !=
          std::string::npos);
    CHECK(graph.node_count() == 0 && probe->next_instance == 0);
  }
  CHECK(probe->live_instances == 0);
}

void test_tempo_reaches_declaring_modules(const Paths &paths) {
  current_test = "tempo_reaches_declaring_modules";
  for (int variant : {PROBE_DESCRIPTOR_VALID, PROBE_DESCRIPTOR_TEMPO_AND_MIDI}) {
    reset_probe();
    probe->descriptor = variant;
    Graph graph;
    graph.add_module(paths.probe);
    graph.prepare(stereo(16));
    Buffers io(2, 16);
    const vs_tempo_context tempo{sizeof(vs_tempo_context), VS_TEMPO_VALID, 120.0, 0.0};
    Block block = io.block(16);
    block.tempo = &tempo;
    CHECK(graph.process(block) == VS_OK);
    CHECK(probe->blocks[0].calls == 1);
    CHECK(probe->blocks[0].tempo == (variant == PROBE_DESCRIPTOR_TEMPO_AND_MIDI ? 1u : 0u));
  }
  CHECK(probe->live_instances == 0);
}

void test_unicode_and_extended_descriptors(const Paths &paths) {
  current_test = "unicode_and_extended_descriptors";
  reset_probe();
  probe->descriptor = PROBE_DESCRIPTOR_UNICODE;
  {
    Graph graph;
    graph.add_module(paths.probe);
    const vespera::ModuleMetadata module = graph.nodes()[0].module;
    CHECK(module.name == "Probe \xe2\x9c\x93");
    CHECK(module.version == "0.0.1-\xce\xb2");
    CHECK(module.parameters.size() == 2 && module.parameters[0].name == "Lautst\xc3\xa4rke" &&
          module.parameters[1].unit == "\xc2\xb5s \xf0\x9f\x8e\xb8");
  }

  reset_probe();
  probe->descriptor = PROBE_DESCRIPTOR_EXTENDED_STRUCT;
  {
    Graph graph;
    const NodeId node = graph.add_module(paths.probe);
    graph.prepare(stereo(16));
    Buffers io(2, 16);
    io.fill_input(0.5f);
    const Event change[] = {event(node, PROBE_PARAM_LEVEL, 0, 2.0f)};
    CHECK(graph.process(io.block(16), change) == VS_OK);
    CHECK(io.output_is(1.0f, 16));
  }
  CHECK(probe->live_instances == 0);
}

void test_parameter_values(const Paths &paths) {
  current_test = "parameter_values";
  reset_probe();
  Graph graph;
  const NodeId gain = graph.add_module(paths.gain);
  const NodeId legacy = graph.add_module(paths.probe);
  CHECK(graph.nodes()[0].module.parameter_values);
  CHECK(!graph.nodes()[1].module.parameter_values);
  CHECK(graph.parameter_value(gain, 1) == 1.0f);
  graph.set_parameter_value(gain, 1, 2.0f);
  CHECK(graph.parameter_value(gain, 1) == 2.0f);

  CHECK(throws<std::runtime_error>([&] { graph.parameter_value(legacy, PROBE_PARAM_LEVEL); }));
  CHECK(
      throws<std::runtime_error>([&] { graph.set_parameter_value(legacy, PROBE_PARAM_LEVEL, 1); }));
  CHECK(throws<std::invalid_argument>([&] { graph.parameter_value(legacy, 1); }));
  CHECK(throws<std::invalid_argument>([&] { graph.parameter_value(gain, 2); }));
  CHECK(throws<std::invalid_argument>([&] { graph.parameter_value(99, 1); }));
  for (float bad : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                    -0.0001f, 4.0001f}) {
    CHECK(throws<std::invalid_argument>([&] { graph.set_parameter_value(gain, 1, bad); }));
  }
  CHECK(graph.parameter_value(gain, 1) == 2.0f);
  graph.set_parameter_value(gain, 1, 4.0f);
  graph.set_parameter_value(gain, 1, 0.0f);
  CHECK(graph.parameter_value(gain, 1) == 0.0f);
  graph.set_parameter_value(gain, 1, 2.0f);

  graph.prepare(stereo(16));
  Buffers io(2, 16);
  io.fill_input(0.5f);
  CHECK(graph.process(io.block(16)) == VS_OK);
  CHECK(io.output_is(1.0f, 16));
  const Event change[] = {event(gain, 1, 8, 3.0f)};
  CHECK(graph.process(io.block(16), change) == VS_OK);
  CHECK(graph.parameter_value(gain, 1) == 3.0f);

  graph.set_parameter_value(gain, 1, 0.5f);
  CHECK(graph.process(io.block(16)) == VS_OK);
  CHECK(io.output_is(0.25f, 16));
  const Event rejected[] = {event(gain, 1, 0, 1.0f), event(gain, 9, 0, 1.0f)};
  CHECK(graph.process(io.block(16), rejected) == VS_ERROR_ARGUMENT);
  CHECK(graph.parameter_value(gain, 1) == 0.5f);
  graph.reset();
  CHECK(graph.parameter_value(gain, 1) == 0.5f);
  graph.release();
  CHECK(graph.parameter_value(gain, 1) == 0.5f);

  graph.prepare(stereo(16));
  probe->fault = PROBE_FAULT_ERROR;
  probe->fault_instance = 0;
  CHECK(graph.process(io.block(16)) == VS_ERROR_PROCESS);
  CHECK(graph.state() == GraphState::failed);
  CHECK(throws<std::logic_error>([&] { graph.parameter_value(gain, 1); }));
  CHECK(throws<std::logic_error>([&] { graph.set_parameter_value(gain, 1, 1.0f); }));
  graph.release();
  CHECK(graph.parameter_value(gain, 1) == 0.5f);
}

void test_allocation_free_process(const Paths &paths) {
  current_test = "allocation_free_process";
  reset_probe();
  Graph graph;
  graph.add_module(paths.gain);
  graph.add_module(paths.probe);
  graph.add_module(paths.gain);
  graph.prepare(stereo(64));
  Buffers io(2, 64);
  io.fill_input(0.5f);
  std::vector<Event> events;
  for (uint32_t i = 0; i < Graph::max_events_per_block; ++i) {
    events.push_back(event(i % 3, i % 3 == 1 ? PROBE_PARAM_LEVEL : 1,
                           i / (Graph::max_events_per_block / 64), 1.0f));
  }
  std::vector<Event> invalid = {event(5, 1, 0, 1.0f)};

  allocation_count = 0;
  counting_allocations = true;
  vs_result results[4] = {};
  for (uint32_t i = 0; i < 100; ++i) {
    results[0] |= graph.process(io.block(64, i * 64), events);
    results[1] |= graph.process(io.block(1 + i % 64, i * 64));
  }
  results[2] = graph.process(io.block(64), invalid);
  probe->fault = PROBE_FAULT_NAN;
  probe->fault_instance = 0;
  results[3] = graph.process(io.block(64));
  counting_allocations = false;

  CHECK(allocation_count == 0);
  CHECK(results[0] == VS_OK && results[1] == VS_OK);
  CHECK(results[2] == VS_ERROR_ARGUMENT && results[3] == VS_ERROR_PROCESS);
}

}

int main(int argc, char **argv) {
  if (argc != 4) {
    std::fprintf(stderr, "usage: %s <gain-module> <probe-fixture> <no-entry-fixture>\n",
                 argc > 0 ? argv[0] : "runtime_test");
    return 2;
  }
  Paths paths{fs::canonical(argv[1]), fs::canonical(argv[2]), fs::canonical(argv[3]), {}};
  paths.temp = fs::temp_directory_path() / ("vespera-runtime-test-" + std::to_string(getpid()));
  fs::remove_all(paths.temp);
  fs::create_directories(paths.temp);

  void *probe_library = dlopen(paths.probe.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (probe_library == nullptr) {
    std::fprintf(stderr, "cannot open probe fixture: %s\n", dlerror());
    return 2;
  }
  const auto control =
      reinterpret_cast<probe_control_fn>(dlsym(probe_library, PROBE_CONTROL_SYMBOL));
  if (control == nullptr) {
    std::fprintf(stderr, "probe fixture lacks %s\n", PROBE_CONTROL_SYMBOL);
    return 2;
  }
  probe = control();

  const std::function<void(const Paths &)> tests[] = {
      test_unload_after_destroy,
      test_gain_processing,
      test_mono_gain,
      [](const Paths &) { test_empty_graph(); },
      test_in_place_single_node,
      test_multiple_instances,
      test_duplicate_identity,
      test_load_failures,
      test_lifecycle_order,
      test_partial_prepare_failure,
      test_event_routing,
      test_invalid_events,
      test_block_validation,
      test_block_segmentation,
      test_process_failure,
      test_timing,
      test_state_control,
      test_reset_and_reprepare,
      test_node_identity,
      test_invalid_edits,
      test_routing_after_reorder,
      test_metadata,
      test_generation,
      test_metadata_during_processing,
      test_duplicate_library_removal,
      test_unicode_and_extended_descriptors,
      test_capabilities,
      test_tempo_reaches_declaring_modules,
      test_parameter_values,
      test_allocation_free_process,
  };
  for (const auto &test : tests) {
    try {
      test(paths);
    } catch (const std::exception &error) {
      std::fprintf(stderr, "[%s] unexpected exception: %s\n", current_test, error.what());
      ++failures;
    }
  }

  dlclose(probe_library);
  fs::remove_all(paths.temp);
  if (failures != 0) {
    std::fprintf(stderr, "%d runtime check(s) failed\n", failures);
    return 1;
  }
  std::puts("runtime tests passed");
  return 0;
}
