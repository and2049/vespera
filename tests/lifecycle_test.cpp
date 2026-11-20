// The plugin lifecycle lock excludes holders in separately loaded libraries and in the host.
#include <vespera/module.h>
#include <vespera/sdk/plugin_lifecycle.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <dlfcn.h>
#include <functional>
#include <thread>

namespace {
using Hook = void (*)();
struct Library {
  Hook lock;
  Hook unlock;
};

Library load(const char *path) {
  void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!handle) {
    std::fprintf(stderr, "%s\n", dlerror());
    std::exit(1);
  }
  return {reinterpret_cast<Hook>(dlsym(handle, "lifecycle_lock")),
          reinterpret_cast<Hook>(dlsym(handle, "lifecycle_unlock"))};
}

// Holds the lock through `hold`, then checks that `contend` on another thread waits for it.
bool excludes(const std::function<void()> &hold, const std::function<void()> &release,
              const std::function<void()> &contend, const std::function<void()> &done) {
  hold();
  std::atomic<bool> acquired{false};
  std::thread other([&] {
    contend();
    acquired = true;
    done();
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  const bool waited = !acquired;
  release();
  other.join();
  return waited && acquired;
}
}

// prepare and engine_prepare of the uninstalled-component fixture at `path`.
std::pair<vs_result, vs_result> prepare_results(const char *path) {
  void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!handle) {
    std::fprintf(stderr, "%s\n", dlerror());
    std::exit(1);
  }
  const auto *api = reinterpret_cast<vs_module_entry_fn>(dlsym(handle, VS_MODULE_ENTRY_NAME))(
      VS_MODULE_ABI_VERSION);
  const auto *shared = reinterpret_cast<vs_module_shared_entry_fn>(
      dlsym(handle, VS_MODULE_SHARED_ENTRY_NAME))(VS_MODULE_SHARED_ABI_VERSION);
  void *instance = nullptr;
  api->create(&instance);
  const vs_prepare_info info{sizeof(info), 48000, 128, 2};
  vs_module_timing timing{sizeof(timing), 0, 0};
  const vs_result ordinary = api->prepare(instance, &info, &timing);
  const vs_result engine =
      shared->engine_prepare(instance, &info, nullptr, "test.lifecycle.au", 0, nullptr, 0,
                             sizeof(vs_shared_target), nullptr, 0, &timing);
  api->release(instance);
  api->destroy(instance);
  return {ordinary, engine};
}

int main(int argc, char **argv) {
  if (argc != 6) {
    return 2;
  }
  const auto a = load(argv[1]);
  const auto b = load(argv[2]);
  if (!a.lock || !b.lock) {
    return 1;
  }
  vespera::sdk::PluginLifecycleLock *host = nullptr;
  const bool libraries = excludes(a.lock, a.unlock, b.lock, b.unlock);
  const bool host_first = excludes([&] { host = new vespera::sdk::PluginLifecycleLock; },
                                   [&] { delete host; }, a.lock, a.unlock);
  const bool same_library = excludes(b.lock, b.unlock, b.lock, b.unlock);
  {
    vespera::sdk::PluginLifecycleLock outer;
    vespera::sdk::PluginLifecycleLock reentrant;
  }
  using Probe = int (*)();
  auto acquired = [](const char *path) {
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    return handle ? reinterpret_cast<Probe>(dlsym(handle, "lifecycle_acquired"))() : -1;
  };
  const bool reported = acquired(argv[1]) == 1 && acquired(argv[3]) == 0;
  const auto unusable = prepare_results(argv[4]);
  const auto usable = prepare_results(argv[5]);
  const bool refused = unusable.first == VS_ERROR_PROCESS && unusable.second == VS_ERROR_PROCESS;
  const bool reached =
      usable.first == VS_ERROR_UNSUPPORTED && usable.second == VS_ERROR_UNSUPPORTED;
  std::printf("libraries %d host %d same library %d failure reported %d prepare refused %d/%d "
              "control %d/%d\n",
              libraries, host_first, same_library, reported, unusable.first, unusable.second,
              usable.first, usable.second);
  return libraries && host_first && same_library && reported && refused && reached ? 0 : 1;
}
