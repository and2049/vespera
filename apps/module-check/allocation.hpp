#ifndef VESPERA_MODULE_CHECK_ALLOCATION_HPP
#define VESPERA_MODULE_CHECK_ALLOCATION_HPP

#include <cstdint>

namespace vespera::module_check::allocation {

// How this process observes heap allocation, or null when it cannot (unsupported platform, or a
// sanitizer that replaces the allocator).
const char *mechanism();
// Counts allocation and free events on the calling thread until disarm(), which returns them.
void arm() noexcept;
uint64_t disarm() noexcept;
// Stops counting while module work runs inline on the armed thread, which may allocate. Returns
// whether it paused, so that only a pausing call is followed by resume().
bool pause() noexcept;
void resume() noexcept;

}

#endif
