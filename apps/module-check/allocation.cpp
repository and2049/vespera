#include "allocation.hpp"

#include <atomic>

#if defined(__APPLE__)
#include <cstdlib>
#include <pthread.h>

// libmalloc calls malloc_logger, when set, for every allocation and free made through any malloc
// zone, including those behind operator new; Instruments and MallocStackLogging use the same hook.
extern "C" {
typedef void(malloc_logger_t)(uint32_t type, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                              uintptr_t result, uint32_t num_hot_frames_to_skip);
extern malloc_logger_t *malloc_logger;
}

namespace vespera::module_check::allocation {
namespace {

std::atomic<bool> armed{false};
std::atomic<pthread_t> armed_thread{};
std::atomic<uint64_t> events{0};
std::atomic<uint32_t> paused{0};
malloc_logger_t *previous = nullptr;

void observe(uint32_t type, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t result,
             uint32_t skip) {
  if (armed.load(std::memory_order_acquire) && paused.load(std::memory_order_acquire) == 0 &&
      pthread_equal(pthread_self(), armed_thread.load(std::memory_order_relaxed))) {
    events.fetch_add(1, std::memory_order_relaxed);
  }
  if (previous) {
    previous(type, arg1, arg2, arg3, result, skip + 1);
  }
}

bool install() {
  previous = malloc_logger;
  malloc_logger = observe;
  void *(*volatile allocate)(size_t) = std::malloc;
  void (*volatile deallocate)(void *) = std::free;
  arm();
  deallocate(allocate(64));
  if (disarm() >= 2) {
    return true;
  }
  malloc_logger = previous;
  return false;
}

}

const char *mechanism() {
  static const bool available = install();
  return available ? "libmalloc malloc_logger hook on the processing thread" : nullptr;
}

void arm() noexcept {
  events.store(0, std::memory_order_relaxed);
  armed_thread.store(pthread_self(), std::memory_order_relaxed);
  armed.store(true, std::memory_order_release);
}

uint64_t disarm() noexcept {
  armed.store(false, std::memory_order_release);
  return events.load(std::memory_order_relaxed);
}

bool pause() noexcept {
  if (!armed.load(std::memory_order_acquire) ||
      !pthread_equal(pthread_self(), armed_thread.load(std::memory_order_relaxed))) {
    return false;
  }
  paused.fetch_add(1, std::memory_order_acq_rel);
  return true;
}

void resume() noexcept { paused.fetch_sub(1, std::memory_order_acq_rel); }

}

#else

namespace vespera::module_check::allocation {

const char *mechanism() { return nullptr; }
void arm() noexcept {}
uint64_t disarm() noexcept { return 0; }
bool pause() noexcept { return false; }
void resume() noexcept {}

}

#endif
