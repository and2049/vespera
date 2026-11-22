#include "app_audio_client.hpp"
#include "fake_audio_engine.h"
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <nlohmann/json.hpp>
#include <source_location>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <vespera/app_audio.h>
#ifdef __APPLE__
#include <CoreMIDI/CoreMIDI.h>
#include <mach/mach_time.h>
#endif

namespace {
using Json = nlohmann::json;
void check(bool condition, std::source_location location = std::source_location::current()) {
  if (!condition) {
    throw std::runtime_error("audio bridge check failed at line " +
                             std::to_string(location.line()));
  }
}

#ifdef __APPLE__
// Reports CoreMIDI's status when client creation fails. A refused first client creation has been
// seen to persist for the rest of the process (OSStatus -304), so retrying here does not help.
OSStatus create_midi_client(CFStringRef name, MIDIClientRef &client) {
  const auto status = MIDIClientCreate(name, nullptr, nullptr, &client);
  if (status != noErr) {
    std::fprintf(stderr, "MIDIClientCreate failed with OSStatus %d\n", static_cast<int>(status));
  }
  return status;
}
#endif

struct SpecialFixtures {
  const char *amp;
  const char *shared_a;
  const char *shared_b;
  const char *shared_lead;
  std::array<const char *, 3> pedals;
  std::array<const char *, 9> midi;
};

constexpr std::array<const char *, 6> placements{"before-pre-fx", "before-amp", "before-cabinet",
                                                 "after-cabinet", "after-eq",   "after-post-fx"};

struct Affine {
  float gain, offset;
};

// Special FX placement, ordering, validation, parameters, latency and Note MIDI fan-out. Every
// pedal applies x * gain + offset, so the processing order is audible.
void special_fx(const vs_app_audio_api *api, const SpecialFixtures &fixtures) {
  fake_ae::reset();
  void *instance = api->create();
  check(instance != nullptr);
  auto send = [&](const Json &request) { return settled_audio_request(api, instance, request); };
  auto pedal = [](const char *binary, const std::string &slot, Affine affine, float enabled = 1) {
    return Json{{"slot", slot},
                {"binary", binary},
                {"parameters",
                 {{{"id", 1}, {"value", enabled}},
                  {{"id", 2}, {"value", affine.gain}},
                  {{"id", 3}, {"value", affine.offset}}}}};
  };
  auto special = [&](size_t index, const char *placement, Affine affine) {
    auto result = pedal(fixtures.pedals[index],
                        std::string("specialFX.test.special.") + char('a' + index), affine);
    if (placement) {
      result["placement"] = placement;
    }
    return result;
  };
  const Affine pre{1.5f, .01f}, amp{2, 0}, cabinet{1.1f, .03f}, eq{.9f, .04f}, post{1.2f, .02f};
  const std::array<Affine, 3> specials{{{1.3f, .05f}, {.7f, .08f}, {1.6f, .02f}}};
  Json neighbours{{"amp", {{"binary", fixtures.amp}, {"parameters", {{{"id", 1}, {"value", 2}}}}}},
                  {"preFX", Json::array({pedal(fixtures.pedals[2], "pre-fx.0", pre)})},
                  {"cabinet", Json::array({pedal(fixtures.pedals[2], "cabinet", cabinet)})},
                  {"eq", Json::array({pedal(fixtures.pedals[2], "eq", eq)})},
                  {"postFX", Json::array({pedal(fixtures.pedals[2], "post-fx.0", post)})}};
  constexpr std::array<const char *, 5> sections{"preFX", "amp", "cabinet", "eq", "postFX"};
  const std::array<Affine, 5> section_affine{pre, amp, cabinet, eq, post};
  auto preset = [&](unsigned mask, const Json &special_fx) {
    Json request{{"op", "preset"}, {"globals", Json::object()}, {"specialFX", special_fx}};
    for (size_t s = 0; s < sections.size(); ++s) {
      const bool present = mask & (1u << s);
      if (std::string_view(sections[s]) == "amp") {
        request["amp"] = present ? neighbours["amp"] : Json();
      } else {
        request[sections[s]] = present ? neighbours[sections[s]] : Json::array();
      }
    }
    return request;
  };
  // The expected output for an input when each special (placement, affine) runs in its gap, the
  // listed specials of one gap already in their expected order.
  auto expected = [&](float x, unsigned mask,
                      const std::vector<std::pair<size_t, Affine>> &gap_entries) {
    for (size_t gap = 0; gap <= sections.size(); ++gap) {
      for (const auto &[placement, affine] : gap_entries) {
        if (placement == gap) {
          x = x * affine.gain + affine.offset;
        }
      }
      if (gap < sections.size() && (mask & (1u << gap))) {
        x = x * section_affine[gap].gain + section_affine[gap].offset;
      }
    }
    return x;
  };
  Json config{{"audio",
               {{"backend", "fake"},
                {"inputDevice", "input-device"},
                {"outputDevice", "output-device"},
                {"inputChannels", {0}},
                {"outputChannels", {0, 1}},
                {"sampleRate", 48000},
                {"bufferSize", 128},
                {"monitor", true}}},
              {"globals", Json::object()}};
  check(send({{"op", "start"}, {"configuration", config}}).at("ok"));
  std::array<float, 128> input{}, left{}, right{};
  input.fill(0.1f);
  // A continuous timeline: a repeated position is a discontinuity that drops scheduled events.
  uint64_t position = 0;
  auto output = [&](std::source_location location = std::source_location::current()) {
    check(fake_ae::invoke(input.data(), left.data(), right.data(), 128, position) == AE_PROCESS_OK,
          location);
    position += 128;
    return left[127];
  };
  auto near = [](float actual, float value) { return std::abs(actual - value) < 1e-5f; };
  // Knob changes are smoothed over a few blocks.
  auto smoothed = [&](std::source_location location = std::source_location::current()) {
    for (int block = 0; block < 8; ++block) {
      output(location);
    }
    return output(location);
  };

  // Each placement, with every neighbour, none, and each one missing, matches the gap model.
  const std::array<unsigned, 7> masks{31, 0, 31 & ~1u, 31 & ~2u, 31 & ~4u, 31 & ~8u, 31 & ~16u};
  for (size_t p = 0; p < placements.size(); ++p) {
    for (const auto mask : masks) {
      check(send(preset(mask, Json::array({special(0, placements[p], specials[0])}))).at("ok"));
      check(near(output(), expected(0.1f, mask, {{p, specials[0]}})));
    }
  }
  // Every placement is distinguishable with all neighbours present.
  for (size_t p = 1; p < placements.size(); ++p) {
    check(
        !near(expected(0.1f, 31, {{p - 1, specials[0]}}), expected(0.1f, 31, {{p, specials[0]}})));
  }

  // Within one gap the bridge orders by slot, whatever the request order.
  const auto ab = expected(0.1f, 31, {{2, specials[0]}, {2, specials[1]}});
  check(!near(ab, expected(0.1f, 31, {{2, specials[1]}, {2, specials[0]}})));
  for (const auto &request : {Json::array({special(0, "before-cabinet", specials[0]),
                                           special(1, "before-cabinet", specials[1])}),
                              Json::array({special(1, "before-cabinet", specials[1]),
                                           special(0, "before-cabinet", specials[0])})}) {
    check(send(preset(31, request)).at("ok"));
    check(near(output(), ab));
  }

  // Three gaps at once, listed in reverse.
  const auto spread =
      Json::array({special(2, "after-post-fx", specials[2]), special(1, "after-eq", specials[1]),
                   special(0, "before-pre-fx", specials[0])});
  check(send(preset(31, spread)).at("ok"));
  const auto spread_output =
      expected(0.1f, 31, {{0, specials[0]}, {4, specials[1]}, {5, specials[2]}});
  check(near(output(), spread_output));

  // The legacy bare slot without a placement runs after the cabinet, ahead of namespaced slots.
  auto legacy = pedal(fixtures.pedals[2], "specialFX", specials[2]);
  check(send(preset(31, Json::array({legacy}))).at("ok"));
  check(near(output(), expected(0.1f, 31, {{3, specials[2]}})));
  check(send(preset(31, Json::array({special(0, "after-cabinet", specials[0]), legacy}))).at("ok"));
  check(near(output(), expected(0.1f, 31, {{3, specials[2]}, {3, specials[0]}})));
  auto null_placement = special(0, nullptr, specials[0]);
  null_placement["placement"] = nullptr;
  check(send(preset(31, Json::array({null_placement}))).at("ok"));
  check(near(output(), expected(0.1f, 31, {{3, specials[0]}})));

  // Rejected requests leave the previous graph audible.
  check(send(preset(31, spread)).at("ok"));
  auto rejected = [&](const Json &request,
                      std::source_location location = std::source_location::current()) {
    check(!send(request).at("ok").get<bool>(), location);
    check(near(output(), spread_output), location);
  };
  for (const Json &placement : {Json("after-amp"), Json(""), Json("AFTER-CABINET"), Json(3),
                                Json(true), Json::array({"after-eq"})}) {
    auto bad = special(0, nullptr, specials[0]);
    bad["placement"] = placement;
    rejected(preset(31, Json::array({bad})));
  }
  for (const char *section : {"amp", "preFX", "cabinet", "eq", "postFX"}) {
    auto request = preset(31, spread);
    auto &target = std::string_view(section) == "amp" ? request[section] : request[section][0];
    target["placement"] = "after-cabinet";
    rejected(request);
  }
  for (const char *slot : {"specialFX.", "pre-fx.0", "cabinet", "eq", ""}) {
    rejected(preset(31, Json::array({pedal(fixtures.pedals[0], slot, specials[0])})));
  }
  for (const char *slot : {"specialFX", "specialFX.test.special.a"}) {
    auto request = preset(31, Json::array());
    request["postFX"] = Json::array({pedal(fixtures.pedals[0], slot, specials[0])});
    rejected(request);
    request = preset(31, Json::array());
    request["preFX"][0]["slot"] = slot;
    rejected(request);
  }
  auto twin = special(1, "after-eq", specials[1]);
  twin["slot"] = "specialFX.test.special.a";
  rejected(preset(31, Json::array({special(0, "after-eq", specials[0]), twin})));
  auto declared = [&](size_t index, const char *slot) {
    auto result = special(index, "after-eq", specials[index]);
    result["module"] = "test.special.a";
    result["slot"] = slot;
    return result;
  };
  rejected(preset(31, Json::array({declared(0, "specialFX.one"), declared(0, "specialFX.two")})));
  auto copy = special(0, "after-eq", specials[0]);
  copy["slot"] = "specialFX.copy";
  rejected(preset(31, Json::array({special(0, "after-eq", specials[0]), copy})));
  copy["bypassed"] = true;
  rejected(preset(31, Json::array({special(0, "after-eq", specials[0]), copy})));
  rejected({{"op", "specialFX"}, {"modules", Json::object()}});

  // Live and staged parameter updates address specials by slot.
  auto parameter = [&](const char *slot, uint32_t id, float value) {
    return send({{"op", "parameter"}, {"slot", slot}, {"id", id}, {"value", value}});
  };
  const auto &control = fake_ae::control();
  auto opens = control.open_calls;
  auto stops = control.stop_calls;
  check(parameter("specialFX.test.special.b", 2, 1.5f).at("ok"));
  check(control.open_calls == opens && control.stop_calls == stops);
  check(near(
      smoothed(),
      expected(0.1f, 31, {{0, specials[0]}, {4, {1.5f, specials[1].offset}}, {5, specials[2]}})));
  check(!parameter("specialFX.test.special.b", 2, 9).at("ok").get<bool>());
  check(!parameter("specialFX.test.special.missing", 2, 1).at("ok").get<bool>());
  check(!parameter("specialFX", 2, 1).at("ok").get<bool>());
  check(parameter("specialFX.test.special.b", 2, specials[1].gain).at("ok"));
  // Pedal fixtures declare parameter 1 as their bypass and do not share an engine, so switching
  // one off rebuilds without it.
  check(parameter("specialFX.test.special.a", 1, 0).at("ok"));
  check(control.stop_calls == stops + 1);
  check(near(smoothed(), expected(0.1f, 31, {{4, specials[1]}, {5, specials[2]}})));
  check(parameter("specialFX.test.special.a", 2, 2).at("ok"));
  check(near(smoothed(), expected(0.1f, 31, {{4, specials[1]}, {5, specials[2]}})));
  check(parameter("specialFX.test.special.a", 1, 1).at("ok"));
  check(
      near(smoothed(),
           expected(0.1f, 31, {{0, {2, specials[0].offset}}, {4, specials[1]}, {5, specials[2]}})));
  check(parameter("specialFX.test.special.a", 2, specials[0].gain).at("ok"));
  check(near(smoothed(), spread_output));

  // A parameter sent while a section change is still a candidate lands in that candidate.
  auto raw = [&](const Json &request) {
    return Json::parse(api->request(instance, request.dump().c_str()));
  };
  auto moved =
      Json::array({special(0, "after-eq", specials[0]), special(2, "before-amp", specials[2])});
  check(raw({{"op", "specialFX"}, {"modules", moved}}).at("ok"));
  check(raw({{"op", "parameter"}, {"slot", "specialFX.test.special.c"}, {"id", 3}, {"value", .06f}})
            .at("ok"));
  const auto settle_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  for (;;) {
    const auto candidate = send({{"op", "status"}}).at("value").at("candidate");
    check(candidate.at("state") != "failed");
    if (candidate.at("state") == "ready" &&
        candidate.at("generation") == candidate.at("activeGeneration")) {
      break;
    }
    check(std::chrono::steady_clock::now() < settle_deadline);
    std::this_thread::yield();
  }
  auto moved_output = [&](float c_offset) {
    return expected(0.1f, 31, {{1, {specials[2].gain, c_offset}}, {4, specials[0]}});
  };
  check(near(smoothed(), moved_output(.06f)));
  moved[1]["parameters"][2]["value"] = .06f;

  // A rejected section change leaves the previous graph and its parameters.
  fake_ae::control().start_failures = 1;
  auto failed = send({{"op", "specialFX"}, {"modules", spread}});
  check(!failed.at("ok").get<bool>());
  check(failed.at("error").get<std::string>().find("restored") == std::string::npos);
  check(near(smoothed(), moved_output(.06f)));
  check(parameter("specialFX.test.special.c", 3, .07f).at("ok"));
  check(near(smoothed(), moved_output(.07f)));
  auto broken = moved;
  broken[0]["binary"] = "/missing/special.so";
  check(!send({{"op", "specialFX"}, {"modules", broken}}).at("ok").get<bool>());
  check(near(smoothed(), moved_output(.07f)));
  check(parameter("specialFX.test.special.c", 3, .06f).at("ok"));

  // Shared-engine specials: adjacent in one gap they form one run whatever the request order,
  // switch off live, and split when another node separates them. A MIDI-capable section never
  // joins a run.
  auto shared = [](const char *binary, const char *slot, const char *placement, Affine affine) {
    return Json{{"slot", slot},
                {"binary", binary},
                {"placement", placement},
                {"parameters",
                 {{{"id", 1}, {"value", 1}},
                  {{"id", 2}, {"value", affine.gain}},
                  {{"id", 3}, {"value", affine.offset}}}}};
  };
  auto latency = [&]() {
    return send({{"op", "status"}}).at("value").at("latencyFrames").get<uint32_t>();
  };
  check(send(preset(2, Json::array())).at("ok"));
  const auto base_latency = latency();
  const Affine shared_a{2, 0}, shared_b{1, .1f};
  const auto shared_ab = expected(0.1f, 2, {{1, shared_a}, {1, shared_b}});
  for (const auto &request :
       {Json::array({shared(fixtures.shared_a, "specialFX.test.shared.a", "before-amp", shared_a),
                     shared(fixtures.shared_b, "specialFX.test.shared.b", "before-amp", shared_b)}),
        Json::array(
            {shared(fixtures.shared_b, "specialFX.test.shared.b", "before-amp", shared_b),
             shared(fixtures.shared_a, "specialFX.test.shared.a", "before-amp", shared_a)})}) {
    check(send(preset(2, request)).at("ok"));
    check(near(output(), shared_ab));
    check(latency() == base_latency + 7);
  }
  opens = control.open_calls;
  stops = control.stop_calls;
  check(parameter("specialFX.test.shared.a", 1, 0).at("ok"));
  check(near(output(), expected(0.1f, 2, {{1, shared_b}})));
  check(parameter("specialFX.test.shared.a", 1, 1).at("ok"));
  check(near(output(), shared_ab));
  check(control.open_calls == opens && control.stop_calls == stops);
  check(latency() == base_latency + 7);
  check(send(preset(2, Json::array({shared(fixtures.shared_a, "specialFX.test.shared.a",
                                           "before-amp", shared_a),
                                    shared(fixtures.shared_b, "specialFX.test.shared.b",
                                           "before-cabinet", shared_b)})))
            .at("ok"));
  check(near(output(), expected(0.1f, 2, {{1, shared_a}, {2, shared_b}})));
  check(latency() == base_latency + 14);
  check(send(preset(2, Json::array({shared(fixtures.shared_a, "specialFX.test.shared.a",
                                           "before-amp", shared_a),
                                    shared(fixtures.shared_lead, "specialFX.test.shared.lead",
                                           "before-amp", shared_b)})))
            .at("ok"));
  check(latency() == base_latency + 14);

#ifdef __APPLE__
  // Note MIDI reaches every active MIDI-capable special in processing order, up to eight.
  struct VirtualSource {
    MIDIClientRef client = 0;
    MIDIEndpointRef source = 0;
    VirtualSource() {
      check(create_midi_client(CFSTR("vespera bridge test"), client) == noErr);
      check(MIDISourceCreate(client, CFSTR("vespera bridge source"), &source) == noErr);
    }
    ~VirtualSource() {
      MIDIEndpointDispose(source);
      MIDIClientDispose(client);
    }
  } virtual_source;
  SInt32 source_id = 0;
  check(MIDIObjectGetIntegerProperty(virtual_source.source, kMIDIPropertyUniqueID, &source_id) ==
        noErr);
  auto send_bytes = [&](const std::vector<uint8_t> &bytes) {
    std::vector<std::byte> storage(bytes.size() + 1024);
    auto *packets = reinterpret_cast<MIDIPacketList *>(storage.data());
    check(MIDIPacketListAdd(packets, storage.size(), MIDIPacketListInit(packets),
                            mach_absolute_time(), bytes.size(), bytes.data()) != nullptr);
    check(MIDIReceived(virtual_source.source, packets) == noErr);
  };
  auto settles_at = [&](float value) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
      if (near(output(), value)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return near(output(), value);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
  };
  auto stays_at = [&](float value) {
    for (int block = 0; block < 20; ++block) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      if (!near(output(), value)) {
        return false;
      }
    }
    return true;
  };
  auto midi = [&](size_t index, const char *placement) {
    return Json{{"slot", "specialFX.test.midi.special." + std::to_string(index)},
                {"binary", fixtures.midi[index]},
                {"placement", placement},
                {"parameters", Json::array()}};
  };
  const std::vector<uint8_t> note_on{0x91, 64, 100}, note_off{0x81, 64, 0};
  auto special_a = special(0, "after-cabinet", specials[0]);
  check(
      send(preset(0, Json::array({midi(1, "before-pre-fx"), special_a, midi(2, "after-post-fx")})))
          .at("ok"));
  check(send({{"op", "midi"}, {"source", source_id}, {"channel", 2}, {"slot", "specialFX"}})
            .at("ok"));
  const float two_base = expected(0.1f, 0, {{3, specials[0]}});
  check(near(output(), two_base));
  send_bytes(note_on);
  check(settles_at(expected(0.1f, 0, {{0, {1, .5f}}, {3, specials[0]}, {5, {1, .5f}}})));
  send_bytes(note_off);
  check(settles_at(two_base));
  send_bytes(note_on);
  check(settles_at(expected(0.1f, 0, {{0, {1, .5f}}, {3, specials[0]}, {5, {1, .5f}}})));
  check(send({{"op", "midiPanic"}}).at("ok"));
  check(near(output(), two_base));

  // A burst that exceeds the shared queue once per target panics instead of keeping a partial
  // set of notes.
  const auto dropped_before =
      send({{"op", "status"}}).at("value").at("midiDropped").get<uint64_t>();
  std::vector<uint8_t> burst;
  for (int note = 0; note < 65; ++note) {
    burst.insert(burst.end(), {0x91, static_cast<uint8_t>(note), 100});
  }
  send_bytes(burst);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  uint64_t dropped = dropped_before;
  while (dropped < dropped_before + 2 && std::chrono::steady_clock::now() < deadline) {
    dropped = send({{"op", "status"}}).at("value").at("midiDropped").get<uint64_t>();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  check(dropped == dropped_before + 2);
  check(near(output(), two_base));
  check(stays_at(two_base));

  // No MIDI-capable special: nothing receives notes.
  check(send(preset(0, Json::array({special_a}))).at("ok"));
  send_bytes(note_on);
  check(stays_at(expected(0.1f, 0, {{3, specials[0]}})));

  // Nine MIDI specials: the last in processing order, here the bytewise-first slot placed after
  // Post FX behind a non-MIDI special, is left out.
  auto nine = Json::array({midi(0, "after-post-fx"), special_a});
  for (size_t index = 1; index < 9; ++index) {
    nine.push_back(midi(index, "before-pre-fx"));
  }
  check(send(preset(0, nine)).at("ok"));
  const float nine_base = expected(0.1f, 0, {{3, specials[0]}});
  check(near(output(), nine_base));
  send_bytes(note_on);
  const float first_eight = expected(0.1f, 0, {{0, {1, 8 * .5f}}, {3, specials[0]}});
  check(
      !near(first_eight, expected(0.1f, 0, {{0, {1, 7 * .5f}}, {3, specials[0]}, {5, {1, .5f}}})));
  check(settles_at(first_eight));
  check(stays_at(first_eight));
  check(send({{"op", "midiPanic"}}).at("ok"));
  check(near(output(), nine_base));

  // Disconnecting releases held notes and stops delivery.
  check(send(preset(0, Json::array({midi(1, "before-amp")}))).at("ok"));
  send_bytes(note_on);
  check(settles_at(0.6f));
  check(send({{"op", "midi"}, {"source", 0}, {"channel", 2}, {"slot", "specialFX"}}).at("ok"));
  check(near(output(), 0.1f));
  send_bytes(note_on);
  check(stays_at(0.1f));

  // A source that disappears is forgotten at the next status poll and stays forgotten when a
  // source with the same unique ID returns, until it is chosen again.
  auto status_source = [&] {
    return send({{"op", "status"}}).at("value").at("midiSource").get<int32_t>();
  };
  MIDIEndpointRef transient = 0;
  check(MIDISourceCreate(virtual_source.client, CFSTR("vespera transient source"), &transient) ==
        noErr);
  SInt32 transient_id = 0;
  check(MIDIObjectGetIntegerProperty(transient, kMIDIPropertyUniqueID, &transient_id) == noErr);
  check(send({{"op", "midi"}, {"source", transient_id}, {"channel", 2}, {"slot", "specialFX"}})
            .at("ok"));
  check(status_source() == transient_id);
  check(status_source() == transient_id);
  check(MIDIEndpointDispose(transient) == noErr);
  check(status_source() == 0);
  check(!send({{"op", "midi"}, {"source", transient_id}, {"channel", 2}, {"slot", "specialFX"}})
             .at("ok")
             .get<bool>());
  check(MIDISourceCreate(virtual_source.client, CFSTR("vespera returning source"), &transient) ==
        noErr);
  check(MIDIObjectSetIntegerProperty(transient, kMIDIPropertyUniqueID, transient_id) == noErr);
  check(status_source() == 0);
  check(send({{"op", "midi"}, {"source", transient_id}, {"channel", 2}, {"slot", "specialFX"}})
            .at("ok"));
  check(status_source() == transient_id);
  check(MIDIEndpointDispose(transient) == noErr);
  check(status_source() == 0);
#endif

  check(send({{"op", "stop"}}).at("ok"));
  api->destroy(instance);
  check(fake_ae::control().contexts_alive == 0);
}
}

int main(int argc, char **argv) {
  if (argc != 20) {
    return 1;
  }
  try {
    fake_ae::reset();
    const auto *api = vespera_app_audio_entry(1);
    check(api && !vespera_app_audio_entry(0));
    void *instance = api->create();
    check(instance != nullptr);
    auto send = [&](const Json &request) { return settled_audio_request(api, instance, request); };
    check(send({{"op", "midiDevices"}}).at("value").is_array());
    check(!send({{"op", "midi"}, {"source", 0}, {"channel", 17}, {"slot", "specialFX"}})
               .at("ok")
               .get<bool>());
    check(send({{"op", "midi"}, {"source", 0}, {"channel", 16}, {"slot", "specialFX"}}).at("ok"));
    check(send({{"op", "midiPanic"}}).at("ok"));
    Json config{{"audio",
                 {{"backend", "fake"},
                  {"inputDevice", "input-device"},
                  {"outputDevice", "output-device"},
                  {"inputChannels", {2}},
                  {"outputChannels", {3, 0}},
                  {"sampleRate", 48000},
                  {"bufferSize", 128},
                  {"monitor", true}}},
                {"globals", Json::object()},
                {"amp", {{"binary", argv[1]}, {"parameters", {{{"id", 1}, {"value", 2.0}}}}}}};
    check(send({{"op", "start"}, {"configuration", config}}).at("ok"));
    check(fake_ae::control().requested_input_device == "input-device");
    check(fake_ae::control().requested_output_device == "output-device");
    check(fake_ae::control().requested_inputs == std::vector<uint32_t>{2});
    check(fake_ae::control().requested_outputs == std::vector<uint32_t>({3, 0}));
    std::array<float, 128> input{}, left{}, right{};
    input.fill(0.25f);
    check(fake_ae::invoke(input.data(), left.data(), right.data(), 128, 0) == AE_PROCESS_OK);
    check(left[127] == 0.5f && right[127] == 0.5f);
    auto levels = send({{"op", "status"}}).at("value");
    check(levels["input"][0] == 0.25f && levels["output"][0] == 0.5f);
    check(send({{"op", "parameter"}, {"id", 1}, {"value", 3.0}}).at("ok"));
    check(fake_ae::invoke(input.data(), left.data(), right.data(), 128, 128) == AE_PROCESS_OK);
    check(left[127] == 0.75f);
    auto invalid = config;
    invalid["audio"]["inputChannels"] = Json::array();
    check(!send({{"op", "start"}, {"configuration", invalid}}).at("ok").get<bool>());
    check(fake_ae::invoke(input.data(), left.data(), right.data(), 128, 0) == AE_PROCESS_OK);
    check(left[127] == 0.75f);
    invalid = config;
    invalid["amp"]["binary"] = "/missing/vespera-module.dylib";
    check(!send({{"op", "start"}, {"configuration", invalid}}).at("ok").get<bool>());
    check(fake_ae::invoke(input.data(), left.data(), right.data(), 128, 0) == AE_PROCESS_OK);
    check(left[127] == 0.75f);
    check(send({{"op", "globals"}, {"globals", {{"outputGain", -6.0}}}}).at("ok"));
    send({{"op", "status"}});
    for (uint64_t block = 1; block <= 10; ++block) {
      check(fake_ae::invoke(input.data(), left.data(), right.data(), 128, block * 128) ==
            AE_PROCESS_OK);
    }
    check(std::abs(left[127] - 0.75f * std::pow(10.0f, -6.0f / 20.0f)) < 0.00001f);
    config["audio"]["outputChannels"] = {1};
    check(send({{"op", "start"}, {"configuration", config}}).at("ok"));
    const float *inputs[] = {input.data()};
    float *outputs[] = {left.data()};
    const auto &control = fake_ae::control();
    ae_audio_block block{};
    block.struct_size = sizeof(block);
    block.inputs = inputs;
    block.outputs = outputs;
    block.input_channels = 1;
    block.output_channels = 1;
    block.frames = 128;
    block.stream_id = control.stream_id;
    block.configuration_generation = control.configuration_generation;
    block.run_generation = control.run_generation;
    check(control.process(control.user, &block) == AE_PROCESS_OK);
    check(left[127] == 0.5f);
    auto pedal = [&](const char *slot, float gain, float offset) {
      return Json{{"slot", slot},
                  {"binary", argv[2]},
                  {"parameters",
                   {{{"id", 1}, {"value", 0}},
                    {{"id", 2}, {"value", gain}},
                    {{"id", 3}, {"value", offset}}}}};
    };
    auto pedals = Json::array({pedal("first", 1.5f, 0.01f), pedal("second", 1.2f, 0.02f)});
    config["preFX"] = pedals;
    config["audio"]["outputChannels"] = {3, 0};
    input.fill(0.1f);
    check(send({{"op", "start"}, {"configuration", config}}).at("ok"));
    auto output = [&](std::source_location location = std::source_location::current()) {
      check(fake_ae::invoke(input.data(), left.data(), right.data(), 128, 0) == AE_PROCESS_OK,
            location);
      return left[127];
    };
    check(std::abs(output() - 0.2f) < 1e-6f);
    auto parameter = [&](const char *slot, uint32_t id, float value) {
      return send({{"op", "parameter"}, {"slot", slot}, {"id", id}, {"value", value}});
    };
    check(parameter("first", 1, 1).at("ok"));
    check(std::abs(output() - 0.32f) < 1e-6f);
    check(parameter("second", 1, 1).at("ok"));
    check(std::abs(output() - 0.424f) < 1e-6f);
    pedals[0]["parameters"][0]["value"] = 1;
    pedals[1]["parameters"][0]["value"] = 1;
    auto reordered = Json::array({pedals[1], pedals[0]});
    check(send({{"op", "preFX"}, {"modules", reordered}}).at("ok"));
    check(std::abs(output() - 0.44f) < 1e-6f);
    check(parameter("second", 1, 0).at("ok"));
    check(std::abs(output() - 0.32f) < 1e-6f);
    check(parameter("second", 2, 2).at("ok"));
    check(std::abs(output() - 0.32f) < 1e-6f);
    check(!parameter("second", 2, 9).at("ok").get<bool>());
    check(!parameter("unknown", 1, 1).at("ok").get<bool>());
    check(parameter("second", 1, 1).at("ok"));
    check(std::abs(output() - 0.68f) < 1e-6f);
    auto broken = reordered;
    broken[0]["binary"] = "/missing/pedal.so";
    check(!send({{"op", "preFX"}, {"modules", broken}}).at("ok").get<bool>());
    check(std::abs(output() - 0.68f) < 1e-6f);
    check(!send({{"op", "preFX"}, {"modules", Json::array({pedals[0], pedals[0]})}})
               .at("ok")
               .get<bool>());
    check(std::abs(output() - 0.68f) < 1e-6f);
    check(parameter("first", 1, 0).at("ok"));
    check(parameter("second", 1, 0).at("ok"));
    check(std::abs(output() - 0.2f) < 1e-6f);
    check(send({{"op", "stop"}}).at("ok"));
    auto four =
        Json::array({pedal("a", 1, 0), pedal("b", 1, 0), pedal("c", 1, 0), pedal("d", 1, 0)});
    config["preFX"] = four;
    check(send({{"op", "start"}, {"configuration", config}}).at("ok"));
    auto six = four;
    six.push_back(pedal("e", 1, 0));
    six.push_back(pedal("f", 1, 0));
    check(send({{"op", "preFX"}, {"modules", six}}).at("ok"));
    auto post_four =
        Json::array({pedal("g", 1, 0), pedal("h", 1, 0), pedal("i", 1, 0), pedal("j", 1, 0)});
    check(send({{"op", "postFX"}, {"modules", post_four}}).at("ok"));
    check(!send({{"op", "postFX"}, {"modules", four}}).at("ok").get<bool>());
    check(!send({{"op", "postFX"}, {"modules", Json::array({pedal("", 1, 0)})}})
               .at("ok")
               .get<bool>());
    check(std::abs(output() - 0.2f) < 1e-6f);
    auto bypassing = six;
    bypassing[4]["parameters"][0]["value"] = 1;
    bypassing[4]["parameters"][1]["value"] = 1.5f;
    bypassing[4]["bypassed"] = true;
    check(send({{"op", "preFX"}, {"modules", bypassing}}).at("ok"));
    check(std::abs(output() - 0.2f) < 1e-6f);
    check(parameter("e", 2, 2).at("ok"));
    check(parameter("e", 1, 1).at("ok"));
    check(std::abs(output() - 0.2f) < 1e-6f);
    bypassing[4]["parameters"][1]["value"] = 2;
    bypassing[4]["bypassed"] = false;
    check(send({{"op", "preFX"}, {"modules", bypassing}}).at("ok"));
    check(std::abs(output() - 0.4f) < 1e-6f);
    auto malformed_bypass = bypassing;
    malformed_bypass[4]["bypassed"] = "yes";
    check(!send({{"op", "preFX"}, {"modules", malformed_bypass}}).at("ok").get<bool>());
    check(std::abs(output() - 0.4f) < 1e-6f);
    check(send({{"op", "stop"}}).at("ok"));
    config["preFX"] = Json::array({pedal("pre", 1.5f, 0.01f)});
    config["preFX"][0]["parameters"][0]["value"] = 1;
    auto post = Json::array({pedal("post-a", 1.2f, 0.02f), pedal("post-b", 1.5f, 0.01f)});
    post[0]["parameters"][0]["value"] = 1;
    post[1]["parameters"][0]["value"] = 1;
    config["postFX"] = post;
    check(send({{"op", "start"}, {"configuration", config}}).at("ok"));
    check(std::abs(output() - 0.616f) < 1e-6f);
    auto cabinet = Json::array({pedal("cabinet", 1.1f, .03f)});
    cabinet[0]["parameters"][0]["value"] = 1;
    check(send({{"op", "rig"}, {"amp", config["amp"]}, {"cabinet", cabinet}}).at("ok"));
    check(std::abs(output() - .7276f) < 1e-6f);
    check(parameter("cabinet", 1, 0).at("ok"));
    check(std::abs(output() - .616f) < 1e-6f);
    check(parameter("cabinet", 2, 1.2f).at("ok"));
    check(std::abs(output() - .616f) < 1e-6f);
    check(parameter("cabinet", 1, 1).at("ok"));
    check(std::abs(output() - .7852f) < 1e-6f);
    auto special = Json::array({pedal("specialFX", 1.3f, .02f)});
    special[0]["parameters"][0]["value"] = 1;
    check(send({{"op", "specialFX"}, {"modules", special}}).at("ok"));
    check(std::abs(output() - 1.04476f) < 1e-6f);
    auto live_cabinet = cabinet;
    live_cabinet[0]["parameters"][1]["value"] = 1.2f;
    uint64_t position = 0;
    auto rig = [&](const Json &equalizer) {
      position = 0;
      return send(
          {{"op", "rig"}, {"amp", config["amp"]}, {"cabinet", live_cabinet}, {"eq", equalizer}});
    };
    auto continuous = [&](std::source_location location = std::source_location::current()) {
      check(fake_ae::invoke(input.data(), left.data(), right.data(), 128, position) ==
                AE_PROCESS_OK,
            location);
      position += 128;
      return left[127];
    };
    auto eq_parameter = [&](uint32_t id, float value) {
      const auto reply = parameter("eq", id, value);
      if (id == 1) {
        position = 0;
      }
      return reply;
    };
    auto eq = Json::array({pedal("eq", 2, .05f)});
    eq[0]["parameters"][0]["value"] = 1;
    check(rig(eq).at("ok"));
    check(std::abs(continuous() - 2.13952f) < 1e-5f);
    check(eq_parameter(2, 3).at("ok"));
    for (int iteration = 0; iteration < 4; ++iteration) {
      continuous();
    }
    check(std::abs(continuous() - 3.14428f) < 1e-5f);
    check(!eq_parameter(2, 9).at("ok").get<bool>());
    check(!eq_parameter(9, 1).at("ok").get<bool>());
    check(std::abs(continuous() - 3.14428f) < 1e-5f);
    check(eq_parameter(1, 0).at("ok"));
    check(std::abs(continuous() - 1.04476f) < 1e-6f);
    check(eq_parameter(1, 1).at("ok"));
    check(std::abs(continuous() - 3.14428f) < 1e-5f);
    auto broken_eq = eq;
    broken_eq[0]["binary"] = "/missing/eq.so";
    auto misnamed_eq = Json::array({pedal("cabinet", 2, 0)});
    for (auto malformed : {broken_eq, Json::array({eq[0], eq[0]}), misnamed_eq,
                           Json(Json::object()), Json::array({pedal("first", 1, 0)})}) {
      check(!rig(malformed).at("ok").get<bool>());
      check(std::abs(continuous() - 3.14428f) < 1e-5f);
    }
    for (const char *section : {"preFX", "postFX", "specialFX"}) {
      position = 0;
      check(!send({{"op", section}, {"modules", eq}}).at("ok").get<bool>());
      check(std::abs(continuous() - 3.14428f) < 1e-5f);
    }
    check(rig(Json::array()).at("ok"));
    check(std::abs(continuous() - 1.04476f) < 1e-6f);
    check(!eq_parameter(2, 1).at("ok").get<bool>());
    check(send({{"op", "specialFX"}, {"modules", Json::array()}}).at("ok"));
    auto bad_cabinet = cabinet;
    bad_cabinet[0]["binary"] = "/missing/cabinet.so";
    auto changed_amp = config["amp"];
    changed_amp["parameters"][0]["value"] = 3;
    check(!send({{"op", "rig"}, {"amp", changed_amp}, {"cabinet", bad_cabinet}})
               .at("ok")
               .get<bool>());
    check(std::abs(output() - .7852f) < 1e-6f);
    for (auto malformed : {Json::array({cabinet[0], cabinet[0]}), Json::object(),
                           Json::array({pedal("wrong", 1, 0)})}) {
      check(!send({{"op", "rig"}, {"amp", changed_amp}, {"cabinet", malformed}})
                 .at("ok")
                 .get<bool>());
      check(std::abs(output() - .7852f) < 1e-6f);
    }
    check(!send({{"op", "preFX"}, {"modules", cabinet}}).at("ok").get<bool>());
    check(send({{"op", "rig"}, {"amp", config["amp"]}, {"cabinet", Json::array()}}).at("ok"));
    check(std::abs(output() - .616f) < 1e-6f);
    auto post_reversed = Json::array({post[1], post[0]});
    check(send({{"op", "postFX"}, {"modules", post_reversed}}).at("ok"));
    check(std::abs(output() - 0.608f) < 1e-6f);
    check(parameter("post-b", 1, 0).at("ok"));
    check(std::abs(output() - 0.404f) < 1e-6f);
    check(parameter("post-b", 2, 2).at("ok"));
    check(std::abs(output() - 0.404f) < 1e-6f);
    check(parameter("post-b", 1, 1).at("ok"));
    check(std::abs(output() - 0.8f) < 1e-6f);
    post_reversed[0]["binary"] = "/missing/post.so";
    check(!send({{"op", "postFX"}, {"modules", post_reversed}}).at("ok").get<bool>());
    check(std::abs(output() - 0.8f) < 1e-6f);
    check(parameter("pre", 1, 0).at("ok"));
    check(std::abs(output() - 0.512f) < 1e-6f);
    check(send({{"op", "stop"}}).at("ok"));
    check(send({{"op", "status"}}).at("value").at("state") == "stopped");
    check(!send({{"op", "parameter"}, {"id", 1}, {"value", 1.0}}).at("ok").get<bool>());
    check(!Json::parse(api->request(instance, "bad json")).at("ok").get<bool>());
    config["preFX"] = Json::array();
    config["postFX"] = Json::array();
    config["globals"] = Json::object();
    config["amp"] = {{"binary", argv[3]}, {"parameters", Json::array()}};
    config["tempo"] = {{"bpm", 90}, {"enabled", false}, {"level", -18}};
    input.fill(0);
    check(send({{"op", "start"}, {"configuration", config}}).at("ok"));
    check(std::abs(output() - (0.09 + 127 * 90.0 / (60 * 48000) / 100)) < 1e-7);
    auto beat = [&]() { return send({{"op", "status"}}).at("value").at("beat").get<double>(); };
    double checkpoint = beat();
    check(std::abs(checkpoint - 128 * 90.0 / (60 * 48000)) < 1e-12);
    check(send({{"op", "tempo"}, {"tempo", {{"bpm", 180}}}}).at("ok"));
    beat();
    check(std::abs(output() - (0.18 + (checkpoint + 127 * 180.0 / (60 * 48000)) / 100)) < 1e-7);
    checkpoint = beat();
    check(send({{"op", "preFX"}, {"modules", Json::array()}}).at("ok"));
    check(beat() == checkpoint);
    invalid = config;
    invalid["amp"]["binary"] = "/missing/tempo.so";
    check(!send({{"op", "start"}, {"configuration", invalid}}).at("ok").get<bool>());
    check(beat() == checkpoint);
    check(std::abs(output() - (0.18 + (checkpoint + 127 * 180.0 / (60 * 48000)) / 100)) < 1e-7);
    config["amp"] = {{"binary", argv[1]}, {"parameters", {{{"id", 1}, {"value", 0}}}}};
    config["globals"] = {{"outputGain", -60}};
    config["tempo"] = {{"bpm", 120}, {"enabled", true}, {"level", -6}};
    check(send({{"op", "start"}, {"configuration", config}}).at("ok"));
    output();
    check(beat() == 128 * 120.0 / (60 * 48000));
    check(*std::max_element(left.begin(), left.end()) > 0.1f);
    config["audio"]["monitor"] = false;
    check(send({{"op", "start"}, {"configuration", config}}).at("ok"));
    check(fake_ae::invoke(input.data(), left.data(), right.data(), 128, 0) == AE_PROCESS_SILENCE);
    for (float sample : left) {
      check(sample == 0);
    }
    check(beat() > 0);

    auto shared_pedal = [&](const char *slot, const char *binary, float gain, float offset) {
      return Json{{"slot", slot},
                  {"binary", binary},
                  {"parameters",
                   {{{"id", 1}, {"value", 1}},
                    {{"id", 2}, {"value", gain}},
                    {{"id", 3}, {"value", offset}}}}};
    };
    config["audio"]["monitor"] = true;
    config["globals"] = Json::object();
    config["tempo"] = {{"bpm", 120}, {"enabled", false}, {"level", -18}};
    config["amp"] = {{"binary", argv[1]}, {"parameters", {{{"id", 1}, {"value", 1}}}}};
    config["preFX"] = Json::array(
        {shared_pedal("shared-a", argv[4], 2, 0), shared_pedal("shared-b", argv[5], 1, .1f)});
    input.fill(0.1f);
    check(send({{"op", "start"}, {"configuration", config}}).at("ok"));
    position = 0;
    auto latency = [&]() { return send({{"op", "status"}}).at("value").at("latencyFrames"); };
    const auto grouped_latency = latency().get<uint32_t>();
    check(std::abs(continuous() - .3f) < 1e-6f);
    const auto opens = control.open_calls;
    const auto stops = control.stop_calls;
    check(parameter("shared-a", 1, 0).at("ok"));
    check(std::abs(continuous() - .2f) < 1e-6f);
    check(parameter("shared-a", 1, 1).at("ok"));
    check(std::abs(continuous() - .3f) < 1e-6f);
    check(control.open_calls == opens && control.stop_calls == stops);
    check(latency() == grouped_latency);
    check(send({{"op", "preFX"},
                {"modules", Json::array({shared_pedal("shared-b", argv[5], 1, .1f),
                                         shared_pedal("shared-a", argv[4], 2, 0)})}})
              .at("ok"));
    position = 0;
    check(std::abs(continuous() - .4f) < 1e-6f);
    check(control.open_calls == opens && control.stop_calls == stops + 1);
    check(latency() == grouped_latency + 7);
    auto missing = config["preFX"];
    missing[1]["binary"] = "/missing/shared.so";
    check(!send({{"op", "preFX"}, {"modules", missing}}).at("ok").get<bool>());
    check(control.open_calls == opens && control.stop_calls == stops + 1);
    check(std::abs(continuous() - .4f) < 1e-6f);
    check(send({{"op", "preFX"}, {"modules", config["preFX"]}}).at("ok"));
    position = 0;
    check(std::abs(continuous() - .3f) < 1e-6f);
    check(latency() == grouped_latency);
    const auto reversed = Json::array(
        {shared_pedal("shared-b", argv[5], 1, .1f), shared_pedal("shared-a", argv[4], 2, 0)});
    fake_ae::control().start_failures = 1;
    auto failed = send({{"op", "preFX"}, {"modules", reversed}});
    check(!failed.at("ok").get<bool>());
    check(failed.at("error").get<std::string>().find("restored") == std::string::npos);
    position = 0;
    check(std::abs(continuous() - .3f) < 1e-6f);
    check(latency() == grouped_latency);
    check(parameter("shared-a", 1, 0).at("ok"));
    check(std::abs(continuous() - .2f) < 1e-6f);
    check(parameter("shared-a", 1, 1).at("ok"));
    fake_ae::control().start_failures = 2;
    failed = send({{"op", "preFX"}, {"modules", reversed}});
    check(!failed.at("ok").get<bool>());
    check(failed.at("error").get<std::string>().find("could not be restored") != std::string::npos);
    check(send({{"op", "status"}}).at("value").at("state") == "prepared");
    check(send({{"op", "start"}, {"configuration", config}}).at("ok"));
    position = 0;
    check(std::abs(continuous() - .3f) < 1e-6f);

    // This pedal declares parameter 3 as its bypass; parameter 1, although named "Enabled", is an
    // ordinary parameter its processor ignores.
    auto offset_bypass = [&](float enabled, float offset) {
      return Json{{"slot", "offset-bypass"},
                  {"binary", argv[6]},
                  {"parameters",
                   {{{"id", 1}, {"value", enabled}},
                    {{"id", 2}, {"value", 1}},
                    {{"id", 3}, {"value", offset}}}}};
    };
    config["preFX"] = Json::array({offset_bypass(1, 0)});
    check(send({{"op", "start"}, {"configuration", config}}).at("ok"));
    position = 0;
    check(std::abs(continuous() - .1f) < 1e-6f);
    check(parameter("offset-bypass", 3, 1).at("ok"));
    check(std::abs(continuous() - 1.1f) < 1e-6f);
    check(parameter("offset-bypass", 1, 0).at("ok"));
    check(std::abs(continuous() - 1.1f) < 1e-6f);
    check(parameter("offset-bypass", 3, 0).at("ok"));
    check(std::abs(continuous() - .1f) < 1e-6f);
    config["preFX"] = Json::array({offset_bypass(0, 1)});
    check(send({{"op", "preFX"}, {"modules", config["preFX"]}}).at("ok"));
    check(std::abs(continuous() - 1.1f) < 1e-6f);
    check(send({{"op", "stop"}}).at("ok"));
    api->destroy(instance);
    check(fake_ae::control().contexts_alive == 0);
    special_fx(api, {argv[1],
                     argv[4],
                     argv[5],
                     argv[7],
                     {argv[8], argv[9], argv[10]},
                     {argv[11], argv[12], argv[13], argv[14], argv[15], argv[16], argv[17],
                      argv[18], argv[19]}});
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
