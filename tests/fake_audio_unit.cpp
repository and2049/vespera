#include "fake_audio_unit.hpp"

#include <dispatch/dispatch.h>
#include <pthread.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <vector>

namespace fake_au {

namespace {

std::mutex registry_mutex;
struct Unit;

std::map<uint64_t, Unit *> &units() {
  static auto *instances = new std::map<uint64_t, Unit *>;
  return *instances;
}

// Disposing on the main thread cancels an instance's queued work there, as the message thread's
// own cleanup does; disposing elsewhere leaves it queued.
std::set<uint64_t> &disposed_off_main() {
  static auto *instances = new std::set<uint64_t>;
  return *instances;
}

struct Unit {
  AudioComponentPlugInInterface interface;
  uint64_t serial = 0;
  bool initialized = false;
  std::atomic<float> active{active_default};
  std::atomic<bool> started{false};
  bool silent_until_startup = false;
  bool startup_reverts = false;
  bool always_reverts = false;
  bool reverts_first_write = false;
  bool written = false;
  bool drifting = false;
  bool drift_to_nan = false;
  bool unreadable_after_startup = false;
  std::atomic<float> drift{0};
  std::atomic<float> mode{0};
  std::atomic<float> gain{0};
  UInt32 max_frames = 1156;
  AURenderCallbackStruct input{};
  std::vector<float> scratch[2];
  UInt32 channels = 1;
};

Unit &self(void *instance) { return *static_cast<Unit *>(instance); }

constexpr AudioUnitParameterID parameter_list[] = {active_parameter, drift_parameter,
                                                   mode_parameter, gain_parameter};

void queued_work(void *context) {
  const auto serial = reinterpret_cast<uintptr_t>(context);
  std::lock_guard lock(registry_mutex);
  if (disposed_off_main().contains(serial)) {
    ++counters().stale_work;
  }
  if (const auto found = units().find(serial); found != units().end()) {
    auto &unit = *found->second;
    if (unit.startup_reverts) {
      unit.active = active_default;
    }
    unit.started = true;
  }
}

OSStatus initialize(void *instance) {
  auto &unit = self(instance);
  unit.initialized = true;
  ++counters().initialized;
  dispatch_async_f(dispatch_get_main_queue(), reinterpret_cast<void *>(uintptr_t(unit.serial)),
                   queued_work);
  return noErr;
}

OSStatus uninitialize(void *instance) {
  self(instance).initialized = false;
  return noErr;
}

OSStatus get_property_info(void *, AudioUnitPropertyID id, AudioUnitScope, AudioUnitElement,
                           UInt32 *size, Boolean *writable) {
  UInt32 bytes = 0;
  switch (id) {
  case kAudioUnitProperty_ParameterList:
    bytes = sizeof(parameter_list);
    break;
  case kAudioUnitProperty_ParameterInfo:
    bytes = sizeof(AudioUnitParameterInfo);
    break;
  case kAudioUnitProperty_Latency:
  case kAudioUnitProperty_TailTime:
    bytes = sizeof(Float64);
    break;
  default:
    return kAudioUnitErr_InvalidProperty;
  }
  if (size) {
    *size = bytes;
  }
  if (writable) {
    *writable = false;
  }
  return noErr;
}

OSStatus get_property(void *, AudioUnitPropertyID id, AudioUnitScope, AudioUnitElement element,
                      void *data, UInt32 *size) {
  if (!data || !size) {
    return kAudio_ParamError;
  }
  switch (id) {
  case kAudioUnitProperty_ParameterList:
    if (*size < sizeof(parameter_list)) {
      return kAudio_ParamError;
    }
    std::memcpy(data, parameter_list, sizeof(parameter_list));
    *size = sizeof(parameter_list);
    return noErr;
  case kAudioUnitProperty_ParameterInfo: {
    if (element > gain_parameter) {
      return kAudioUnitErr_InvalidParameter;
    }
    static constexpr const char *names[] = {"Active", "Drift", "Mode", "Gain"};
    AudioUnitParameterInfo info{};
    std::strncpy(info.name, names[element], sizeof(info.name));
    info.unit = element == mode_parameter ? kAudioUnitParameterUnit_Indexed
                                          : kAudioUnitParameterUnit_Generic;
    info.minValue = 0;
    info.maxValue = element == mode_parameter ? 3 : 1;
    info.defaultValue = element == active_parameter ? 1 : 0;
    info.flags = kAudioUnitParameterFlag_IsReadable | kAudioUnitParameterFlag_IsWritable;
    std::memcpy(data, &info, sizeof(info));
    *size = sizeof(info);
    return noErr;
  }
  case kAudioUnitProperty_Latency:
  case kAudioUnitProperty_TailTime:
    *static_cast<Float64 *>(data) = 0;
    *size = sizeof(Float64);
    return noErr;
  default:
    return kAudioUnitErr_InvalidProperty;
  }
}

OSStatus set_property(void *instance, AudioUnitPropertyID id, AudioUnitScope, AudioUnitElement,
                      const void *data, UInt32 size) {
  auto &unit = self(instance);
  switch (id) {
  case kAudioUnitProperty_SetRenderCallback:
    if (!data || size < sizeof(AURenderCallbackStruct)) {
      return kAudio_ParamError;
    }
    std::memcpy(&unit.input, data, sizeof(unit.input));
    return noErr;
  case kAudioUnitProperty_MaximumFramesPerSlice:
    if (!data || size < sizeof(UInt32)) {
      return kAudio_ParamError;
    }
    std::memcpy(&unit.max_frames, data, sizeof(UInt32));
    return noErr;
  case kAudioUnitProperty_StreamFormat: {
    if (!data || size < sizeof(AudioStreamBasicDescription)) {
      return kAudio_ParamError;
    }
    AudioStreamBasicDescription format;
    std::memcpy(&format, data, sizeof(format));
    unit.channels = format.mChannelsPerFrame;
    return noErr;
  }
  case kAudioUnitProperty_HostCallbacks:
    return noErr;
  default:
    return kAudioUnitErr_InvalidProperty;
  }
}

OSStatus get_parameter(void *instance, AudioUnitParameterID id, AudioUnitScope, AudioUnitElement,
                       AudioUnitParameterValue *value) {
  auto &unit = self(instance);
  if (!value) {
    return kAudio_ParamError;
  }
  switch (id) {
  case active_parameter:
    if (unit.unreadable_after_startup && unit.started) {
      return kAudioUnitErr_CannotDoInCurrentContext;
    }
    *value = unit.active;
    return noErr;
  case drift_parameter:
    *value = unit.drift;
    return noErr;
  case mode_parameter:
    *value = unit.mode;
    return noErr;
  case gain_parameter:
    *value = unit.gain;
    return noErr;
  default:
    return kAudioUnitErr_InvalidParameter;
  }
}

OSStatus set_parameter(void *instance, AudioUnitParameterID id, AudioUnitScope, AudioUnitElement,
                       AudioUnitParameterValue value, UInt32) {
  auto &unit = self(instance);
  switch (id) {
  case active_parameter:
    unit.active = unit.reverts_first_write && !unit.written ? active_default : value;
    unit.written = true;
    return noErr;
  case drift_parameter:
    unit.drift = value;
    return noErr;
  case mode_parameter:
    unit.mode = std::clamp(std::trunc(value), 0.0f, 3.0f);
    return noErr;
  case gain_parameter:
    unit.gain = std::clamp(value, 0.0f, 1.0f);
    return noErr;
  default:
    return kAudioUnitErr_InvalidParameter;
  }
}

OSStatus render(void *instance, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *timestamp,
                UInt32, UInt32 frames, AudioBufferList *data) {
  auto &unit = self(instance);
  if (!unit.initialized || !unit.input.inputProc || !data || frames > unit.max_frames) {
    return kAudioUnitErr_Uninitialized;
  }
  struct Buffers {
    UInt32 count;
    AudioBuffer buffers[2];
  } pulled{};
  pulled.count = data->mNumberBuffers;
  for (UInt32 c = 0; c < data->mNumberBuffers && c < 2; ++c) {
    unit.scratch[c].resize(frames);
    pulled.buffers[c] = {1, frames * UInt32(sizeof(float)), unit.scratch[c].data()};
  }
  const auto status = unit.input.inputProc(unit.input.inputProcRefCon, flags, timestamp, 0, frames,
                                           reinterpret_cast<AudioBufferList *>(&pulled));
  if (status != noErr) {
    return status;
  }
  if (unit.always_reverts) {
    unit.active = active_default;
  }
  if (unit.drifting) {
    unit.drift = unit.drift_to_nan ? std::nanf("") : std::fmod(unit.drift + .001f, 1.0f);
  }
  const float gain = unit.silent_until_startup && !unit.started ? 0.0f : unit.active.load();
  for (UInt32 c = 0; c < data->mNumberBuffers && c < 2; ++c) {
    auto *out = static_cast<float *>(data->mBuffers[c].mData);
    const auto *in = static_cast<const float *>(pulled.buffers[c].mData);
    for (UInt32 i = 0; i < frames; ++i) {
      out[i] = in[i] * gain;
    }
  }
  return noErr;
}

OSStatus reset(void *, AudioUnitScope, AudioUnitElement) {
  ++fake_au::counters().resets;
  return noErr;
}

AudioComponentMethod lookup(SInt16 selector) {
  switch (selector) {
  case kAudioUnitInitializeSelect:
    return reinterpret_cast<AudioComponentMethod>(initialize);
  case kAudioUnitUninitializeSelect:
    return reinterpret_cast<AudioComponentMethod>(uninitialize);
  case kAudioUnitGetPropertyInfoSelect:
    return reinterpret_cast<AudioComponentMethod>(get_property_info);
  case kAudioUnitGetPropertySelect:
    return reinterpret_cast<AudioComponentMethod>(get_property);
  case kAudioUnitSetPropertySelect:
    return reinterpret_cast<AudioComponentMethod>(set_property);
  case kAudioUnitGetParameterSelect:
    return reinterpret_cast<AudioComponentMethod>(get_parameter);
  case kAudioUnitSetParameterSelect:
    return reinterpret_cast<AudioComponentMethod>(set_parameter);
  case kAudioUnitRenderSelect:
    return reinterpret_cast<AudioComponentMethod>(render);
  case kAudioUnitResetSelect:
    return reinterpret_cast<AudioComponentMethod>(reset);
  default:
    return nullptr;
  }
}

OSStatus open(void *instance, AudioComponentInstance) {
  static std::atomic<uint64_t> next{1};
  auto &unit = self(instance);
  unit.serial = next++;
  unit.silent_until_startup = behaviour().silent_until_startup;
  unit.startup_reverts = behaviour().startup_reverts;
  unit.always_reverts = behaviour().always_reverts;
  unit.reverts_first_write = behaviour().reverts_first_write;
  unit.drifting = behaviour().drifting;
  unit.drift_to_nan = behaviour().drift_to_nan;
  unit.unreadable_after_startup = behaviour().unreadable_after_startup;
  {
    std::lock_guard lock(registry_mutex);
    units()[unit.serial] = &unit;
  }
  ++counters().opened;
  return noErr;
}

OSStatus close(void *instance) {
  auto *unit = static_cast<Unit *>(instance);
  {
    std::lock_guard lock(registry_mutex);
    units().erase(unit->serial);
    if (!pthread_main_np()) {
      disposed_off_main().insert(unit->serial);
      ++counters().closed_off_main;
    }
  }
  ++counters().closed;
  delete unit;
  return noErr;
}

AudioComponentPlugInInterface *factory(const AudioComponentDescription *) {
  auto *unit = new Unit;
  unit->interface = {open, close, lookup, nullptr};
  return &unit->interface;
}

}

Behaviour &behaviour() {
  static auto *values = new Behaviour;
  return *values;
}

Counters &counters() {
  static auto *values = new Counters;
  return *values;
}

int open_instances() { return counters().opened - counters().closed; }

void register_component() {
  static const bool registered = [] {
    const AudioComponentDescription description{type, subtype, manufacturer, 0, 0};
    return AudioComponentRegister(&description, CFSTR("vespera: Fake Message Thread Unit"), 1,
                                  factory) != nullptr;
  }();
  (void)registered;
}

}
