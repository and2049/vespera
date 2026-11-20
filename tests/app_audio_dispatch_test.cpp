#include "fake_audio_engine.h"
#include "fixtures/bridge_dispatch_control.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <source_location>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vespera/app_audio.h>

namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

void check(bool condition, std::source_location at = std::source_location::current()) {
  if (!condition) {
    throw std::runtime_error("bridge dispatch check failed at line " + std::to_string(at.line()));
  }
}

void pump() { CFRunLoopRunInMode(kCFRunLoopDefaultMode, .010, false); }

template <class Predicate>
void eventually(Predicate predicate, bool service_main = true,
                std::source_location at = std::source_location::current()) {
  const auto deadline = Clock::now() + 5s;
  while (!predicate()) {
    check(Clock::now() < deadline, at);
    if (service_main) {
      pump();
    } else {
      std::this_thread::yield();
    }
  }
}

struct NestedRequest {
  const vs_app_audio_api *api;
  void *instance;
  bool called = false;
  bool refused = false;
};

void request_stop(void *context) {
  auto &probe = *static_cast<NestedRequest *>(context);
  probe.called = true;
  const auto reply =
      Json::parse(probe.api->request(probe.instance, R"({"op":"stop"})"), nullptr, false);
  probe.refused = reply == Json{{"ok", false}, {"error", "Audio backend is busy"}};
}
}

int main(int argc, char **argv) {
  if (argc != 2) {
    return 2;
  }
  try {
    const auto *api = vespera_app_audio_entry(1);
    check(api != nullptr);
    const std::unique_ptr<void, decltype(&dlclose)> library(dlopen(argv[1], RTLD_NOW | RTLD_LOCAL),
                                                            dlclose);
    check(library != nullptr);
    const auto control_fn = reinterpret_cast<BridgeDispatchControlFn>(
        dlsym(library.get(), "vespera_bridge_dispatch_control"));
    check(control_fn != nullptr);
    auto &control = *control_fn();
    fake_ae::reset();
    const Json amp{{"binary", argv[1]}, {"parameters", Json::array()}};
    const Json config{{"audio",
                       {{"backend", "fake"},
                        {"inputDevice", "input-device"},
                        {"outputDevice", "output-device"},
                        {"inputChannels", {0}},
                        {"outputChannels", {0, 1}},
                        {"sampleRate", 48000},
                        {"bufferSize", 128},
                        {"monitor", true}}},
                      {"globals", Json::object()},
                      {"amp", amp}};
    const Json start{{"op", "start"}, {"configuration", config}};
    auto send = [&](void *instance, const Json &request) {
      auto reply = Json::parse(api->request(instance, request.dump().c_str()));
      if (!reply.at("ok").get<bool>()) {
        throw std::runtime_error(reply.at("error").get<std::string>());
      }
      return reply.at("value");
    };

    NestedRequest shutdown_probe{api, nullptr}, render_probe{api, nullptr};
    std::unique_ptr<void, decltype(api->destroy)> bridge(api->create(), api->destroy);
    check(bridge != nullptr);
    shutdown_probe.instance = bridge.get();
    control.on_main = request_stop;
    control.context = &shutdown_probe;
    send(bridge.get(), start);
    eventually([&] { return control.entered == 1; }, false);
    check(control.called == 0);
    bridge.reset();
    check(control.called == 1 && control.timed_out == 0 && control.live == 0);
    check(shutdown_probe.called && shutdown_probe.refused);
    control.on_main = nullptr;
    control.context = nullptr;
    eventually([&] { return control.released == 1; });

    bridge.reset(api->create());
    send(bridge.get(), start);
    eventually([&] { return control.entered == 2; }, false);
    auto *off_main = bridge.release();
    std::thread destroyer([&] { api->destroy(off_main); });
    destroyer.join(); // Main does not service the queued call.
    check(control.timed_out == 1 && control.live == 0);
    pump();
    check(control.called == 1);

    bridge.reset(api->create());
    render_probe.instance = bridge.get();
    auto current = [&] { return send(bridge.get(), {{"op", "status"}}).at("candidate"); };
    auto settle = [&] {
      eventually([&] {
        const auto status = current();
        check(status.at("state") != "failed");
        return status.at("state") == "ready" &&
               status.at("activeGeneration") == status.at("generation");
      });
    };
    send(bridge.get(), start);
    settle();
    const auto playing = current().at("activeGeneration");
    std::array<float, 128> input{}, left{}, right{};
    input.fill(.25f);
    uint64_t frame = 0;
    auto output = [&] {
      check(fake_ae::invoke(input.data(), left.data(), right.data(), 128, frame) == AE_PROCESS_OK);
      frame += 128;
      return left.back();
    };
    check(output() == .5f);

    const Json change{{"op", "amp"}, {"amp", amp}};
    auto begin = [&] {
      const auto entered = control.entered.load();
      send(bridge.get(), change);
      eventually([&] { return control.entered > entered; }, false);
    };
    begin();
    send(bridge.get(), {{"op", "cancelCandidate"}, {"discard", true}});
    eventually([&] { return control.live == 1; });
    check(current().at("activeGeneration") == playing && output() == .5f);

    control.fail = true;
    begin();
    eventually([&] { return current().at("state") == "failed"; });
    check(!current().at("error").get<std::string>().empty());
    check(current().at("activeGeneration") == playing && output() == .5f);
    control.fail = false;

    begin();
    send(bridge.get(), {{"op", "amp"}, {"amp", nullptr}});
    settle();
    check(current().at("activeGeneration") != playing && output() == .25f);
    eventually([&] { return control.live == 0; });

    setenv("VESPERA_ENABLE_RENDER_GRAPH", "1", 1);
    CFRunLoopTimerContext timer_context{0, &render_probe, nullptr, nullptr, nullptr};
    const auto dispose_timer = [](CFRunLoopTimerRef timer) {
      CFRunLoopTimerInvalidate(timer);
      CFRelease(timer);
    };
    const std::unique_ptr<std::remove_pointer_t<CFRunLoopTimerRef>, decltype(dispose_timer)> timer(
        CFRunLoopTimerCreate(
            nullptr, CFAbsoluteTimeGetCurrent(), 0, 0, 0,
            [](CFRunLoopTimerRef, void *context) { request_stop(context); }, &timer_context),
        dispose_timer);
    check(timer != nullptr);
    CFRunLoopAddTimer(CFRunLoopGetMain(), timer.get(), kCFRunLoopDefaultMode);
    control.on_main = [](void *) {}; // Ask the fixture to service the main run loop.
    const auto reply = Json::parse(api->request(
        bridge.get(),
        Json{{"op", "renderGraph"}, {"configuration", {{"amp", amp}}}, {"input", {{.25f, .25f}}}}
            .dump()
            .c_str()));
    control.on_main = nullptr;
    CFRunLoopTimerInvalidate(timer.get());
    check(render_probe.called && render_probe.refused);
    check(reply.at("ok").get<bool>() && reply.at("value").at("output") == Json{{.5f, .5f}});
    check(send(bridge.get(), {{"op", "status"}}).at("state") == "running");
    bridge.reset();
    check(control.live == 0 && fake_ae::control().streams_alive == 0);
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
