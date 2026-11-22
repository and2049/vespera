// Hosted plugin instances released off the main thread must be disposed on it, after the work
// they queued there, by the SDK module itself, by a module library the host has closed, and by
// the bridge's reamp worker and graph loader before the bridge is destroyed.
#include "app_audio_client.hpp"
#include "fake_audio_engine.h"
#include "fake_audio_unit.hpp"
#include "wav_file.hpp"

#include <vespera/app_audio.h>
#include <vespera/sdk/audio_unit.hpp>

#include <nlohmann/json.hpp>

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <source_location>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;
using vespera::sdk::MainThread;

std::string fixture;

void check(bool condition, std::source_location location = std::source_location::current()) {
  if (!condition) {
    throw std::runtime_error("disposal check failed at line " + std::to_string(location.line()));
  }
}

struct Configuration {
  static constexpr uint32_t type = fake_au::type, subtype = fake_au::subtype,
                            manufacturer = fake_au::manufacturer;
  static constexpr const char *id = "test.fake-au.inline";
  static constexpr const char *name = "Fake Audio Unit";
  static constexpr auto parameters =
      std::to_array<vs_parameter_descriptor>({{1, "Enabled", "bool", 0, 1, 1}});
  static constexpr auto mapping = std::to_array<vespera::sdk::AudioUnitParameter>(
      {{fake_au::active_parameter, "Active", 0, 1}});
  static constexpr auto settings = std::array<vespera::sdk::AudioUnitSetting, 0>{};
};
using Module = vespera::sdk::AudioUnitModule<Configuration>;

struct MainLifecycle : Configuration {
  static constexpr const char *id = "test.fake-au.main-lifecycle";
  static constexpr bool main_thread_lifecycle = true;
};
using MainModule = vespera::sdk::AudioUnitModule<MainLifecycle>;

// Creates, prepares, processes one block through and destroys an instance of `api`.
void use_once(const vs_module_api &api) {
  void *instance = nullptr;
  check(api.create(&instance) == VS_OK);
  vs_prepare_info info{sizeof(info), 48000, 128, 1};
  vs_module_timing timing{sizeof(timing), 0, 0};
  check(api.prepare(instance, &info, &timing) == VS_OK);
  std::array<float, 128> in{}, out{};
  in.fill(.25f);
  const float *inputs[] = {in.data()};
  float *outputs[] = {out.data()};
  vs_process_block block{};
  block.struct_size = sizeof(block);
  block.inputs = inputs;
  block.outputs = outputs;
  block.channels = 1;
  block.frames = 128;
  block.flags = VS_PROCESS_DISCONTINUITY;
  check(api.process(instance, &block) == VS_OK);
  check(out[64] == .25f);
  api.destroy(instance);
}

void on_worker(const std::function<void()> &work) {
  std::thread worker(work);
  worker.join();
}

void pump_until_disposed() {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (fake_au::open_instances() != 0 && std::chrono::steady_clock::now() < deadline) {
    MainThread::wait(.005);
  }
  check(fake_au::open_instances() == 0);
}

void check_disposed_on_main() {
  check(fake_au::open_instances() == 0);
  check(fake_au::counters().closed_off_main == 0);
  check(fake_au::counters().stale_work == 0);
}

void released_on_main_is_disposed_at_once() {
  use_once(Module::api);
  check_disposed_on_main();
}

void released_on_worker_is_disposed_on_main() {
  on_worker([] { use_once(Module::api); });
  check(fake_au::open_instances() == 1);
  check(fake_au::counters().closed_off_main == 0);
  pump_until_disposed();
  check_disposed_on_main();
}

// A process that never services its main run loop keeps exactly the released instances and is
// never blocked by them.
void unserviced_main_thread_does_not_block() {
  const auto before = std::chrono::steady_clock::now();
  on_worker([] {
    for (int i = 0; i < 20; ++i) {
      use_once(Module::api);
    }
  });
  check(std::chrono::steady_clock::now() - before < 5s);
  check(fake_au::open_instances() == 20);
  pump_until_disposed();
  check_disposed_on_main();
}

// The posted disposal runs code of the module library after the host closed it, so the library
// must stay loaded.
void closed_library_still_disposes() {
  void *library = dlopen(fixture.c_str(), RTLD_NOW | RTLD_LOCAL);
  check(library != nullptr);
  auto entry = reinterpret_cast<vs_module_entry_fn>(dlsym(library, VS_MODULE_ENTRY_NAME));
  check(entry != nullptr);
  const auto *api = entry(VS_MODULE_ABI_VERSION);
  check(api != nullptr);
  on_worker([api] { use_once(*api); });
  check(dlclose(library) == 0);
  check(dlopen(fixture.c_str(), RTLD_NOW | RTLD_NOLOAD) != nullptr);
  check(fake_au::open_instances() == 1);
  pump_until_disposed();
  check_disposed_on_main();
}

// Holds the lifecycle lock as this library does, or as another library would through its own
// open file description of the per-process lock file.
enum class Holder { this_library, other_library };

// A disposal posted while another thread holds the lifecycle lock, for example while it creates
// a plugin, waits without blocking the main thread and runs once the lock is free.
void busy_lock_defers_disposal(Holder holder) {
  void *instance = nullptr;
  check(MainModule::api.create(&instance) == VS_OK);
  std::atomic<bool> prepared = false;
  std::thread preparing([&] {
    vs_prepare_info info{sizeof(info), 48000, 128, 1};
    vs_module_timing timing{sizeof(timing), 0, 0};
    prepared = MainModule::api.prepare(instance, &info, &timing) == VS_OK;
  });
  while (!prepared) {
    MainThread::wait(.002);
  }
  preparing.join();
  check(fake_au::open_instances() == 1);

  std::atomic<bool> held = false, done = false;
  std::thread holding([&] {
    if (holder == Holder::this_library) {
      vespera::sdk::PluginLifecycleLock lock;
      held = bool(lock);
      while (!done) {
        std::this_thread::sleep_for(1ms);
      }
      return;
    }
    char path[1024];
    const size_t length = confstr(_CS_DARWIN_USER_TEMP_DIR, path, sizeof(path));
    std::string file = std::string(path, length ? length - 1 : 0);
    file += (file.empty() || file.back() == '/' ? "" : "/") +
            ("vespera-plugin-lifecycle-" + std::to_string(getpid()) + ".lock");
    const int descriptor = open(file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    held = descriptor >= 0 && flock(descriptor, LOCK_EX) == 0;
    while (!done) {
      std::this_thread::sleep_for(1ms);
    }
    close(descriptor);
  });
  while (!held) {
    std::this_thread::sleep_for(1ms);
  }
  on_worker([&] { MainModule::api.destroy(instance); });
  auto longest = std::chrono::steady_clock::duration::zero();
  const auto until = std::chrono::steady_clock::now() + 300ms;
  while (std::chrono::steady_clock::now() < until) {
    const auto begun = std::chrono::steady_clock::now();
    MainThread::wait(.005);
    longest = std::max(longest, std::chrono::steady_clock::now() - begun);
  }
  check(fake_au::open_instances() == 1);
  done = true;
  holding.join();
  check(longest < 100ms);
  pump_until_disposed();
  check_disposed_on_main();
}

Json rig() {
  return {{"globals", Json::object()},
          {"amp", {{"binary", fixture}, {"parameters", {{{"id", 1}, {"value", 1.0}}}}}}};
}

// The reamp worker and the graph loader release instances off the main thread; destroying the
// bridge on the main thread disposes them without the caller servicing its run loop.
void bridge_teardown_disposes_released_instances() {
  const auto directory =
      std::filesystem::temp_directory_path() / ("vespera-disposal-" + std::to_string(getpid()));
  std::filesystem::create_directories(directory);
  const auto input = directory / "take.wav";
  {
    std::vector<float> samples(4800, .25f);
    vespera::WavWriter writer(input, 48000, 1);
    const float *planes[] = {samples.data()};
    writer.write(planes, samples.size());
    writer.close();
  }
  const auto *api = vespera_app_audio_entry(1);
  void *bridge = api->create();
  check(bridge != nullptr);
  auto call = [&](const Json &request) { return settled_audio_request(api, bridge, request); };

  auto live = rig();
  live["audio"] = {{"backend", "fake"},
                   {"inputDevice", "input-device"},
                   {"outputDevice", "output-device"},
                   {"inputChannels", {0}},
                   {"outputChannels", {0, 1}},
                   {"sampleRate", 48000},
                   {"bufferSize", 128},
                   {"monitor", true}};
  const auto started = call({{"op", "start"}, {"configuration", live}});
  if (!started.at("ok").get<bool>()) {
    std::fprintf(stderr, "%s\n", started.dump().c_str());
  }
  check(started.at("ok").get<bool>());
  check(call({{"op", "amp"}, {"amp", nullptr}}).at("ok").get<bool>());
  check(fake_au::counters().opened >= 2);

  const auto output = directory / "reamp.wav";
  check(call({{"op", "reamp"},
              {"action", "start"},
              {"input", input.string()},
              {"output", output.string()},
              {"configuration", rig()}})
            .at("ok")
            .get<bool>());
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  for (;;) {
    const auto status = call({{"op", "status"}});
    if (status.at("value").at("reamp").at("state") != "rendering") {
      check(status.at("value").at("reamp").at("state") == "done");
      break;
    }
    check(std::chrono::steady_clock::now() < deadline);
    std::this_thread::sleep_for(1ms);
  }
  check(call({{"op", "stop"}}).at("ok").get<bool>());
  api->destroy(bridge);
  check_disposed_on_main();
  check(fake_au::counters().opened >= 2);
  std::filesystem::remove_all(directory);
}

}

int main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s FAKE_AU_MODULE\n", argv[0]);
    return 2;
  }
  fixture = argv[1];
  fake_au::register_component();
  const std::pair<const char *, void (*)()> tests[] = {
      {"released_on_main_is_disposed_at_once", released_on_main_is_disposed_at_once},
      {"released_on_worker_is_disposed_on_main", released_on_worker_is_disposed_on_main},
      {"unserviced_main_thread_does_not_block", unserviced_main_thread_does_not_block},
      {"closed_library_still_disposes", closed_library_still_disposes},
      {"busy_lock_in_this_library_defers_disposal",
       [] { busy_lock_defers_disposal(Holder::this_library); }},
      {"busy_lock_in_another_library_defers_disposal",
       [] { busy_lock_defers_disposal(Holder::other_library); }},
      {"bridge_teardown_disposes_released_instances", bridge_teardown_disposes_released_instances},
  };
  for (const auto &[name, test] : tests) {
    try {
      test();
    } catch (const std::exception &error) {
      std::fprintf(stderr, "%s: %s\n", name, error.what());
      return 1;
    }
  }
  std::puts("audio unit disposal tests passed");
  return 0;
}
