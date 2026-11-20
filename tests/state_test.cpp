#include <vespera/runtime.hpp>

#include "fixtures/probe_control.h"
#include "fixtures/state_control.h"

#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using vespera::Block;
using vespera::Event;
using vespera::Format;
using vespera::Graph;
using vespera::GraphState;
using vespera::invalid_node_id;
using vespera::ModuleCatalog;
using vespera::NodeId;
using vespera::Preset;
using vespera::PresetIssue;
using vespera::PresetNode;

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
bool throws(const std::function<void()> &action, const std::string &fragment = {}) {
  try {
    action();
  } catch (const Exception &error) {
    if (std::string(error.what()).find(fragment) != std::string::npos) {
      return true;
    }
    std::fprintf(stderr, "[%s] message lacks '%s': %s\n", current_test, fragment.c_str(),
                 error.what());
    return false;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "[%s] unexpected exception type: %s\n", current_test, error.what());
    return false;
  }
  return false;
}

struct Paths {
  fs::path gain;
  fs::path state;
  fs::path probe;
  fs::path temp;
};

state_control *state = nullptr;
probe_control *probe = nullptr;

// Copied modules stay mapped for the whole run: AddressSanitizer on macOS keeps stale global
// redzones after dlclose, which misfires when a later module is mapped at the same address.
void *keep_resident(const fs::path &path) { return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL); }

void reset_controls() {
  CHECK(state->live_instances == 0);
  CHECK(probe->live_instances == 0);
  std::memset(state, 0, sizeof(*state));
  std::memset(probe, 0, sizeof(*probe));
  probe->fail_prepare_instance = -1;
  probe->fault_instance = -1;
}

struct Buffers {
  Buffers(uint32_t channels, uint32_t frames)
      : input(channels, std::vector<float>(frames)), output(channels, std::vector<float>(frames)) {
    for (uint32_t c = 0; c < channels; ++c) {
      in_ptrs.push_back(input[c].data());
      out_ptrs.push_back(output[c].data());
    }
  }

  Block block(uint32_t frames) const {
    return Block{
        in_ptrs.data(), out_ptrs.data(), static_cast<uint32_t>(in_ptrs.size()), frames, 0, 0};
  }

  void fill(float left, float right) {
    std::fill(input[0].begin(), input[0].end(), left);
    std::fill(input[1].begin(), input[1].end(), right);
  }

  bool output_is(float left, float right, uint32_t frames) const {
    for (uint32_t i = 0; i < frames; ++i) {
      if (output[0][i] != left || output[1][i] != right) {
        return false;
      }
    }
    return true;
  }

  std::vector<std::vector<float>> input;
  std::vector<std::vector<float>> output;
  std::vector<const float *> in_ptrs;
  std::vector<float *> out_ptrs;
};

Event event(NodeId node, uint32_t parameter, float value) {
  return Event{node, vs_parameter_event{parameter, 0, value}};
}

Format stereo() { return Format{48000.0, 16, 2}; }

std::vector<uint8_t> le_float(float value) {
  uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return {static_cast<uint8_t>(bits), static_cast<uint8_t>(bits >> 8),
          static_cast<uint8_t>(bits >> 16), static_cast<uint8_t>(bits >> 24)};
}

std::vector<uint8_t> concat(std::vector<uint8_t> a, const std::vector<uint8_t> &b) {
  a.insert(a.end(), b.begin(), b.end());
  return a;
}

PresetNode gain_node(NodeId id, float value) {
  return PresetNode{id, "dev.vespera.gain", "0.1.0", VS_MODULE_ABI_VERSION, {1, le_float(value)}};
}

PresetNode state_node(NodeId id, uint32_t schema, std::vector<uint8_t> bytes,
                      const std::string &version = "1.0.0") {
  return PresetNode{
      id, STATE_MODULE_ID, version, VS_MODULE_ABI_VERSION, {schema, std::move(bytes)}};
}

std::vector<NodeId> ids(const Graph &graph) {
  std::vector<NodeId> result;
  for (const auto &node : graph.nodes()) {
    result.push_back(node.instance_id);
  }
  return result;
}

ModuleCatalog catalog_for(const Paths &paths) {
  ModuleCatalog catalog;
  catalog.add(paths.gain);
  catalog.add(paths.state);
  catalog.add(paths.probe);
  return catalog;
}

float process_gain(Graph &graph, float input) {
  Buffers io(2, 16);
  io.fill(input, -input);
  CHECK(graph.process(io.block(16)) == VS_OK);
  CHECK(io.output[1][15] == -io.output[0][15]);
  return io.output[0][15];
}

void test_gain_roundtrip(const Paths &paths) {
  current_test = "gain_roundtrip";
  Graph source;
  source.add_module(paths.gain);
  CHECK(source.nodes()[0].module.state_schema_version == 1);
  source.prepare(stereo());
  Buffers io(2, 16);
  io.fill(0.5f, -0.5f);
  const Event change[] = {event(0, 1, 2.5f)};
  CHECK(source.process(io.block(16), change) == VS_OK);
  CHECK(io.output_is(1.25f, -1.25f, 16));

  Preset preset = source.capture_preset();
  CHECK(preset.schema_version == 1);
  CHECK(preset.next_instance_id == 1);
  CHECK(preset.nodes.size() == 1);
  const PresetNode &node = preset.nodes[0];
  CHECK(node.instance_id == 0);
  CHECK(node.module_id == "dev.vespera.gain");
  CHECK(node.module_version == "0.1.0");
  CHECK(node.module_abi_version == VS_MODULE_ABI_VERSION);
  CHECK(node.state.schema_version == 1);
  CHECK((node.state.bytes == std::vector<uint8_t>{0x00, 0x00, 0x20, 0x40}));
  CHECK(source.state() == GraphState::prepared);

  source.reset();
  CHECK(source.capture_preset().nodes[0].state.bytes == node.state.bytes);
  source.release();
  CHECK(source.capture_preset().nodes[0].state.bytes == node.state.bytes);

  const ModuleCatalog catalog = catalog_for(paths);
  CHECK(catalog.inspect(preset).empty());
  std::unique_ptr<Graph> restored = Graph::restore_preset(preset, catalog);
  CHECK(restored->state() == GraphState::editing);
  CHECK(restored->generation() == 0);
  CHECK(ids(*restored) == std::vector<NodeId>{0});
  restored->prepare(stereo());
  io.fill(0.5f, -0.5f);
  CHECK(restored->process(io.block(16)) == VS_OK);
  CHECK(io.output_is(1.25f, -1.25f, 16));
  CHECK(restored->capture_preset().nodes[0].state.bytes == node.state.bytes);
}

void test_load_module_state(const Paths &paths) {
  current_test = "load_module_state";
  Graph graph;
  const NodeId gain = graph.add_module(paths.gain);
  const NodeId other = graph.add_module(paths.gain);
  graph.load_module_state(gain, {1, le_float(3.0f)});
  CHECK(graph.parameter_value(gain, 1) == 3.0f);
  CHECK(graph.parameter_value(other, 1) == 1.0f);
  CHECK(throws<std::runtime_error>([&] { graph.load_module_state(gain, {2, le_float(1.0f)}); },
                                   "does not support state schema version 2"));
  CHECK(throws<std::runtime_error>([&] { graph.load_module_state(gain, {1, {0, 0}}); },
                                   "rejected its state"));
  CHECK(throws<std::length_error>(
      [&] {
        graph.load_module_state(gain,
                                {1, std::vector<uint8_t>(vespera::max_module_state_bytes + 1)});
      },
      "exceeds the limit"));
  CHECK(throws<std::invalid_argument>([&] { graph.load_module_state(99, {1, le_float(1.0f)}); },
                                      "no node with ID 99"));
  CHECK(graph.parameter_value(gain, 1) == 3.0f);
  graph.prepare(stereo());
  CHECK(throws<std::logic_error>([&] { graph.load_module_state(gain, {1, le_float(2.0f)}); },
                                 "requires an editing graph"));
  Buffers io(2, 16);
  io.fill(0.5f, -0.5f);
  CHECK(graph.process(io.block(16)) == VS_OK);
  CHECK(io.output_is(1.5f, -1.5f, 16));
}

void test_gain_state_rejection(const Paths &paths) {
  current_test = "gain_state_rejection";
  const ModuleCatalog catalog = catalog_for(paths);
  auto restore = [&](PresetNode node) {
    Preset preset;
    preset.next_instance_id = node.instance_id + 1;
    preset.nodes.push_back(std::move(node));
    return Graph::restore_preset(preset, catalog);
  };
  PresetNode wrong_schema = gain_node(0, 1.0f);
  wrong_schema.state.schema_version = 2;
  CHECK(throws<std::runtime_error>([&] { restore(wrong_schema); },
                                   "does not support state schema version 2"));
  PresetNode short_blob = gain_node(0, 1.0f);
  short_blob.state.bytes.pop_back();
  CHECK(throws<std::runtime_error>([&] { restore(short_blob); }, "rejected its state"));
  PresetNode long_blob = gain_node(0, 1.0f);
  long_blob.state.bytes.push_back(0);
  CHECK(throws<std::runtime_error>([&] { restore(long_blob); }, "rejected its state"));
  PresetNode empty_blob = gain_node(0, 1.0f);
  empty_blob.state.bytes.clear();
  CHECK(throws<std::runtime_error>([&] { restore(empty_blob); }, "rejected its state"));
  for (float bad : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                    4.0001f, -0.0001f}) {
    CHECK(throws<std::runtime_error>([&] { restore(gain_node(0, bad)); }, "rejected its state"));
  }
  for (float edge : {0.0f, 4.0f}) {
    auto graph = restore(gain_node(0, edge));
    graph->prepare(stereo());
    CHECK(process_gain(*graph, 0.5f) == 0.5f * edge);
  }
}

void test_identity_preservation(const Paths &paths) {
  current_test = "identity_preservation";
  const ModuleCatalog catalog = catalog_for(paths);
  Graph source;
  for (int i = 0; i < 4; ++i) {
    source.add_module(paths.gain);
  }
  source.remove_module(3);
  source.remove_module(1);
  source.move_module(2, 0);
  source.prepare(stereo());
  Buffers io(2, 16);
  const Event change[] = {event(2, 1, 2.0f), event(0, 1, 0.75f)};
  CHECK(source.process(io.block(1), change) == VS_OK);

  Preset preset = source.capture_preset();
  CHECK(preset.next_instance_id == 4);
  CHECK(preset.nodes.size() == 2);
  CHECK(preset.nodes[0].instance_id == 2 && preset.nodes[1].instance_id == 0);
  CHECK(preset.nodes[0].state.bytes == le_float(2.0f));
  CHECK(preset.nodes[1].state.bytes == le_float(0.75f));

  std::unique_ptr<Graph> restored = Graph::restore_preset(preset, catalog);
  CHECK((ids(*restored) == std::vector<NodeId>{2, 0}));
  restored->prepare(stereo());
  CHECK(process_gain(*restored, 1.0f) == 1.5f);
  restored->release();
  CHECK(restored->add_module(paths.gain) == 4);

  Preset scattered;
  scattered.next_instance_id = 9;
  scattered.nodes = {gain_node(7, 1.0f), gain_node(3, 2.0f), gain_node(5, 0.5f)};
  restored = Graph::restore_preset(scattered, catalog);
  CHECK((ids(*restored) == std::vector<NodeId>{7, 3, 5}));
  CHECK(restored->capture_preset().next_instance_id == 9);
  CHECK(restored->add_module(paths.gain) == 9);

  Graph emptied;
  emptied.add_module(paths.gain);
  emptied.add_module(paths.gain);
  emptied.remove_module(1);
  emptied.remove_module(0);
  const Preset empty = emptied.capture_preset();
  CHECK(empty.nodes.empty());
  CHECK(empty.next_instance_id == 2);
  restored = Graph::restore_preset(empty, catalog);
  CHECK(restored->node_count() == 0);
  CHECK(restored->add_module(paths.gain) == 2);

  Preset exhausted;
  exhausted.next_instance_id = invalid_node_id;
  exhausted.nodes = {gain_node(invalid_node_id - 1, 1.0f)};
  restored = Graph::restore_preset(exhausted, catalog);
  CHECK(restored->capture_preset().next_instance_id == invalid_node_id);
  CHECK(throws<std::overflow_error>([&] { restored->add_module(paths.gain); }));
}

void test_blob_ownership(const Paths &paths) {
  current_test = "blob_ownership";
  const ModuleCatalog catalog = catalog_for(paths);
  Preset preset;
  {
    auto source = std::make_unique<Graph>();
    source->add_module(paths.gain);
    source->prepare(stereo());
    Buffers io(2, 16);
    const Event first[] = {event(0, 1, 2.5f)};
    CHECK(source->process(io.block(1), first) == VS_OK);
    preset = source->capture_preset();
    const Event second[] = {event(0, 1, 3.0f)};
    CHECK(source->process(io.block(1), second) == VS_OK);
    CHECK(preset.nodes[0].state.bytes == le_float(2.5f));
    CHECK(source->capture_preset().nodes[0].state.bytes == le_float(3.0f));
  }
  CHECK(preset.nodes[0].state.bytes == le_float(2.5f));

  Preset copy = preset;
  std::unique_ptr<Graph> restored = Graph::restore_preset(copy, catalog);
  copy.nodes[0].state.bytes = le_float(0.0f);
  copy.nodes.clear();
  restored->prepare(stereo());
  CHECK(process_gain(*restored, 2.0f) == 5.0f);
  CHECK(preset.nodes[0].state.bytes == le_float(2.5f));
}

void test_legacy_module(const Paths &paths) {
  current_test = "legacy_module";
  Graph graph;
  graph.add_module(paths.gain);
  graph.add_module(paths.probe);
  CHECK(graph.nodes()[1].module.state_schema_version == 0);
  graph.prepare(stereo());
  CHECK(throws<std::runtime_error>([&] { graph.capture_preset(); },
                                   "'dev.vespera.test.probe' at node 1 does not support state"));
  CHECK(graph.state() == GraphState::prepared);
  Buffers io(2, 16);
  io.fill(0.5f, 0.25f);
  const Event change[] = {event(1, PROBE_PARAM_LEVEL, 2.0f)};
  CHECK(graph.process(io.block(16), change) == VS_OK);
  CHECK(io.output_is(1.0f, 0.5f, 16));

  const ModuleCatalog catalog = catalog_for(paths);
  Preset preset;
  preset.next_instance_id = 2;
  preset.nodes = {gain_node(0, 1.0f),
                  PresetNode{1, "dev.vespera.test.probe", "0.0.1", VS_MODULE_ABI_VERSION, {1, {}}}};
  const std::vector<PresetIssue> issues = catalog.inspect(preset);
  CHECK(issues.size() == 1);
  CHECK(issues.size() == 1 && issues[0].instance_id == 1 &&
        issues[0].message.find("does not support state") != std::string::npos);
  const int32_t live = probe->live_instances;
  CHECK(throws<std::runtime_error>([&] { Graph::restore_preset(preset, catalog); },
                                   "does not support state restore"));
  CHECK(probe->live_instances == live);
}

void test_declined_state_entry(const Paths &paths) {
  current_test = "declined_state_entry";
  state->descriptor = STATE_DESCRIPTOR_DECLINE;
  Graph graph;
  graph.add_module(paths.state);
  CHECK(graph.nodes()[0].module.state_schema_version == 0);
  CHECK(throws<std::runtime_error>([&] { graph.capture_preset(); }, "does not support state"));
  graph.prepare(stereo());
  CHECK(process_gain(graph, 0.5f) == 0.5f);
}

void test_malformed_state_descriptors(const Paths &paths) {
  current_test = "malformed_state_descriptors";
  const struct {
    int32_t descriptor;
    const char *message;
  } cases[] = {
      {STATE_DESCRIPTOR_SMALL_STRUCT, "state descriptor struct_size"},
      {STATE_DESCRIPTOR_WRONG_ABI, "state ABI version 2"},
      {STATE_DESCRIPTOR_ZERO_SCHEMA, "state schema version 0"},
      {STATE_DESCRIPTOR_NULL_SAVE, "missing required state functions"},
  };
  for (const auto &c : cases) {
    state->descriptor = c.descriptor;
    Graph graph;
    CHECK(throws<std::runtime_error>([&] { graph.add_module(paths.state); }, c.message));
    CHECK(graph.node_count() == 0);
    ModuleCatalog catalog;
    CHECK(throws<std::runtime_error>([&] { catalog.add(paths.state); }, c.message));
    CHECK(state->creates == 0);
  }
}

void test_capture_failures(const Paths &paths) {
  current_test = "capture_failures";
  Graph graph;
  graph.add_module(paths.gain);
  graph.add_module(paths.state);
  graph.prepare(stereo());
  const struct {
    int32_t size_mode;
    int32_t save_mode;
    const char *message;
  } cases[] = {
      {STATE_SIZE_ERROR, STATE_SAVE_NORMAL, "failed to report its state size (2)"},
      {STATE_SIZE_NORMAL, STATE_SAVE_ERROR, "failed to save its state (2)"},
      {STATE_SIZE_NORMAL, STATE_SAVE_SHORT, "wrote 7 state bytes after reporting 8"},
      {STATE_SIZE_NORMAL, STATE_SAVE_LONG, "wrote 9 state bytes after reporting 8"},
      {STATE_SIZE_LIMIT, STATE_SAVE_SHORT, "wrote 1048575 state bytes after reporting 1048576"},
  };
  for (const auto &c : cases) {
    state->size_mode = c.size_mode;
    state->save_mode = c.save_mode;
    CHECK(throws<std::runtime_error>([&] { graph.capture_preset(); }, c.message));
    CHECK(graph.state() == GraphState::prepared);
    CHECK(process_gain(graph, 0.5f) == 0.5f);
  }
  state->size_mode = STATE_SIZE_OVER_LIMIT;
  state->save_mode = STATE_SAVE_NORMAL;
  CHECK(throws<std::length_error>([&] { graph.capture_preset(); },
                                  "at node 1 reported 1048577 state bytes"));
  CHECK(process_gain(graph, 0.5f) == 0.5f);

  state->size_mode = STATE_SIZE_LIMIT;
  const Preset at_limit = graph.capture_preset();
  CHECK(at_limit.nodes[1].state.bytes.size() == vespera::max_module_state_bytes);
  CHECK(at_limit.nodes[1].state.bytes[1000] == static_cast<uint8_t>(1000 & 0xff));
  graph.release();
  graph.remove_module(0);
  for (int i = 0; i < 15; ++i) {
    graph.add_module(paths.state);
  }
  const Preset sixteen = graph.capture_preset();
  CHECK(sixteen.nodes.size() == 16);
  vespera::validate_preset(sixteen);
  graph.add_module(paths.state);
  CHECK(throws<std::length_error>([&] { graph.capture_preset(); }, "preset limit"));

  Graph failed;
  failed.add_module(paths.probe);
  failed.prepare(stereo());
  probe->fault_instance = 0;
  probe->fault = PROBE_FAULT_ERROR;
  Buffers io(2, 16);
  CHECK(failed.process(io.block(16)) == VS_ERROR_PROCESS);
  CHECK(failed.state() == GraphState::failed);
  CHECK(throws<std::logic_error>([&] { failed.capture_preset(); }, "editing or prepared"));
}

void test_validate_preset(const Paths &paths) {
  current_test = "validate_preset";
  const ModuleCatalog catalog = catalog_for(paths);
  const auto valid = [] {
    Preset preset;
    preset.next_instance_id = 4;
    preset.nodes = {state_node(1, 2, std::vector<uint8_t>(8)), gain_node(3, 1.0f)};
    return preset;
  };
  vespera::validate_preset(valid());
  Preset empty;
  empty.next_instance_id = 12;
  vespera::validate_preset(empty);

  const std::vector<std::pair<const char *, std::function<void(Preset &)>>> cases = {
      {"unsupported preset schema", [](Preset &p) { p.schema_version = 3; }},
      {"unsupported preset schema", [](Preset &p) { p.schema_version = 0; }},
      {"more than 128",
       [](Preset &p) {
         p.next_instance_id = 200;
         p.nodes.clear();
         for (NodeId i = 0; i < 129; ++i) {
           p.nodes.push_back(gain_node(i, 1.0f));
         }
       }},
      {"repeats instance ID 3", [](Preset &p) { p.nodes[0].instance_id = 3; }},
      {"invalid instance ID",
       [](Preset &p) {
         p.next_instance_id = invalid_node_id;
         p.nodes[0].instance_id = invalid_node_id;
       }},
      {"not below next instance ID", [](Preset &p) { p.next_instance_id = 3; }},
      {"not below next instance ID", [](Preset &p) { p.next_instance_id = 0; }},
      {"invalid module ID", [](Preset &p) { p.nodes[0].module_id = "state"; }},
      {"invalid module ID", [](Preset &p) { p.nodes[0].module_id = "dev..state"; }},
      {"invalid module ID", [](Preset &p) { p.nodes[0].module_id = ""; }},
      {"invalid module ID", [](Preset &p) { p.nodes[0].module_id = std::string("dev.a\0b", 7); }},
      {"invalid module ID",
       [](Preset &p) { p.nodes[0].module_id = "dev." + std::string(252, 'a'); }},
      {"invalid module version", [](Preset &p) { p.nodes[0].module_version = ""; }},
      {"invalid module version",
       [](Preset &p) { p.nodes[0].module_version = std::string(256, '1'); }},
      {"invalid module version",
       [](Preset &p) { p.nodes[0].module_version = std::string("1\0", 2); }},
      {"module ABI version 0", [](Preset &p) { p.nodes[1].module_abi_version = 0; }},
      {"state schema version 0", [](Preset &p) { p.nodes[1].state.schema_version = 0; }},
      {"state exceeds 1048576",
       [](Preset &p) { p.nodes[0].state.bytes.resize(vespera::max_module_state_bytes + 1); }},
      {"preset state exceeds 16777216",
       [](Preset &p) {
         p.next_instance_id = 100;
         p.nodes.clear();
         for (NodeId i = 0; i < 17; ++i) {
           p.nodes.push_back(
               state_node(i, 2, std::vector<uint8_t>(vespera::max_module_state_bytes)));
         }
       }},
  };
  for (const auto &entry : cases) {
    const std::string message = entry.first;
    Preset preset = valid();
    entry.second(preset);
    CHECK(throws<std::invalid_argument>([&] { vespera::validate_preset(preset); }, message));
    CHECK(throws<std::invalid_argument>([&] { catalog.inspect(preset); }, message));
    CHECK(throws<std::invalid_argument>([&] { Graph::restore_preset(preset, catalog); }, message));
    CHECK(state->creates == 0);
  }

  Preset long_version = valid();
  long_version.nodes[0].module_version = std::string(255, '1');
  vespera::validate_preset(long_version);

  Preset future_abi = valid();
  future_abi.nodes[1].module_abi_version = 7;
  vespera::validate_preset(future_abi);
  const std::vector<PresetIssue> issues = catalog.inspect(future_abi);
  CHECK(issues.size() == 1 && issues[0].instance_id == 3 &&
        issues[0].message.find("module ABI version 7") != std::string::npos);
  CHECK(throws<std::runtime_error>([&] { catalog.resolve(future_abi.nodes[1]); }, "ABI version 7"));
  CHECK(throws<std::runtime_error>([&] { Graph::restore_preset(future_abi, catalog); },
                                   "ABI version 7"));
  CHECK(state->creates == 0);
}

void test_catalog(const Paths &paths) {
  current_test = "catalog";
  ModuleCatalog catalog;
  catalog.add(paths.gain);
  catalog.add(paths.gain);
  catalog.add(paths.gain.parent_path() / "." / paths.gain.filename());
  const fs::path link = paths.temp / "gain-link";
  fs::create_symlink(paths.gain, link);
  catalog.add(link);
  CHECK(catalog.resolve(gain_node(0, 1.0f)) == paths.gain);

  const fs::path impostor = paths.temp / ("gain-copy" + paths.gain.extension().string());
  fs::copy_file(paths.gain, impostor);
  CHECK(keep_resident(impostor) != nullptr);
  CHECK(throws<std::runtime_error>([&] { catalog.add(impostor); }, "is already provided by"));
  CHECK(catalog.resolve(gain_node(0, 1.0f)) == paths.gain);
  CHECK(throws<std::runtime_error>([&] { catalog.add(paths.temp / "missing.so"); }));

  catalog.add(paths.state);
  const fs::path newer = paths.temp / ("state-v2" + paths.state.extension().string());
  fs::copy_file(paths.state, newer);
  void *newer_library = keep_resident(newer);
  CHECK(newer_library != nullptr);
  if (newer_library == nullptr) {
    return;
  }
  auto *newer_control =
      reinterpret_cast<state_control_fn>(dlsym(newer_library, STATE_CONTROL_SYMBOL))();
  std::strcpy(newer_control->version, "2.0.0");
  catalog.add(newer);
  CHECK(newer_control->creates == 0);
  CHECK(state->creates == 0);

  CHECK(catalog.resolve(state_node(0, 2, {})) == paths.state);
  CHECK(catalog.resolve(state_node(0, 2, {}, "2.0.0")) == fs::canonical(newer));
  CHECK(throws<std::runtime_error>([&] { catalog.resolve(state_node(0, 2, {}, "1.5.0")); },
                                   "not in the module catalog"));
  CHECK(throws<std::runtime_error>([&] { catalog.resolve(state_node(0, 2, {}, "2.0")); },
                                   "not in the module catalog"));

  Preset preset;
  preset.next_instance_id = 4;
  preset.nodes = {state_node(0, 2, std::vector<uint8_t>(8), "2.0.0"),
                  state_node(1, 1, le_float(1.0f), "0.9.0"),
                  PresetNode{2, "dev.vespera.test.probe", "0.0.1", VS_MODULE_ABI_VERSION, {1, {}}},
                  PresetNode{3, "dev.vespera.other", "1", VS_MODULE_ABI_VERSION + 1, {1, {}}}};
  const std::vector<PresetIssue> issues = catalog.inspect(preset);
  CHECK(issues.size() == 3);
  if (issues.size() == 3) {
    CHECK(issues[0].instance_id == 1 &&
          issues[0].message.find("'0.9.0' is not in the module catalog") != std::string::npos);
    CHECK(issues[1].instance_id == 2 &&
          issues[1].message.find("not in the module catalog") != std::string::npos);
    CHECK(issues[2].instance_id == 3 &&
          issues[2].message.find("module ABI version " +
                                 std::to_string(VS_MODULE_ABI_VERSION + 1)) != std::string::npos);
  }

  Preset newer_only;
  newer_only.next_instance_id = 1;
  newer_only.nodes = {state_node(0, 2, concat(le_float(3.0f), le_float(0.5f)), "2.0.0")};
  std::unique_ptr<Graph> restored = Graph::restore_preset(newer_only, catalog);
  CHECK(restored->nodes()[0].module.version == "2.0.0");
  CHECK(newer_control->creates == 1 && state->creates == 0);
  restored->prepare(stereo());
  Buffers io(2, 16);
  io.fill(0.5f, -0.5f);
  CHECK(restored->process(io.block(16)) == VS_OK);
  CHECK(io.output_is(2.0f, -1.0f, 16));
  restored.reset();
  CHECK(newer_control->live_instances == 0);
  Preset mixed = newer_only;
  mixed.next_instance_id = 2;
  mixed.nodes.push_back(state_node(1, 1, le_float(2.0f)));
  CHECK(catalog.inspect(mixed).empty());
  restored = Graph::restore_preset(mixed, catalog);
  CHECK(restored->nodes()[1].module.version == "1.0.0");
  CHECK(restored->capture_preset().nodes.size() == 2);
  restored->prepare(stereo());
  CHECK(restored->process(io.block(16)) == VS_OK);
  CHECK(io.output_is(4.0f, -2.0f, 16));
}

// Settings saved by modules built for an older module ABI whose saved settings keep their meaning
// still restore, and are saved again under the current ABI.
void test_saved_module_abi(const Paths &paths) {
  current_test = "saved_module_abi";
  CHECK(!vespera::restores_saved_module_abi(0));
  CHECK(vespera::restores_saved_module_abi(VS_MODULE_SETTINGS_ABI_MINIMUM));
  CHECK(vespera::restores_saved_module_abi(VS_MODULE_ABI_VERSION));
  CHECK(!vespera::restores_saved_module_abi(VS_MODULE_ABI_VERSION + 1));
  CHECK(VS_MODULE_SETTINGS_ABI_MINIMUM == 1);

  const ModuleCatalog catalog = catalog_for(paths);
  Preset saved;
  saved.next_instance_id = 4;
  saved.nodes = {state_node(1, 2, concat(le_float(2.0f), le_float(0.25f))), gain_node(3, 3.0f)};
  for (PresetNode &node : saved.nodes) {
    node.module_abi_version = 1;
  }
  CHECK(catalog.inspect(saved).empty());
  CHECK(catalog.resolve(saved.nodes[1]) == paths.gain);
  std::unique_ptr<Graph> restored = Graph::restore_preset(saved, catalog);
  CHECK(ids(*restored) == std::vector<NodeId>({1, 3}));
  const Preset captured = restored->capture_preset();
  CHECK(captured.nodes.size() == 2);
  for (const PresetNode &node : captured.nodes) {
    CHECK(node.module_abi_version == VS_MODULE_ABI_VERSION);
  }
  CHECK(captured.nodes[0].state.bytes == saved.nodes[0].state.bytes);
  CHECK(captured.nodes[1].state.bytes == saved.nodes[1].state.bytes);
}

void test_replaced_binary(const Paths &paths) {
  current_test = "replaced_binary";
  const ModuleCatalog catalog = catalog_for(paths);
  std::strcpy(state->version, "1.0.1");
  Preset preset;
  preset.next_instance_id = 2;
  preset.nodes = {gain_node(0, 2.0f), state_node(1, 2, std::vector<uint8_t>(8))};
  CHECK(catalog.inspect(preset).empty());
  CHECK(throws<std::runtime_error>([&] { Graph::restore_preset(preset, catalog); },
                                   "provides 'dev.vespera.test.state' version '1.0.1'"));
  CHECK(state->creates == 1);
  CHECK(state->loads == 0);
  CHECK(state->live_instances == 0);

  state->version[0] = '\0';
  state->descriptor = STATE_DESCRIPTOR_DECLINE;
  CHECK(catalog.inspect(preset).empty());
  CHECK(throws<std::runtime_error>([&] { Graph::restore_preset(preset, catalog); },
                                   "at node 1 does not support state restore"));
  CHECK(state->loads == 0);
  CHECK(state->live_instances == 0);
}

void test_schema_migration(const Paths &paths) {
  current_test = "schema_migration";
  const ModuleCatalog catalog = catalog_for(paths);
  Preset legacy;
  legacy.next_instance_id = 6;
  legacy.nodes = {state_node(5, STATE_SCHEMA_LEGACY, le_float(-3.0f))};
  std::unique_ptr<Graph> restored = Graph::restore_preset(legacy, catalog);
  CHECK(state->last_load_schema == STATE_SCHEMA_LEGACY);
  CHECK(restored->nodes()[0].module.state_schema_version == STATE_SCHEMA_CURRENT);
  const Preset migrated = restored->capture_preset();
  CHECK(migrated.nodes[0].state.schema_version == STATE_SCHEMA_CURRENT);
  CHECK(migrated.nodes[0].state.bytes == concat(le_float(-3.0f), le_float(0.0f)));
  restored->prepare(stereo());
  CHECK(process_gain(*restored, 0.5f) == -1.5f);

  Preset future = legacy;
  future.nodes[0].state.schema_version = 3;
  CHECK(throws<std::runtime_error>([&] { Graph::restore_preset(future, catalog); },
                                   "does not support state schema version 3"));
  Preset mismatched = legacy;
  mismatched.nodes[0].state.schema_version = STATE_SCHEMA_CURRENT;
  CHECK(throws<std::runtime_error>([&] { Graph::restore_preset(mismatched, catalog); },
                                   "rejected its state (1)"));
  CHECK(state->loads_while_prepared == 0);
}

void test_restore_failures_keep_source(const Paths &paths) {
  current_test = "restore_failures_keep_source";
  const ModuleCatalog catalog = catalog_for(paths);
  Graph source;
  source.add_module(paths.gain);
  source.add_module(paths.state);
  source.prepare(stereo());
  Buffers io(2, 16);
  io.fill(0.5f, -0.5f);
  const Event change[] = {event(0, 1, 2.0f), event(1, STATE_PARAM_OFFSET, 0.25f)};
  CHECK(source.process(io.block(16), change) == VS_OK);
  CHECK(io.output_is(1.25f, -0.75f, 16));
  const uint64_t generation = source.generation();
  const Preset preset = source.capture_preset();

  state->fail_load = 1;
  CHECK(throws<std::runtime_error>([&] { Graph::restore_preset(preset, catalog); },
                                   "at node 1 rejected its state (2)"));
  CHECK(state->live_instances == 1);
  CHECK(source.state() == GraphState::prepared && source.generation() == generation);
  CHECK((ids(source) == std::vector<NodeId>{0, 1}));
  io.fill(0.5f, -0.5f);
  CHECK(source.process(io.block(16)) == VS_OK);
  CHECK(io.output_is(1.25f, -0.75f, 16));

  state->fail_load = 0;
  state->fail_prepare = 1;
  {
    std::unique_ptr<Graph> candidate = Graph::restore_preset(preset, catalog);
    CHECK(state->live_instances == 2);
    CHECK(throws<std::runtime_error>([&] { candidate->prepare(stereo()); }, "failed to prepare"));
    CHECK(candidate->state() == GraphState::editing);
  }
  CHECK(state->live_instances == 1);
  CHECK(source.state() == GraphState::prepared && source.generation() == generation);
  CHECK(source.process(io.block(16)) == VS_OK);
  CHECK(io.output_is(1.25f, -0.75f, 16));

  state->fail_prepare = 0;
  std::unique_ptr<Graph> candidate = Graph::restore_preset(preset, catalog);
  candidate->prepare(stereo());
  Buffers other(2, 16);
  other.fill(0.5f, -0.5f);
  CHECK(candidate->process(other.block(16)) == VS_OK);
  CHECK(other.output_is(1.25f, -0.75f, 16));
  CHECK(state->loads_while_prepared == 0);
}

void test_parameter_values(const Paths &paths) {
  current_test = "parameter_values";
  const ModuleCatalog catalog = catalog_for(paths);
  Preset preset;
  {
    Graph graph;
    const NodeId node = graph.add_module(paths.state);
    CHECK(graph.nodes()[0].module.parameter_values);
    CHECK(graph.parameter_value(node, STATE_PARAM_LEVEL) == 1.0f);
    CHECK(graph.parameter_value(node, STATE_PARAM_OFFSET) == 0.0f);
    graph.set_parameter_value(node, STATE_PARAM_LEVEL, -3.0f);
    graph.set_parameter_value(node, STATE_PARAM_OFFSET, 0.25f);
    CHECK(state->sets_while_prepared == 0);
    preset = graph.capture_preset();
    CHECK(preset.nodes[0].state.bytes == concat(le_float(-3.0f), le_float(0.25f)));

    graph.prepare(stereo());
    graph.set_parameter_value(node, STATE_PARAM_OFFSET, 0.5f);
    CHECK(state->sets_while_prepared == 1);
    Buffers io(2, 16);
    io.fill(0.5f, 0.25f);
    CHECK(graph.process(io.block(16)) == VS_OK);
    CHECK(io.output_is(-1.0f, -0.25f, 16));
    CHECK(graph.capture_preset().nodes[0].state.bytes == concat(le_float(-3.0f), le_float(0.5f)));

    state->get_mode = STATE_GET_ERROR;
    CHECK(throws<std::runtime_error>([&] { graph.parameter_value(node, STATE_PARAM_LEVEL); },
                                     "failed to report parameter 1 (2)"));
    state->get_mode = STATE_GET_UNWRITTEN;
    CHECK(throws<std::runtime_error>([&] { graph.parameter_value(node, STATE_PARAM_LEVEL); },
                                     "outside the bounds"));
    state->get_mode = STATE_GET_NAN;
    CHECK(throws<std::runtime_error>([&] { graph.parameter_value(node, STATE_PARAM_LEVEL); },
                                     "outside the bounds of parameter 1"));
    state->get_mode = STATE_GET_OUT_OF_RANGE;
    CHECK(throws<std::runtime_error>([&] { graph.parameter_value(node, STATE_PARAM_OFFSET); },
                                     "outside the bounds of parameter 2"));
    state->get_mode = STATE_GET_NORMAL;
    state->fail_set = 1;
    CHECK(throws<std::runtime_error>(
        [&] { graph.set_parameter_value(node, STATE_PARAM_OFFSET, 0.0f); },
        "rejected parameter 2 value (2)"));
    CHECK(graph.parameter_value(node, STATE_PARAM_OFFSET) == 0.5f);
    const uint32_t sets = state->sets;
    CHECK(throws<std::invalid_argument>(
        [&] { graph.set_parameter_value(node, STATE_PARAM_OFFSET, 1.5f); }, "within ["));
    CHECK(throws<std::invalid_argument>([&] { graph.parameter_value(node, 3); }, "no parameter 3"));
    CHECK(state->sets == sets);
    CHECK(graph.state() == GraphState::prepared);
  }
  state->fail_set = 0;

  std::unique_ptr<Graph> restored = Graph::restore_preset(preset, catalog);
  CHECK(restored->parameter_value(0, STATE_PARAM_LEVEL) == -3.0f);
  CHECK(restored->parameter_value(0, STATE_PARAM_OFFSET) == 0.25f);
  restored.reset();

  state->parameter_descriptor = STATE_PARAMETERS_DECLINE;
  {
    Graph graph;
    const NodeId node = graph.add_module(paths.state);
    CHECK(!graph.nodes()[0].module.parameter_values);
    CHECK(throws<std::runtime_error>([&] { graph.parameter_value(node, STATE_PARAM_LEVEL); },
                                     "does not support parameter value access"));
    CHECK(throws<std::runtime_error>(
        [&] { graph.set_parameter_value(node, STATE_PARAM_LEVEL, 1.0f); },
        "does not support parameter value access"));
    graph.prepare(stereo());
    CHECK(process_gain(graph, 0.5f) == 0.5f);
    Buffers io(2, 16);
    const Event change[] = {event(node, STATE_PARAM_LEVEL, 2.0f)};
    CHECK(graph.process(io.block(16), change) == VS_OK);
    const Preset saved = graph.capture_preset();
    CHECK(saved.nodes[0].state.bytes == concat(le_float(2.0f), le_float(0.0f)));
    std::unique_ptr<Graph> copy = Graph::restore_preset(saved, catalog);
    CHECK(!copy->nodes()[0].module.parameter_values);
    copy->prepare(stereo());
    CHECK(process_gain(*copy, 0.5f) == 1.0f);
  }

  const struct {
    int32_t descriptor;
    const char *message;
  } malformed[] = {
      {STATE_PARAMETERS_SMALL_STRUCT, "parameter value descriptor struct_size"},
      {STATE_PARAMETERS_WRONG_ABI, "parameter value ABI version 2"},
      {STATE_PARAMETERS_NULL_GET, "missing required parameter value functions"},
  };
  const uint32_t creates = state->creates;
  for (const auto &c : malformed) {
    state->parameter_descriptor = c.descriptor;
    Graph graph;
    CHECK(throws<std::runtime_error>([&] { graph.add_module(paths.state); }, c.message));
    ModuleCatalog rejecting;
    CHECK(throws<std::runtime_error>([&] { rejecting.add(paths.state); }, c.message));
  }
  CHECK(state->creates == creates);

  state->parameter_descriptor = STATE_PARAMETERS_EXTENDED_STRUCT;
  {
    Graph graph;
    const NodeId node = graph.add_module(paths.state);
    CHECK(graph.nodes()[0].module.parameter_values);
    graph.set_parameter_value(node, STATE_PARAM_LEVEL, 2.0f);
    CHECK(graph.parameter_value(node, STATE_PARAM_LEVEL) == 2.0f);
  }
}

void test_unicode_strings(const Paths &paths) {
  current_test = "unicode_strings";
  const ModuleCatalog catalog = catalog_for(paths);
  const auto with_version = [](const std::string &version) {
    Preset preset;
    preset.next_instance_id = 1;
    preset.nodes = {state_node(0, 2, std::vector<uint8_t>(8), version)};
    return preset;
  };
  const char *invalid[] = {"1.0-\x80",
                           "1.0-\xbf",
                           "1.0-\xc0\xaf",
                           "1.0-\xc1\xbf",
                           "1.0-\xe0\x80\xaf",
                           "1.0-\xf0\x80\x80\xaf",
                           "1.0-\xed\xa0\x80",
                           "1.0-\xed\xbf\xbf",
                           "1.0-\xf4\x90\x80\x80",
                           "1.0-\xf5\x80\x80\x80",
                           "1.0-\xf8\x88\x80\x80\x80",
                           "1.0-\xfe",
                           "1.0-\xff",
                           "1.0-\xe2\x82",
                           "1.0-\xe2\x28\xa1",
                           "1.0-\xf0\x9f\x8e",
                           "\xc3"};
  for (const char *version : invalid) {
    const Preset preset = with_version(version);
    CHECK(throws<std::invalid_argument>([&] { vespera::validate_preset(preset); },
                                        "invalid module version"));
    CHECK(
        throws<std::invalid_argument>([&] { catalog.inspect(preset); }, "invalid module version"));
    CHECK(throws<std::invalid_argument>([&] { Graph::restore_preset(preset, catalog); },
                                        "invalid module version"));
  }
  CHECK(state->creates == 0);
  for (const char *version : {"1.0.0-\xc3\xa9", "\xce\xb2", "\xe0\xa0\x80\xdf\xbf\xc2\x80",
                              "\xed\x9f\xbf\xee\x80\x80\xef\xbf\xbf", "\xf0\x90\x80\x80",
                              "\xf4\x8f\xbf\xbf", "1.0 \xf0\x9f\x8e\xb8"}) {
    vespera::validate_preset(with_version(version));
  }
  const Preset longest = with_version(std::string(253, 'x') + "\xc3\xa9");
  CHECK(longest.nodes[0].module_version.size() == 255);
  vespera::validate_preset(longest);
  CHECK(throws<std::invalid_argument>(
      [&] { vespera::validate_preset(with_version(std::string(254, 'x') + "\xc3\xa9")); },
      "invalid module version"));

  std::strcpy(state->version, "1.0\xff");
  Graph graph;
  CHECK(throws<std::runtime_error>([&] { graph.add_module(paths.state); }, "invalid version"));
  ModuleCatalog rejecting;
  CHECK(throws<std::runtime_error>([&] { rejecting.add(paths.state); }, "invalid version"));
  state->version[0] = '\0';

  const std::string precomposed = "1.0.0-\xc3\xa9";
  const std::string decomposed = "1.0.0-e\xcc\x81";
  std::strcpy(state->version, precomposed.c_str());
  ModuleCatalog unicode_catalog;
  unicode_catalog.add(paths.state);
  CHECK(unicode_catalog.resolve(state_node(0, 2, {}, precomposed)) == paths.state);
  CHECK(
      throws<std::runtime_error>([&] { unicode_catalog.resolve(state_node(0, 2, {}, decomposed)); },
                                 "not in the module catalog"));
  const Preset document = with_version(precomposed);
  CHECK(unicode_catalog.inspect(document).empty());
  std::unique_ptr<Graph> restored = Graph::restore_preset(document, unicode_catalog);
  CHECK(restored->nodes()[0].module.version == precomposed);
  CHECK(restored->capture_preset().nodes[0].module_version == precomposed);
  restored.reset();
  CHECK(throws<std::runtime_error>(
      [&] { Graph::restore_preset(with_version(decomposed), unicode_catalog); },
      "not in the module catalog"));
}

void *open_control(const fs::path &path, const char *symbol, void **control) {
  void *library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (library == nullptr) {
    std::fprintf(stderr, "cannot open %s: %s\n", path.c_str(), dlerror());
    return nullptr;
  }
  using fn = void *(*)();
  const auto entry = reinterpret_cast<fn>(dlsym(library, symbol));
  if (entry == nullptr) {
    std::fprintf(stderr, "%s lacks %s\n", path.c_str(), symbol);
    return nullptr;
  }
  *control = entry();
  return library;
}

}

int main(int argc, char **argv) {
  if (argc != 4) {
    std::fprintf(stderr, "usage: %s <gain-module> <state-fixture> <probe-fixture>\n",
                 argc > 0 ? argv[0] : "state_test");
    return 2;
  }
  Paths paths{fs::canonical(argv[1]), fs::canonical(argv[2]), fs::canonical(argv[3]), {}};
  paths.temp = fs::temp_directory_path() / ("vespera-state-test-" + std::to_string(getpid()));
  fs::remove_all(paths.temp);
  fs::create_directories(paths.temp);

  void *state_control_ptr = nullptr;
  void *probe_control_ptr = nullptr;
  if (open_control(paths.state, STATE_CONTROL_SYMBOL, &state_control_ptr) == nullptr ||
      open_control(paths.probe, PROBE_CONTROL_SYMBOL, &probe_control_ptr) == nullptr) {
    return 2;
  }
  state = static_cast<state_control *>(state_control_ptr);
  probe = static_cast<probe_control *>(probe_control_ptr);
  reset_controls();

  const std::function<void(const Paths &)> tests[] = {
      test_gain_roundtrip,
      test_load_module_state,
      test_gain_state_rejection,
      test_identity_preservation,
      test_blob_ownership,
      test_legacy_module,
      test_declined_state_entry,
      test_malformed_state_descriptors,
      test_capture_failures,
      test_validate_preset,
      test_catalog,
      test_saved_module_abi,
      test_replaced_binary,
      test_schema_migration,
      test_restore_failures_keep_source,
      test_parameter_values,
      test_unicode_strings,
  };
  for (const auto &test : tests) {
    try {
      test(paths);
    } catch (const std::exception &error) {
      std::fprintf(stderr, "[%s] unexpected exception: %s\n", current_test, error.what());
      ++failures;
    }
    reset_controls();
  }

  fs::remove_all(paths.temp);
  if (failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::puts("state tests passed");
  return 0;
}
