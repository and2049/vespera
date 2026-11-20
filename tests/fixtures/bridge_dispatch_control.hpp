#pragma once

#include <atomic>

struct BridgeDispatchControl {
  std::atomic<unsigned> entered{0}, called{0}, released{0}, timed_out{0}, live{0};
  std::atomic<bool> fail{false};
  void (*on_main)(void *) = nullptr;
  void *context = nullptr;
};

using BridgeDispatchControlFn = BridgeDispatchControl *(*)();
