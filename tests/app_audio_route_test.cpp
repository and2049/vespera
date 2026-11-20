// The audio bridge's Special FX routes: static branch routes at every placement, routing
// selectors resolved from the description and rechecked after loading, invalid shapes, selector
// changes as graph transactions, per-route bypass, latency, and offline renders and presets that
// match the live topology. Every module applies x * gain + offset; the route fixture adds 0.25 per
// route it renders, so audio shows both its position and the route the module itself chose.
#include "app_audio_client.hpp"
#include "fake_audio_engine.h"

#include <array>
#include <bit>
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

namespace {
using Json = nlohmann::json;

void check(bool condition, std::source_location location = std::source_location::current()) {
  if (!condition) {
    throw std::runtime_error("audio bridge route check failed at line " +
                             std::to_string(location.line()));
  }
}

constexpr std::array<const char *, 6> placements{"before-pre-fx", "before-amp", "before-cabinet",
                                                 "after-cabinet", "after-eq",   "after-post-fx"};

struct Affine {
  float gain, offset;
};

// One entry of the expected chain: serial, or a branch that reads the chain input and is mixed in
// with crossfade weights 1 - mix and mix.
struct Step {
  size_t gap;
  Affine affine;
  std::optional<float> mix;
};

}

int main(int argc, char **argv) {
  if (argc != 9) {
    return 1;
  }
  try {
    const char *gain_binary = argv[1], *pedal_binary = argv[2], *shared_binary = argv[3],
               *route_a = argv[4], *route_b = argv[5], *route_three = argv[6],
               *route_slow = argv[7], *route_failing = argv[8];
    fake_ae::reset();
    const auto *api = vespera_app_audio_entry(1);
    void *instance = api->create();
    check(instance != nullptr);
    auto send = [&](const Json &request) { return settled_audio_request(api, instance, request); };
    auto raw = [&](const Json &request) {
      return Json::parse(api->request(instance, request.dump().c_str()));
    };
    auto pedal = [](const char *binary, const std::string &slot, Affine affine) {
      return Json{{"slot", slot},
                  {"binary", binary},
                  {"parameters",
                   {{{"id", 1}, {"value", 1}},
                    {{"id", 2}, {"value", affine.gain}},
                    {{"id", 3}, {"value", affine.offset}}}}};
    };
    const Json routing{{"input", "chain-input"}, {"mix", {{"parameter", 5}, {"law", "crossfade"}}}};
    const Json selector{
        {"parameter", 4},
        {"routes", Json::array({{{"placement", "before-pre-fx"}},
                                {{"placement", "after-eq"}, {"routing", routing}}})}};
    // A route fixture description: Enabled, Gain, Offset, Route and Mix, plus routing fields.
    auto routed = [](const char *binary, const std::string &slot, const Json &fields, Affine affine,
                     float route, float mix, float enabled = 1) {
      Json result{{"slot", slot},
                  {"binary", binary},
                  {"parameters",
                   {{{"id", 1}, {"value", enabled}},
                    {{"id", 2}, {"value", affine.gain}},
                    {{"id", 3}, {"value", affine.offset}},
                    {{"id", 4}, {"value", route}},
                    {{"id", 5}, {"value", mix}}}}};
      for (const auto &[key, value] : fields.items()) {
        result[key] = value;
      }
      return result;
    };
    const Affine pre{1.5f, .01f}, amp{2, 0}, cabinet{1.1f, .03f}, eq{.9f, .04f}, post{1.2f, .02f};
    const Affine wet{.6f, .07f};
    const Json neighbours{
        {"amp", {{"binary", gain_binary}, {"parameters", {{{"id", 1}, {"value", 2}}}}}},
        {"preFX", Json::array({pedal(pedal_binary, "pre-fx.0", pre)})},
        {"cabinet", Json::array({pedal(pedal_binary, "cabinet", cabinet)})},
        {"eq", Json::array({pedal(pedal_binary, "eq", eq)})},
        {"postFX", Json::array({pedal(pedal_binary, "post-fx.0", post)})}};
    constexpr std::array<const char *, 5> sections{"preFX", "amp", "cabinet", "eq", "postFX"};
    const std::array<Affine, 5> section_affine{pre, amp, cabinet, eq, post};
    auto preset = [&](unsigned mask, const Json &special) {
      Json request{{"op", "preset"}, {"globals", Json::object()}, {"specialFX", special}};
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
    auto expected = [&](float input, unsigned mask, const std::vector<Step> &steps) {
      float x = input;
      for (size_t gap = 0; gap <= sections.size(); ++gap) {
        for (const auto &step : steps) {
          if (step.gap == gap) {
            const float processed = (step.mix ? input : x) * step.affine.gain + step.affine.offset;
            x = step.mix ? (1 - *step.mix) * x + *step.mix * processed : processed;
          }
        }
        if (gap < sections.size() && (mask & (1u << gap))) {
          x = x * section_affine[gap].gain + section_affine[gap].offset;
        }
      }
      return x;
    };
    auto shifted = [](Affine affine, uint32_t route) {
      return Affine{affine.gain, affine.offset + 0.25f * static_cast<float>(route)};
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
    uint64_t position = 0;
    auto output = [&](std::source_location location = std::source_location::current()) {
      check(fake_ae::invoke(input.data(), left.data(), right.data(), 128, position) ==
                AE_PROCESS_OK,
            location);
      position += 128;
      return left[127];
    };
    // Smoothed knobs and the graph's 10 ms branch gain ramps settle within a few blocks.
    auto settled = [&](std::source_location location = std::source_location::current()) {
      for (int block = 0; block < 10; ++block) {
        output(location);
      }
      return output(location);
    };
    auto near = [](float actual, float value) { return std::abs(actual - value) < 1e-5f; };
    auto status = [&] { return send({{"op", "status"}}).at("value"); };
    auto generation = [&] { return status().at("candidate").at("activeGeneration"); };
    auto latency = [&] { return status().at("latencyFrames").get<uint32_t>(); };
    const auto &control = fake_ae::control();

    // A static branch at each placement, with every neighbour, none and each one missing.
    const std::array<unsigned, 7> masks{31, 0, 31 & ~1u, 31 & ~2u, 31 & ~4u, 31 & ~8u, 31 & ~16u};
    for (size_t p = 0; p < placements.size(); ++p) {
      for (const auto mask : masks) {
        const auto special =
            routed(route_a, "specialFX.test.route.a",
                   {{"placement", placements[p]}, {"routing", routing}}, wet, 0, .3f);
        check(send(preset(mask, Json::array({special}))).at("ok"));
        check(near(output(), expected(0.1f, mask, {{p, wet, .3f}})));
      }
    }
    // Branch and serial readings of the same module at one gap differ, so the merge is audible.
    check(!near(expected(0.1f, 31, {{4, wet, .3f}}), expected(0.1f, 31, {{4, wet, {}}})));
    check(send(preset(31, Json::array({routed(route_a, "specialFX.test.route.a",
                                              {{"placement", "after-eq"}, {"routing", nullptr}},
                                              wet, 0, .3f)})))
              .at("ok"));
    check(near(output(), expected(0.1f, 31, {{4, wet, {}}})));

    // Two branches in one gap merge in slot order, whatever the request order.
    const Affine wet_b{1.4f, .02f};
    const auto two_branches = expected(0.1f, 31, {{2, wet, .3f}, {2, wet_b, .6f}});
    check(!near(two_branches, expected(0.1f, 31, {{2, wet_b, .6f}, {2, wet, .3f}})));
    const Json static_branch{{"placement", "before-cabinet"}, {"routing", routing}};
    const auto branch_a = routed(route_a, "specialFX.test.route.a", static_branch, wet, 0, .3f);
    const auto branch_b = routed(route_b, "specialFX.test.route.b", static_branch, wet_b, 0, .6f);
    for (const auto &request :
         {Json::array({branch_a, branch_b}), Json::array({branch_b, branch_a})}) {
      check(send(preset(31, request)).at("ok"));
      check(near(output(), two_branches));
    }

    // A selector resolves its route from the description: nearest value, ties upward.
    auto selected = [&](const char *binary, float route, float mix = .3f,
                        const Json &chooser = Json()) {
      return routed(binary, std::string("specialFX.") + (binary == route_a ? "test.route.a" : "x"),
                    {{"routingSelector", chooser.is_null() ? selector : chooser}}, wet, route, mix);
    };
    const auto serial_route = expected(0.1f, 31, {{0, wet, {}}});
    const auto branch_route = expected(0.1f, 31, {{4, shifted(wet, 1), .3f}});
    check(!near(serial_route, branch_route));
    for (const auto &[value, result] :
         {std::pair{0.0f, serial_route}, std::pair{0.4999f, serial_route},
          std::pair{0.5f, branch_route}, std::pair{1.0f, branch_route}}) {
      check(send(preset(31, Json::array({selected(route_a, value)}))).at("ok"));
      check(near(output(), result));
    }
    const Json three_routes{
        {"parameter", 4},
        {"routes", Json::array({{{"placement", "before-amp"}},
                                {{"placement", "after-cabinet"}},
                                {{"placement", "after-post-fx"}, {"routing", routing}}})}};
    for (const auto &[value, result] :
         {std::pair{0.0f, expected(0.1f, 31, {{1, wet, {}}})},
          std::pair{1.49f, expected(0.1f, 31, {{3, shifted(wet, 1), {}}})},
          std::pair{1.5f, expected(0.1f, 31, {{5, shifted(wet, 2), .3f}})},
          std::pair{2.0f, expected(0.1f, 31, {{5, shifted(wet, 2), .3f}})}}) {
      check(send(preset(31, Json::array({selected(route_three, value, .3f, three_routes)})))
                .at("ok"));
      check(near(output(), result));
    }

    // Invalid shapes are rejected and the previous graph keeps playing.
    check(send(preset(31, Json::array({selected(route_a, 1)}))).at("ok"));
    auto rejected = [&](const Json &request,
                        std::source_location location = std::source_location::current()) {
      check(!send(request).at("ok").get<bool>(), location);
      check(near(output(), branch_route), location);
    };
    auto special_with = [&](auto edit) {
      auto special = selected(route_a, 1);
      edit(special);
      return preset(31, Json::array({special}));
    };
    rejected(special_with([](Json &s) { s["placement"] = "after-eq"; }));
    rejected(special_with([&](Json &s) { s["routing"] = routing; }));
    rejected(special_with([&](Json &s) { s["routing"] = nullptr; }));
    rejected(special_with([](Json &s) { s["parameters"].erase(3); }));
    rejected(special_with([](Json &s) { s["parameters"][3]["value"] = "1"; }));
    rejected(special_with([](Json &s) {
      s["routingSelector"]["routes"] = Json::array({s["routingSelector"]["routes"][0]});
    }));
    rejected(special_with([](Json &s) {
      auto &routes = s["routingSelector"]["routes"];
      while (routes.size() < 9) {
        routes.push_back(routes[0]);
      }
    }));
    rejected(special_with([](Json &s) { s["routingSelector"]["parameter"] = -4; }));
    rejected(special_with([](Json &s) { s["routingSelector"]["parameter"] = 5; }));
    rejected(special_with([](Json &s) { s["routingSelector"]["parameter"] = 1; }));
    rejected(special_with([](Json &s) { s["routingSelector"]["parameter"] = 2; }));
    rejected(special_with(
        [](Json &s) { s["routingSelector"]["routes"][1]["routing"]["mix"]["parameter"] = 4; }));
    rejected(special_with([](Json &s) { s["routingSelector"]["extra"] = true; }));
    rejected(special_with([](Json &s) { s["routingSelector"]["routes"][0].erase("placement"); }));
    rejected(
        special_with([](Json &s) { s["routingSelector"]["routes"][0]["placement"] = nullptr; }));
    rejected(special_with(
        [](Json &s) { s["routingSelector"]["routes"][0]["placement"] = "after-amp"; }));
    rejected(special_with(
        [](Json &s) { s["routingSelector"]["routes"][1]["routing"]["input"] = "previous"; }));
    rejected(
        special_with([](Json &s) { s["routingSelector"]["routes"][1]["routing"].erase("mix"); }));
    rejected(special_with([](Json &s) {
      s["routingSelector"]["routes"][1]["routing"]["mix"]["law"] = "equal-power";
    }));
    rejected(special_with(
        [](Json &s) { s["routingSelector"]["routes"][1]["routing"]["mix"].erase("law"); }));
    rejected(special_with(
        [](Json &s) { s["routingSelector"]["routes"][1]["routing"]["mix"]["parameter"] = 2; }));
    rejected(special_with(
        [](Json &s) { s["routingSelector"]["routes"][1]["routing"]["mix"]["parameter"] = 1; }));
    rejected(special_with(
        [](Json &s) { s["routingSelector"]["routes"][1]["routing"]["mix"]["parameter"] = 9; }));
    rejected(
        special_with([](Json &s) { s["routingSelector"]["routes"][1]["routing"]["channel"] = 0; }));
    rejected(special_with([&](Json &s) { s["routingSelector"] = three_routes; }));
    rejected(preset(31, Json::array({selected(route_three, 1)})));
    // State and parameters disagree with the description: the module ends on another route.
    rejected(special_with([](Json &s) { s["parameters"].push_back({{"id", 4}, {"value", 0}}); }));
    for (const char *key : {"routing", "routingSelector"}) {
      auto request = preset(31, Json::array({selected(route_a, 1)}));
      request["postFX"][0][key] = std::string_view(key) == "routing" ? routing : selector;
      rejected(request);
      request = preset(31, Json::array({selected(route_a, 1)}));
      request["amp"][key] = std::string_view(key) == "routing" ? routing : selector;
      rejected(request);
      request = preset(31, Json::array({selected(route_a, 1)}));
      request["cabinet"][0][key] = std::string_view(key) == "routing" ? routing : selector;
      rejected(request);
    }

    // A host-bypassed branch is checked like an active one, although it leaves the graph: start,
    // preset and offline render all reject a mix parameter that is missing, not ranged 0..1 or
    // the bypass parameter, and keep the previous graph.
    setenv("VESPERA_ENABLE_RENDER_GRAPH", "1", 1);
    auto start_with = [&](const Json &request) {
      auto configuration = config;
      for (const char *key : {"amp", "preFX", "cabinet", "eq", "postFX", "specialFX"}) {
        configuration[key] = request.at(key);
      }
      return Json{{"op", "start"}, {"configuration", configuration}};
    };
    auto render_with = [&](const Json &request) {
      auto configuration = request;
      configuration.erase("op");
      configuration.erase("globals");
      return Json{{"op", "renderGraph"},
                  {"configuration", configuration},
                  {"input", Json::array({std::vector<float>(256, 0.1f)})}};
    };
    for (const int mix : {9, 2, 1}) {
      auto bad_routing = routing;
      bad_routing["mix"]["parameter"] = mix;
      auto static_bypassed =
          routed(route_a, "specialFX.test.route.a",
                 {{"placement", "after-eq"}, {"routing", bad_routing}}, wet, 0, .3f);
      auto bad_selector = selector;
      bad_selector["routes"][1]["routing"] = bad_routing;
      auto selected_bypassed = selected(route_a, 1, .3f, bad_selector);
      for (auto special : {static_bypassed, selected_bypassed}) {
        special["bypassed"] = true;
        const auto request = preset(31, Json::array({special}));
        rejected(request);
        rejected(start_with(request));
        check(!send(render_with(request)).at("ok").get<bool>());
        check(near(output(), branch_route));
      }
    }
    auto valid_bypassed = selected(route_a, 1);
    valid_bypassed["bypassed"] = true;
    check(send(start_with(preset(31, Json::array({valid_bypassed})))).at("ok"));
    check(near(output(), expected(0.1f, 31, {})));
    check(send(render_with(preset(31, Json::array({valid_bypassed})))).at("ok"));
    check(send(preset(31, Json::array({selected(route_a, 1)}))).at("ok"));
    check(near(output(), branch_route));

    // A selector change to another route is a graph transaction: the playing graph receives no
    // selector event, and the new graph has the new position and value with every other setting.
    check(send(preset(31, Json::array({selected(route_a, 0)}))).at("ok"));
    check(near(output(), serial_route));
    const Affine edited{.8f, .05f};
    auto parameter = [&](const char *slot, uint32_t id, float value) {
      return send({{"op", "parameter"}, {"slot", slot}, {"id", id}, {"value", value}});
    };
    check(parameter("specialFX.test.route.a", 2, edited.gain).at("ok"));
    check(parameter("specialFX.test.route.a", 3, edited.offset).at("ok"));
    check(parameter("specialFX.test.route.a", 5, .7f).at("ok"));
    check(near(settled(), expected(0.1f, 31, {{0, edited, {}}})));
    auto before = generation();
    auto stops = control.stop_calls;
    check(raw({{"op", "parameter"}, {"slot", "specialFX.test.route.a"}, {"id", 4}, {"value", 1}})
              .at("ok"));
    for (int block = 0; block < 3; ++block) {
      check(near(output(), expected(0.1f, 31, {{0, edited, {}}})));
    }
    const auto edited_branch = expected(0.1f, 31, {{4, shifted(edited, 1), .7f}});
    check(send({{"op", "parameter"}, {"slot", "specialFX.test.route.a"}, {"id", 4}, {"value", 1}})
              .at("ok"));
    check(generation() != before && control.stop_calls == stops + 1);
    check(near(output(), edited_branch));

    // A value within the same route is a live event.
    before = generation();
    stops = control.stop_calls;
    check(parameter("specialFX.test.route.a", 4, .9f).at("ok"));
    check(generation() == before && control.stop_calls == stops);
    check(near(settled(), edited_branch));

    // Per-route bypass: a branch's power is a live gain change keeping the graph; a serial,
    // unshared module's power rebuilds without it.
    check(parameter("specialFX.test.route.a", 1, 0).at("ok"));
    check(generation() == before && control.stop_calls == stops);
    check(near(settled(), expected(0.1f, 31, {})));
    check(parameter("specialFX.test.route.a", 5, .2f).at("ok"));
    check(near(settled(), expected(0.1f, 31, {})));
    check(parameter("specialFX.test.route.a", 1, 1).at("ok"));
    check(generation() == before && control.stop_calls == stops);
    check(near(settled(), expected(0.1f, 31, {{4, shifted(edited, 1), .2f}})));
    check(parameter("specialFX.test.route.a", 4, 0).at("ok"));
    check(near(output(), expected(0.1f, 31, {{0, edited, {}}})));
    stops = control.stop_calls;
    check(parameter("specialFX.test.route.a", 1, 0).at("ok"));
    check(control.stop_calls == stops + 1);
    check(near(output(), expected(0.1f, 31, {})));
    check(parameter("specialFX.test.route.a", 1, 1).at("ok"));
    check(near(output(), expected(0.1f, 31, {{0, edited, {}}})));

    // Changes while a candidate is pending: only the last is adopted.
    check(send(preset(31, Json::array({selected(route_three, 0, .3f, three_routes)}))).at("ok"));
    check(near(output(), expected(0.1f, 31, {{1, wet, {}}})));
    check(raw({{"op", "parameter"}, {"slot", "specialFX.x"}, {"id", 4}, {"value", 1}}).at("ok"));
    check(raw({{"op", "parameter"}, {"slot", "specialFX.x"}, {"id", 4}, {"value", 2}}).at("ok"));
    check(send({{"op", "parameter"}, {"slot", "specialFX.x"}, {"id", 4}, {"value", 2}}).at("ok"));
    check(near(output(), expected(0.1f, 31, {{5, shifted(wet, 2), .3f}})));

    // A candidate that fails to prepare leaves the previous audio and reports the error.
    check(send(preset(31, Json::array({selected(route_failing, 0)}))).at("ok"));
    check(near(output(), serial_route));
    before = generation();
    check(raw({{"op", "parameter"}, {"slot", "specialFX.x"}, {"id", 4}, {"value", 1}}).at("ok"));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    Json candidate;
    do {
      check(std::chrono::steady_clock::now() < deadline);
      std::this_thread::yield();
      candidate = raw({{"op", "status"}}).at("value").at("candidate");
    } while (candidate.at("state") != "failed");
    check(!candidate.at("error").get<std::string>().empty());
    check(candidate.at("activeGeneration") == before);
    check(near(output(), serial_route));
    check(raw({{"op", "cancelCandidate"}, {"discard", true}}).at("ok"));
    check(parameter("specialFX.x", 2, edited.gain).at("ok"));
    check(near(settled(), expected(0.1f, 31, {{0, Affine{edited.gain, wet.offset}, {}}})));

    // Opaque state survives a mode switch: the new candidate is built from the description's
    // saved state, with the current parameter edits applied on top. The fixture's bias exists
    // only in its state.
    auto saved_state = [](std::initializer_list<float> values) {
      std::string hex;
      for (const float value : values) {
        const auto bits = std::bit_cast<uint32_t>(value);
        for (int byte = 0; byte < 4; ++byte) {
          char text[3];
          std::snprintf(text, sizeof text, "%02x", (bits >> (8 * byte)) & 0xffu);
          hex += text;
        }
      }
      return Json{{"schema", 2}, {"data", hex}};
    };
    const float bias = .125f;
    const auto biased = [&](Affine affine) { return Affine{affine.gain, affine.offset + bias}; };
    auto stateful = selected(route_a, 0);
    stateful["state"] = saved_state({1, wet.gain, wet.offset, 0, .3f, bias});
    check(send(preset(31, Json::array({stateful}))).at("ok"));
    check(!near(serial_route, expected(0.1f, 31, {{0, biased(wet), {}}})));
    check(near(output(), expected(0.1f, 31, {{0, biased(wet), {}}})));
    check(parameter("specialFX.test.route.a", 2, edited.gain).at("ok"));
    check(parameter("specialFX.test.route.a", 5, .7f).at("ok"));
    const Affine edited_gain{edited.gain, wet.offset};
    check(near(settled(), expected(0.1f, 31, {{0, biased(edited_gain), {}}})));
    before = generation();
    check(parameter("specialFX.test.route.a", 4, 1).at("ok"));
    check(generation() != before);
    check(near(output(), expected(0.1f, 31, {{4, biased(shifted(edited_gain, 1)), .7f}})));
    check(parameter("specialFX.test.route.a", 4, 0).at("ok"));
    check(near(output(), expected(0.1f, 31, {{0, biased(edited_gain), {}}})));

    // A switch whose candidate fails keeps the old graph audible with its state; the next switch
    // is built from the same saved state and the edits made before the failure.
    auto failing = selected(route_failing, 0);
    failing["state"] = saved_state({1, wet.gain, wet.offset, 0, .3f, bias});
    check(send(preset(31, Json::array({failing}))).at("ok"));
    check(parameter("specialFX.x", 2, edited.gain).at("ok"));
    const auto failing_serial = expected(0.1f, 31, {{0, biased(edited_gain), {}}});
    check(near(settled(), failing_serial));
    before = generation();
    check(raw({{"op", "parameter"}, {"slot", "specialFX.x"}, {"id", 4}, {"value", 1}}).at("ok"));
    const auto failure_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    Json failure;
    do {
      check(std::chrono::steady_clock::now() < failure_deadline);
      std::this_thread::yield();
      failure = raw({{"op", "status"}}).at("value").at("candidate");
    } while (failure.at("state") != "failed");
    check(failure.at("activeGeneration") == before);
    for (int block = 0; block < 3; ++block) {
      check(near(output(), failing_serial));
    }
    check(send({{"op", "parameter"}, {"slot", "specialFX.x"}, {"id", 4}, {"value", 0}}).at("ok"));
    check(generation() != before);
    check(near(output(), failing_serial));
    check(parameter("specialFX.x", 3, edited.offset).at("ok"));
    check(near(settled(), expected(0.1f, 31, {{0, biased(edited), {}}})));

    // Host bypass removes a branch; latency follows the slower path and changes with the route.
    check(send(preset(0, Json::array())).at("ok"));
    const auto base_latency = latency();
    auto slow = selected(route_slow, 1);
    check(send(preset(0, Json::array({slow}))).at("ok"));
    check(latency() == base_latency + 24);
    check(near(settled(), expected(0.1f, 0, {{4, shifted(wet, 1), .3f}})));
    check(send({{"op", "parameter"}, {"slot", "specialFX.x"}, {"id", 4}, {"value", 0}}).at("ok"));
    check(latency() == base_latency);
    check(near(settled(), expected(0.1f, 0, {{0, wet, {}}})));
    slow["bypassed"] = true;
    check(send(preset(0, Json::array({slow}))).at("ok"));
    check(latency() == base_latency && near(output(), 0.1f));
    auto faster = preset(0, Json::array({selected(route_a, 1)}));
    faster["preFX"] = Json::array({Json{
        {"slot", "pre-fx.shared"},
        {"binary", shared_binary},
        {"parameters",
         {{{"id", 1}, {"value", 1}}, {{"id", 2}, {"value", 2}}, {{"id", 3}, {"value", 0}}}}}});
    check(send(faster).at("ok"));
    check(latency() == base_latency + 7);
    check(near(settled(), expected(0.1f, 0, {{0, {2, 0}, {}}, {4, shifted(wet, 1), .3f}})));

    // Offline renders and presets build the live topology in each route and reject the same
    // invalid descriptions.
    setenv("VESPERA_ENABLE_RENDER_GRAPH", "1", 1);
    for (const float route : {0.0f, 1.0f}) {
      const auto request = preset(31, Json::array({selected(route_a, route)}));
      check(send(request).at("ok"));
      const auto live = output();
      Json configuration = request;
      configuration.erase("op");
      configuration.erase("globals");
      const auto rendered = send({{"op", "renderGraph"},
                                  {"configuration", configuration},
                                  {"input", Json::array({std::vector<float>(512, 0.1f)})}});
      check(rendered.at("ok"));
      check(near(rendered.at("value").at("output").at(0).at(511).get<float>(), live));
      check(near(live, route == 0 ? serial_route : branch_route));
      auto invalid = configuration;
      invalid["specialFX"][0]["placement"] = "after-eq";
      check(!send({{"op", "renderGraph"},
                   {"configuration", invalid},
                   {"input", Json::array({std::vector<float>(512, 0.1f)})}})
                 .at("ok")
                 .get<bool>());
      invalid = configuration;
      invalid["globals"] = {{"outputGain", 99.0}};
      check(!send({{"op", "renderGraph"},
                   {"configuration", invalid},
                   {"input", Json::array({std::vector<float>(512, 0.1f)})}})
                 .at("ok")
                 .get<bool>());
      invalid = configuration;
      invalid["specialFX"][0]["parameters"].push_back({{"id", 4}, {"value", 1 - route}});
      check(!send({{"op", "renderGraph"},
                   {"configuration", invalid},
                   {"input", Json::array({std::vector<float>(512, 0.1f)})}})
                 .at("ok")
                 .get<bool>());
    }

    check(send({{"op", "stop"}}).at("ok"));
    api->destroy(instance);
    check(fake_ae::control().contexts_alive == 0);
    std::puts("Special FX routes, selectors, transactions, bypass, latency and renders passed");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
