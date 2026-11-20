// Graph integration of host services: attach order, descriptor validation, waiting for work,
// closing at release, notices and status, with the raw-ABI fixture in fixtures/services_module.
#include <vespera/host_services.hpp>
#include <vespera/runtime.hpp>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <functional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <thread>

#include "fixtures/services_control.h"

namespace {

namespace fs = std::filesystem;
using vespera::Graph;
using vespera::HostServices;
using vespera::HostWorkMode;
using Clock = std::chrono::steady_clock;

int failures = 0;
ServicesControl *control = nullptr;
fs::path fixture;
fs::path probe;

void check(bool condition, std::source_location location = std::source_location::current()) {
  if (!condition) {
    std::fprintf(stderr, "services runtime check failed at line %u\n",
                 static_cast<unsigned>(location.line()));
    ++failures;
  }
}

void expect_throws(const std::function<void()> &action, const std::string &text,
                   std::source_location location = std::source_location::current()) {
  try {
    action();
  } catch (const std::exception &error) {
    if (std::string(error.what()).find(text) == std::string::npos) {
      std::fprintf(stderr, "line %u: unexpected message: %s\n",
                   static_cast<unsigned>(location.line()), error.what());
      ++failures;
    }
    return;
  }
  std::fprintf(stderr, "line %u: expected an exception\n", static_cast<unsigned>(location.line()));
  ++failures;
}

std::shared_ptr<HostServices> make(HostWorkMode mode) {
  HostServices::Options options;
  options.mode = mode;
  return HostServices::create(options);
}

// Processes one block of silence and returns the first output sample.
float render(Graph &graph) {
  std::array<float, 16> input{}, left{}, right{};
  std::array<const float *, 2> inputs{input.data(), input.data()};
  std::array<float *, 2> outputs{left.data(), right.data()};
  if (graph.process({inputs.data(), outputs.data(), 2, 16, 0, 0, nullptr}) != VS_OK) {
    return -1.0f;
  }
  return left[0];
}

void test_attach_order() {
  control->restore();
  auto services = make(HostWorkMode::manual);
  {
    Graph graph;
    graph.set_host_services(services);
    const auto node = graph.add_module(fixture, {fs::path("/tmp/vespera-resources")});
    check(graph.nodes()[0].module.services);
    check(control->attached != nullptr &&
          std::string(control->attached->resource_directory) == "/tmp/vespera-resources");
    const float level = 0.25f;
    vespera::ModuleState state{1, std::vector<uint8_t>(sizeof level)};
    std::memcpy(state.bytes.data(), &level, sizeof level);
    graph.load_module_state(node, state);
    graph.set_parameter_value(node, 1, 0.75f);
    graph.prepare({48000, 16, 2});
    graph.release();
    expect_throws([&] { graph.set_host_services(make(HostWorkMode::manual)); },
                  "requires a graph without nodes");
    graph.set_host_services(services);
  }
  check(std::string(control->calls) == "casvprd");
  check(services->statistics().opened == 1 && services->statistics().closed == 1);

  control->restore();
  {
    Graph graph;
    graph.set_host_services(services);
    expect_throws([&] { graph.add_module(fixture, {fs::path("relative/resources")}); },
                  "absolute path");
    graph.add_module(fixture);
    check(control->attached->resource_directory == nullptr);
  }
}

void test_without_services() {
  control->restore();
  Graph graph;
  graph.add_module(fixture);
  graph.prepare({48000, 16, 2});
  check(render(graph) == 0.5f);
  check(std::string(control->calls) == "cp");
  check(graph.module_status(graph.nodes()[0].instance_id) == std::nullopt);
  check(graph.take_notices().empty());
  check(graph.wait_for_work(Clock::now()));
}

void test_descriptor_validation() {
  auto services = make(HostWorkMode::manual);
  auto load = [&] {
    Graph graph;
    graph.set_host_services(services);
    graph.add_module(fixture);
  };
  control->restore();
  control->api.struct_size = sizeof(vs_module_services_api) - 1;
  expect_throws(load, "services descriptor struct_size");
  control->restore();
  control->api.abi_version = 2;
  expect_throws(load, "services ABI version 2");
  control->restore();
  control->api.attach = nullptr;
  expect_throws(load, "missing required services functions");
  control->restore();
  control->api.status = nullptr;
  expect_throws(load, "missing required services functions");
  control->restore();
  control->api.struct_size = sizeof(vs_module_services_api) - 1;
  expect_throws(
      [&] {
        vespera::ModuleCatalog catalog;
        catalog.add(fixture);
      },
      "services descriptor");
  control->restore();
  control->attach_result = VS_ERROR_UNSUPPORTED;
  expect_throws(load, "rejected host services (3)");
  check(control->created == control->destroyed && control->created == 1);
  control->restore();
  control->decline = true;
  {
    Graph graph;
    graph.set_host_services(services);
    graph.add_module(fixture);
    check(!graph.nodes()[0].module.services && control->attached == nullptr);
  }
  control->restore();
}

void test_waiting_and_adoption() {
  control->restore();
  auto services = make(HostWorkMode::manual);
  Graph graph;
  graph.set_host_services(services);
  graph.add_module(fixture);
  graph.prepare({48000, 16, 2});
  check(render(graph) == 0.0f);
  check(!graph.wait_for_work(Clock::now()));
  check(services->run_pending() == 1);
  check(graph.wait_for_work(Clock::now()));
  check(render(graph) == 0.5f);
  graph.release();
  const auto stats = services->statistics();
  check(stats.produced == 1 && stats.disposed == 1 && stats.left_open == 0);
}

void test_inline_and_threaded() {
  control->restore();
  {
    auto services = make(HostWorkMode::synchronous);
    Graph graph;
    graph.set_host_services(services);
    graph.add_module(fixture);
    graph.prepare({48000, 16, 2});
    check(control->attached->flags == VS_HOST_WORK_INLINE);
    check(render(graph) == 0.5f);
  }
  control->restore();
  {
    auto services = make(HostWorkMode::threaded);
    Graph graph;
    graph.set_host_services(services);
    const auto node = graph.add_module(fixture);
    graph.set_parameter_value(node, 1, 0.125f);
    graph.prepare({48000, 16, 2});
    check(graph.wait_for_work(Clock::now() + std::chrono::seconds(5)));
    check(render(graph) == 0.125f);
    graph.release();
    check(services->statistics().produced == services->statistics().disposed);
  }
}

void test_leftover_work_is_closed() {
  control->restore();
  control->leave_open = true;
  auto services = make(HostWorkMode::synchronous);
  Graph graph;
  graph.set_host_services(services);
  graph.add_module(fixture);
  graph.prepare({48000, 16, 2});
  graph.release();
  const auto stats = services->statistics();
  check(stats.left_open == 1 && stats.closed == 1);
  check(stats.produced == 1 && stats.disposed == 1);
  control->restore();
}

void test_release_while_running() {
  control->restore();
  control->hold = true;
  auto services = make(HostWorkMode::threaded);
  {
    Graph graph;
    graph.set_host_services(services);
    graph.add_module(fixture);
    graph.prepare({48000, 16, 2});
    check(services->wait_running(Clock::now() + std::chrono::seconds(5)));
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (control->held == 0 && Clock::now() < deadline) {
      std::this_thread::yield();
    }
    check(control->held == 1);
    graph.release();
    check(control->runs == 1);
    const auto stats = services->statistics();
    check(stats.cancelled_runs == 1 && stats.produced == stats.disposed);
    check(stats.closed == 1);
  }
  check(control->runs == 1);
  control->restore();
}

void test_notices_and_status() {
  control->restore();
  auto services = make(HostWorkMode::manual);
  Graph graph;
  graph.set_host_services(services);
  const auto node = graph.add_module(fixture);
  const auto other = graph.add_module(probe);
  control->attached->notify(control->attached->context, VS_NOTIFY_STATUS | 64);
  auto notices = graph.take_notices();
  check(notices.size() == 1 && notices[0].node == node && notices[0].what == VS_NOTIFY_STATUS);
  check(graph.take_notices().empty());
  control->attached->notify(control->attached->context, VS_NOTIFY_TIMING);
  check(graph.take_notices()[0].what == VS_NOTIFY_TIMING);

  check(graph.module_status(other) == std::nullopt);
  auto status = graph.module_status(node);
  check(status && status->kind == VS_STATUS_READY && status->message.empty() &&
        status->node == node);
  control->status_kind = VS_STATUS_BUSY;
  std::strcpy(control->status_message, "loading \xe2\x80\x94 table");
  status = graph.module_status(node);
  check(status && status->kind == VS_STATUS_BUSY &&
        status->message == "loading \xe2\x80\x94 table" && status->progress < 0.0f);
  control->status_kind = 7;
  status = graph.module_status(node);
  check(status && status->kind == VS_STATUS_ERROR &&
        status->message.find("unknown status kind") != std::string::npos);
  control->status_kind = VS_STATUS_WARNING;
  std::strcpy(control->status_message, "bad \xff");
  check(graph.module_status(node)->message.find("malformed") != std::string::npos);
  std::strcpy(control->status_message, "fine");
  control->terminate_message = false;
  check(graph.module_status(node)->message.find("malformed") != std::string::npos);
  control->terminate_message = true;
  control->status_result = VS_ERROR_PROCESS;
  check(graph.module_status(node)->kind == VS_STATUS_ERROR);
  control->status_result = VS_OK;
  check(graph.module_status(node)->message == "fine");
  expect_throws([&] { (void)graph.module_status(9999); }, "no node");
  control->restore();
}

void test_restored_presets_attach() {
  control->restore();
  vespera::Preset preset;
  {
    Graph graph;
    graph.add_module(fixture);
    preset = graph.capture_preset();
  }
  vespera::ModuleCatalog catalog;
  catalog.add(fixture);
  control->restore();
  auto services = make(HostWorkMode::manual);
  auto restored = Graph::restore_preset(preset, catalog, services);
  check(std::string(control->calls) == "cas");
  check(restored->nodes()[0].module.services);
  control->restore();
}

}

int main(int argc, char **argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s <services-fixture> <probe-fixture>\n", argv[0]);
    return 2;
  }
  fixture = fs::canonical(argv[1]);
  probe = fs::canonical(argv[2]);
  void *library = dlopen(fixture.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!library) {
    std::fprintf(stderr, "cannot open the fixture\n");
    return 1;
  }
  control = reinterpret_cast<services_control_fn>(dlsym(library, SERVICES_CONTROL_SYMBOL))();
  const std::function<void()> tests[] = {
      test_attach_order,          test_without_services,    test_descriptor_validation,
      test_waiting_and_adoption,  test_inline_and_threaded, test_leftover_work_is_closed,
      test_release_while_running, test_notices_and_status,  test_restored_presets_attach};
  for (const auto &test : tests) {
    try {
      test();
    } catch (const std::exception &error) {
      std::fprintf(stderr, "unexpected exception: %s\n", error.what());
      ++failures;
    }
  }
  if (failures != 0) {
    std::fprintf(stderr, "%d services runtime check(s) failed\n", failures);
    return 1;
  }
  std::puts("services runtime tests passed");
  return 0;
}
