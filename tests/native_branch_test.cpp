#include <vespera/instance_cache.hpp>
#include <vespera/runtime.hpp>

#include "fixtures/native_branch_control.hpp"

#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
bool watch_allocations = false;
unsigned allocations = 0;
}

void *operator new(std::size_t size) {
  if (watch_allocations) {
    ++allocations;
  }
  if (void *p = std::malloc(size ? size : 1)) {
    return p;
  }
  throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }

namespace {
using namespace vespera;
using namespace native_fixture;

#define CHECK(...)                                                                                 \
  do {                                                                                             \
    if (!(__VA_ARGS__)) {                                                                          \
      throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #__VA_ARGS__);            \
    }                                                                                              \
  } while (false)

struct Fixture {
  const char *path;
  void *handle;
  Control *control;

  explicit Fixture(const char *file) : path(file), handle(dlopen(file, RTLD_NOW | RTLD_LOCAL)) {
    CHECK(handle);
    auto entry = reinterpret_cast<Control *(*)()>(dlsym(handle, "native_branch_fixture_control"));
    CHECK(entry);
    control = entry();
  }
  ~Fixture() { dlclose(handle); }
};

struct Fixtures {
  Fixture main, branch, post, extra, older;
  explicit Fixtures(char **paths)
      : main(paths[0]), branch(paths[1]), post(paths[2]), extra(paths[3]), older(paths[4]) {}
  void clear() {
    for (auto *fixture : {&main, &branch, &post, &extra, &older}) {
      fixture->control->reset();
    }
  }
};

using IDs = std::vector<NodeId>;
using Groups = std::vector<IDs>;
constexpr Format format{48000, 32, 1};

NodeId add_branch(Graph &graph, const Fixture &fixture, uint32_t mix_id = mix) {
  const NodeId id = graph.add_module(fixture.path);
  graph.set_branch(id, Branch{mix_id});
  return id;
}

IDs rig(Graph &graph, Fixtures &f, bool sharing = true) {
  graph.set_sharing(sharing);
  return {graph.add_module(f.main.path), add_branch(graph, f.branch),
          graph.add_module(f.post.path)};
}

template <class F> void rejects(F &&fn, std::string_view why) {
  try {
    fn();
  } catch (const std::exception &e) {
    CHECK(std::string_view(e.what()).find(why) != std::string_view::npos);
    return;
  }
  throw std::runtime_error("expected rejection: " + std::string(why));
}

struct Audio {
  std::array<float, 8> input{0.2f, -0.4f, 0.7f, 0.3f, -0.6f, -0.1f, 0.45f, 0.8f};
  std::array<float, 8> output{};
  const float *in = input.data();
  float *out = output.data();
  Block block{&in, &out, 1, static_cast<uint32_t>(input.size())};

  vs_result process(Graph &graph, std::span<const Event> events = {}) {
    const auto original = input;
    watch_allocations = true;
    const vs_result result = graph.process(block, events);
    watch_allocations = false;
    CHECK(input == original);
    CHECK(allocations == 0);
    return result;
  }
};

void expect_audio(const Audio &audio, std::array<float, 8> mix_values, bool main = true,
                  bool post = true) {
  float previous = 0;
  for (size_t i = 0; i < audio.input.size(); ++i) {
    const float x = audio.input[i];
    const float dry = main ? x * x + 0.125f : x;
    const float wet = 2 * x - 0.25f;
    const float merged = dry + mix_values[i] * (wet - dry);
    CHECK(std::fabs(audio.output[i] - (post ? merged + previous * 0.5f : merged)) < 2e-7f);
    previous = merged;
  }
}

auto fixed_mix(float value) {
  std::array<float, 8> m;
  m.fill(value);
  return m;
}

void native_audio(Fixtures &f) {
  f.clear();
  Graph graph;
  auto cache = std::make_shared<InstanceCache>();
  graph.set_instance_cache(cache);
  const IDs ids = rig(graph, f);
  const NodeId synth = ids[1];
  graph.set_parameter_value(synth, mix, 0.125f);
  graph.prepare(format);
  CHECK(graph.processing_groups() == Groups{ids});
  CHECK(graph.latency_frames() == 17 && graph.tail_frames() == 29);
  CHECK(f.main.control->engine_prepares == 1 && f.branch.control->native_assignments == 1);
  CHECK(f.branch.control->engine_prepares == 0 && f.branch.control->standalone_prepares == 0 &&
        f.post.control->engine_prepares == 0 && f.post.control->standalone_prepares == 0);
  CHECK(std::string_view(f.main.control->signature) ==
        "native-branch/v1/index=1/fence=90:test.native.main,test.native.branch,test.native.post");
  CHECK(f.main.control->cache_received == 0);
  const auto generation = graph.generation();
  Audio audio;
  const Event events[] = {{synth, {mix, 2, 0.375f}},
                          {synth, {mix, 2, 0.625f}},
                          {synth, {power, 4, 0}},
                          {synth, {power, 6, 1}},
                          {synth, {mix, 6, 0.875f}}};
  CHECK(audio.process(graph, events) == VS_OK);
  expect_audio(audio, {0.13f, 0.13f, 0.63f, 0.63f, 0, 0, 0.88f, 0.88f});
  CHECK(f.main.control->processes == 1 && f.branch.control->processes == 0 &&
        f.post.control->processes == 0 && f.branch.control->native_translations == 5);
  CHECK(graph.generation() == generation && graph.parameter_value(synth, mix) == 0.88f);

  graph.reset();
  const Event post_bypass{ids[2], {power, 0, 0}};
  CHECK(audio.process(graph, {&post_bypass, 1}) == VS_OK);
  expect_audio(audio, fixed_mix(0.88f), true, false);
  CHECK(f.branch.control->native_translations == 5);
  graph.set_parameter_value(ids[2], power, 1);

  graph.set_parameter_value(synth, mix, 0.375f);
  graph.set_parameter_value(synth, selector, 0.6f);
  graph.reset();
  CHECK(audio.process(graph) == VS_OK);
  expect_audio(audio, fixed_mix(0.38f));
  CHECK(graph.parameter_value(synth, selector) == 1);
  rejects([&] { graph.set_parameter_value(synth, selector, 0); }, "rejected parameter");
  CHECK(graph.parameter_value(synth, selector) == 1);
  f.main.control->fail_apply_once = true;
  rejects([&] { graph.set_parameter_value(synth, mix, 0.875f); }, "rejected parameter");
  CHECK(graph.parameter_value(synth, mix) == 0.38f);
  graph.reset();
  CHECK(audio.process(graph) == VS_OK);
  expect_audio(audio, fixed_mix(0.38f));

  const auto preset = graph.capture_preset();
  graph.release();
  CHECK(cache->idle() == 0 && f.main.control->releases == 1);
  CHECK(graph.parameter_value(synth, mix) == 0.38f);
  graph.set_sharing(false);
  graph.prepare(format);
  CHECK(graph.processing_groups() == Groups{{ids[0]}, {ids[1]}, {ids[2]}});
  CHECK(audio.process(graph) == VS_OK);
  expect_audio(audio, fixed_mix(0.38f));
  CHECK(f.branch.control->cache_received == 1);

  ModuleCatalog catalog;
  for (const auto *module : {&f.main, &f.branch, &f.post}) {
    catalog.add(module->path);
  }
  auto restored = Graph::restore_preset(preset, catalog);
  CHECK(restored->branch_nodes().empty());
  restored->set_branch(synth, Branch{mix});
  restored->set_sharing(true);
  restored->prepare(format);
  CHECK(restored->processing_groups() == Groups{ids});
  CHECK(audio.process(*restored) == VS_OK);
  expect_audio(audio, fixed_mix(0.38f));
}

void eligibility(Fixtures &f) {
  struct Case {
    const char *name;
    void (*configure)(Fixtures &);
    std::array<int, 4> order; // main, branch, post, extra, older; -1 terminates
    bool native = false;
    bool sharing = true;
    uint32_t mix_id = mix;
    float route = 1;
  };
  constexpr auto unchanged = +[](Fixtures &) {};
  const Case cases[] = {
      {"sharing off", unchanged, {0, 1, 2, -1}, false, false},
      {"solo", unchanged, {1, -1, -1, -1}},
      {"missing prefix", unchanged, {1, 2, -1, -1}},
      {"missing suffix is permitted", unchanged, {0, 1, -1, -1}, true},
      {"multiple branches", unchanged, {0, 1, 1, 2}},
      {"no export", unchanged, {0, 4, 2, -1}},
      {"unsupported extension",
       [](Fixtures &x) { x.branch.control->descriptor = Fault::absent; },
       {0, 1, 2, -1}},
      {"static branch",
       [](Fixtures &x) { x.branch.control->static_branch = true; },
       {0, 1, 2, -1},
       true,
       true,
       mix,
       0},
      {"wrong mix", unchanged, {0, 1, 2, -1}, false, true, alternate_mix},
      {"wrong selector", unchanged, {0, 1, 2, -1}, false, true, mix, 0},
      {"foreign prefix", [](Fixtures &x) { x.extra.control->no_shared = true; }, {3, 0, 1, 2}},
      {"foreign suffix", [](Fixtures &x) { x.extra.control->no_shared = true; }, {0, 1, 2, 3}},
      {"different identity",
       [](Fixtures &x) { x.post.control->foreign_engine = true; },
       {0, 1, 2, -1}},
      {"before fence", [](Fixtures &x) { x.main.control->stage = 90; }, {0, 1, -1, -1}},
      {"after fence", [](Fixtures &x) { x.post.control->stage = 89; }, {0, 1, 2, -1}},
      {"at fence valid", unchanged, {0, 1, 2, -1}, true},
      {"duplicate branch stage", [](Fixtures &x) { x.main.control->stage = 10; }, {0, 1, 2, -1}},
      {"duplicate serial stage", unchanged, {0, 0, 1, 2}},
      {"decreasing serial prefix", [](Fixtures &x) { x.extra.control->stage = 50; }, {0, 3, 1, 2}},
      {"decreasing serial suffix", [](Fixtures &x) { x.post.control->stage = 91; }, {0, 1, 2, 3}},
      {"owned conflict",
       [](Fixtures &x) { x.post.control->conflict_role = VS_SHARED_TARGET_OWNED; },
       {0, 1, 2, -1}},
      {"owned-required conflict",
       [](Fixtures &x) { x.post.control->conflict_role = VS_SHARED_TARGET_REQUIRED; },
       {0, 1, 2, -1}},
      {"required disagreement",
       [](Fixtures &x) { x.post.control->required_value = 1; },
       {0, 1, 2, -1}},
      {"branch MIDI", [](Fixtures &x) { x.branch.control->midi = true; }, {0, 1, 2, -1}},
      {"suffix MIDI", [](Fixtures &x) { x.post.control->midi = true; }, {0, 1, 2, -1}},
      {"MIDI leader allowed",
       [](Fixtures &x) { x.main.control->midi = true; },
       {0, 1, 2, -1},
       true},
      {"branch leading-only", [](Fixtures &x) { x.branch.control->leading = true; }, {0, 1, 2, -1}},
      {"suffix leading-only", [](Fixtures &x) { x.post.control->leading = true; }, {0, 1, 2, -1}},
      {"leading-only leader allowed",
       [](Fixtures &x) { x.main.control->leading = true; },
       {0, 1, 2, -1},
       true},
  };
  for (const auto &test : cases) {
    try {
      f.clear();
      test.configure(f);
      Graph graph;
      graph.set_sharing(test.sharing);
      Fixture *modules[] = {&f.main, &f.branch, &f.post, &f.extra, &f.older};
      IDs order;
      IDs branches;
      for (const int index : test.order) {
        if (index < 0) {
          break;
        }
        const bool branch = index == 1 || index == 4;
        const NodeId id = branch ? add_branch(graph, *modules[index], test.mix_id)
                                 : graph.add_module(modules[index]->path);
        order.push_back(id);
        graph.set_parameter_value(id, power, 0); // Retained off nodes still constrain eligibility.
        if (branch) {
          branches.push_back(id);
          graph.set_parameter_value(id, selector, test.route);
        }
      }
      graph.prepare(format);
      if (test.native || order.size() == 1) {
        CHECK(graph.processing_groups() == Groups{order});
      } else {
        CHECK(graph.processing_groups() != Groups{order});
      }
      CHECK(f.branch.control->native_assignments == (test.native ? 1u : 0u));
      CHECK(graph.branch_nodes() == branches);
      if (!test.native) {
        const auto groups = graph.processing_groups();
        for (const auto id : branches) {
          CHECK(std::find(groups.begin(), groups.end(), IDs{id}) != groups.end());
        }
      }
      Audio audio;
      CHECK(audio.process(graph) == VS_OK);
      CHECK(audio.output == audio.input);
    } catch (const std::exception &e) {
      throw std::runtime_error(std::string(test.name) + ": " + e.what());
    }
  }
}

void failures(Fixtures &f) {
  for (const auto fault :
       {Fault::short_descriptor, Fault::bad_version, Fault::bad_callback, Fault::bad_law,
        Fault::bad_flags, Fault::bad_fence, Fault::bad_mix, Fault::bad_selector}) {
    f.clear();
    f.branch.control->descriptor = fault;
    Graph graph;
    rejects([&] { graph.add_module(f.branch.path); }, "shared branch descriptor");
    CHECK(graph.node_count() == 0);
  }
  f.clear();
  f.branch.control->no_shared = true;
  Graph absent_base;
  rejects([&] { absent_base.add_module(f.branch.path); }, "shared branch descriptor");

  for (const auto fault : {Fault::foreign_target, Fault::bad_value, Fault::duplicate,
                           Fault::missing, Fault::overflow, Fault::none}) {
    f.clear();
    Graph graph;
    rig(graph, f);
    f.branch.control->assignments = fault;
    f.main.control->fail_prepare = fault == Fault::none;
    rejects([&] { graph.prepare(format); }, "failed to prepare");
    CHECK(graph.state() == GraphState::editing && graph.processing_groups().empty());
    CHECK(f.branch.control->native_assignments == 1 && f.branch.control->standalone_prepares == 0);
    CHECK(f.main.control->engine_prepares == (fault == Fault::none ? 1u : 0u));
    f.branch.control->assignments = Fault::none;
    f.main.control->fail_prepare = false;
    graph.prepare(format);
    CHECK(graph.processing_groups().size() == 1);
  }

  for (const auto fault :
       {Fault::foreign_target, Fault::bad_value, Fault::duplicate, Fault::overflow}) {
    f.clear();
    Graph graph;
    const auto ids = rig(graph, f);
    graph.prepare(format);
    f.branch.control->translation = fault;
    Audio audio;
    const Event event{ids[1], {mix, 3, 0.875f}};
    CHECK(audio.process(graph, {&event, 1}) == VS_ERROR_PROCESS);
    CHECK(graph.state() == GraphState::failed);
    CHECK(audio.output == fixed_mix(0));
    CHECK(f.main.control->processes == 0);
  }
}

void failed_candidate_preserves_previous(Fixtures &f) {
  f.clear();
  Graph active;
  const auto ids = rig(active, f);
  active.prepare(format);
  const auto generation = active.generation();
  f.main.control->fail_prepare = true;
  Graph candidate;
  rig(candidate, f);
  rejects([&] { candidate.prepare(format); }, "failed to prepare");
  CHECK(candidate.state() == GraphState::editing);
  CHECK(active.processing_groups() == Groups{ids} && active.generation() == generation);
  Audio audio;
  CHECK(audio.process(active) == VS_OK);
  expect_audio(audio, fixed_mix(0.5f));
}
}

int main(int argc, char **argv) {
  if (argc != 6) {
    return 2;
  }
  try {
    Fixtures fixtures(argv + 1);
    native_audio(fixtures);
    eligibility(fixtures);
    failures(fixtures);
    failed_candidate_preserves_previous(fixtures);
  } catch (const std::exception &error) {
    std::fprintf(stderr, "native branch: %s\n", error.what());
    return 1;
  }
  return 0;
}
