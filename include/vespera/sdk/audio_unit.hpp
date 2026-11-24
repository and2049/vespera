#ifndef VESPERA_SDK_AUDIO_UNIT_HPP
#define VESPERA_SDK_AUDIO_UNIT_HPP

#include <vespera/binary32.hpp>
#include <vespera/module.h>
#include <vespera/sdk/plugin_lifecycle.hpp>
#include <vespera/sdk/shared_policy.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <concepts>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef __APPLE__
#include <AudioToolbox/AudioToolbox.h>
#include <vespera/sdk/main_thread.hpp>
#endif

namespace vespera::sdk {

template <class Configuration> class AudioUnitModule;

#ifdef __APPLE__
// Renders exactly `frames` of offline silence through `step`, in blocks of at most `block`
// frames, reusing the silence and stereo output scratch. Both callers prepare a newly settled
// AU before user frame zero; the blocks must cover the budget exactly.
template <class Step>
bool render_silence(uint32_t frames, uint32_t block, uint32_t channels, std::vector<float> &silence,
                    std::array<std::vector<float>, 2> &output, Step &&step) {
  silence.resize(block);
  for (uint32_t c = 0; c < channels; ++c) {
    output[c].resize(block);
  }
  const float *inputs[]{silence.data(), silence.data()};
  float *outputs[]{output[0].data(), output[1].data()};
  while (frames) {
    const auto count = std::min(frames, block);
    const vs_process_block audio{
        sizeof(audio), inputs, outputs, channels, count, 0, VS_PROCESS_OFFLINE,
        nullptr,       0,      nullptr, nullptr,  0,     0};
    if (!step(audio, count)) {
      return false;
    }
    frames -= count;
  }
  return true;
}

// A readiness hook runs before live processing and supplies an exact frame budget before
// rendering. Adaptive procedures must reserve and fill a budget to end at zero without a jump.
class Preparation {
public:
  double sample_rate() const { return rate_; }
  uint32_t channels() const { return channels_; }
  uint32_t max_frames() const { return max_frames_; }

  bool begin(uint32_t frames) {
    if (started_ || failed_) {
      return fail();
    }
    started_ = true;
    remaining_ = frames;
    timestamp_ = -static_cast<double>(frames);
    return true;
  }

  bool silence(double seconds) {
    const auto frames = std::ceil(seconds * rate_);
    if (!std::isfinite(seconds) || seconds < 0 || frames > remaining_) {
      return fail();
    }
    return silence_frames(static_cast<uint32_t>(frames));
  }

  bool silence_frames(uint32_t left) {
    if (!started_ || failed_ || left > remaining_) {
      return fail();
    }
    if (!left) {
      return true;
    }
    return render_silence(
        left, max_frames_, channels_, silence_, output_,
        [this](const vs_process_block &audio, uint32_t) { return std::isfinite(render(audio)); });
  }

  // Only audio buffers and frame count are consumed. Returns NaN on error.
  float render(const vs_process_block &block) {
    if (!started_ || failed_ || block.struct_size < VS_PROCESS_BLOCK_V1_SIZE ||
        block.channels != channels_ || !block.frames || block.frames > max_frames_ ||
        block.frames > remaining_ || !block.inputs || !block.outputs) {
      fail();
      return std::numeric_limits<float>::quiet_NaN();
    }
    for (uint32_t c = 0; c < channels_; ++c) {
      if (!block.inputs[c] || !block.outputs[c]) {
        fail();
        return std::numeric_limits<float>::quiet_NaN();
      }
    }
    const vs_process_block audio{sizeof(audio),
                                 block.inputs,
                                 block.outputs,
                                 channels_,
                                 block.frames,
                                 0,
                                 VS_PROCESS_OFFLINE,
                                 nullptr,
                                 0,
                                 nullptr,
                                 nullptr,
                                 0,
                                 0};
    if (render_(context_, audio) != VS_OK) {
      fail();
      return std::numeric_limits<float>::quiet_NaN();
    }
    remaining_ -= block.frames;
    float peak = 0;
    for (uint32_t c = 0; c < channels_; ++c) {
      for (uint32_t f = 0; f < block.frames; ++f) {
        peak = std::max(peak, std::abs(block.outputs[c][f]));
      }
    }
    return peak;
  }

  bool set_target(uint32_t id, float value) {
    return (!failed_ && write_target(id, value)) || fail();
  }

  // Reads the prepared AU, not the leader's module values.
  bool get_target(uint32_t id, float &value) {
    AudioUnitParameterValue actual = 0;
    if (failed_ || AudioUnitGetParameter(unit_, id, kAudioUnitScope_Global, 0, &actual) != noErr ||
        !std::isfinite(actual)) {
      return fail();
    }
    value = actual;
    return true;
  }

  // False from the callback fails readiness. Exceptions propagate after restoration.
  template <class Job> bool with_temporary_target(uint32_t id, float value, Job &&job) {
    float previous = 0;
    if (!get_target(id, previous) || !std::isfinite(value)) {
      return fail();
    }
    const auto restore = [&] { return write_target(id, previous) || fail(); };
    bool ok;
    try {
      ok = set_target(id, value) && std::forward<Job>(job)();
    } catch (...) {
      restore();
      fail();
      throw;
    }
    const bool restored = restore();
    return (ok && restored && !failed_) || fail();
  }

  // A floor for the entire prepared run, including settings that may become active live.
  bool minimum_tail_seconds(double seconds) {
    if (failed_ || !std::isfinite(seconds) || seconds < 0 || seconds > 3600) {
      return fail();
    }
    return minimum_tail_frames(static_cast<uint64_t>(std::ceil(seconds * rate_)));
  }

  // UINT64_MAX indicates that no finite bound is reported.
  bool minimum_tail_frames(uint64_t frames) {
    if (failed_) {
      return false;
    }
    tail_floor_frames_ = std::max(tail_floor_frames_, frames);
    return true;
  }
  bool alive(double timeout_seconds) {
    return (!failed_ && MainThread::alive(timeout_seconds)) || fail();
  }
  void wait(double seconds) { MainThread::wait(seconds); }
  void log(std::string_view text) {
    std::fputs("Audio Unit preparation: ", stderr);
    std::fwrite(text.data(), 1, text.size(), stderr);
    std::fputc('\n', stderr);
  }

private:
  template <class> friend class AudioUnitModule;
  using Render = vs_result (*)(void *, const vs_process_block &);
  Preparation(AudioUnit unit, void *context, Render render, double &timestamp,
              const vs_prepare_info &info)
      : unit_(unit), context_(context), render_(render), timestamp_(timestamp),
        rate_(info.sample_rate), channels_(info.channels), max_frames_(info.max_frames) {}
  Preparation(const Preparation &) = delete;
  Preparation &operator=(const Preparation &) = delete;
  bool write_target(uint32_t id, float value) {
    return std::isfinite(value) &&
           AudioUnitSetParameter(unit_, id, kAudioUnitScope_Global, 0, value, 0) == noErr;
  }
  bool fail() {
    failed_ = true;
    return false;
  }

  AudioUnit unit_;
  void *context_;
  Render render_;
  double &timestamp_;
  double rate_;
  uint64_t tail_floor_frames_ = 0;
  uint32_t channels_, max_frames_, remaining_ = 0;
  bool started_ = false, failed_ = false;
  std::vector<float> silence_;
  std::array<std::vector<float>, 2> output_;
};
#else
class Preparation;
#endif

struct AudioUnitParameter {
  uint32_t id;
  const char *name;
  float minimum;
  float maximum;
};

struct AudioUnitSetting {
  AudioUnitParameter parameter;
  float value;
};

// Revision of the shared engine implementation below. Part of every engine identity, so modules
// built against different engine code never share an instance.
inline constexpr uint32_t shared_engine_revision = 2;

// While the route parameter selects `route`, module parameter `parameter` writes `value` (in
// Audio Unit units) to its target instead of its own mapped value.
struct RoutePin {
  uint32_t route;
  uint32_t parameter;
  float value;
};

/*
 * Wraps one installed Audio Unit as a vespera module. Configuration supplies the component,
 * module parameters, their AU mapping and the fixed settings that isolate the module's section.
 *
 * Capabilities (vs_module_api.capabilities) follow optional Configuration members:
 *   bypass_parameter the id of a parameter ranging from 0 to 1 that bypasses the module
 *   host_tempo       present when the plugin reads the host tempo
 *   midi_input       true when the plugin accepts MIDI channel input
 *
 * A Configuration may also opt into shared engines (vs_module_shared_api) by declaring
 *   shared_engine    contract name; with the AU component and neutral version it forms the
 *                    engine identity, so it must change whenever stage or target meaning changes
 *   shared_stage     the section's position in the plugin's internal processing order
 *   shared_owned     optional setting ids that belong to this section besides its mapping
 *   shared_required  optional setting ids whose value this section depends on and that every
 *                    member of an engine must agree on, such as a model selector
 *   shared_leading   optional; true when the section cannot follow another in one instance
 *   shared_retains_audio  optional; true when the section keeps audio across Reset, so engines
 *                    running it are disposed instead of returned to the host cache
 * Every other setting is neutral: it isolates sections the module does not own. A shared
 * module's bypass parameter must bypass exactly its section, except in a route where it is pinned
 * (below); hosts toggle it live.
 *
 * A module spanning several sections may declare power_gates, the ids of its boolean section
 * parameters. Its bypass parameter, which must then be its first parameter, has no target of its
 * own (its mapping entry is ignored): while it is off every gated section is written as its mapped
 * minimum, and while on each gated section follows its own value, so section values survive
 * bypassing the module.
 *
 * Optional metadata is an array of vs_parameter_metadata. Discrete entries canonicalize setters,
 * events, shared assignments and state loads to the nearest step (halfway chooses the higher
 * step); wrappers export metadata_api through vespera_module_metadata_entry.
 *
 * A module whose rendering depends on its route through the host may declare route_parameter,
 * the id of a parameter ranging from 0 to routes - 1 (2 to 8 routes; discrete metadata, if any,
 * must have exactly that many steps), and route_pins, an array of RoutePin. The current route is
 * floor(value + 0.5) of that parameter's canonical value. While a pin's route is current, its
 * parameter's value is still stored, reported, saved and translated as usual, but its target is
 * held at the pin's value; in other routes it follows its mapping. The route parameter keeps its
 * own mapping and, when set, also rewrites every pinned parameter's target, in the same apply,
 * event or translation. Pinned parameters must be targeted, distinct from the route parameter and
 * not power gates; distinct pinned parameters plus one may not exceed
 * shared_assignments_per_parameter. A route change while processing takes effect at its event
 * frame without ramp or reset; hosts wanting a clean change prepare a new instance with the new
 * value instead. Pins are part of the module's code, not of cache or engine identity: reused
 * instances receive every target again when prepared.
 * A routed shared member can opt into native branch absorption with
 * native_branch_mix_parameter, native_branch_first_after_stage and native_branch_route.
 * The selector must have discrete metadata.
 * In that route the optional member callbacks release only the Mix and bypass pins.
 * Ordinary processing and shared v2 callbacks keep the pins, including on older hosts.
 *
 * Optional preparation_seconds (0..1) renders silence after initialization and before frame zero,
 * without resetting afterwards or increasing reported latency. It runs only during preparation;
 * the host tempo is not known until processing begins. All members of a shared run must use the
 * same policy; it participates in engine and cache identity. Preparation runs outside the
 * lifecycle lock.
 * Optional preserve_preparation_on_start keeps fresh AU state through the first process block's
 * discontinuity. Explicit resets, later discontinuities and reused instances still reset.
 * Optional preparation_in_real_time, for plugins whose startup work on the main thread must
 * overlap rendering, renders a fresh instance's preparation_seconds no faster than playback off
 * the main thread, then gives the instance again any parameter values that work reverted and
 * prepares again, failing after three attempts. Cached instances and the main thread, where that
 * work cannot run during preparation, prepare at full speed. Optional preparation_paced_seconds
 * (0 < value <= preparation_seconds) paces only that leading part; the rest renders at full
 * speed, so the rendered length is unchanged. Both participate in engine and cache identity.
 * Verification covers only the values the wrapper wrote while preparing, within the plugin's
 * representation of them (see holds), and gives again only those it no longer holds.
 * Instances released off the main thread are disposed on it, where hosted plugins handle their
 * message-thread work, through a posted job that pins this library.
 * Optional main_thread_lifecycle runs creation and disposal on main and disables caching.
 * The lifecycle lock serializes AU calls, not instance lifetimes: deferred teardown may run
 * after a later inline creation or a job from another library.
 * With it, prepare_instance(Preparation&) can replace preparation_seconds. It runs on the
 * preparing thread outside the lifecycle lock and returns false on readiness failure. A
 * readiness hook that renders must declare its frame budget with Preparation::begin first.
 *
 * Idle instances go to the host's vs_host_instance_cache when it offers one, keyed by component
 * and instance policy variant, preparation policy and channel count; the cache disposes them
 * through this module on the main thread under the plugin lifecycle lock.
 */
template <class Configuration> class AudioUnitModule {
  static constexpr bool accepts_midi = [] {
    if constexpr (requires { Configuration::midi_input; }) {
      return Configuration::midi_input;
    } else {
      return false;
    }
  }();
  static constexpr bool host_tempo = requires { Configuration::host_tempo; };
  static constexpr bool bypassable = requires { Configuration::bypass_parameter; };
  // Module version string reported through vs_module_api; defaults to the SDK's.
  static constexpr const char *version = [] {
    if constexpr (requires { Configuration::version; }) {
      return Configuration::version;
    } else {
      return "0.2.0";
    }
  }();
  static constexpr auto metadata = [] {
    if constexpr (requires { Configuration::metadata; }) {
      return Configuration::metadata;
    } else {
      return std::array<vs_parameter_metadata, 0>{};
    }
  }();
  static_assert(
      [] {
        for (size_t i = 0; i < metadata.size(); ++i) {
          const auto &entry = metadata[i];
          bool found = false;
          for (const auto &parameter : Configuration::parameters) {
            if (parameter.id == entry.parameter_id) {
              found = parameter.maximum > parameter.minimum;
            }
          }
          if (!found || entry.kind > VS_PARAMETER_ENUMERATED ||
              (entry.kind != VS_PARAMETER_CONTINUOUS && entry.steps < 2) ||
              (entry.kind == VS_PARAMETER_TOGGLE && entry.steps != 2) ||
              (entry.kind == VS_PARAMETER_ENUMERATED && !entry.labels)) {
            return false;
          }
          for (size_t j = 0; j < i; ++j) {
            if (metadata[j].parameter_id == entry.parameter_id) {
              return false;
            }
          }
        }
        return true;
      }(),
      "Audio Unit metadata must describe distinct declared parameters and valid discrete ranges");
  // Opt-in silent preparation advances internal section fades before user frame zero.
  static constexpr double preparation_seconds = [] {
    if constexpr (requires { Configuration::preparation_seconds; }) {
      return double{Configuration::preparation_seconds};
    } else {
      return 0.0;
    }
  }();
  static_assert(preparation_seconds >= 0 && preparation_seconds <= 1);
  static constexpr bool main_lifecycle = [] {
    if constexpr (requires { Configuration::main_thread_lifecycle; }) {
      return bool(Configuration::main_thread_lifecycle);
    }
    return false;
  }();
  static constexpr bool realtime_preparation = [] {
    if constexpr (requires { Configuration::preparation_in_real_time; }) {
      return bool(Configuration::preparation_in_real_time);
    }
    return false;
  }();
  static_assert(!realtime_preparation || preparation_seconds > 0,
                "preparation_in_real_time requires preparation_seconds");
  static constexpr double paced_seconds = [] {
    if constexpr (requires { Configuration::preparation_paced_seconds; }) {
      return double{Configuration::preparation_paced_seconds};
    } else {
      return preparation_seconds;
    }
  }();
  static_assert(
      !requires { Configuration::preparation_paced_seconds; } ||
          (realtime_preparation && paced_seconds > 0 && paced_seconds <= preparation_seconds),
      "preparation_paced_seconds requires preparation_in_real_time and lies within "
      "(0, preparation_seconds]");
  static constexpr int preparation_attempts = 3;
  static constexpr bool custom_prepare =
      requires(Preparation &p) { Configuration::prepare_instance(p); };
  static_assert(
      !custom_prepare || !requires { Configuration::preparation_seconds; },
      "prepare_instance replaces preparation_seconds");
  static_assert(!custom_prepare || main_lifecycle,
                "prepare_instance requires main_thread_lifecycle to run outside the lock");
  static constexpr uint32_t bypass_parameter = [] {
    if constexpr (bypassable) {
      return uint32_t{Configuration::bypass_parameter};
    } else {
      return uint32_t{0};
    }
  }();
  static constexpr bool retains_audio = [] {
    if constexpr (requires { Configuration::shared_retains_audio; }) {
      return Configuration::shared_retains_audio;
    } else {
      return false;
    }
  }();
  // An optional `instance_policy` object adjusts every acquired or reused instance through
  // `configure(AudioUnit, uint32_t channels)` and names that adjustment in `variant()` so
  // differently configured instances never share a cache entry. A policy may also declare
  // `verify(AudioUnit, uint32_t channels)`, which then checks a reused instance instead of
  // configuring it again: the cache keeps instances per variant and channel count.
  static constexpr bool policed = requires { Configuration::instance_policy; };
  static constexpr bool shared = requires {
    Configuration::shared_engine;
    Configuration::shared_stage;
  };
  static constexpr size_t count = Configuration::parameters.size();
  static_assert(Configuration::mapping.size() == count);
  static_assert(
      [] {
        for (const auto &p : Configuration::parameters) {
          if (p.id == bypass_parameter) {
            return p.minimum == 0 && p.maximum == 1;
          }
        }
        return !bypassable;
      }(),
      "bypass_parameter must name a parameter ranging from 0 to 1");
  static constexpr bool gated = requires { Configuration::power_gates; };
  static constexpr auto gates = [] {
    if constexpr (gated) {
      return Configuration::power_gates;
    } else {
      return std::array<uint32_t, 0>{};
    }
  }();
  // The parameter without an Audio Unit target: a gated module's power.
  static constexpr bool targetless(size_t i) { return gated && i == 0; }
  static constexpr bool gated_section(size_t i) {
    for (auto id : gates) {
      if (Configuration::parameters[i].id == id) {
        return true;
      }
    }
    return false;
  }
  static_assert(
      [] {
        if constexpr (!gated) {
          return true;
        } else {
          if (gates.empty() || !bypassable || Configuration::parameters[0].id != bypass_parameter) {
            return false;
          }
          size_t found = 0;
          for (size_t i = 1; i < count; ++i) {
            const auto &p = Configuration::parameters[i];
            if (gated_section(i)) {
              found += std::string_view(p.unit) == "bool" && p.minimum == 0 && p.maximum == 1;
            }
          }
          return found == gates.size();
        }
      }(),
      "power_gates requires the bypass parameter first and names distinct boolean parameters");
  static_assert(gates.size() <= shared_assignments_per_parameter);
  static constexpr size_t targeted_count = count - (gated ? 1 : 0);

  static_assert(
      route_pins_valid<Configuration>(),
      "route_pins must pin distinct targeted parameters other than the route parameter "
      "and power gates, within the route range and target range, and fit one translation");
  static constexpr bool routed = requires { Configuration::route_parameter; };
  static constexpr bool native_branch = requires { Configuration::native_branch_route; };
  static_assert(native_branch_valid<Configuration>(),
                "native branch requires a shared stage before its fence and route pins for its "
                "distinct 0..1 mix and optional bypass parameters, with a discrete route selector");
  static constexpr auto pins = [] {
    if constexpr (routed) {
      return Configuration::route_pins;
    } else {
      return std::array<RoutePin, 0>{};
    }
  }();
  // Index of the route parameter, or count without one.
  static constexpr size_t selector = [] {
    if constexpr (routed) {
      for (size_t i = 0; i < count; ++i) {
        if (Configuration::parameters[i].id == Configuration::route_parameter) {
          return i;
        }
      }
    }
    return count;
  }();
  static constexpr size_t pinned_count = [] {
    size_t n = 0;
    for (size_t k = 0; k < pins.size(); ++k) {
      bool repeated = false;
      for (size_t m = 0; m < k; ++m) {
        repeated = repeated || pins[m].parameter == pins[k].parameter;
      }
      n += repeated ? 0 : 1;
    }
    return n;
  }();
  // Indices of parameters pinned in any route; the route parameter rewrites their targets.
  static constexpr auto pinned_parameters = [] {
    std::array<size_t, pinned_count> result{};
    size_t n = 0;
    for (size_t i = 0; i < count; ++i) {
      bool pinned = false;
      for (const auto &pin : pins) {
        pinned = pinned || pin.parameter == Configuration::parameters[i].id;
      }
      if (pinned) {
        result[n++] = i;
      }
    }
    return result;
  }();

  static uint32_t route(float value) {
    const double last = Configuration::parameters[selector].maximum;
    return static_cast<uint32_t>(std::clamp(std::floor(double(value) + .5), 0.0, last));
  }

  static const RoutePin *pin(size_t i, const std::array<float, count> &values) {
    if constexpr (routed) {
      const auto current = route(values[selector]);
      for (const auto &p : pins) {
        if (p.route == current && p.parameter == Configuration::parameters[i].id) {
          return &p;
        }
      }
    }
    return nullptr;
  }

  static constexpr auto owned_settings = [] {
    if constexpr (requires { Configuration::shared_owned; }) {
      return Configuration::shared_owned;
    } else {
      return std::array<uint32_t, 0>{};
    }
  }();
  static constexpr auto required_settings = [] {
    if constexpr (requires { Configuration::shared_required; }) {
      return Configuration::shared_required;
    } else {
      return std::array<uint32_t, 0>{};
    }
  }();
  static constexpr bool contains(const auto &ids, uint32_t id) {
    for (auto value : ids) {
      if (value == id) {
        return true;
      }
    }
    return false;
  }
  static constexpr bool mapped(uint32_t id) {
    for (size_t i = 0; i < count; ++i) {
      if (!targetless(i) && Configuration::mapping[i].id == id) {
        return true;
      }
    }
    return false;
  }
  static constexpr bool configured(uint32_t id) {
    for (const auto &s : Configuration::settings) {
      if (s.parameter.id == id) {
        return true;
      }
    }
    return false;
  }
  static_assert(
      [] {
        for (auto id : owned_settings) {
          if (!configured(id) || mapped(id)) {
            return false;
          }
        }
        for (auto id : required_settings) {
          if (!configured(id) || mapped(id) || contains(owned_settings, id)) {
            return false;
          }
        }
        return true;
      }(),
      "shared_owned and shared_required must name settings outside the mapping");
  static constexpr size_t unmapped_settings = [] {
    size_t n = 0;
    for (const auto &s : Configuration::settings) {
      n += mapped(s.parameter.id) ? 0 : 1;
    }
    return n;
  }();
  static constexpr size_t owned_count = targeted_count + owned_settings.size();
  static constexpr auto target_table = [] {
    std::array<vs_shared_target, targeted_count + unmapped_settings> result{};
    size_t k = 0;
    for (size_t i = 0; i < count; ++i) {
      if (!targetless(i)) {
        const auto &p = Configuration::mapping[i];
        result[k++] = {p.id, p.name, p.minimum, p.maximum, VS_SHARED_TARGET_OWNED, 0};
      }
    }
    for (const auto &s : Configuration::settings) {
      if (!mapped(s.parameter.id)) {
        const uint32_t role = contains(owned_settings, s.parameter.id) ? VS_SHARED_TARGET_OWNED
                              : contains(required_settings, s.parameter.id)
                                  ? VS_SHARED_TARGET_REQUIRED
                                  : VS_SHARED_TARGET_NEUTRAL;
        result[k++] = {s.parameter.id, s.parameter.name, s.parameter.minimum, s.parameter.maximum,
                       role,           s.value};
      }
    }
    return result;
  }();

  static std::string four_cc(uint32_t value) {
    char text[9];
    std::snprintf(text, sizeof(text), "%08x", value);
    return text;
  }

  static std::string cache_key() {
    std::string key = "au:" + four_cc(Configuration::type) + "-" + four_cc(Configuration::subtype) +
                      "-" + four_cc(Configuration::manufacturer);
    if constexpr (policed) {
      key += "|" + Configuration::instance_policy.variant();
    }
    if constexpr (preparation_seconds > 0) {
      key += "|settle=" + std::to_string(std::bit_cast<uint64_t>(preparation_seconds));
    }
    if constexpr (realtime_preparation) {
      key += "|paced=" + std::to_string(std::bit_cast<uint64_t>(paced_seconds));
    }
    if constexpr (main_lifecycle) {
      key += "|main-lifecycle";
    }
    if constexpr (custom_prepare) {
      key += "|prepare-instance";
    }
    if constexpr (preserve_preparation) {
      key += "|preserve-first-block";
    }
    return key;
  }

  enum class State { idle, prepared, failed };
  static constexpr bool preserve_preparation = [] {
    if constexpr (requires { Configuration::preserve_preparation_on_start; }) {
      return bool(Configuration::preserve_preparation_on_start);
    }
    return false;
  }();
  bool first_fresh_block_ = false;
  State state_ = State::idle;
  std::array<float, count> values_{};
  uint32_t channels_ = 0;
  uint32_t max_frames_ = 0;
#ifdef __APPLE__
  AudioUnit unit_ = nullptr;
  bool provide_tempo_ = false;
  const vs_host_instance_cache *cache_ = nullptr;
  std::string cache_signature_;
  std::string acquired_cache_key_;
  CFDataRef defaults_ = nullptr;
  // False when a section of the engine keeps audio across Reset (VS_SHARED_RETAINS_AUDIO).
  bool reusable_ = true;
  bool fresh_ = false;
  // The last value written to each target while preparing, for real-time preparation to verify.
  std::vector<vs_shared_assignment> intended_;
  std::array<std::array<bool, 128>, 16> held_notes_{};
  struct CallbackState {
    uint32_t channels = 0;
    uint32_t render_frames = 0;
    bool provide_tempo = false;
    double bpm = 120;
    double beat = 0;
    std::array<std::vector<float>, 2> buffers;
  };
  // Main-thread teardown owns its callback state even after destroy deletes this module.
  std::conditional_t<main_lifecycle, CallbackState *, CallbackState> callbacks_{};
  CallbackState &callbacks() {
    if constexpr (main_lifecycle) {
      return *callbacks_;
    } else {
      return callbacks_;
    }
  }
  double sample_time_ = 0;
  double sample_rate_ = 48000;

  static OSStatus beat_and_tempo(void *context, Float64 *beat, Float64 *tempo) {
    auto &state = *static_cast<CallbackState *>(context);
    if (state.provide_tempo) {
      if (tempo) {
        *tempo = state.bpm;
      }
      if (beat) {
        *beat = state.beat;
      }
    }
    return noErr;
  }

  static OSStatus input(void *context, AudioUnitRenderActionFlags *, const AudioTimeStamp *, UInt32,
                        UInt32 frames, AudioBufferList *data) {
    auto &state = *static_cast<CallbackState *>(context);
    if (!data || frames != state.render_frames || data->mNumberBuffers != state.channels) {
      return kAudio_ParamError;
    }
    for (uint32_t c = 0; c < state.channels; ++c) {
      auto &buffer = data->mBuffers[c];
      const auto bytes = frames * static_cast<uint32_t>(sizeof(float));
      if (buffer.mData) {
        if (buffer.mDataByteSize < bytes) {
          return kAudio_ParamError;
        }
        std::memcpy(buffer.mData, state.buffers[c].data(), bytes);
      } else {
        buffer.mData = state.buffers[c].data();
      }
      buffer.mNumberChannels = 1;
      buffer.mDataByteSize = bytes;
    }
    return noErr;
  }

  bool validate(uint32_t id, const char *expected, float minimum, float maximum) const {
    AudioUnitParameterInfo info{};
    UInt32 size = sizeof(info);
    if (AudioUnitGetProperty(unit_, kAudioUnitProperty_ParameterInfo, kAudioUnitScope_Global, id,
                             &info, &size) != noErr) {
      return false;
    }
    char name[256]{};
    if (info.flags & kAudioUnitParameterFlag_HasCFNameString) {
      if (info.cfNameString) {
        CFStringGetCString(info.cfNameString, name, sizeof(name), kCFStringEncodingUTF8);
        if (info.flags & kAudioUnitParameterFlag_CFNameRelease) {
          CFRelease(info.cfNameString);
        }
      }
    } else {
      std::memcpy(name, info.name, sizeof(info.name));
    }
    return std::strcmp(name, expected) == 0 && info.minValue == minimum &&
           info.maxValue == maximum && (info.flags & kAudioUnitParameterFlag_IsWritable);
  }

  bool validate(const AudioUnitParameter &parameter) const {
    return validate(parameter.id, parameter.name, parameter.minimum, parameter.maximum);
  }

  bool parameter_values(std::vector<vs_shared_assignment> &values) const {
    values.clear();
    UInt32 size = 0;
    if (AudioUnitGetPropertyInfo(unit_, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0,
                                 &size, nullptr) != noErr) {
      return false;
    }
    std::vector<AudioUnitParameterID> ids(size / sizeof(AudioUnitParameterID));
    if (!ids.empty() &&
        AudioUnitGetProperty(unit_, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0,
                             ids.data(), &size) != noErr) {
      return false;
    }
    values.reserve(ids.size());
    for (auto id : ids) {
      AudioUnitParameterValue value = 0;
      if (AudioUnitGetParameter(unit_, id, kAudioUnitScope_Global, 0, &value) != noErr) {
        return false;
      }
      values.push_back({id, value});
    }
    return true;
  }

  // Global parameter values of a freshly configured instance, so a reused instance can start
  // from the same values whatever its previous owner changed.
  CFDataRef capture_defaults() const {
    std::vector<vs_shared_assignment> values;
    if (!parameter_values(values)) {
      return nullptr;
    }
    return CFDataCreate(nullptr, reinterpret_cast<const UInt8 *>(values.data()),
                        static_cast<CFIndex>(values.size() * sizeof(vs_shared_assignment)));
  }

  // Gives the instance a value, remembering it while preparing for real-time verification.
  bool write_target(AudioUnitParameterID target, float value) {
    if (AudioUnitSetParameter(unit_, target, kAudioUnitScope_Global, 0, value, 0) != noErr) {
      return false;
    }
    if constexpr (realtime_preparation) {
      if (state_ != State::prepared) {
        const auto found = std::find_if(intended_.begin(), intended_.end(),
                                        [&](const auto &entry) { return entry.target == target; });
        if (found == intended_.end()) {
          intended_.push_back({target, value});
        } else {
          found->value = value;
        }
      }
    }
    return true;
  }

  // Whether a plugin holding `actual` still holds the written `intended`, as it represents
  // values: clamped to the parameter's range, any index less than one step away for indexed and
  // boolean parameters (it may round or truncate), and within single-precision normalization
  // error of the range otherwise.
  static bool holds(const AudioUnitParameterInfo &info, float intended, float actual) {
    if (!std::isfinite(actual)) {
      return false;
    }
    const auto low = std::min(info.minValue, info.maxValue);
    const auto high = std::max(info.minValue, info.maxValue);
    const double expected = std::clamp(intended, low, high);
    const double difference = std::abs(double(actual) - expected);
    if (info.unit == kAudioUnitParameterUnit_Indexed ||
        info.unit == kAudioUnitParameterUnit_Boolean) {
      return difference < 1;
    }
    return difference <= (double(high) - low) * 1e-5;
  }

  enum class Verification { kept, reverted, unreadable };
  struct Difference {
    AudioUnitParameterID target;
    float intended;
    float actual;
  };

  // Compares every value written while preparing with what the plugin holds now. Some plugins
  // revert their parameters to their own defaults from work they queue on the main thread at
  // startup; parameters the wrapper never wrote are not its concern.
  Verification verify_intended(std::vector<Difference> &differences) const {
    differences.clear();
    for (const auto &entry : intended_) {
      AudioUnitParameterInfo info{};
      UInt32 size = sizeof(info);
      AudioUnitParameterValue actual = 0;
      const bool described =
          AudioUnitGetProperty(unit_, kAudioUnitProperty_ParameterInfo, kAudioUnitScope_Global,
                               entry.target, &info, &size) == noErr;
      if (described && (info.flags & kAudioUnitParameterFlag_HasCFNameString) &&
          (info.flags & kAudioUnitParameterFlag_CFNameRelease) && info.cfNameString) {
        CFRelease(info.cfNameString);
      }
      if (!described ||
          AudioUnitGetParameter(unit_, entry.target, kAudioUnitScope_Global, 0, &actual) != noErr) {
        differences = {{entry.target, entry.value, 0}};
        return Verification::unreadable;
      }
      if (!holds(info, entry.value, actual)) {
        differences.push_back({entry.target, entry.value, actual});
      }
    }
    return differences.empty() ? Verification::kept : Verification::reverted;
  }

  bool restore_defaults() const {
    if (!defaults_ || CFGetTypeID(defaults_) != CFDataGetTypeID() ||
        CFDataGetLength(defaults_) % static_cast<CFIndex>(sizeof(vs_shared_assignment)) != 0) {
      return false;
    }
    const auto entries =
        static_cast<size_t>(CFDataGetLength(defaults_)) / sizeof(vs_shared_assignment);
    const auto *bytes = CFDataGetBytePtr(defaults_);
    for (size_t i = 0; i < entries; ++i) {
      vs_shared_assignment entry;
      std::memcpy(&entry, bytes + i * sizeof(entry), sizeof(entry));
      if (AudioUnitSetParameter(unit_, entry.target, kAudioUnitScope_Global, 0, entry.value, 0) !=
          noErr) {
        return false;
      }
    }
    return true;
  }

  static constexpr bool verified_on_reuse = requires(AudioUnit unit) {
    { Configuration::instance_policy.verify(unit, uint32_t{}) } -> std::convertible_to<bool>;
  };

  // Obtains an instance in its configured channel mode with default parameter values, reusing a
  // cached one when the host offers it.
  vs_result acquire(uint32_t channels, bool &reused) {
    reused = false;
    acquired_cache_key_ = cache_key() + "|channels=" + std::to_string(channels);
    if (cache_) {
      void *payload = nullptr;
      unit_ = static_cast<AudioUnit>(cache_->acquire(cache_->context, acquired_cache_key_.c_str(),
                                                     cache_signature_.c_str(), &payload));
      defaults_ = static_cast<CFDataRef>(payload);
      if (unit_) {
        reused = true;
        if constexpr (verified_on_reuse) {
          if (!Configuration::instance_policy.verify(unit_, channels)) {
            return VS_ERROR_UNSUPPORTED;
          }
        } else if constexpr (policed) {
          if (!Configuration::instance_policy.configure(unit_, channels)) {
            return VS_ERROR_UNSUPPORTED;
          }
        }
        return restore_defaults() ? VS_OK : VS_ERROR_UNSUPPORTED;
      }
    }
    AudioComponentDescription description{Configuration::type, Configuration::subtype,
                                          Configuration::manufacturer, 0, 0};
    auto component = AudioComponentFindNext(nullptr, &description);
    if (!component) {
      return VS_ERROR_UNSUPPORTED;
    }
    if (AudioComponentInstanceNew(component, &unit_) != noErr) {
      unit_ = nullptr;
      return VS_ERROR_PROCESS;
    }
    if constexpr (policed) {
      if (!Configuration::instance_policy.configure(unit_, channels)) {
        return VS_ERROR_UNSUPPORTED;
      }
    }
    if (cache_) {
      defaults_ = capture_defaults();
      return defaults_ ? VS_OK : VS_ERROR_UNSUPPORTED;
    }
    return VS_OK;
  }

  // Uninitializes and disposes an instance, then frees what it referenced, on the main thread:
  // plugins built on JUCE post message-thread work referencing the instance, and disposing it on
  // another thread lets that work run on a destroyed instance. Off the main thread the work is
  // posted, which pins this library until it has run. Without the lifecycle lock the instance and
  // what it references are abandoned rather than disposed unserialized. The main thread never
  // waits for the lock: while another thread holds it, for example creating a plugin, the
  // disposal is retried later from a main run-loop timer.
  struct Disposal {
    AudioUnit unit;
    CFDataRef defaults;
    CallbackState *callbacks;
    double retry_seconds = .002;

    void operator()() {
      PluginLifecycleLock lock(std::try_to_lock);
      switch (unit ? lock.acquisition() : PluginLifecycleLock::Acquisition::held) {
      case PluginLifecycleLock::Acquisition::busy: {
        auto later = *this;
        later.retry_seconds = std::min(retry_seconds * 2, .05);
        if (!MainThread::post_after(retry_seconds, later)) {
          std::fputs("Audio Unit: could not queue main-thread disposal\n", stderr);
        }
        return;
      }
      case PluginLifecycleLock::Acquisition::failed:
        return;
      case PluginLifecycleLock::Acquisition::held:
        if (unit && (AudioUnitUninitialize(unit), AudioComponentInstanceDispose(unit) != noErr)) {
          return;
        }
        delete callbacks;
        if (defaults) {
          CFRelease(defaults);
        }
        return;
      }
    }
  };

  static void dispose_on_main(AudioUnit unit, CFDataRef defaults, CallbackState *callbacks) {
    Disposal disposal{unit, defaults, callbacks};
    if (!unit || MainThread::on_main()) {
      disposal();
    } else if (!MainThread::post(disposal)) {
      std::fputs("Audio Unit: could not queue main-thread disposal\n", stderr);
    }
  }

  // Disposes an instance with its defaults snapshot on behalf of the host cache.
  static void dispose_for_cache(void *instance, void *payload) {
    dispose_on_main(static_cast<AudioUnit>(instance), static_cast<CFDataRef>(payload), nullptr);
  }

  // Detaches the render and host callbacks, which point into this module, from an instance that
  // outlives it in the cache or in a posted disposal.
  bool detach_callbacks() const {
    AURenderCallbackStruct none{};
    HostCallbackInfo callbacks{};
    return AudioUnitSetProperty(unit_, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input,
                                0, &none, sizeof(none)) == noErr &&
           AudioUnitSetProperty(unit_, kAudioUnitProperty_HostCallbacks, kAudioUnitScope_Global, 0,
                                &callbacks, sizeof(callbacks)) == noErr;
  }

  // Returns the instance to the cache when it can be detached cleanly, otherwise disposes it.
  // The cache is called after the lifecycle lock is released because evicting takes it too.
  void detach(bool reusable) {
    if constexpr (main_lifecycle) {
      dispose_on_main(std::exchange(unit_, nullptr), std::exchange(defaults_, nullptr),
                      std::exchange(callbacks_, nullptr));
      held_notes_ = {};
      acquired_cache_key_.clear();
      return;
    }
    AudioUnit cached = nullptr;
    CFDataRef payload = nullptr;
    {
      PluginLifecycleLock lock;
      if (!lock) {
        // Disposing without the lifecycle lock risks the concurrent-lifecycle crash, so the
        // instance is abandoned: it is never rendered again and is reclaimed at process exit.
        unit_ = nullptr;
      }
      if (unit_) {
        reusable = reusable && cache_ && defaults_ && reusable_;
        if constexpr (accepts_midi) {
          reusable = reusable && panic_midi() == VS_OK;
        }
        if (reusable) {
          AudioUnitUninitialize(unit_);
          reusable = detach_callbacks();
        }
        if (reusable) {
          cached = unit_;
          payload = defaults_;
          defaults_ = nullptr;
        } else {
          detach_callbacks();
          dispose_on_main(unit_, nullptr, nullptr);
        }
        unit_ = nullptr;
      }
      if (defaults_) {
        CFRelease(defaults_);
        defaults_ = nullptr;
      }
    }
    if (cached) {
      cache_->release(cache_->context, acquired_cache_key_.c_str(), cache_signature_.c_str(),
                      cached, const_cast<void *>(static_cast<const void *>(payload)),
                      dispose_for_cache);
    }
    held_notes_ = {};
    cache_ = nullptr;
    cache_signature_.clear();
    acquired_cache_key_.clear();
  }

  vs_result panic_midi() {
    for (uint32_t channel = 0; channel < 16; ++channel) {
      for (uint32_t cc : {64u, 120u, 123u}) {
        if (MusicDeviceMIDIEvent(unit_, 0xb0 | channel, cc, 0, 0) != noErr) {
          return VS_ERROR_PROCESS;
        }
      }
      for (uint32_t note = 0; note < 128; ++note) {
        if (held_notes_[channel][note] &&
            MusicDeviceMIDIEvent(unit_, 0x80 | channel, note, 0, 0) != noErr) {
          return VS_ERROR_PROCESS;
        }
        held_notes_[channel][note] = false;
      }
    }
    return VS_OK;
  }
#endif

  static float native_value(size_t i, float value) {
    if constexpr (requires { Configuration::native_value(i, value); }) {
      return Configuration::native_value(i, value);
    } else {
      const auto &p = Configuration::parameters[i];
      const auto &mapped = Configuration::mapping[i];
      return mapped.minimum +
             (value - p.minimum) / (p.maximum - p.minimum) * (mapped.maximum - mapped.minimum);
    }
  }

  static float canonical(size_t i, float value) {
    const auto &parameter = Configuration::parameters[i];
    for (const auto &entry : metadata) {
      if (entry.parameter_id == parameter.id && entry.kind != VS_PARAMETER_CONTINUOUS &&
          entry.steps >= 2) {
        const auto span = double(parameter.maximum) - parameter.minimum;
        const auto step =
            std::round((double(value) - parameter.minimum) / span * (entry.steps - 1));
        return static_cast<float>(parameter.minimum + step * span / (entry.steps - 1));
      }
    }
    return value;
  }

  // The target value of parameter i holding value while the module's parameters hold values.
  template <bool Native = false>
  static float effective(size_t i, float value, const std::array<float, count> &values) {
    const auto *held = pin(i, values);
    if constexpr (Native) {
      const auto id = Configuration::parameters[i].id;
      if (held && held->route == Configuration::native_branch_route &&
          (id == Configuration::native_branch_mix_parameter ||
           (bypassable && id == bypass_parameter))) {
        held = nullptr;
      }
    }
    if (held) {
      return held->value;
    }
    return gated_section(i) && values[0] < .5f ? Configuration::mapping[i].minimum
                                               : native_value(i, value);
  }

#ifdef __APPLE__
  vs_result apply(size_t i, float value) {
    value = canonical(i, value);
    if constexpr (requires { Configuration::midi_mode_parameter; }) {
      if (unit_ && state_ == State::prepared &&
          Configuration::parameters[i].id == Configuration::midi_mode_parameter &&
          (values_[i] >= .5f) != (value >= .5f)) {
        if (panic_midi() != VS_OK) {
          return VS_ERROR_PROCESS;
        }
      }
    }
    auto next = values_;
    next[i] = value;
    auto write = [&](size_t j) {
      return write_target(Configuration::mapping[j].id, effective(j, next[j], next));
    };
    if (unit_ && targetless(i)) {
      for (size_t j = 0; j < count; ++j) {
        if (gated_section(j) && !write(j)) {
          return VS_ERROR_PROCESS;
        }
      }
    } else if (unit_) {
      if (!write(i)) {
        return VS_ERROR_PROCESS;
      }
      if (i == selector) {
        for (auto j : pinned_parameters) {
          if (!write(j)) {
            return VS_ERROR_PROCESS;
          }
        }
      }
    }
    values_[i] = value;
    return VS_OK;
  }

  vs_result render(const vs_process_block &block, uint32_t offset, uint32_t frames) {
    auto &state = callbacks();
    state.bpm = 120;
    state.beat = sample_time_ / sample_rate_ * 2;
    if (block.struct_size >= offsetof(vs_process_block, tempo) + sizeof(block.tempo) &&
        block.tempo && block.tempo->struct_size >= sizeof(vs_tempo_context) &&
        (block.tempo->flags & VS_TEMPO_VALID) && std::isfinite(block.tempo->bpm) &&
        block.tempo->bpm > 0 && std::isfinite(block.tempo->beat_position)) {
      state.bpm = block.tempo->bpm;
      state.beat =
          block.tempo->beat_position + static_cast<double>(offset) / sample_rate_ * state.bpm / 60;
    }
    struct Buffers {
      UInt32 count;
      AudioBuffer buffers[2];
    } output{};
    output.count = channels_;
    for (uint32_t c = 0; c < channels_; ++c) {
      // A non-finite sample can poison an Audio Unit's filter state until it is reinitialized.
      std::transform(block.inputs[c] + offset, block.inputs[c] + offset + frames,
                     state.buffers[c].data(),
                     [](float sample) { return std::isfinite(sample) ? sample : 0.0f; });
      output.buffers[c] = {1, frames * static_cast<UInt32>(sizeof(float)),
                           block.outputs[c] + offset};
    }
    state.render_frames = frames;
    AudioTimeStamp timestamp{};
    timestamp.mFlags = kAudioTimeStampSampleTimeValid;
    timestamp.mSampleTime = sample_time_;
    AudioUnitRenderActionFlags flags = 0;
    const auto result = AudioUnitRender(unit_, &flags, &timestamp, 0, frames,
                                        reinterpret_cast<AudioBufferList *>(&output));
    state.render_frames = 0;
    sample_time_ += frames;
    if (result != noErr) {
      return VS_ERROR_PROCESS;
    }
    for (uint32_t c = 0; c < channels_; ++c) {
      if (output.buffers[c].mData != block.outputs[c] + offset) {
        return VS_ERROR_PROCESS;
      }
      for (uint32_t f = 0; f < frames; ++f) {
        if (!std::isfinite(block.outputs[c][offset + f])) {
          return VS_ERROR_PROCESS;
        }
      }
    }
    return VS_OK;
  }

  // Formats, callbacks and validation shared by ordinary and engine preparation. The caller has
  // applied every parameter value; on success the instance is initialized.
  vs_result finish_prepare(const vs_prepare_info &info, vs_module_timing &timing, bool reused) {
    if (provide_tempo_) {
      HostCallbackInfo callbacks{};
      callbacks.hostUserData = &this->callbacks();
      callbacks.beatAndTempoProc = beat_and_tempo;
      if (AudioUnitSetProperty(unit_, kAudioUnitProperty_HostCallbacks, kAudioUnitScope_Global, 0,
                               &callbacks, sizeof(callbacks)) != noErr) {
        return VS_ERROR_UNSUPPORTED;
      }
    }
    AudioStreamBasicDescription format{};
    format.mSampleRate = info.sample_rate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = static_cast<UInt32>(kAudioFormatFlagsNativeFloatPacked) |
                          static_cast<UInt32>(kAudioFormatFlagIsNonInterleaved);
    format.mBytesPerPacket = format.mBytesPerFrame = sizeof(float);
    format.mFramesPerPacket = 1;
    format.mChannelsPerFrame = info.channels;
    format.mBitsPerChannel = 32;
    if (AudioUnitSetProperty(unit_, kAudioUnitProperty_MaximumFramesPerSlice,
                             kAudioUnitScope_Global, 0, &info.max_frames,
                             sizeof(info.max_frames)) != noErr) {
      return VS_ERROR_UNSUPPORTED;
    }
    for (auto scope : {kAudioUnitScope_Input, kAudioUnitScope_Output}) {
      if (AudioUnitSetProperty(unit_, kAudioUnitProperty_StreamFormat, scope, 0, &format,
                               sizeof(format)) != noErr) {
        return VS_ERROR_UNSUPPORTED;
      }
    }
    AURenderCallbackStruct callback{input, &callbacks()};
    if (AudioUnitSetProperty(unit_, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0,
                             &callback, sizeof(callback)) != noErr) {
      return VS_ERROR_UNSUPPORTED;
    }
    if (AudioUnitInitialize(unit_) != noErr) {
      return VS_ERROR_UNSUPPORTED;
    }
    if (reused && AudioUnitReset(unit_, kAudioUnitScope_Global, 0) != noErr) {
      return VS_ERROR_UNSUPPORTED;
    }
    Float64 latency = 0, tail = 0;
    UInt32 size = sizeof(latency);
    if (AudioUnitGetProperty(unit_, kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0, &latency,
                             &size) != noErr) {
      return VS_ERROR_UNSUPPORTED;
    }
    size = sizeof(tail);
    if (AudioUnitGetProperty(unit_, kAudioUnitProperty_TailTime, kAudioUnitScope_Global, 0, &tail,
                             &size) != noErr) {
      return VS_ERROR_UNSUPPORTED;
    }
    if (!std::isfinite(latency) || latency < 0 || latency * info.sample_rate > UINT32_MAX ||
        !std::isfinite(tail) || tail < 0 || tail > 3600) {
      return VS_ERROR_UNSUPPORTED;
    }
    timing.latency_frames = static_cast<uint32_t>(std::ceil(latency * info.sample_rate));
    timing.tail_frames = static_cast<uint64_t>(std::ceil(tail * info.sample_rate));
    return VS_OK;
  }

  vs_result readiness(const vs_prepare_info &info, vs_module_timing &timing) {
    if constexpr (custom_prepare) {
      Preparation steps(
          unit_, this,
          [](void *context, const vs_process_block &block) {
            return static_cast<AudioUnitModule *>(context)->render(block, 0, block.frames);
          },
          sample_time_, info);
      if (!Configuration::prepare_instance(steps) || steps.failed_ || steps.remaining_) {
        return VS_ERROR_PROCESS;
      }
      timing.tail_frames = std::max(timing.tail_frames, steps.tail_floor_frames_);
      callbacks().beat = 0;
    }
    if constexpr (preparation_seconds > 0) {
      const auto frames = static_cast<uint32_t>(std::ceil(preparation_seconds * info.sample_rate));
      // A fresh instance's startup work on the main thread must overlap rendering, so off the
      // main thread its preparation advances at playback speed. On the main thread that work
      // cannot run during preparation.
      const bool paced = realtime_preparation && fresh_ && !MainThread::on_main();
      std::vector<Difference> differences;
      for (int attempt = 1;; ++attempt) {
        if (!prepare_silence(frames, info, paced)) {
          return VS_ERROR_PROCESS;
        }
        if constexpr (realtime_preparation) {
          switch (verify_intended(differences)) {
          case Verification::kept:
            break;
          case Verification::unreadable:
            std::fprintf(stderr, "Audio Unit preparation: could not read parameter %u\n",
                         unsigned(differences.front().target));
            return VS_ERROR_PROCESS;
          case Verification::reverted:
            if (attempt == preparation_attempts) {
              const auto &first = differences.front();
              std::fprintf(stderr,
                           "Audio Unit preparation: the plugin kept reverting parameter %u "
                           "(given %g, holds %g; %zu differ)\n",
                           unsigned(first.target), double(first.intended), double(first.actual),
                           differences.size());
              return VS_ERROR_UNSUPPORTED;
            }
            for (const auto &difference : differences) {
              if (AudioUnitSetParameter(unit_, difference.target, kAudioUnitScope_Global, 0,
                                        difference.intended, 0) != noErr) {
                return VS_ERROR_PROCESS;
              }
            }
            continue;
          }
        }
        break;
      }
      callbacks().beat = 0;
    }
    return VS_OK;
  }

  // Renders `frames` of silence ending at frame zero, without resetting the AU's newly settled
  // state, and when paced no faster than playback speed.
  bool prepare_silence(uint32_t frames, const vs_prepare_info &info, bool paced) {
    std::vector<float> silence;
    std::array<std::vector<float>, 2> output;
    sample_time_ = -static_cast<double>(frames);
    const auto begun = std::chrono::steady_clock::now();
    const auto paced_frames = static_cast<uint64_t>(std::ceil(paced_seconds * info.sample_rate));
    uint64_t rendered = 0;
    return render_silence(
        frames, info.max_frames, info.channels, silence, output,
        [&](const vs_process_block &audio, uint32_t count) {
          if (render(audio, 0, count) != VS_OK) {
            return false;
          }
          rendered += count;
          if (paced) {
            std::this_thread::sleep_until(
                begun +
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double>(
                    static_cast<double>(std::min(rendered, paced_frames)) / info.sample_rate)));
          }
          return true;
        });
  }

  template <class Setup>
  vs_result prepare_unit(const vs_prepare_info &info, vs_module_timing &timing, bool tempo,
                         Setup &&setup) {
    auto lifecycle = [&]() -> vs_result {
      PluginLifecycleLock lock;
      if (!lock) {
        cache_ = nullptr;
        cache_signature_.clear();
        return VS_ERROR_PROCESS;
      }
      try {
        bool reused = false;
        if (const auto acquired = acquire(info.channels, reused); acquired != VS_OK) {
          detach(false);
          return acquired;
        }
        if constexpr (main_lifecycle) {
          callbacks_ = new CallbackState;
        }
        channels_ = info.channels;
        sample_rate_ = info.sample_rate;
        provide_tempo_ = tempo;
        max_frames_ = info.max_frames;
        callbacks().channels = channels_;
        callbacks().provide_tempo = tempo;
        for (uint32_t c = 0; c < channels_; ++c) {
          callbacks().buffers[c].resize(max_frames_);
        }
        fresh_ = !reused;
        intended_.clear();
        if (!setup() || finish_prepare(info, timing, reused) != VS_OK) {
          detach(false);
          return VS_ERROR_UNSUPPORTED;
        }
        return VS_OK;
      } catch (...) {
        if constexpr (main_lifecycle) {
          detach(false);
          return VS_ERROR_PROCESS;
        } else {
          throw;
        }
      }
    };
    try {
      vs_result result = VS_ERROR_PROCESS;
      if constexpr (main_lifecycle) {
        if (MainThread::call([&] { result = lifecycle(); }, 5) != MainThread::Result::done) {
          std::fputs("Audio Unit preparation: the main thread did not become available\n", stderr);
          return VS_ERROR_PROCESS;
        }
      } else {
        result = lifecycle();
      }
      // Outside the lifecycle lock: preparation can wait on the main thread, whose posted
      // disposals take the lock.
      if (result == VS_OK && (result = readiness(info, timing)) != VS_OK) {
        detach(false);
        if constexpr (!main_lifecycle) {
          result = VS_ERROR_UNSUPPORTED;
        }
      }
      if (result == VS_OK) {
        first_fresh_block_ = fresh_;
        state_ = State::prepared;
      } else {
        sample_time_ = 0;
      }
      return result;
    } catch (...) {
      detach(false);
      sample_time_ = 0;
      return VS_ERROR_PROCESS;
    }
  }

  vs_result run(const vs_process_block *block, const vs_parameter_event *targets,
                uint32_t target_count) {
    if (!block || block->struct_size < VS_PROCESS_BLOCK_V1_SIZE) {
      return VS_ERROR_ARGUMENT;
    }
    if (state_ != State::prepared) {
      return VS_ERROR_STATE;
    }
    if (block->channels != channels_ || !block->frames || block->frames > max_frames_ ||
        !block->inputs || !block->outputs || (block->event_count && !block->events) ||
        (target_count && !targets)) {
      return VS_ERROR_ARGUMENT;
    }
    for (uint32_t c = 0; c < block->channels; ++c) {
      if (!block->inputs[c] || !block->outputs[c]) {
        return VS_ERROR_ARGUMENT;
      }
    }
    for (uint32_t i = 0; i < block->event_count; ++i) {
      const auto &e = block->events[i];
      if (!valid(index(e.parameter_id), e.value) || e.frame_offset >= block->frames ||
          (i && e.frame_offset < block->events[i - 1].frame_offset)) {
        return VS_ERROR_ARGUMENT;
      }
    }
    for (uint32_t i = 0; i < target_count; ++i) {
      const auto &e = targets[i];
      if (!std::isfinite(e.value) || e.frame_offset >= block->frames ||
          (i && e.frame_offset < targets[i - 1].frame_offset)) {
        return VS_ERROR_ARGUMENT;
      }
    }
    const bool has_midi =
        block->struct_size >= offsetof(vs_process_block, midi_event_size) + sizeof(uint32_t);
    const uint32_t midi_count = has_midi ? block->midi_event_count : 0;
    if (midi_count &&
        (!accepts_midi || !block->midi_events || block->midi_event_size < sizeof(vs_midi_event) ||
         midi_count > 1024 || block->midi_event_size > 4096)) {
      return VS_ERROR_ARGUMENT;
    }
    auto midi_at = [&](uint32_t i) {
      vs_midi_event result{};
      std::memcpy(&result,
                  static_cast<const uint8_t *>(block->midi_events) +
                      static_cast<size_t>(i) * block->midi_event_size,
                  sizeof(result));
      return result;
    };
    uint32_t previous_midi_offset = 0;
    for (uint32_t i = 0; i < midi_count; ++i) {
      const auto e = midi_at(i);
      if (!vs_midi_event_valid(&e) || e.frame_offset >= block->frames ||
          e.frame_offset < previous_midi_offset) {
        return VS_ERROR_ARGUMENT;
      }
      previous_midi_offset = e.frame_offset;
    }
    if ((block->flags & VS_PROCESS_DISCONTINUITY) &&
        !(preserve_preparation && first_fresh_block_)) {
      reset(this);
    }
    first_fresh_block_ = false;
    if (state_ != State::prepared) {
      return VS_ERROR_PROCESS;
    }
    if constexpr (accepts_midi) {
      if (block->flags & (VS_PROCESS_MIDI_PANIC | VS_PROCESS_DISCONTINUITY)) {
        if (panic_midi() != VS_OK) {
          return VS_ERROR_PROCESS;
        }
      }
    }
    uint32_t offset = 0, event = 0, target = 0, midi = 0;
    while (offset < block->frames) {
      while (event < block->event_count && block->events[event].frame_offset == offset) {
        const auto &e = block->events[event++];
        if (apply(index(e.parameter_id), e.value) != VS_OK) {
          state_ = State::failed;
          return VS_ERROR_PROCESS;
        }
      }
      while (target < target_count && targets[target].frame_offset == offset) {
        const auto &e = targets[target++];
        if (AudioUnitSetParameter(unit_, e.parameter_id, kAudioUnitScope_Global, 0, e.value, 0) !=
            noErr) {
          state_ = State::failed;
          return VS_ERROR_PROCESS;
        }
      }
      uint32_t end = block->frames;
      if (event < block->event_count) {
        end = std::min(end, block->events[event].frame_offset);
      }
      if (target < target_count) {
        end = std::min(end, targets[target].frame_offset);
      }
      while (midi < midi_count && midi_at(midi).frame_offset < end) {
        const auto e = midi_at(midi++);
        if ((e.status & 0xf0) == 0x90 || (e.status & 0xf0) == 0x80) {
          held_notes_[e.status & 15][e.data1] = (e.status & 0xf0) == 0x90 && e.data2 != 0;
        }
        if (MusicDeviceMIDIEvent(unit_, e.status, e.data1, e.data2, e.frame_offset - offset) !=
            noErr) {
          state_ = State::failed;
          return VS_ERROR_PROCESS;
        }
      }
      if (render(*block, offset, end - offset) != VS_OK) {
        state_ = State::failed;
        for (uint32_t c = 0; c < block->channels; ++c) {
          std::fill_n(block->outputs[c], block->frames, 0);
        }
        return VS_ERROR_PROCESS;
      }
      offset = end;
    }
    return VS_OK;
  }
#else
  vs_result apply(size_t i, float value) {
    values_[i] = canonical(i, value);
    return VS_OK;
  }
#endif

  static constexpr size_t index(uint32_t id) {
    for (size_t i = 0; i < count; ++i) {
      if (Configuration::parameters[i].id == id) {
        return i;
      }
    }
    return count;
  }

  static bool valid(size_t i, float value) {
    return i < count && std::isfinite(value) && value >= Configuration::parameters[i].minimum &&
           value <= Configuration::parameters[i].maximum;
  }

  static bool valid_info(const vs_prepare_info *info, const vs_module_timing *timing) {
    return info && timing && info->struct_size >= sizeof(*info) &&
           timing->struct_size >= sizeof(*timing) && std::isfinite(info->sample_rate) &&
           info->sample_rate >= 8000 && info->sample_rate <= 384000 && info->max_frames &&
           info->max_frames <= 65536 && info->channels >= 1 && info->channels <= 2;
  }

  static vs_result create(void **instance) {
    if (!instance) {
      return VS_ERROR_ARGUMENT;
    }
    auto *self = new (std::nothrow) AudioUnitModule;
    *instance = self;
    if (!self) {
      return VS_ERROR_PROCESS;
    }
    for (size_t i = 0; i < count; ++i) {
      self->values_[i] = canonical(i, Configuration::parameters[i].default_value);
    }
    return VS_OK;
  }

  static void release(void *instance) {
    if (!instance) {
      return;
    }
    auto &self = *static_cast<AudioUnitModule *>(instance);
#ifdef __APPLE__
    self.detach(self.state_ == State::prepared);
    self.sample_time_ = 0;
#endif
    self.state_ = State::idle;
  }

  static void destroy(void *instance) {
    release(instance);
    delete static_cast<AudioUnitModule *>(instance);
  }

  static vs_result prepare(void *instance, const vs_prepare_info *info, vs_module_timing *timing) {
    if (!instance || !valid_info(info, timing)) {
      return VS_ERROR_ARGUMENT;
    }
    auto &self = *static_cast<AudioUnitModule *>(instance);
    if (self.state_ != State::idle) {
      return VS_ERROR_STATE;
    }
#ifdef __APPLE__
    self.cache_ = nullptr;
    return self.prepare_unit(*info, *timing, host_tempo, [&] {
      bool valid = true;
      for (size_t i = 0; i < count; ++i) {
        valid = valid && (targetless(i) || self.validate(Configuration::mapping[i]));
      }
      for (const auto &s : Configuration::settings) {
        valid = valid && self.validate(s.parameter);
      }
      for (const auto &s : Configuration::settings) {
        valid = valid && self.write_target(s.parameter.id, s.value);
      }
      for (size_t i = 0; valid && i < count; ++i) {
        valid = self.apply(i, self.values_[i]) == VS_OK;
      }
      return valid;
    });
#else
    return VS_ERROR_UNSUPPORTED;
#endif
  }

  static void reset(void *instance) {
    if (!instance) {
      return;
    }
#ifdef __APPLE__
    auto &self = *static_cast<AudioUnitModule *>(instance);
    if (self.state_ == State::prepared) {
      self.first_fresh_block_ = false;
      if (AudioUnitReset(self.unit_, kAudioUnitScope_Global, 0) != noErr) {
        self.state_ = State::failed;
      }
      self.sample_time_ = 0;
    }
#endif
  }

  static vs_result process(void *instance, const vs_process_block *block) {
    if (!instance) {
      return VS_ERROR_ARGUMENT;
    }
#ifdef __APPLE__
    return static_cast<AudioUnitModule *>(instance)->run(block, nullptr, 0);
#else
    return block ? VS_ERROR_UNSUPPORTED : VS_ERROR_ARGUMENT;
#endif
  }

  static vs_result get_value(void *instance, uint32_t id, float *value) {
    const auto i = index(id);
    if (!instance || !value || i == count) {
      return VS_ERROR_ARGUMENT;
    }
    *value = static_cast<AudioUnitModule *>(instance)->values_[i];
    return VS_OK;
  }

  static vs_result set_value(void *instance, uint32_t id, float value) {
    const auto i = index(id);
    if (!instance || !valid(i, value)) {
      return VS_ERROR_ARGUMENT;
    }
    return static_cast<AudioUnitModule *>(instance)->apply(i, value);
  }

  static vs_result state_size(void *instance, uint64_t *bytes) {
    if (!instance || !bytes) {
      return VS_ERROR_ARGUMENT;
    }
    *bytes = count * sizeof(float);
    return VS_OK;
  }

  static vs_result save(void *instance, uint8_t *buffer, uint64_t capacity, uint64_t *written) {
    if (!instance || !buffer || !written || capacity < count * sizeof(float)) {
      return VS_ERROR_ARGUMENT;
    }
    auto &self = *static_cast<AudioUnitModule *>(instance);
    for (size_t i = 0; i < count; ++i) {
      store_binary32(self.values_[i], buffer + i * binary32_bytes);
    }
    *written = count * sizeof(float);
    return VS_OK;
  }

  static vs_result load(void *instance, uint32_t schema, const uint8_t *buffer, uint64_t bytes) {
    if (!instance || !buffer || bytes != count * sizeof(float)) {
      return VS_ERROR_ARGUMENT;
    }
    if (schema != 2) {
      return VS_ERROR_UNSUPPORTED;
    }
    auto &self = *static_cast<AudioUnitModule *>(instance);
    if (self.state_ != State::idle) {
      return VS_ERROR_STATE;
    }
    std::array<float, count> values{};
    for (size_t i = 0; i < count; ++i) {
      values[i] = load_binary32(buffer + i * binary32_bytes);
      if (!valid(i, values[i])) {
        return VS_ERROR_ARGUMENT;
      }
      values[i] = canonical(i, values[i]);
    }
    self.values_ = values;
    return VS_OK;
  }

  template <bool Native = false>
  static vs_result assignments(void *instance, vs_shared_assignment *out, uint32_t capacity,
                               uint32_t *written) {
    if (!instance || !written || (capacity && !out)) {
      return VS_ERROR_ARGUMENT;
    }
    *written = 0;
    if (capacity < owned_count) {
      return VS_ERROR_ARGUMENT;
    }
    auto &self = *static_cast<AudioUnitModule *>(instance);
    if constexpr (Native) {
      if (self.values_[selector] != static_cast<float>(Configuration::native_branch_route)) {
        return VS_ERROR_STATE;
      }
    }
    uint32_t n = 0;
    for (size_t i = 0; i < count; ++i) {
      if (!targetless(i)) {
        out[n++] = {Configuration::mapping[i].id,
                    effective<Native>(i, self.values_[i], self.values_)};
      }
    }
    for (const auto &s : Configuration::settings) {
      if (contains(owned_settings, s.parameter.id)) {
        out[n++] = {s.parameter.id, s.value};
      }
    }
    *written = n;
    return VS_OK;
  }

  template <bool Native = false>
  static vs_result translate(void *instance, uint32_t id, float value, vs_shared_assignment *out,
                             uint32_t capacity, uint32_t *written) {
    const auto i = index(id);
    if (!instance || !written || !valid(i, value) ||
        capacity < (targetless(i)              ? gates.size()
                    : !Native && i == selector ? 1 + pinned_count
                                               : 1) ||
        !out) {
      return VS_ERROR_ARGUMENT;
    }
    auto &self = *static_cast<AudioUnitModule *>(instance);
    value = canonical(i, value);
    if constexpr (Native) {
      constexpr auto selected = static_cast<float>(Configuration::native_branch_route);
      if (self.values_[selector] != selected || (i == selector && value != selected)) {
        return VS_ERROR_STATE;
      }
    }
    auto next = self.values_;
    next[i] = value;
    uint32_t n = 0;
    if (targetless(i)) {
      for (size_t j = 0; j < count; ++j) {
        if (gated_section(j)) {
          out[n++] = {Configuration::mapping[j].id, effective<Native>(j, next[j], next)};
        }
      }
    } else {
      out[n++] = {Configuration::mapping[i].id, effective<Native>(i, value, next)};
      if (!Native && i == selector) {
        for (auto j : pinned_parameters) {
          out[n++] = {Configuration::mapping[j].id, effective(j, next[j], next)};
        }
      }
    }
    self.values_[i] = value;
    *written = n;
    return VS_OK;
  }

  static vs_result engine_prepare(void *instance, const vs_prepare_info *info,
                                  const vs_host_instance_cache *cache, const char *signature,
                                  uint32_t flags, const vs_shared_target *shared_targets,
                                  uint32_t target_count, uint32_t target_size,
                                  const vs_shared_assignment *state, uint32_t state_count,
                                  vs_module_timing *timing) {
    if (!instance || !valid_info(info, timing) || !signature ||
        (target_count && (!shared_targets || target_size < sizeof(vs_shared_target))) ||
        (state_count && !state) ||
        (cache && (cache->struct_size < sizeof(vs_host_instance_cache) || !cache->acquire ||
                   !cache->release))) {
      return VS_ERROR_ARGUMENT;
    }
    auto &self = *static_cast<AudioUnitModule *>(instance);
    if (self.state_ != State::idle) {
      return VS_ERROR_STATE;
    }
#ifdef __APPLE__
    try {
      if constexpr (!main_lifecycle) {
        self.cache_ = cache;
        self.cache_signature_ = signature;
      }
      return self.prepare_unit(*info, *timing, host_tempo || (flags & VS_SHARED_HOST_TEMPO), [&] {
        self.reusable_ = !retains_audio && !(flags & VS_SHARED_RETAINS_AUDIO);
        bool valid = true;
        const auto *bytes = reinterpret_cast<const uint8_t *>(shared_targets);
        for (uint32_t i = 0; valid && i < target_count; ++i) {
          vs_shared_target target;
          std::memcpy(&target, bytes + static_cast<size_t>(i) * target_size, sizeof(target));
          valid =
              target.name && self.validate(target.id, target.name, target.minimum, target.maximum);
        }
        for (uint32_t i = 0; valid && i < state_count; ++i) {
          valid =
              std::isfinite(state[i].value) && self.write_target(state[i].target, state[i].value);
        }
        return valid;
      });
    } catch (...) {
      self.detach(false);
      return VS_ERROR_PROCESS;
    }
#else
    static_cast<void>(flags);
    static_cast<void>(cache);
    return VS_ERROR_UNSUPPORTED;
#endif
  }

  static vs_result engine_process(void *instance, const vs_process_block *block,
                                  const vs_parameter_event *shared_targets, uint32_t target_count) {
    if (!instance) {
      return VS_ERROR_ARGUMENT;
    }
#ifdef __APPLE__
    return static_cast<AudioUnitModule *>(instance)->run(block, shared_targets, target_count);
#else
    return block && (shared_targets || !target_count) ? VS_ERROR_UNSUPPORTED : VS_ERROR_ARGUMENT;
#endif
  }

  static vs_result engine_apply(void *instance, const vs_shared_assignment *values,
                                uint32_t value_count) {
    if (!instance || (value_count && !values)) {
      return VS_ERROR_ARGUMENT;
    }
    auto &self = *static_cast<AudioUnitModule *>(instance);
    if (self.state_ != State::prepared) {
      return VS_ERROR_STATE;
    }
#ifdef __APPLE__
    for (uint32_t i = 0; i < value_count; ++i) {
      if (!std::isfinite(values[i].value) ||
          AudioUnitSetParameter(self.unit_, values[i].target, kAudioUnitScope_Global, 0,
                                values[i].value, 0) != noErr) {
        return VS_ERROR_PROCESS;
      }
    }
    return VS_OK;
#else
    return VS_ERROR_UNSUPPORTED;
#endif
  }

public:
  inline static const vs_module_api api = {
      sizeof(vs_module_api),
      VS_MODULE_ABI_VERSION,
      Configuration::id,
      Configuration::name,
      version,
      VS_LAYOUT_MONO | VS_LAYOUT_STEREO,
      Configuration::parameters.data(),
      static_cast<uint32_t>(count),
      create,
      destroy,
      prepare,
      release,
      reset,
      process,
      (bypassable ? uint32_t(VS_MODULE_BYPASS_PARAMETER) : 0u) |
          (host_tempo ? uint32_t(VS_MODULE_HOST_TEMPO) : 0u) |
          (accepts_midi ? uint32_t(VS_MODULE_MIDI_INPUT) : 0u),
      bypass_parameter};
  inline static const vs_module_state_api state_api = {
      sizeof(vs_module_state_api), VS_MODULE_STATE_ABI_VERSION, 2, state_size, save, load};
  inline static const vs_module_parameter_api parameter_api = {
      sizeof(vs_module_parameter_api), VS_MODULE_PARAMETER_ABI_VERSION, get_value, set_value};
  inline static const vs_module_metadata_api metadata_api = {
      sizeof(vs_module_metadata_api), VS_MODULE_METADATA_ABI_VERSION, metadata.data(),
      static_cast<uint32_t>(metadata.size()), sizeof(vs_parameter_metadata)};

  static const vs_module_shared_api *shared_api()
    requires shared
  {
    static const std::string engine = std::string(Configuration::shared_engine) + "|" +
                                      cache_key() +
                                      "|sdk-engine:" + std::to_string(shared_engine_revision);
    static const vs_module_shared_api result = {
        sizeof(vs_module_shared_api),
        VS_MODULE_SHARED_ABI_VERSION,
        engine.c_str(),
        Configuration::shared_stage,
        [] {
          uint32_t flags = 0;
          if (retains_audio) {
            flags |= VS_SHARED_RETAINS_AUDIO;
          }
          if constexpr (requires { Configuration::shared_leading; }) {
            if (Configuration::shared_leading) {
              flags |= VS_SHARED_LEADING_ONLY;
            }
          }
          return flags;
        }(),
        target_table.data(),
        static_cast<uint32_t>(target_table.size()),
        sizeof(vs_shared_target),
        assignments<>,
        translate<>,
        engine_prepare,
        engine_process,
        engine_apply};
    return &result;
  }

  static const vs_module_shared_branch_api *shared_branch_api(uint32_t requested)
    requires native_branch
  {
    static constexpr vs_module_shared_branch_api result{
        sizeof(vs_module_shared_branch_api),
        VS_MODULE_SHARED_BRANCH_ABI_VERSION,
        Configuration::native_branch_mix_parameter,
        VS_SHARED_BRANCH_CROSSFADE,
        Configuration::native_branch_first_after_stage,
        VS_SHARED_BRANCH_HAS_SELECTOR,
        Configuration::route_parameter,
        static_cast<float>(Configuration::native_branch_route),
        assignments<true>,
        translate<true>};
    return requested == VS_MODULE_SHARED_BRANCH_ABI_VERSION ? &result : nullptr;
  }
};

}
#endif
