// Shared engine grouping with the deterministic fixture in fixtures/shared_module.cpp.
#include <vespera/host_services.hpp>
#include <vespera/instance_cache.hpp>
#include <vespera/runtime.hpp>

#include <array>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <map>
#include <source_location>
#include <stdexcept>
#include <string>
#include <vector>

#include "fixtures/shared_control.h"

namespace {
using vespera::Event;
using vespera::Graph;
using vespera::NodeId;

void check(bool condition, std::source_location location = std::source_location::current()) {
  if (!condition) {
    throw std::runtime_error("shared engine check failed at line " +
                             std::to_string(location.line()));
  }
}

std::map<std::string, std::string> paths;
std::map<std::string, SharedFixtureStats *> stats;

void load(const std::string &name, const char *path) {
  void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  check(handle != nullptr);
  auto entry = reinterpret_cast<SharedFixtureStats *(*)()>(dlsym(handle, "shared_fixture_stats"));
  check(entry != nullptr);
  paths[name] = path;
  stats[name] = entry();
}

void clear_stats() {
  for (auto &[name, s] : stats) {
    *s = SharedFixtureStats{};
  }
}

struct Built {
  std::unique_ptr<Graph> graph;
  std::vector<NodeId> nodes;
};

Built build(const std::vector<std::string> &names, bool sharing,
            std::shared_ptr<vespera::InstanceCache> cache = nullptr) {
  Built built{std::make_unique<Graph>(), {}};
  built.graph->set_sharing(sharing);
  built.graph->set_instance_cache(std::move(cache));
  for (const auto &name : names) {
    built.nodes.push_back(built.graph->add_module(paths.at(name)));
  }
  return built;
}

std::vector<size_t> sizes(const Graph &graph) {
  std::vector<size_t> result;
  for (const auto &group : graph.processing_groups()) {
    result.push_back(group.size());
  }
  return result;
}

std::vector<size_t> grouping(const std::vector<std::string> &names) {
  auto built = build(names, true);
  built.graph->prepare({48000, 64, 2});
  return sizes(*built.graph);
}

using Signal = std::array<std::vector<float>, 2>;

// Renders blocks of a ramp through the graph, applying events given per block as (node index,
// parameter, offset, value) where node index refers to the order the modules were added.
struct Change {
  uint32_t block;
  size_t node;
  uint32_t parameter;
  uint32_t offset;
  float value;
};

Signal render(Built &built, const std::vector<Change> &changes, uint32_t blocks = 6) {
  constexpr uint32_t frames = 64;
  Signal result;
  for (uint32_t b = 0; b < blocks; ++b) {
    std::array<std::array<float, frames>, 2> in{}, out{};
    for (uint32_t f = 0; f < frames; ++f) {
      in[0][f] = 0.01f * float(b * frames + f);
      in[1][f] = -0.02f * float(f);
    }
    const float *inputs[]{in[0].data(), in[1].data()};
    float *outputs[]{out[0].data(), out[1].data()};
    std::vector<Event> events;
    for (const auto &change : changes) {
      if (change.block == b) {
        Event event;
        event.node = built.nodes[change.node];
        event.parameter = {change.parameter, change.offset, change.value};
        events.push_back(event);
      }
    }
    vespera::Block block{inputs, outputs, 2, frames, uint64_t{b} * frames, 0, nullptr};
    check(built.graph->process(block, events) == VS_OK);
    for (size_t c = 0; c < 2; ++c) {
      result[c].insert(result[c].end(), out[c].begin(), out[c].end());
    }
  }
  return result;
}

void grouping_rules() {
  using V = std::vector<size_t>;
  check(grouping({"a", "b", "c"}) == V{3});
  check(grouping({"b", "a"}) == V{1, 1});
  check(grouping({"a", "a"}) == V{1, 1});
  check(grouping({"a", "other", "b"}) == V{1, 1, 1});
  check(grouping({"a", "b", "other", "a", "b"}) == V{2, 1, 2});
  check(grouping({"c", "d"}) == V{1, 1});
  check(grouping({"c", "e"}) == V{2});
  check(grouping({"a", "lead", "late"}) == V{1, 2});
  check(grouping({"lead", "a"}) == V{1, 1});
  check(grouping({"b", "twin"}) == V{1, 1});
  check(grouping({"a", "renamed"}) == V{1, 1});
  check(grouping({"a", "b", "c", "e", "late"}) == V{5});
  bool rejected = false;
  try {
    Graph graph;
    graph.add_module(paths.at("bad"));
  } catch (const std::runtime_error &) {
    rejected = true;
  }
  check(rejected);
}

void engine_state_and_latency() {
  clear_stats();
  auto grouped = build({"a", "b", "c"}, true);
  grouped.graph->prepare({48000, 64, 2});
  check(grouped.graph->latency_frames() == 7);
  check(stats["a"]->engine_prepares == 1 && stats["b"]->prepares == 0 &&
        stats["c"]->engine_prepares == 0);
  check(std::string(stats["a"]->last_signature) == "test.shared.a,test.shared.b,test.shared.c");
  check(stats["a"]->last_model == 1);
  check(stats["a"]->last_cache == nullptr);
  auto separate = build({"a", "b", "c"}, false);
  separate.graph->prepare({48000, 64, 2});
  check(separate.graph->latency_frames() == 21);
  check(sizes(*separate.graph) == std::vector<size_t>({1, 1, 1}));
  check(stats["a"]->prepares == 1 && stats["b"]->prepares == 1);
  check(grouped.graph->nodes().size() == 3);
}

void events_match_separate_processing() {
  const std::vector<Change> changes{{0, 1, 2, 5, 2.0f},  {1, 0, 3, 9, 0.25f}, {1, 2, 2, 9, 0.5f},
                                    {2, 1, 1, 13, 0.0f}, {3, 0, 1, 0, 0.0f},  {3, 2, 3, 63, 0.1f},
                                    {4, 1, 1, 30, 1.0f}, {5, 0, 1, 2, 1.0f}};
  clear_stats();
  auto grouped = build({"a", "b", "c"}, true);
  grouped.graph->set_parameter_value(grouped.nodes[0], 2, 1.5f);
  grouped.graph->set_parameter_value(grouped.nodes[2], 3, 0.2f);
  grouped.graph->prepare({48000, 64, 2});
  auto separate = build({"a", "b", "c"}, false);
  separate.graph->set_parameter_value(separate.nodes[0], 2, 1.5f);
  separate.graph->set_parameter_value(separate.nodes[2], 3, 0.2f);
  separate.graph->prepare({48000, 64, 2});
  const auto expected = render(separate, changes);
  const auto actual = render(grouped, changes);
  for (size_t c = 0; c < 2; ++c) {
    for (size_t i = 0; i < expected[c].size(); ++i) {
      if (expected[c][i] != actual[c][i]) {
        std::fprintf(stderr, "channel %zu sample %zu: separate %g grouped %g\n", c, i,
                     double(expected[c][i]), double(actual[c][i]));
        break;
      }
    }
  }
  check(expected == actual);
  check(stats["a"]->target_events == 5);
  double energy = 0;
  for (float sample : actual[0]) {
    energy += double(sample) * sample;
  }
  check(energy > 1);
  check(grouped.graph->parameter_value(grouped.nodes[1], 1) == 1.0f);
  check(grouped.graph->parameter_value(grouped.nodes[1], 2) == 2.0f);
  check(grouped.graph->parameter_value(grouped.nodes[2], 3) == 0.1f);

  grouped.graph->set_parameter_value(grouped.nodes[1], 2, 3.0f);
  separate.graph->set_parameter_value(separate.nodes[1], 2, 3.0f);
  check(stats["a"]->applies == 1);
  check(grouped.graph->parameter_value(grouped.nodes[1], 2) == 3.0f);
  check(render(grouped, {}, 2) == render(separate, {}, 2));

  const auto before = render(grouped, {}, 1);
  stats["a"]->fail_apply = 1;
  bool rejected = false;
  try {
    grouped.graph->set_parameter_value(grouped.nodes[1], 2, 0.5f);
  } catch (const std::runtime_error &error) {
    rejected = std::string(error.what()).find("rejected parameter 2") != std::string::npos;
  }
  check(rejected && stats["a"]->fail_apply == 0);
  check(grouped.graph->parameter_value(grouped.nodes[1], 2) == 3.0f);
  check(render(grouped, {}, 1) == before);
  stats["a"]->fail_apply = 2;
  rejected = false;
  try {
    grouped.graph->set_parameter_value(grouped.nodes[1], 2, 0.5f);
  } catch (const std::runtime_error &error) {
    rejected = std::string(error.what()).find("could not restore") != std::string::npos;
  }
  check(rejected && grouped.graph->parameter_value(grouped.nodes[1], 2) == 3.0f);

  grouped.graph->reset();
  check(stats["a"]->resets == 1 && stats["b"]->resets == 0);
  grouped.graph->release();
  check(grouped.graph->processing_groups().empty());
  check(grouped.graph->parameter_value(grouped.nodes[1], 2) == 3.0f);
  grouped.graph->move_module(grouped.nodes[0], 2);
  grouped.graph->prepare({48000, 64, 2});
  check(sizes(*grouped.graph) == std::vector<size_t>({2, 1}));
}

void midi_reaches_leading_module() {
  clear_stats();
  auto built = build({"lead", "late"}, true);
  built.graph->prepare({48000, 64, 1});
  check(sizes(*built.graph) == std::vector<size_t>({2}));
  std::array<float, 64> in{}, out{};
  const float *inputs[]{in.data()};
  float *outputs[]{out.data()};
  Event note;
  note.node = built.nodes[0];
  note.kind = Event::Kind::midi;
  note.midi = {3, 0x90, 60, 100, 0};
  Event late;
  late.node = built.nodes[1];
  late.parameter = {2, 10, 2.0f};
  const std::vector<Event> events{note, late};
  vespera::Block block{inputs, outputs, 1, 64, 0, 0, nullptr};
  check(built.graph->process(block, events) == VS_OK);
  check(stats["lead"]->midi_events == 1 && stats["lead"]->target_events == 1);
  Event wrong = note;
  wrong.node = built.nodes[1];
  check(built.graph->process(block, std::vector<Event>{wrong}) == VS_ERROR_ARGUMENT);
}

void host_tempo_reaches_engines() {
  clear_stats();
  auto plain = build({"a", "b"}, true);
  plain.graph->prepare({48000, 64, 2});
  check(sizes(*plain.graph) == std::vector<size_t>({2}));
  check((stats["a"]->last_flags & VS_SHARED_HOST_TEMPO) == 0);
  auto tempo = build({"a", "tempo"}, true);
  tempo.graph->prepare({48000, 64, 2});
  check(sizes(*tempo.graph) == std::vector<size_t>({2}));
  check((stats["a"]->last_flags & VS_SHARED_HOST_TEMPO) != 0);
  check(tempo.graph->nodes()[1].module.host_tempo && !tempo.graph->nodes()[0].module.host_tempo);
}

// The fixture takes a pooled engine instance from the cache in engine_prepare and returns it, or
// disposes it when the run retains audio, in release.
void cache_reuse_retirement_and_eviction() {
  clear_stats();
  const vespera::Format prepare_format{48000, 64, 2};
  auto cache = std::make_shared<vespera::InstanceCache>(1);
  {
    auto single = build({"a"}, false, cache);
    single.graph->prepare(prepare_format);
    check(stats["a"]->engine_prepares == 1 && stats["a"]->prepares == 0);
    check(stats["a"]->last_cache != nullptr && stats["a"]->pool_created == 1);
    single.graph->release();
    check(stats["a"]->live == 0 && cache->idle() == 1 && stats["a"]->disposed == 0);
    single.graph->prepare(prepare_format);
    check(stats["a"]->reused == 1 && stats["a"]->pool_created == 1 && cache->idle() == 0);
  }
  check(cache->idle() == 1 && stats["a"]->disposed == 0);

  {
    auto other = build({"b"}, false, cache);
    other.graph->prepare(prepare_format);
    check(stats["b"]->reused == 1 && stats["b"]->reused_foreign == 1 && cache->idle() == 0);
  }
  check(cache->idle() == 1);

  {
    auto retaining = build({"retain"}, false, cache);
    retaining.graph->prepare(prepare_format);
    check(stats["retain"]->reused == 1 && cache->idle() == 0);
  }
  check(cache->idle() == 0 && stats["retain"]->disposed == 1);

  {
    auto run = build({"a", "retain"}, true, cache);
    run.graph->prepare(prepare_format);
    check(sizes(*run.graph) == std::vector<size_t>({2}));
    check((stats["a"]->last_flags & VS_SHARED_RETAINS_AUDIO) != 0 && stats["a"]->pool_created == 2);
  }
  check(cache->idle() == 0 && stats["a"]->disposed == 1);

  {
    auto first = build({"a"}, false, cache);
    auto second = build({"a"}, false, cache);
    first.graph->prepare(prepare_format);
    second.graph->prepare(prepare_format);
    check(stats["a"]->pool_created == 4);
    first.graph->release();
    second.graph->release();
    check(cache->idle() == 1 && stats["a"]->disposed == 2);
    check(cache->statistics().evicted == 1);
  }
  cache->trim(0);
  check(cache->idle() == 0 && stats["a"]->disposed == 3);
  const auto counts = cache->statistics();
  check(counts.hits == 3 && counts.signature_hits == 1 && counts.misses == 4 &&
        counts.stored == 5 && counts.evicted == 2);
}

void cache_and_failures() {
  clear_stats();

  clear_stats();
  auto failing = build({"a", "b", "c", "other"}, true);
  stats["other"]->fail_prepare = true;
  bool threw = false;
  try {
    failing.graph->prepare({48000, 64, 2});
  } catch (const std::runtime_error &) {
    threw = true;
  }
  check(threw && failing.graph->state() == vespera::GraphState::editing);
  check(stats["a"]->live == 0 && stats["other"]->live == 0);
  stats["other"]->fail_prepare = false;
  stats["a"]->fail_prepare = true;
  threw = false;
  try {
    failing.graph->prepare({48000, 64, 2});
  } catch (const std::runtime_error &error) {
    threw = std::string(error.what()).find("shared engine for 3 modules") != std::string::npos;
  }
  check(threw && stats["a"]->live == 0 && stats["other"]->live == 0);
  stats["a"]->fail_prepare = false;
  failing.graph->prepare({48000, 64, 2});
  check(sizes(*failing.graph) == std::vector<size_t>({3, 1}));
  check(failing.graph->prepared_format().max_frames == 64);
}

// A run's leader may open work while preparing its engine; it adopts results while processing and
// closes the work in its release. Hosted members are never prepared, so they open none.
void leaders_use_host_services() {
  clear_stats();
  auto services = vespera::HostServices::create({vespera::HostWorkMode::manual, 2, true});
  Built built{std::make_unique<Graph>(), {}};
  built.graph->set_sharing(true);
  built.graph->set_host_services(services);
  for (const char *name : {"work", "a", "b"}) {
    built.nodes.push_back(built.graph->add_module(paths.at(name)));
  }
  built.graph->prepare({48000, 64, 2});
  check(sizes(*built.graph) == std::vector<size_t>({3}));
  check(stats["work"]->works_opened == 1);
  check(!built.graph->wait_for_work(std::chrono::steady_clock::now()));
  check(services->run_pending() == 1);
  check(built.graph->wait_for_work(std::chrono::steady_clock::now()));
  std::array<float, 64> input{}, left{}, right{};
  std::array<const float *, 2> inputs{input.data(), input.data()};
  std::array<float *, 2> outputs{left.data(), right.data()};
  check(built.graph->process({inputs.data(), outputs.data(), 2, 64, 0, 0, nullptr}) == VS_OK);
  check(stats["work"]->adopted == 1);
  built.graph->release();
  check(stats["work"]->works_closed == 1);
  const auto statistics = services->statistics();
  check(statistics.opened == 1 && statistics.closed == 1 && statistics.left_open == 0);
  check(statistics.produced == statistics.disposed && statistics.disposed == 1);
  check(statistics.open_outside_prepare == 0 && statistics.close_outside_release == 0);
}
}

int main(int argc, char **argv) {
  const std::vector<std::string> names{"a",    "b",      "c",     "d",     "e",
                                       "lead", "late",   "twin",  "other", "renamed",
                                       "bad",  "retain", "tempo", "work"};
  if (argc != static_cast<int>(names.size()) + 1) {
    std::fprintf(stderr, "usage: shared_test <fixtures...>\n");
    return 2;
  }
  try {
    for (size_t i = 0; i < names.size(); ++i) {
      if (names[i] == "bad") {
        paths["bad"] = argv[i + 1];
      } else {
        load(names[i], argv[i + 1]);
      }
    }
    grouping_rules();
    engine_state_and_latency();
    events_match_separate_processing();
    midi_reaches_leading_module();
    host_tempo_reaches_engines();
    cache_reuse_retirement_and_eviction();
    cache_and_failures();
    leaders_use_host_services();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
  return 0;
}
