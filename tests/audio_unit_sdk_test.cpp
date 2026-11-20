#include <vespera/instance_cache.hpp>
#include <vespera/sdk/audio_unit.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <exception>
#include <limits>
#include <source_location>
#include <stdexcept>
#include <thread>

namespace {
void check(bool condition, std::source_location location = std::source_location::current()) {
  if (!condition) {
    throw std::runtime_error("line " + std::to_string(location.line()));
  }
}

struct Configuration {
  static constexpr uint32_t type = kAudioUnitType_Effect;
  static constexpr uint32_t subtype = kAudioUnitSubType_LowPassFilter;
  static constexpr uint32_t manufacturer = kAudioUnitManufacturer_Apple;
  static constexpr const char *id = "test.audio-unit";
  static constexpr const char *name = "Audio Unit fixture";
  static constexpr const char *shared_engine = "test.audio-unit/1";
  static constexpr uint32_t shared_stage = 1;
  static constexpr std::array<vs_parameter_descriptor, 0> parameters{};
  static constexpr std::array<vespera::sdk::AudioUnitParameter, 0> mapping{};
  static constexpr std::array<vespera::sdk::AudioUnitSetting, 0> settings{};
};
using Module = vespera::sdk::AudioUnitModule<Configuration>;

struct DiscreteConfiguration : Configuration {
  static constexpr auto parameters =
      std::to_array<vs_parameter_descriptor>({{1, "Mode", "", 0, 2, 1}});
  static constexpr auto mapping = std::to_array<vespera::sdk::AudioUnitParameter>(
      {{kLowPassParam_CutoffFrequency, "Cutoff Frequency", 10, 21829.5f}});
  static constexpr const char *labels[]{"Low", "Medium", "High"};
  static constexpr auto metadata =
      std::to_array<vs_parameter_metadata>({{1, VS_PARAMETER_ENUMERATED, 3, 0, labels, 0, 0, 0,
                                             nullptr, nullptr, 0, VS_PARAMETER_OFF_NONE, 0}});
};

void discrete_values() {
  using Processor = vespera::sdk::AudioUnitModule<DiscreteConfiguration>;
  void *instance = nullptr;
  check(Processor::api.create(&instance) == VS_OK);
  const auto *shared = Processor::shared_api();
  for (auto value : {0.f, .4999f, .5f, .5001f, 1.4999f, 1.5f, 2.f}) {
    const auto expected = std::round(value);
    float actual = -1;
    check(Processor::parameter_api.set_value(instance, 1, value) == VS_OK);
    check(Processor::parameter_api.get_value(instance, 1, &actual) == VS_OK);
    check(actual == expected);
    vs_shared_assignment assignment{};
    uint32_t written = 0;
    check(shared->translate(instance, 1, value, &assignment, 1, &written) == VS_OK);
    check(written == 1);
    check(assignment.value == 10 + expected * (21829.5f - 10) / 2);
    std::array<uint8_t, 4> bytes{};
    auto bits = std::bit_cast<uint32_t>(value);
    for (size_t b = 0; b < 4; ++b) {
      bytes[b] = static_cast<uint8_t>(bits >> (8 * b));
    }
    check(Processor::state_api.load(instance, 2, bytes.data(), bytes.size()) == VS_OK);
    check(Processor::parameter_api.get_value(instance, 1, &actual) == VS_OK);
    check(actual == expected);
    uint64_t saved = 0;
    check(Processor::state_api.save(instance, bytes.data(), bytes.size(), &saved) == VS_OK);
    uint32_t restored = 0;
    for (size_t b = 0; b < 4; ++b) {
      restored |= uint32_t(bytes[b]) << (8 * b);
    }
    check(saved == bytes.size() && std::bit_cast<float>(restored) == expected);
  }
  vs_prepare_info info{sizeof(info), 48000, 128, 1};
  vs_module_timing timing{sizeof(timing), 0, 0};
  check(Processor::api.prepare(instance, &info, &timing) == VS_OK);
  std::array<float, 128> input{}, output{};
  const float *inputs[]{input.data()};
  float *outputs[]{output.data()};
  vs_parameter_event event{1, 37, .5f};
  vs_process_block block{sizeof(block), inputs, outputs, 1,       128, 0, 0,
                         &event,        1,      nullptr, nullptr, 0,   0};
  check(Processor::api.process(instance, &block) == VS_OK);
  float actual = -1;
  check(Processor::parameter_api.get_value(instance, 1, &actual) == VS_OK && actual == 1);
  event.value = std::numeric_limits<float>::quiet_NaN();
  check(Processor::api.process(instance, &block) == VS_ERROR_ARGUMENT);
  check(Processor::parameter_api.get_value(instance, 1, &actual) == VS_OK && actual == 1);
  Processor::api.destroy(instance);
}

void hostile_blocks() {
  void *instance = nullptr;
  check(Module::api.create(&instance) == VS_OK);
  vs_prepare_info info{sizeof(info), 48000, 128, 1};
  vs_module_timing timing{sizeof(timing), 0, 0};
  check(Module::api.prepare(instance, &info, &timing) == VS_OK);
  std::array<float, 128> input{}, output{};
  const float *inputs[]{input.data()};
  float *outputs[]{output.data()};
  vs_process_block block{sizeof(block), inputs, outputs, 1,       128, 0, 0,
                         nullptr,       0,      nullptr, nullptr, 0,   0};
  block.frames = 0;
  check(Module::api.process(instance, &block) == VS_ERROR_ARGUMENT);
  block.frames = 128;
  for (size_t i = 0; i < input.size(); ++i) {
    input[i] = i % 3 == 0   ? std::numeric_limits<float>::quiet_NaN()
               : i % 3 == 1 ? std::numeric_limits<float>::infinity()
                            : -std::numeric_limits<float>::infinity();
  }
  input[1] = .5f;
  check(Module::api.process(instance, &block) == VS_OK);
  check(std::ranges::all_of(output, [](float sample) { return std::isfinite(sample); }));
  check(std::ranges::any_of(output, [](float sample) { return sample != 0; }));
  input.fill(0);
  input[0] = 1;
  output.fill(-1);
  check(Module::api.process(instance, &block) == VS_OK);
  check(std::ranges::all_of(output, [](float sample) { return std::isfinite(sample); }));
  check(std::ranges::any_of(output, [](float sample) { return sample != 0; }));
  Module::api.destroy(instance);
}

struct SettledConfiguration : Configuration {
  static constexpr double preparation_seconds = .1;
};

struct RenderObserver {
  inline static uint32_t frames = 0;
  inline static double first_time = 0, last_time = 0;
  static std::string variant() { return "render-observer"; }
  static OSStatus observe(void *, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *time,
                          UInt32, UInt32 count, AudioBufferList *) {
    if (*flags & kAudioUnitRenderAction_PreRender) {
      if (!frames) {
        first_time = time->mSampleTime;
      }
      last_time = time->mSampleTime;
      frames += count;
    }
    return noErr;
  }
  static bool configure(AudioUnit unit, uint32_t) {
    frames = 0;
    return AudioUnitAddRenderNotify(unit, observe, nullptr) == noErr;
  }
};

struct ObservedConfiguration : SettledConfiguration {
  static constexpr RenderObserver instance_policy{};
};

template <class Config>
std::array<float, 128> first_block(uint32_t channels, double rate, vs_module_timing &timing) {
  using Processor = vespera::sdk::AudioUnitModule<Config>;
  void *instance = nullptr;
  check(Processor::api.create(&instance) == VS_OK);
  vs_prepare_info info{sizeof(info), rate, 128, channels};
  check(Processor::api.prepare(instance, &info, &timing) == VS_OK);
  std::array<float, 128> input{}, left{}, right{};
  input[0] = 1;
  const float *inputs[]{input.data(), input.data()};
  float *outputs[]{left.data(), right.data()};
  vs_tempo_context tempo{sizeof(tempo), VS_TEMPO_VALID, 90, 0};
  vs_process_block block{sizeof(block), inputs, outputs, channels, 128, 0, 0,
                         nullptr,       0,      &tempo,  nullptr,  0,   0};
  check(Processor::api.process(instance, &block) == VS_OK);
  Processor::api.destroy(instance);
  return left;
}

void preparation_settling() {
  for (auto channels : {1u, 2u}) {
    for (auto rate : {44100.0, 48000.0, 96000.0}) {
      vs_module_timing plain{sizeof(plain), 0, 0}, settled{sizeof(settled), 0, 0};
      auto reference = first_block<Configuration>(channels, rate, plain);
      auto output = first_block<SettledConfiguration>(channels, rate, settled);
      check(plain.latency_frames == settled.latency_frames);
      check(plain.tail_frames == settled.tail_frames);
      for (size_t i = 0; i < output.size(); ++i) {
        check(std::abs(output[i] - reference[i]) < 1e-6f);
      }
    }
  }
  vs_module_timing timing{sizeof(timing), 0, 0};
  first_block<ObservedConfiguration>(2, 44100, timing);
  check(RenderObserver::frames == 4410 + 128);
  check(RenderObserver::first_time == -4410);
  check(RenderObserver::last_time == 0);
}

void cache_formats() {
  auto cache = std::make_shared<vespera::InstanceCache>();
  auto client = cache->client(nullptr);
  const auto *shared = Module::shared_api();
  void *instance = nullptr;
  check(Module::api.create(&instance) == VS_OK);
  auto prepare = [&](uint32_t channels, double rate, uint32_t frames, bool fail = false) {
    vs_prepare_info info{sizeof(info), rate, frames, channels};
    vs_module_timing timing{sizeof(timing), 0, 0};
    vs_shared_target bad{0xffffffff, "Missing parameter", 0, 1, 0, 0};
    return shared->engine_prepare(instance, &info, client.get(), "fixture", 0,
                                  fail ? &bad : nullptr, fail ? 1u : 0u, sizeof(bad), nullptr, 0,
                                  &timing);
  };
  auto render = [&](uint32_t channels) {
    std::array<float, 64> input{}, left{}, right{};
    input.fill(.25f);
    const float *inputs[]{input.data(), input.data()};
    float *outputs[]{left.data(), right.data()};
    vs_process_block block{sizeof(block), inputs, outputs, channels, 64, 0, 0,
                           nullptr,       0,      nullptr, nullptr,  0,  0};
    check(shared->engine_process(instance, &block, nullptr, 0) == VS_OK);
    check(std::abs(left.back()) > .01f);
    if (channels == 2) {
      check(std::abs(right.back()) > .01f);
    }
  };
  for (uint32_t channels : {1u, 2u, 1u, 2u}) {
    check(prepare(channels, 48000, 128) == VS_OK);
    render(channels);
    Module::api.release(instance);
  }
  check(cache->statistics().misses == 2);
  check(cache->statistics().hits == 2);
  check(cache->idle() == 2);
  check(prepare(1, 96000, 512) == VS_OK);
  render(1);
  Module::api.release(instance);
  check(cache->statistics().hits == 3);
  check(prepare(2, 44100, 64, true) != VS_OK);
  check(cache->idle() == 1);
  check(prepare(2, 44100, 64) == VS_OK);
  render(2);
  Module::api.release(instance);
  check(cache->statistics().misses == 3);
  Module::api.destroy(instance);
  cache->trim(0);
  check(cache->idle() == 0);
}

using vespera::sdk::RoutePin;
constexpr float cutoff_maximum = 21829.5f;

struct UnitObserver {
  inline static AudioUnit unit = nullptr;
  static std::string variant() { return "unit-observer"; }
  static bool configure(AudioUnit configured, uint32_t) {
    unit = configured;
    return true;
  }
};

float target(AudioUnitParameterID id) {
  AudioUnitParameterValue value = 0;
  check(UnitObserver::unit &&
        AudioUnitGetParameter(UnitObserver::unit, id, kAudioUnitScope_Global, 0, &value) == noErr);
  return value;
}

bool near(float actual, float expected) {
  return std::abs(actual - expected) <= 1e-6f * std::max(1.f, std::abs(expected));
}

// Route 1 holds the bypass parameter's target (cutoff) at 5000 Hz, as a branch route that leaves
// power to the host would.
struct RouteParameters : Configuration {
  static constexpr auto parameters = std::to_array<vs_parameter_descriptor>(
      {{1, "Route", "", 0, 1, 0}, {2, "Power", "", 0, 1, 1}});
  static constexpr auto mapping = std::to_array<vespera::sdk::AudioUnitParameter>(
      {{kLowPassParam_Resonance, "Resonance", -20, 40},
       {kLowPassParam_CutoffFrequency, "Cutoff Frequency", 10, cutoff_maximum}});
  static constexpr uint32_t bypass_parameter = 2;
  static constexpr auto metadata =
      std::to_array<vs_parameter_metadata>({{1, VS_PARAMETER_TOGGLE, 2, 0, nullptr, 0, 0, 0,
                                             nullptr, nullptr, 0, VS_PARAMETER_OFF_NONE, 0}});
  static constexpr UnitObserver instance_policy{};
};

struct RouteConfiguration : RouteParameters {
  static constexpr uint32_t route_parameter = 1;
  static constexpr auto route_pins = std::to_array<RoutePin>({{1, 2, 5000}});
};

using Routed = vespera::sdk::AudioUnitModule<RouteConfiguration>;

std::array<uint8_t, 8> route_state(float route, float power) {
  std::array<uint8_t, 8> bytes{};
  for (size_t i = 0; i < 2; ++i) {
    const auto bits = std::bit_cast<uint32_t>(i ? power : route);
    for (size_t b = 0; b < 4; ++b) {
      bytes[i * 4 + b] = static_cast<uint8_t>(bits >> (8 * b));
    }
  }
  return bytes;
}

float value_of(void *instance, uint32_t id) {
  float value = -1;
  check(Routed::parameter_api.get_value(instance, id, &value) == VS_OK);
  return value;
}

void route_pins_direct() {
  void *instance = nullptr;
  check(Routed::api.create(&instance) == VS_OK);
  check(Routed::api.capabilities & VS_MODULE_BYPASS_PARAMETER);
  check(Routed::api.bypass_parameter == 2);
  vs_prepare_info info{sizeof(info), 48000, 128, 1};
  vs_module_timing timing{sizeof(timing), 0, 0};
  check(Routed::api.prepare(instance, &info, &timing) == VS_OK);
  const auto cutoff = kLowPassParam_CutoffFrequency;
  const auto resonance = kLowPassParam_Resonance;
  check(near(target(cutoff), cutoff_maximum) && target(resonance) == -20);
  check(Routed::parameter_api.set_value(instance, 2, 0) == VS_OK);
  check(target(cutoff) == 10);
  check(Routed::parameter_api.set_value(instance, 1, 1) == VS_OK);
  check(target(resonance) == 40 && target(cutoff) == 5000);
  check(value_of(instance, 2) == 0);
  check(Routed::parameter_api.set_value(instance, 2, 1) == VS_OK);
  check(target(cutoff) == 5000 && value_of(instance, 2) == 1);
  check(Routed::parameter_api.set_value(instance, 1, .4999f) == VS_OK);
  check(value_of(instance, 1) == 0);
  check(target(resonance) == -20 && near(target(cutoff), cutoff_maximum));

  std::array<float, 128> input{}, output{};
  for (size_t i = 0; i < input.size(); ++i) {
    input[i] = std::sin(float(i) * .7f) * .5f + ((i * 37) % 11 == 0 ? .4f : 0.f);
  }
  const float *inputs[]{input.data()};
  float *outputs[]{output.data()};
  auto process = [&](const vs_parameter_event *events, uint32_t event_count) {
    vs_process_block block{sizeof(block), inputs,      outputs, 1,       128, 0, 0,
                           events,        event_count, nullptr, nullptr, 0,   0};
    check(Routed::api.process(instance, &block) == VS_OK);
  };
  std::array<vs_parameter_event, 2> events{{{1, 0, .5f}, {2, 64, 0}}};
  process(events.data(), 2);
  check(value_of(instance, 1) == 1 && value_of(instance, 2) == 0);
  check(target(resonance) == 40 && target(cutoff) == 5000);
  events[0] = {1, 0, 0};
  process(events.data(), 1);
  check(target(resonance) == -20 && target(cutoff) == 10);

  auto settled = [&](float route, float power) {
    check(Routed::parameter_api.set_value(instance, 1, route) == VS_OK);
    check(Routed::parameter_api.set_value(instance, 2, power) == VS_OK);
    process(nullptr, 0);
    Routed::api.reset(instance);
    process(nullptr, 0);
    return output;
  };
  const auto pinned_on = settled(1, 1);
  const auto pinned_off = settled(1, 0);
  check(pinned_on == pinned_off);
  const auto live_on = settled(0, 1);
  const auto live_off = settled(0, 0);
  check(live_on != live_off);
  check(live_on != pinned_on);

  Routed::api.release(instance);
  for (auto [route, power, cut, res] : std::to_array<std::array<float, 4>>(
           {{1, 0, 5000, 40}, {.5f, 1, 5000, 40}, {0, 0, 10, -20}})) {
    const auto bytes = route_state(route, power);
    check(Routed::state_api.load(instance, 2, bytes.data(), bytes.size()) == VS_OK);
    check(Routed::api.prepare(instance, &info, &timing) == VS_OK);
    check(target(cutoff) == cut && target(resonance) == res);
    check(value_of(instance, 1) == std::round(route) && value_of(instance, 2) == power);
    std::array<uint8_t, 8> saved{};
    uint64_t written = 0;
    check(Routed::state_api.save(instance, saved.data(), saved.size(), &written) == VS_OK);
    check(written == saved.size() && saved == route_state(std::round(route), power));
    Routed::api.release(instance);
  }
  Routed::api.destroy(instance);
}

void route_pins_shared() {
  void *instance = nullptr;
  check(Routed::api.create(&instance) == VS_OK);
  const auto *shared = Routed::shared_api();
  const auto cutoff = kLowPassParam_CutoffFrequency;
  const auto resonance = kLowPassParam_Resonance;
  std::array<vs_shared_assignment, 4> out{};
  uint32_t written = 0;
  auto expect = [&](std::initializer_list<vs_shared_assignment> expected) {
    check(written == expected.size());
    size_t k = 0;
    for (const auto &e : expected) {
      check(out[k].target == e.target && near(out[k].value, e.value));
      ++k;
    }
  };
  auto translate = [&](uint32_t id, float value, uint32_t capacity = 4) {
    return shared->translate(instance, id, value, out.data(), capacity, &written);
  };
  check(shared->assignments(instance, out.data(), 2, &written) == VS_OK);
  expect({{resonance, -20}, {cutoff, cutoff_maximum}});
  check(translate(2, 0, 1) == VS_OK);
  expect({{cutoff, 10}});
  check(translate(1, 1, 1) == VS_ERROR_ARGUMENT);
  check(value_of(instance, 1) == 0);
  check(translate(1, 1, 2) == VS_OK);
  expect({{resonance, 40}, {cutoff, 5000}});
  check(value_of(instance, 2) == 0);
  check(translate(2, 1) == VS_OK);
  expect({{cutoff, 5000}});
  check(value_of(instance, 2) == 1);
  check(shared->assignments(instance, out.data(), 2, &written) == VS_OK);
  expect({{resonance, 40}, {cutoff, 5000}});
  check(translate(1, .4999f) == VS_OK);
  expect({{resonance, -20}, {cutoff, cutoff_maximum}});
  check(value_of(instance, 1) == 0);

  const auto bytes = route_state(1, 0);
  check(Routed::state_api.load(instance, 2, bytes.data(), bytes.size()) == VS_OK);
  check(shared->assignments(instance, out.data(), 2, &written) == VS_OK);
  expect({{resonance, 40}, {cutoff, 5000}});
  const std::array<vs_shared_assignment, 2> state{out[0], out[1]};
  vs_prepare_info info{sizeof(info), 48000, 64, 2};
  vs_module_timing timing{sizeof(timing), 0, 0};
  check(shared->engine_prepare(instance, &info, nullptr, "fixture", 0, shared->targets,
                               shared->target_count, shared->target_size, state.data(),
                               static_cast<uint32_t>(state.size()), &timing) == VS_OK);
  check(target(cutoff) == 5000 && target(resonance) == 40);
  check(translate(1, 0) == VS_OK);
  check(shared->engine_apply(instance, out.data(), written) == VS_OK);
  check(target(cutoff) == 10 && target(resonance) == -20);
  check(value_of(instance, 2) == 0);

  check(translate(1, 1) == VS_OK);
  std::array<vs_parameter_event, 4> targets{};
  for (uint32_t k = 0; k < written; ++k) {
    targets[k] = {out[k].target, 16, out[k].value};
  }
  std::array<float, 64> input{}, left{}, right{};
  const float *inputs[]{input.data(), input.data()};
  float *outputs[]{left.data(), right.data()};
  vs_process_block block{sizeof(block), inputs, outputs, 2,       64, 0, 0,
                         nullptr,       0,      nullptr, nullptr, 0,  0};
  check(shared->engine_process(instance, &block, targets.data(), written) == VS_OK);
  check(target(cutoff) == 5000 && target(resonance) == 40);
  Routed::api.release(instance);
  Routed::api.destroy(instance);
}

void unrouted_default() {
  using Plain = vespera::sdk::AudioUnitModule<RouteParameters>;
  void *instance = nullptr;
  check(Plain::api.create(&instance) == VS_OK);
  const auto *shared = Plain::shared_api();
  vs_shared_assignment out{};
  uint32_t written = 0;
  check(shared->translate(instance, 1, 1, &out, 1, &written) == VS_OK);
  check(written == 1 && out.target == kLowPassParam_Resonance && out.value == 40);
  check(shared->translate(instance, 2, 0, &out, 1, &written) == VS_OK);
  check(written == 1 && out.target == kLowPassParam_CutoffFrequency && out.value == 10);
  Plain::api.destroy(instance);
}

// Routes 1 and 2 hold the mix target at different values; the base has no metadata, so its route
// comes from the raw stored value.
struct ContinuousRouteConfiguration : Configuration {
  static constexpr auto parameters = std::to_array<vs_parameter_descriptor>(
      {{1, "Route", "", 0, 2, 0}, {2, "Mix", "", 0, 1, .5f}});
  static constexpr auto mapping = std::to_array<vespera::sdk::AudioUnitParameter>(
      {{kLowPassParam_Resonance, "Resonance", -20, 40},
       {kLowPassParam_CutoffFrequency, "Cutoff Frequency", 10, cutoff_maximum}});
  static constexpr uint32_t route_parameter = 1;
  static constexpr auto route_pins = std::to_array<RoutePin>({{1, 2, 5000}, {2, 2, 100}});
};

struct DiscreteRouteConfiguration : ContinuousRouteConfiguration {
  static constexpr const char *labels[]{"First", "Second", "Third"};
  static constexpr auto metadata =
      std::to_array<vs_parameter_metadata>({{1, VS_PARAMETER_ENUMERATED, 3, 0, labels, 0, 0, 0,
                                             nullptr, nullptr, 0, VS_PARAMETER_OFF_NONE, 0}});
};

template <class Config> void route_values(bool discrete) {
  using Processor = vespera::sdk::AudioUnitModule<Config>;
  void *instance = nullptr;
  check(Processor::api.create(&instance) == VS_OK);
  const auto *shared = Processor::shared_api();
  const float live = 10 + .5f * (cutoff_maximum - 10);
  for (auto [value, route, cut] : std::to_array<std::array<float, 3>>({{0, 0, live},
                                                                       {.4999f, 0, live},
                                                                       {.5f, 1, 5000},
                                                                       {1.4999f, 1, 5000},
                                                                       {1.5f, 2, 100},
                                                                       {2, 2, 100}})) {
    std::array<vs_shared_assignment, 2> out{};
    uint32_t written = 0;
    check(shared->translate(instance, 1, value, out.data(), 2, &written) == VS_OK);
    const float stored = discrete ? route : value;
    check(written == 2 && out[0].target == kLowPassParam_Resonance &&
          near(out[0].value, -20 + stored * 30));
    check(out[1].target == kLowPassParam_CutoffFrequency && near(out[1].value, cut));
    float actual = -1;
    check(Processor::parameter_api.get_value(instance, 1, &actual) == VS_OK && actual == stored);
    check(shared->translate(instance, 2, 1, out.data(), 1, &written) == VS_OK);
    check(written == 1 && near(out[0].value, route == 0 ? cutoff_maximum : cut));
    check(shared->translate(instance, 2, .5f, out.data(), 1, &written) == VS_OK);
  }
  Processor::api.destroy(instance);
}

using vespera::sdk::route_pins_valid;
static_assert(route_pins_valid<Configuration>());
static_assert(route_pins_valid<RouteParameters>());
static_assert(route_pins_valid<RouteConfiguration>());
static_assert(route_pins_valid<ContinuousRouteConfiguration>());
static_assert(route_pins_valid<DiscreteRouteConfiguration>());

struct MissingPins : RouteParameters {
  static constexpr uint32_t route_parameter = 1;
};
struct MissingSelector : RouteParameters {
  static constexpr auto route_pins = std::to_array<RoutePin>({{1, 2, 5000}});
};
struct UnknownSelector : RouteConfiguration {
  static constexpr uint32_t route_parameter = 9;
};
struct UnknownPinned : RouteConfiguration {
  static constexpr auto route_pins = std::to_array<RoutePin>({{1, 9, 5000}});
};
struct RouteBeyondSelector : RouteConfiguration {
  static constexpr auto route_pins = std::to_array<RoutePin>({{2, 2, 5000}});
};
struct SelectorPinned : RouteConfiguration {
  static constexpr auto route_pins = std::to_array<RoutePin>({{1, 1, 40}});
};
struct SelectorIsBypass : RouteConfiguration {
  static constexpr auto route_pins = std::to_array<RoutePin>({{1, 1, 40}});
  static constexpr uint32_t route_parameter = 2;
  static constexpr uint32_t bypass_parameter = 2;
};
struct PinBeyondTarget : RouteConfiguration {
  static constexpr auto route_pins = std::to_array<RoutePin>({{1, 2, 30000}});
};
struct DuplicatePin : RouteConfiguration {
  static constexpr auto route_pins = std::to_array<RoutePin>({{1, 2, 5000}, {1, 2, 6000}});
};
struct FractionalSelector : ContinuousRouteConfiguration {
  static constexpr auto parameters = std::to_array<vs_parameter_descriptor>(
      {{1, "Route", "", 0, 1.5f, 0}, {2, "Mix", "", 0, 1, .5f}});
};
struct OffsetSelector : ContinuousRouteConfiguration {
  static constexpr auto parameters = std::to_array<vs_parameter_descriptor>(
      {{1, "Route", "", 1, 2, 1}, {2, "Mix", "", 0, 1, .5f}});
  static constexpr auto route_pins = std::to_array<RoutePin>({{1, 2, 5000}});
};
struct SelectorStepsMismatch : ContinuousRouteConfiguration {
  static constexpr auto metadata =
      std::to_array<vs_parameter_metadata>({{1, VS_PARAMETER_STEPPED, 2, 0, nullptr, 0, 0, 0,
                                             nullptr, nullptr, 0, VS_PARAMETER_OFF_NONE, 0}});
};
struct ContinuousSelectorMetadata : ContinuousRouteConfiguration {
  static constexpr auto metadata =
      std::to_array<vs_parameter_metadata>({{1, VS_PARAMETER_CONTINUOUS, 0, 0, nullptr, 0, 0, 0,
                                             nullptr, nullptr, 0, VS_PARAMETER_OFF_NONE, 0}});
};
static_assert(!route_pins_valid<MissingPins>());
static_assert(!route_pins_valid<MissingSelector>());
static_assert(!route_pins_valid<UnknownSelector>());
static_assert(!route_pins_valid<UnknownPinned>());
static_assert(!route_pins_valid<RouteBeyondSelector>());
static_assert(!route_pins_valid<SelectorPinned>());
static_assert(!route_pins_valid<SelectorIsBypass>());
static_assert(!route_pins_valid<PinBeyondTarget>());
static_assert(!route_pins_valid<DuplicatePin>());
static_assert(!route_pins_valid<FractionalSelector>());
static_assert(!route_pins_valid<OffsetSelector>());
static_assert(!route_pins_valid<SelectorStepsMismatch>());
static_assert(!route_pins_valid<ContinuousSelectorMetadata>());

// Gated: parameter 1 is the targetless power, 3 and 4 are gated sections.
struct GatedRoutes : Configuration {
  static constexpr auto parameters =
      std::to_array<vs_parameter_descriptor>({{1, "Power", "bool", 0, 1, 1},
                                              {2, "Route", "", 0, 1, 0},
                                              {3, "A", "bool", 0, 1, 1},
                                              {4, "B", "bool", 0, 1, 1},
                                              {5, "C", "", 0, 1, 0},
                                              {6, "D", "", 0, 1, 0},
                                              {7, "E", "", 0, 1, 0},
                                              {8, "F", "", 0, 1, 0}});
  static constexpr auto mapping =
      std::to_array<vespera::sdk::AudioUnitParameter>({{0, "", 0, 1},
                                                       {10, "R", 0, 1},
                                                       {11, "A", 0, 1},
                                                       {12, "B", 0, 1},
                                                       {13, "C", 0, 1},
                                                       {14, "D", 0, 1},
                                                       {15, "E", 0, 1},
                                                       {16, "F", 0, 1}});
  static constexpr uint32_t bypass_parameter = 1;
  static constexpr auto power_gates = std::to_array<uint32_t>({3, 4});
  static constexpr uint32_t route_parameter = 2;
  static constexpr auto route_pins =
      std::to_array<RoutePin>({{1, 5, 1}, {0, 6, 0}, {1, 6, 1}, {1, 7, 0}});
};
struct PinnedGate : GatedRoutes {
  static constexpr auto route_pins = std::to_array<RoutePin>({{1, 3, 1}});
};
struct PinnedPower : GatedRoutes {
  static constexpr auto route_pins = std::to_array<RoutePin>({{1, 1, 1}});
};
struct GatedSelector : GatedRoutes {
  static constexpr uint32_t route_parameter = 3;
  static constexpr auto route_pins = std::to_array<RoutePin>({{1, 5, 1}});
};
struct TooManyPinned : GatedRoutes {
  static constexpr auto route_pins =
      std::to_array<RoutePin>({{1, 5, 1}, {1, 6, 1}, {1, 7, 0}, {0, 8, 0}});
};
static_assert(route_pins_valid<GatedRoutes>());
static_assert(!route_pins_valid<PinnedGate>());
static_assert(!route_pins_valid<PinnedPower>());
static_assert(!route_pins_valid<GatedSelector>());
static_assert(!route_pins_valid<TooManyPinned>());

void gated_routes() {
  using Processor = vespera::sdk::AudioUnitModule<GatedRoutes>;
  void *instance = nullptr;
  check(Processor::api.create(&instance) == VS_OK);
  const auto *shared = Processor::shared_api();
  std::array<vs_shared_assignment, 8> out{};
  uint32_t written = 0;
  auto expect = [&](std::initializer_list<vs_shared_assignment> expected) {
    check(written == expected.size());
    size_t k = 0;
    for (const auto &e : expected) {
      check(out[k].target == e.target && out[k].value == e.value);
      ++k;
    }
  };
  check(shared->translate(instance, 6, .75f, out.data(), 1, &written) == VS_OK);
  expect({{14, 0}});
  check(shared->translate(instance, 2, 1, out.data(), 3, &written) == VS_ERROR_ARGUMENT);
  check(shared->translate(instance, 2, 1, out.data(), 4, &written) == VS_OK);
  expect({{10, 1}, {13, 1}, {14, 1}, {15, 0}});
  check(shared->translate(instance, 1, 0, out.data(), 2, &written) == VS_OK);
  expect({{11, 0}, {12, 0}});
  check(shared->translate(instance, 5, .25f, out.data(), 1, &written) == VS_OK);
  expect({{13, 1}});
  check(shared->assignments(instance, out.data(), 7, &written) == VS_OK);
  expect({{10, 1}, {11, 0}, {12, 0}, {13, 1}, {14, 1}, {15, 0}, {16, 0}});
  check(shared->translate(instance, 2, 0, out.data(), 4, &written) == VS_OK);
  expect({{10, 0}, {13, .25f}, {14, 0}, {15, 0}});
  check(shared->translate(instance, 1, 1, out.data(), 2, &written) == VS_OK);
  expect({{11, 1}, {12, 1}});
  float value = -1;
  check(Processor::parameter_api.get_value(instance, 6, &value) == VS_OK && value == .75f);
  Processor::api.destroy(instance);
}

using vespera::sdk::MainThread;
using vespera::sdk::Preparation;

struct NativeBranchConfiguration : RouteParameters {
  static constexpr auto parameters =
      std::to_array<vs_parameter_descriptor>({{1, "Route", "", 0, 1, 0},
                                              {2, "Power", "", 0, 1, 0},
                                              {3, "Mix", "", 0, 1, .25f},
                                              {4, "Other", "", 0, 1, .25f}});
  static constexpr auto mapping = std::to_array<vespera::sdk::AudioUnitParameter>(
      {{10, "Route", 0, 1}, {20, "Power", 0, 1}, {30, "Mix", 0, 1}, {40, "Other", 0, 1}});
  static constexpr auto metadata =
      std::to_array<vs_parameter_metadata>({RouteParameters::metadata[0],
                                            {3, VS_PARAMETER_STEPPED, 5, 0, nullptr, 0, 0, 0,
                                             nullptr, nullptr, 0, VS_PARAMETER_OFF_NONE, 0}});
  static constexpr uint32_t route_parameter = 1;
  static constexpr auto route_pins = std::to_array<RoutePin>({{1, 2, 1}, {1, 3, 1}, {1, 4, .75f}});
  static constexpr uint32_t native_branch_mix_parameter = 3;
  static constexpr uint32_t native_branch_first_after_stage = 2;
  static constexpr uint32_t native_branch_route = 1;
};

struct InvalidBranchFence : NativeBranchConfiguration {
  static constexpr uint32_t native_branch_first_after_stage = shared_stage;
};
struct InvalidBranchRoute : NativeBranchConfiguration {
  static constexpr uint32_t native_branch_route = 0;
};
struct InvalidBranchMix : NativeBranchConfiguration {
  static constexpr uint32_t native_branch_mix_parameter = bypass_parameter;
};
struct InvalidBranchPins : NativeBranchConfiguration {
  static constexpr auto route_pins = std::to_array<RoutePin>({{1, 3, 1}});
};
struct InvalidBranchSelector : NativeBranchConfiguration {
  static constexpr auto metadata = std::array<vs_parameter_metadata, 0>{};
};
static_assert(vespera::sdk::native_branch_valid<NativeBranchConfiguration>());
static_assert(!vespera::sdk::native_branch_valid<InvalidBranchFence>());
static_assert(!vespera::sdk::native_branch_valid<InvalidBranchRoute>());
static_assert(!vespera::sdk::native_branch_valid<InvalidBranchMix>());
static_assert(!vespera::sdk::native_branch_valid<InvalidBranchPins>());
static_assert(!vespera::sdk::native_branch_valid<InvalidBranchSelector>());

void native_branch_mapping() {
  using Processor = vespera::sdk::AudioUnitModule<NativeBranchConfiguration>;
  void *instance = nullptr;
  check(Processor::api.create(&instance) == VS_OK);
  const auto *split = Processor::shared_api();
  const auto *native = Processor::shared_branch_api(VS_MODULE_SHARED_BRANCH_ABI_VERSION);
  check(native && !Processor::shared_branch_api(VS_MODULE_SHARED_BRANCH_ABI_VERSION + 1));
  check(native->mix_parameter == 3 && native->mix_law == VS_SHARED_BRANCH_CROSSFADE &&
        native->first_after_stage == 2 && native->flags == VS_SHARED_BRANCH_HAS_SELECTOR &&
        native->selector_parameter == 1 && native->selector_value == 1);
  std::array<vs_shared_assignment, 4> out{};
  uint32_t written = 0;
  auto expect = [&](std::initializer_list<vs_shared_assignment> expected) {
    check(written == expected.size());
    size_t k = 0;
    for (const auto &e : expected) {
      check(out[k].target == e.target && out[k].value == e.value);
      ++k;
    }
  };
  check(split->assignments(instance, out.data(), 4, &written) == VS_OK);
  expect({{10, 0}, {20, 0}, {30, .25f}, {40, .25f}});
  check(native->native_assignments(instance, out.data(), 4, &written) == VS_ERROR_STATE);
  check(Processor::parameter_api.set_value(instance, 1, .5f) == VS_OK);
  check(split->assignments(instance, out.data(), 4, &written) == VS_OK);
  expect({{10, 1}, {20, 1}, {30, 1}, {40, .75f}});
  check(native->native_assignments(instance, out.data(), 4, &written) == VS_OK);
  expect({{10, 1}, {20, 0}, {30, .25f}, {40, .75f}});
  check(native->native_translate(instance, 1, .4999f, out.data(), 1, &written) == VS_ERROR_STATE);
  float current = -1;
  check(Processor::parameter_api.get_value(instance, 1, &current) == VS_OK && current == 1);
  check(native->native_translate(instance, 1, .5f, out.data(), 1, &written) == VS_OK);
  expect({{10, 1}});
  check(native->native_translate(instance, 3, .375f, out.data(), 1, &written) == VS_OK);
  expect({{30, .5f}});
  check(native->native_translate(instance, 4, .5f, out.data(), 1, &written) == VS_OK);
  expect({{40, .75f}});
  check(native->native_translate(instance, 2, 1, out.data(), 1, &written) == VS_OK);
  expect({{20, 1}});
  check(native->native_translate(instance, 2, 0, out.data(), 1, &written) == VS_OK);
  expect({{20, 0}});
  check(split->assignments(instance, out.data(), 4, &written) == VS_OK);
  expect({{10, 1}, {20, 1}, {30, 1}, {40, .75f}});
  check(split->translate(instance, 1, 0, out.data(), 4, &written) == VS_OK);
  expect({{10, 0}, {20, 0}, {30, .5f}, {40, .5f}});
  Processor::api.destroy(instance);
}

struct MainLifecycleObserver : RenderObserver {
  inline static unsigned configured = 0;
  static std::string variant() { return "main-lifecycle-observer"; }
  static bool configure(AudioUnit unit, uint32_t channels) {
    check(MainThread::on_main());
    ++configured;
    return RenderObserver::configure(unit, channels);
  }
};

struct MainLifecycleConfiguration : Configuration {
  static constexpr bool main_thread_lifecycle = true;
  static constexpr MainLifecycleObserver instance_policy{};
  inline static bool refuse = false;
  inline static std::atomic<bool> *liveness_ready = nullptr;
  inline static std::atomic<bool> *liveness_go = nullptr;
  static bool prepare_instance(Preparation &steps) {
    check(!MainThread::on_main());
    if (liveness_ready) {
      check(steps.begin(0));
      *liveness_ready = true;
      liveness_go->wait(false);
      check(!steps.alive(.025));
      return true; // An ignored liveness failure must still fail readiness.
    }
    if (steps.sample_rate() == 44100) {
      return steps.begin(13) && steps.silence_frames(13);
    }
    check(steps.sample_rate() == 48000 && steps.channels() == 1 && steps.max_frames() == 128);
    check(steps.alive(.5));
    if (refuse) {
      return false;
    }
    check(steps.begin(304));
    check(steps.silence(.005)); // 240 frames
    check(steps.set_target(kLowPassParam_CutoffFrequency, 5000));
    std::array<float, 64> in{}, out{};
    in[0] = .25f;
    const float *inputs[]{in.data()};
    float *outputs[]{out.data()};
    const vs_process_block block{sizeof(block), inputs, outputs, 1,       64, 0, 0,
                                 nullptr,       0,      nullptr, nullptr, 0,  0};
    const float peak = steps.render(block);
    check(std::isfinite(peak) && peak > 0);
    return true;
  }
};

template <class Job> void on_worker(bool pump, Job &&job) {
  std::atomic<bool> done = false;
  std::exception_ptr error;
  std::thread worker([&] {
    try {
      job();
    } catch (...) {
      error = std::current_exception();
    }
    done = true;
  });
  if (pump) {
    while (!done) {
      MainThread::wait(.005);
    }
  }
  worker.join();
  if (error) {
    std::rethrow_exception(error);
  }
}

struct ComposedPreparation : Configuration {
  static constexpr bool main_thread_lifecycle = true;
  static constexpr UnitObserver instance_policy{};
  static constexpr uint32_t cutoff = kLowPassParam_CutoffFrequency;
  enum class Case { success, zero_floor, false_return, failed_step, exception, invalid_floor };
  inline static Case scenario = Case::success;
  inline static bool restored = false;
  inline static double native_tail = 0;
  static constexpr float original = 4117;

  static bool prepare_instance(Preparation &steps) {
    float actual = 0;
    check(steps.get_target(cutoff, actual) && actual == original);
    UInt32 size = sizeof(native_tail);
    check(AudioUnitGetProperty(UnitObserver::unit, kAudioUnitProperty_TailTime,
                               kAudioUnitScope_Global, 0, &native_tail, &size) == noErr);
    struct HookError {};
    bool ok = false;
    try {
      ok = steps.with_temporary_target(cutoff, 1600, [&] {
        float temporary = 0;
        check(steps.get_target(cutoff, temporary) && temporary == 1600);
        if (scenario == Case::exception) {
          throw HookError{};
        }
        if (scenario == Case::failed_step) {
          check(!steps.silence_frames(1)); // No frame budget has been declared.
          return true;
        }
        return scenario != Case::false_return;
      });
    } catch (const HookError &) {
      check(scenario == Case::exception);
    }
    // Raw readback verifies cleanup even after Preparation has latched failure.
    restored = target(cutoff) == original;
    if (scenario == Case::false_return || scenario == Case::failed_step ||
        scenario == Case::exception) {
      check(!ok);
      return true;
    }
    check(ok);
    if (scenario == Case::invalid_floor) {
      check(!steps.minimum_tail_seconds(std::numeric_limits<double>::infinity()));
    } else if (scenario == Case::zero_floor) {
      check(steps.minimum_tail_seconds(0));
    } else {
      check(steps.minimum_tail_seconds(.125));
      check(steps.minimum_tail_seconds(.0625));
    }
    return true;
  }
};

void composed_preparation() {
  using Config = ComposedPreparation;
  using Processor = vespera::sdk::AudioUnitModule<Config>;
  using Case = Config::Case;
  const vs_prepare_info info{sizeof(info), 48000, 128, 1};
  const vs_shared_assignment composed{Config::cutoff, Config::original};
  void *instance = nullptr;
  check(Processor::api.create(&instance) == VS_OK);
  for (const auto scenario : {Case::success, Case::zero_floor, Case::false_return,
                              Case::failed_step, Case::exception, Case::invalid_floor}) {
    Config::scenario = scenario;
    Config::restored = false;
    vs_module_timing timing{sizeof(timing), 0, 0};
    const auto result =
        Processor::shared_api()->engine_prepare(instance, &info, nullptr, "composed", 0, nullptr, 0,
                                                sizeof(vs_shared_target), &composed, 1, &timing);
    check(Config::restored);
    if (scenario == Case::success || scenario == Case::zero_floor) {
      check(result == VS_OK);
      const auto native = static_cast<uint64_t>(std::ceil(Config::native_tail * info.sample_rate));
      check(timing.tail_frames == std::max(native, scenario == Case::success ? uint64_t{6000} : 0));
      check(target(Config::cutoff) == Config::original);
      Processor::api.release(instance);
    } else {
      check(result == VS_ERROR_PROCESS);
    }
  }
  Processor::api.destroy(instance);
}

struct FrameTailPreparation : Configuration {
  static constexpr bool main_thread_lifecycle = true;
  inline static uint64_t floor = 0;
  static bool prepare_instance(Preparation &steps) {
    return steps.minimum_tail_frames(floor) && steps.minimum_tail_seconds(.125) &&
           steps.minimum_tail_frames(1);
  }
};

void frame_tail_floors() {
  using Config = FrameTailPreparation;
  using Processor = vespera::sdk::AudioUnitModule<Config>;
  const vs_prepare_info info{sizeof(info), 48000, 128, 1};
  void *instance = nullptr;
  check(Processor::api.create(&instance) == VS_OK);
  constexpr uint64_t exact = (uint64_t{1} << 53) + 1; // Not representable in a double.
  for (bool shared : {false, true}) {
    for (uint64_t floor : {uint64_t{6001}, exact, UINT64_MAX}) {
      Config::floor = floor;
      vs_module_timing timing{sizeof(timing), 0, 0};
      const auto result = shared ? Processor::shared_api()->engine_prepare(
                                       instance, &info, nullptr, "frame-tail", 0, nullptr, 0,
                                       sizeof(vs_shared_target), nullptr, 0, &timing)
                                 : Processor::api.prepare(instance, &info, &timing);
      check(result == VS_OK && timing.tail_frames == floor);
      Processor::api.release(instance);
    }
  }
  Processor::api.destroy(instance);
}

void main_lifecycle() {
  using Config = MainLifecycleConfiguration;
  using Processor = vespera::sdk::AudioUnitModule<Config>;
  const auto cache = std::make_shared<vespera::InstanceCache>();
  auto client = cache->client(nullptr);
  const vs_prepare_info info{sizeof(info), 48000, 128, 1};
  auto prepare = [&](bool engine, vs_result expected) {
    void *instance = nullptr;
    check(Processor::api.create(&instance) == VS_OK);
    vs_module_timing timing{sizeof(timing), 0, 0};
    const auto result = engine ? Processor::shared_api()->engine_prepare(
                                     instance, &info, client.get(), "fixture", 0, nullptr, 0,
                                     sizeof(vs_shared_target), nullptr, 0, &timing)
                               : Processor::api.prepare(instance, &info, &timing);
    if (result == VS_OK) {
      check(RenderObserver::frames == 304);
      check(RenderObserver::first_time == -304 && RenderObserver::last_time == -64);
      std::array<float, 64> in{}, out{};
      const float *inputs[]{in.data()};
      float *outputs[]{out.data()};
      const vs_process_block block{sizeof(block), inputs, outputs, 1,       64, 0, 0,
                                   nullptr,       0,      nullptr, nullptr, 0,  0};
      check(Processor::api.process(instance, &block) == VS_OK);
      check(RenderObserver::frames == 368 && RenderObserver::last_time == 0);
    }
    Processor::api.destroy(instance);
    check(result == expected);
  };
  on_worker(true, [&] {
    prepare(false, VS_OK);
    prepare(true, VS_OK);
    Config::refuse = true;
    prepare(true, VS_ERROR_PROCESS);
  });
  MainThread::wait(.025);
  check(MainLifecycleObserver::configured == 3);
  check(cache->idle() == 0 && cache->statistics().misses == 0 && cache->statistics().hits == 0);
  on_worker(false, [&] { prepare(false, VS_ERROR_PROCESS); });
  MainThread::wait(.025);
  check(MainLifecycleObserver::configured == 3);

  on_worker(true, [&] {
    void *instance = nullptr;
    check(Processor::api.create(&instance) == VS_OK);
    const vs_prepare_info odd{sizeof(odd), 44100, 128, 1};
    vs_module_timing timing{sizeof(timing), 0, 0};
    check(Processor::api.prepare(instance, &odd, &timing) == VS_OK);
    check(RenderObserver::frames == 13 && RenderObserver::first_time == -13);
    std::array<float, 1> in{}, out{};
    const float *inputs[]{in.data()};
    float *outputs[]{out.data()};
    const vs_process_block block{sizeof(block), inputs, outputs, 1,       1, 0, 0,
                                 nullptr,       0,      nullptr, nullptr, 0, 0};
    check(Processor::api.process(instance, &block) == VS_OK);
    check(RenderObserver::frames == 14 && RenderObserver::last_time == 0);
    Processor::api.destroy(instance);
  });
  std::atomic<bool> ready = false, go = false;
  Config::liveness_ready = &ready;
  Config::liveness_go = &go;
  std::jthread worker([&] { prepare(false, VS_ERROR_PROCESS); });
  while (!ready) {
    MainThread::wait(.005);
  }
  go = true;
  go.notify_one();
  worker.join();
  Config::liveness_ready = Config::liveness_go = nullptr;
  MainThread::wait(.025);
}
}

int main(int argc, char **argv) {
  try {
    native_branch_mapping();
    if (argc == 2 && std::string_view(argv[1]) == "--mapping-only") {
      return 0;
    }
    cache_formats();
    preparation_settling();
    discrete_values();
    hostile_blocks();
    unrouted_default();
    gated_routes();
    route_values<ContinuousRouteConfiguration>(false);
    route_values<DiscreteRouteConfiguration>(true);
    route_pins_direct();
    route_pins_shared();
    main_lifecycle();
    composed_preparation();
    frame_tail_floors();
    std::puts("Audio Unit SDK checks passed");
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
