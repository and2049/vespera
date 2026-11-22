#ifndef VESPERA_TESTS_FAKE_AUDIO_UNIT_HPP
#define VESPERA_TESTS_FAKE_AUDIO_UNIT_HPP

#include <AudioToolbox/AudioToolbox.h>

#include <atomic>
#include <cstdint>

// An in-process effect Audio Unit standing in for plugins built on JUCE: initializing an instance
// queues work on the main run loop that refers to the instance, as their message-thread work
// does. A host that disposes an instance off the main thread lets that work find it gone.
namespace fake_au {

inline constexpr uint32_t type = kAudioUnitType_Effect;
inline constexpr uint32_t subtype = 0x7666616b;
inline constexpr uint32_t manufacturer = 0x76737061;
inline constexpr AudioUnitParameterID active_parameter = 0;
// Never written by wrappers; changes on its own while rendering when `drifting` is set.
inline constexpr AudioUnitParameterID drift_parameter = 1;
// Indexed 0..3; writes are truncated to an index and clamped.
inline constexpr AudioUnitParameterID mode_parameter = 2;
// Continuous 0..1; writes are clamped.
inline constexpr AudioUnitParameterID gain_parameter = 3;

struct Counters {
  std::atomic<int> opened{0};
  std::atomic<int> closed{0};
  std::atomic<int> closed_off_main{0};
  std::atomic<int> initialized{0};
  // Main-thread work that ran after its instance had been disposed.
  std::atomic<int> stale_work{0};
};

// How instances created from now on behave once their startup work has run on the main thread.
struct Behaviour {
  // Output stays silent until the startup work has run.
  std::atomic<bool> silent_until_startup{false};
  // The startup work reverts the parameter to its default.
  std::atomic<bool> startup_reverts{false};
  // Every render reverts the parameter to its default.
  std::atomic<bool> always_reverts{false};
  // The first write to the parameter is reverted at once, as startup work landing while the
  // instance is being configured.
  std::atomic<bool> reverts_first_write{false};
  // Every render moves the drift parameter, through not-a-number when `drift_to_nan` is set.
  std::atomic<bool> drifting{false};
  std::atomic<bool> drift_to_nan{false};
  // Reading the active parameter fails once the startup work has run.
  std::atomic<bool> unreadable_after_startup{false};
};

inline constexpr float active_default = 1.0f;

Behaviour &behaviour();

// Registers the component once per process; safe to call repeatedly.
void register_component();
Counters &counters();
int open_instances();

}

#endif
