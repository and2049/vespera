// The audio bridge with modules that use host services: resources reach modules, a swap waits
// for the candidate's work while the old graph plays, gives up after its deadline, the status op
// reports module status by slot, and offline renders run work inline.
#include "fake_audio_engine.h"
#include "fixtures/services_control.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <nlohmann/json.hpp>
#include <source_location>
#include <stdexcept>
#include <string>
#include <thread>
#include <vespera/app_audio.h>

namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

void check(bool condition, std::source_location location = std::source_location::current()) {
  if (!condition) {
    throw std::runtime_error("audio bridge services check failed at line " +
                             std::to_string(location.line()));
  }
}

// Waits without sleeping until the condition holds or five seconds pass.
template <class Condition> bool eventually(Condition condition) {
  const auto deadline = Clock::now() + std::chrono::seconds(5);
  while (!condition()) {
    if (Clock::now() > deadline) {
      return false;
    }
    std::this_thread::yield();
  }
  return true;
}

}

int main(int argc, char **argv) {
  if (argc != 2) {
    return 2;
  }
  try {
    void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    check(library != nullptr);
    ServicesControl &control =
        *reinterpret_cast<services_control_fn>(dlsym(library, SERVICES_CONTROL_SYMBOL))();
    fake_ae::reset();
    const auto *api = vespera_app_audio_entry(1);
    void *instance = api->create();
    auto send = [&](const Json &request) {
      const Json reply = Json::parse(api->request(instance, request.dump().c_str()));
      if (!reply.at("ok").get<bool>()) {
        throw std::runtime_error("request failed: " + reply.dump());
      }
      return reply.at("value");
    };
    auto amp = [&](float level) {
      return Json{{"binary", argv[1]},
                  {"resources", "/tmp/vespera-resources"},
                  {"parameters", {{{"id", 1}, {"value", level}}}}};
    };
    auto settled = [&] {
      check(eventually([&] {
        const auto current = send({{"op", "status"}});
        const auto &candidate = current.at("candidate");
        check(candidate.at("state") != "failed");
        return candidate.at("state") == "ready" &&
               candidate.at("generation") == candidate.at("activeGeneration");
      }));
    };
    Json config{{"audio",
                 {{"backend", "fake"},
                  {"inputDevice", "input-device"},
                  {"outputDevice", "output-device"},
                  {"inputChannels", {0}},
                  {"outputChannels", {0, 1}},
                  {"sampleRate", 48000},
                  {"bufferSize", 128},
                  {"monitor", true}}},
                {"globals", Json::object()},
                {"amp", amp(0.5f)},
                {"preFX", Json::array({Json{{"slot", "pre-fx.7"},
                                            {"binary", argv[1]},
                                            {"parameters", {{{"id", 1}, {"value", 0.5f}}}}}})}};
    std::array<float, 128> input{}, left{}, right{};
    auto output = [&] {
      check(fake_ae::invoke(input.data(), left.data(), right.data(), 128, 0) == AE_PROCESS_OK);
      return left[127];
    };

    // Starting waits for the work the modules requested while preparing.
    auto status = send({{"op", "start"}, {"configuration", config}});
    settled();
    check(status.at("modules").empty());
    check(control.attached &&
          std::string(control.attached->resource_directory) == "/tmp/vespera-resources");
    check(output() == 0.5f);

    // Status by slot, refreshed when a module notifies a change. The amp was attached last.
    control.status_kind = VS_STATUS_BUSY;
    std::strcpy(control.status_message, "loading");
    check(send({{"op", "status"}}).at("modules").empty());
    settled();
    control.attached->notify(control.attached->context, VS_NOTIFY_STATUS);
    status = send({{"op", "status"}});
    check(status.at("modules").size() == 1);
    check(status["modules"][0]["slot"] == "amp" && status["modules"][0]["status"] == "busy" &&
          status["modules"][0]["message"] == "loading" &&
          !status["modules"][0].contains("progress"));
    control.status_kind = VS_STATUS_READY;
    control.status_message[0] = '\0';
    control.attached->notify(control.attached->context, VS_NOTIFY_STATUS);
    check(send({{"op", "status"}}).at("modules").empty());

    // A swap waits for the candidate's work while the old graph keeps playing.
    control.hold = true;
    control.held = 0;
    std::atomic<float> during{-1.0f};
    std::thread audio([&] {
      if (eventually([&] { return control.held > 0; })) {
        for (int block = 0; block < 3; ++block) {
          during = output();
        }
      }
      control.hold = false;
    });
    status = send({{"op", "amp"}, {"amp", amp(0.25f)}});
    audio.join();
    check(during == 0.5f);
    settled();
    check(output() == 0.25f);

    // Pending work cannot displace working audio; newer requests supersede it.
    control.hold = true;
    send({{"op", "amp"}, {"amp", amp(0.75f)}});
    check(eventually([&] { return control.held > 0; }));
    check(output() == 0.25f);
    send({{"op", "amp"}, {"amp", amp(0.8f)}});
    send({{"op", "parameter"}, {"id", 1}, {"value", 0.75f}});
    check(output() == 0.25f);
    control.hold = false;
    settled();
    check(output() == 0.75f);

    {
      std::lock_guard lock(control.prepare_mutex);
      control.hold_prepare = true;
      control.prepare_entered = false;
    }
    send({{"op", "amp"}, {"amp", amp(0.9f)}});
    {
      std::unique_lock lock(control.prepare_mutex);
      check(control.prepare_changed.wait_until(lock, Clock::now() + std::chrono::seconds(5),
                                               [&] { return control.prepare_entered; }));
    }
    check(output() == 0.75f);
    send({{"op", "cancelCandidate"}});
    {
      std::lock_guard lock(control.prepare_mutex);
      control.hold_prepare = false;
      control.prepare_changed.notify_all();
    }
    check(output() == 0.75f);
    send({{"op", "parameter"}, {"id", 1}, {"value", 0.8f}});
    settled();
    check(output() == 0.8f);
    send({{"op", "amp"}, {"amp", amp(0.75f)}});
    settled();

    control.status_kind = VS_STATUS_ERROR;
    std::strcpy(control.status_message, "file load failed");
    send({{"op", "amp"}, {"amp", amp(0.9f)}});
    check(eventually([&] { return send({{"op", "status"}})["candidate"]["state"] == "failed"; }));
    check(output() == 0.75f);
    control.status_kind = VS_STATUS_READY;
    control.status_message[0] = '\0';
    send({{"op", "amp"}, {"amp", amp(0.75f)}});
    settled();

    control.latency = 7;
    control.attached->notify(control.attached->context, VS_NOTIFY_TIMING);
    send({{"op", "status"}});
    settled();
    status = send({{"op", "status"}});
    check(status.at("latencyFrames") == 14);
    const auto generation = status["candidate"]["generation"];
    control.attached->notify(control.attached->context, VS_NOTIFY_TIMING);
    check(send({{"op", "status"}})["candidate"]["generation"] == generation);
    control.latency = 0;
    control.timing_on_prepare = true;
    const auto created = control.created.load();
    send({{"op", "amp"}, {"amp", amp(0.75f)}});
    settled();
    status = send({{"op", "status"}});
    check(status.at("latencyFrames") == 22);
    check(control.created == created + 4);
    control.timing_on_prepare = false;

    // Replacing the graph refreshes every module's status, keyed by its slot.
    control.status_kind = VS_STATUS_WARNING;
    std::strcpy(control.status_message, "remark");
    send({{"op", "preFX"}, {"modules", config["preFX"]}});
    settled();
    status = send({{"op", "status"}});
    check(status.at("modules").size() == 2 && status["modules"][0]["status"] == "warning");
    check((status["modules"][0]["slot"] == "amp" && status["modules"][1]["slot"] == "pre-fx.7") ||
          (status["modules"][1]["slot"] == "amp" && status["modules"][0]["slot"] == "pre-fx.7"));
    send({{"op", "preFX"}, {"modules", Json::array()}});
    settled();
    status = send({{"op", "status"}});
    check(status.at("modules").size() == 1 && status["modules"][0]["slot"] == "amp");
    control.status_kind = VS_STATUS_READY;
    control.status_message[0] = '\0';

    // Offline renders run module work inline: the first sample already has the requested level,
    // for any block size.
    setenv("VESPERA_ENABLE_RENDER_GRAPH", "1", 1);
    control.hold = false;
    Json reference;
    for (const uint32_t block : {1u, 7u, 64u}) {
      const auto rendered = send({{"op", "renderGraph"},
                                  {"configuration", {{"amp", amp(0.3f)}}},
                                  {"blockSize", block},
                                  {"input", {std::vector<float>(256, 0.0f)}}});
      check(rendered["output"][0][0] == 0.3f);
      if (reference.is_null()) {
        reference = rendered["output"];
      } else {
        check(rendered["output"] == reference);
      }
    }

    control.hold = true;
    const auto held = control.held.load();
    send({{"op", "amp"}, {"amp", amp(0.9f)}});
    check(eventually([&] { return control.held > held; }));
    check(send({{"op", "stop"}})["state"] == "stopped");
    control.hold = false;
    api->destroy(instance);
    check(control.created == control.destroyed);
    check(control.runs == control.disposed);
    dlclose(library);
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
  std::puts("audio bridge services tests passed");
  return 0;
}
