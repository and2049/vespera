// Exposes this library's copy of the plugin lifecycle lock so tests can hold it from outside.
#include <vespera/module.h>
#include <vespera/sdk/plugin_lifecycle.hpp>
#ifdef __APPLE__
#include <atomic>
#include <vespera/sdk/main_thread.hpp>
#endif

namespace {
std::atomic<vespera::sdk::PluginLifecycleLock *> held{nullptr};
}

extern "C" VS_MODULE_EXPORT void lifecycle_lock() {
  held.store(new vespera::sdk::PluginLifecycleLock, std::memory_order_release);
}
extern "C" VS_MODULE_EXPORT void lifecycle_unlock() {
  delete held.exchange(nullptr, std::memory_order_acquire);
}

extern "C" VS_MODULE_EXPORT int lifecycle_acquired() {
  vespera::sdk::PluginLifecycleLock lock;
  return lock ? 1 : 0;
}

#ifdef __APPLE__
extern "C" VS_MODULE_EXPORT bool lifecycle_post(bool *completed) {
  return vespera::sdk::MainThread::post([completed] { *completed = lifecycle_acquired() == 1; });
}

extern "C" VS_MODULE_EXPORT bool lifecycle_call(std::atomic<bool> *ready,
                                                std::atomic<bool> *completed) {
  using vespera::sdk::MainThread;
  bool acquired = false;
  *ready = true;
  ready->notify_one();
  const auto result = MainThread::call([&] { acquired = lifecycle_acquired() == 1; }, 2);
  *completed = true;
  return acquired && result == MainThread::Result::done;
}
#endif
