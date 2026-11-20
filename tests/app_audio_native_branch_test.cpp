#include "fake_audio_engine.h"
#include "fixtures/native_branch_control.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <source_location>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vespera/app_audio.h>

#ifdef __APPLE__
#include <CoreMIDI/CoreMIDI.h>
#include <mach/mach_time.h>
#endif

namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using namespace native_fixture;

constexpr auto branch_slot = "specialFX.a-branch";
constexpr float input = .5f;
constexpr float main_output = input * input + .125f;
constexpr float wet_output = 2 * input - .25f;
float post_output(float amount) {
  return 1.5f * ((1 - amount) * main_output + amount * wet_output);
}

void check(bool value, std::source_location at = std::source_location::current()) {
  if (!value) {
    throw std::runtime_error("bridge native branch check failed at line " +
                             std::to_string(at.line()));
  }
}

void near(float actual, float expected, std::source_location at = std::source_location::current()) {
  check(std::abs(actual - expected) < 1e-6f, at);
}

template <class Predicate> void eventually(Predicate ready) {
  const auto deadline = Clock::now() + 5s;
  while (!ready()) {
    check(Clock::now() < deadline);
    std::this_thread::sleep_for(1ms);
  }
}

struct Fixture {
  explicit Fixture(const char *path) : image(dlopen(path, RTLD_NOW | RTLD_LOCAL), dlclose) {
    check(image != nullptr);
    const auto fn =
        reinterpret_cast<Control *(*)()>(dlsym(image.get(), "native_branch_fixture_control"));
    check(fn != nullptr);
    control = fn();
  }
  std::unique_ptr<void, decltype(&dlclose)> image;
  Control *control = nullptr;
};

#ifdef __APPLE__
struct MidiSource {
  MIDIClientRef client = 0;
  MIDIEndpointRef source = 0;
  SInt32 id = 0;
  MidiSource() {
    const auto result =
        MIDIClientCreate(CFSTR("vespera native branch test"), nullptr, nullptr, &client);
    if (result != noErr) {
      throw std::runtime_error("MIDIClientCreate failed with OSStatus " + std::to_string(result));
    }
    if (MIDISourceCreate(client, CFSTR("vespera native branch test source"), &source) != noErr ||
        MIDIObjectGetIntegerProperty(source, kMIDIPropertyUniqueID, &id) != noErr) {
      if (source) {
        MIDIEndpointDispose(source);
      }
      MIDIClientDispose(client);
      throw std::runtime_error("Could not create virtual MIDI source");
    }
  }
  ~MidiSource() {
    MIDIEndpointDispose(source);
    MIDIClientDispose(client);
  }
  void note(bool on) const {
    const uint8_t bytes[]{static_cast<uint8_t>(on ? 0x91 : 0x81), 64, 100};
    MIDIPacketList packets{};
    check(MIDIPacketListAdd(&packets, sizeof packets, MIDIPacketListInit(&packets),
                            mach_absolute_time(), sizeof bytes, bytes) != nullptr);
    check(MIDIReceived(source, &packets) == noErr);
  }
};
#endif
}

int main(int argc, char **argv) {
  if (argc != 5) {
    return 2;
  }
  try {
    Fixture main(argv[1]), branch(argv[2]), post(argv[3]);
#ifdef __APPLE__
    MidiSource midi;
#endif
    fake_ae::reset();
    const auto *api = vespera_app_audio_entry(1);
    check(api != nullptr);
    const std::unique_ptr<void, decltype(api->destroy)> bridge(api->create(), api->destroy);
    check(bridge != nullptr);
    auto raw = [&](const Json &request) {
      return Json::parse(api->request(bridge.get(), request.dump().c_str()));
    };
    auto send = [&](const Json &request) {
      auto reply = raw(request);
      if (!reply.at("ok").get<bool>()) {
        throw std::runtime_error(reply.at("error").get<std::string>());
      }
      return reply.at("value");
    };
    auto status = [&] { return send({{"op", "status"}}); };
    auto generation = [](const Json &s) {
      return s.at("candidate").at("activeGeneration").get<uint64_t>();
    };
    auto settle = [&] {
      Json result;
      eventually([&] {
        result = status();
        const auto &candidate = result.at("candidate");
        if (candidate.at("state") == "failed") {
          throw std::runtime_error(candidate.at("error").get<std::string>());
        }
        return candidate.at("state") == "ready" && candidate.at("generation") == generation(result);
      });
      return result;
    };
    auto param = [&](uint32_t id, float value) {
      send({{"op", "parameter"}, {"slot", branch_slot}, {"id", id}, {"value", value}});
    };
    auto module = [](const char *path, const char *slot) {
      return Json{{"binary", path}, {"slot", slot}, {"parameters", Json::array()}};
    };
    const auto serial_post = module(argv[3], "specialFX.b-post");
    auto branch_description = [&](float route, float amount, float enabled = 1) {
      auto d = module(argv[2], branch_slot);
      const Json branch_route{
          {"placement", "after-cabinet"},
          {"routing",
           {{"input", "chain-input"}, {"mix", {{"parameter", mix}, {"law", "crossfade"}}}}}};
      d["routingSelector"] = {
          {"parameter", selector},
          {"routes", Json::array({Json{{"placement", "before-pre-fx"}}, branch_route})}};
      d["parameters"] = {{{"id", selector}, {"value", route}},
                         {{"id", mix}, {"value", amount}},
                         {{"id", power}, {"value", enabled}}};
      return d;
    };
    auto foreign = module(argv[4], "specialFX.c-foreign");
    foreign["placement"] = "after-post-fx";
    const Json config{{"audio",
                       {{"backend", "fake"},
                        {"inputDevice", "input-device"},
                        {"outputDevice", "output-device"},
                        {"inputChannels", {0}},
                        {"outputChannels", {0, 1}},
                        {"sampleRate", 48000},
                        {"bufferSize", 128},
                        {"monitor", true}}},
                      {"globals", Json::object()},
                      {"amp", module(argv[1], "amp")}};
    auto configured = [&](const Json &specials) {
      auto result = config;
      result["specialFX"] = specials;
      return result;
    };
    auto activate = [&](const Json &specials) {
      send({{"op", "specialFX"}, {"modules", specials}});
      return settle();
    };
    auto start = [&](const Json &specials) {
      send({{"op", "start"}, {"configuration", configured(specials)}});
      return settle();
    };
    std::array<float, 128> in{}, left{}, right{};
    in.fill(input);
    uint64_t frame = 0;
    auto block = [&] {
      check(fake_ae::invoke(in.data(), left.data(), right.data(), 128, frame) == AE_PROCESS_OK);
      frame += 128;
      return left.back();
    };
    auto total_prepares = [&] {
      return main.control->engine_prepares + main.control->standalone_prepares +
             branch.control->engine_prepares + branch.control->standalone_prepares +
             post.control->engine_prepares + post.control->standalone_prepares;
    };
    auto native_signature = [&] {
      return std::string_view(main.control->signature).starts_with("native-branch/v1/");
    };

    auto pre = start(Json::array({serial_post, branch_description(0, .3f)}));
    const auto base_latency = pre.at("latencyFrames").get<unsigned>();
    // The fixture's ordinary run renders only the leader's output; here Pre leads the run.
    near(block(), wet_output);
    check(branch.control->native_assignments == 0);
#ifdef __APPLE__
    send({{"op", "midi"}, {"slot", "specialFX"}, {"source", midi.id}, {"channel", 2}});
#endif
    auto check_midi_setting = [&](const Json &s) {
#ifdef __APPLE__
      check(s.at("midiSource") == midi.id);
#else
      check(s.at("midiSource") == 0);
#endif
    };

    auto count = total_prepares();
    param(selector, 1);
    auto native = settle();
    check(generation(native) > generation(pre) && native.at("latencyFrames") == base_latency + 17);
    check(total_prepares() == count + 1 && native_signature());
    near(block(), post_output(.3f));
    check_midi_setting(native);

    count = total_prepares();
    const unsigned translations = branch.control->native_translations;
    param(mix, .246f);
    near(block(), post_output(.25f));
    param(power, 0);
    near(block(), post_output(0));
    param(power, 1);
    near(block(), post_output(.25f));
    check(branch.control->native_translations == translations + 3);
    check(generation(status()) == generation(native) && total_prepares() == count);

    const auto mixed_list = Json::array({serial_post, foreign, branch_description(1, .25f)});
    const unsigned assignments = branch.control->native_assignments;
    auto mixed = activate(mixed_list);
    check(generation(mixed) > generation(native) && mixed.at("latencyFrames") == base_latency);
    check(branch.control->native_assignments == assignments && !native_signature());
    near(block(), post_output(.25f));
    check_midi_setting(mixed);
#ifdef __APPLE__
    auto check_notes = [&](float base) {
      midi.note(true);
      eventually([&] { return std::abs(block() - (base + .5f)) < 1e-6f; });
      midi.note(false);
      eventually([&] { return std::abs(block() - base) < 1e-6f; });
    };
    check_notes(post_output(.25f));
#endif
    count = total_prepares();
    param(mix, .396f);
    for (int i = 0; i < 4; ++i) {
      block();
    }
    near(block(), post_output(.40f));
    param(power, 0);
    for (int i = 0; i < 4; ++i) {
      block();
    }
    near(block(), post_output(0));
    check(generation(status()) == generation(mixed) && total_prepares() == count);

    // No status poll between edits: only the final desired graph can be adopted.
    const auto stops = fake_ae::control().stop_calls;
    param(selector, 0);
    param(mix, .636f);
    param(selector, 1);
    near(block(), post_output(0));
    auto superseded = settle();
    check(generation(superseded) >= generation(mixed) + 3);
    check(fake_ae::control().stop_calls == stops + 1);
    check(superseded.at("latencyFrames") == base_latency);
    check_midi_setting(superseded);
    near(block(), post_output(0));
    param(power, 1);
    for (int i = 0; i < 4; ++i) {
      block();
    }
    near(block(), post_output(.64f));
#ifdef __APPLE__
    check_notes(post_output(.64f));
#endif

    const auto eligible = Json::array({branch_description(1, .64f), serial_post});
    fake_ae::control().start_failures = 1;
    const auto different = Json::array({branch_description(1, .1f), serial_post});
    auto failure = send({{"op", "specialFX"}, {"modules", different}});
    eventually([&] {
      failure = status();
      return failure.at("candidate").at("state") == "failed";
    });
    check(!failure.at("candidate").at("error").get<std::string>().empty());
    check(generation(failure) == generation(superseded) && failure.at("state") == "running");
    check(failure.at("latencyFrames") == base_latency);
    near(block(), post_output(.64f));
    check_midi_setting(failure);
#ifdef __APPLE__
    check_notes(post_output(.64f));
#endif
    auto restored = activate(eligible);
    check(native_signature() && generation(restored) > generation(failure));
    check(restored.at("latencyFrames") == base_latency + 17);
    near(block(), post_output(.64f));
    check_midi_setting(restored);
    auto reordered = activate(Json::array({serial_post, branch_description(1, .64f)}));
    check(generation(reordered) > generation(restored) && native_signature());
    check(reordered.at("latencyFrames") == base_latency + 17);
    near(block(), post_output(.64f));

    setenv("VESPERA_ENABLE_RENDER_GRAPH", "1", 1);
    auto offline = [&](const Json &specials, float expected, unsigned latency) {
      auto rendered = send({{"op", "renderGraph"},
                            {"configuration", configured(specials)},
                            {"input", {{input, input, input}}}});
      check(rendered.at("latencyFrames") == latency);
      near(rendered.at("output")[0][2].get<float>(), expected);
      return rendered;
    };
    const auto pre_list = Json::array({serial_post, branch_description(0, .3f)});
    send({{"op", "stop"}});
    const auto pre_render = offline(pre_list, wet_output, 0);
    check(start(pre_list).at("latencyFrames") == base_latency);
    near(block(), pre_render.at("output")[0][2].get<float>());
    for (const auto &list : {Json::array({branch_description(1, .25f), serial_post}),
                             Json::array({serial_post, branch_description(1, .25f)})}) {
      send({{"op", "stop"}});
      auto rendered = offline(list, post_output(.25f), 17);
      check(native_signature());
      auto restarted = start(list);
      check(restarted.at("latencyFrames") == base_latency + 17 && native_signature());
      near(block(), rendered.at("output")[0][2].get<float>());
    }
    for (const auto &list :
         {mixed_list, Json::array({branch_description(1, .25f), foreign, serial_post})}) {
      send({{"op", "stop"}});
      const unsigned before_render = branch.control->native_assignments;
      auto rendered = offline(list, post_output(.25f), 0);
      check(branch.control->native_assignments == before_render);
      auto restarted = start(list);
      check(restarted.at("latencyFrames") == base_latency && !native_signature());
      near(block(), rendered.at("output")[0][2].get<float>());
      check_midi_setting(restarted);
    }

    for (bool explicit_empty : {false, true}) {
      auto with_sections = [&](Json description) {
        if (explicit_empty) {
          for (const char *section : {"preFX", "postFX", "cabinet", "eq", "specialFX"}) {
            if (!description.contains(section)) {
              description[section] = Json::array();
            }
          }
        }
        return description;
      };
      for (bool split : {false, true}) {
        auto specials = Json::array({serial_post, branch_description(0, .25f)});
        if (split) {
          specials.push_back(foreign);
        }
        send({{"op", "start"}, {"configuration", with_sections(configured(specials))}});
        const auto before = settle();
        param(selector, 1);
        param(mix, .4f);
        const auto after = settle();
        check(generation(after) == generation(before) + 2);
        check(after.at("latencyFrames") == base_latency + (split ? 0 : 17));
        near(block(), post_output(.4f));
      }

      // Editing a pending Pre FX pedal must not create an absent Special FX section.
      auto pedal = module(argv[3], "pre-fx.0");
      pedal["parameters"] = {{{"id", power}, {"value", 1}}};
      auto pre_only = config;
      pre_only["preFX"] = Json::array({pedal});
      send({{"op", "start"}, {"configuration", with_sections(pre_only)}});
      check(!raw({{"op", "parameter"}, {"slot", branch_slot}, {"id", power}, {"value", 0}})
                 .at("ok")
                 .get<bool>());
      send({{"op", "parameter"}, {"slot", "pre-fx.0"}, {"id", power}, {"value", 0}});
      const auto playing = settle();
      near(block(), main_output);
      for (const char *section : {"preFX", "postFX", "specialFX"}) {
        auto invalid = with_sections(pre_only);
        invalid[section] = nullptr;
        const auto rejected = raw({{"op", "start"}, {"configuration", invalid}});
        check(!rejected.at("ok").get<bool>() && rejected.at("error") == "Invalid module section");
        check(generation(status()) == generation(playing));
        near(block(), main_output);
      }
    }
    send({{"op", "stop"}});
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
