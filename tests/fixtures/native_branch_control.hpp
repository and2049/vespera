#pragma once

#include <vespera/module.h>

#include <atomic>

namespace native_fixture {
enum : uint32_t { selector = 0, power = 1, mix = 2, alternate_mix = 3 };
enum class Fault {
  none,
  absent,
  short_descriptor,
  bad_version,
  bad_callback,
  bad_law,
  bad_flags,
  bad_fence,
  bad_mix,
  bad_selector,
  foreign_target,
  bad_value,
  duplicate,
  overflow,
  missing
};

struct Control {
  Fault descriptor = Fault::none;
  Fault assignments = Fault::none;
  Fault translation = Fault::none;
  bool no_shared = false;
  bool foreign_engine = false;
  bool midi = false;
  bool leading = false;
  bool static_branch = false;
  bool fail_prepare = false;
  bool fail_apply_once = false;
  uint32_t stage = 0;
  uint32_t conflict_role = 0;
  float required_value = 0.0f;
  // The host may call these entry points from its loader worker and audio threads while a test
  // thread reads them, so they are atomic rather than plain counters.
  std::atomic<unsigned> native_assignments{0};
  std::atomic<unsigned> native_translations{0};
  std::atomic<unsigned> engine_prepares{0};
  std::atomic<unsigned> standalone_prepares{0};
  std::atomic<unsigned> releases{0};
  std::atomic<unsigned> processes{0};
  std::atomic<unsigned> cache_received{0};
  char signature[128]{};

  void reset() noexcept {
    descriptor = Fault::none;
    assignments = Fault::none;
    translation = Fault::none;
    no_shared = false;
    foreign_engine = false;
    midi = false;
    leading = false;
    static_branch = false;
    fail_prepare = false;
    fail_apply_once = false;
    stage = 0;
    conflict_role = 0;
    required_value = 0.0f;
    native_assignments.store(0, std::memory_order_relaxed);
    native_translations.store(0, std::memory_order_relaxed);
    engine_prepares.store(0, std::memory_order_relaxed);
    standalone_prepares.store(0, std::memory_order_relaxed);
    releases.store(0, std::memory_order_relaxed);
    processes.store(0, std::memory_order_relaxed);
    cache_received.store(0, std::memory_order_relaxed);
    signature[0] = 0;
  }
};
}
