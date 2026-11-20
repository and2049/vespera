#include "app_audio_client.hpp"
#include "fake_audio_engine.h"

#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <source_location>
#include <stdexcept>
#include <string>
#include <vespera/app_audio.h>

namespace {
using Json = nlohmann::json;

void check(bool condition, std::source_location location = std::source_location::current()) {
  if (!condition) {
    throw std::runtime_error("audio bridge state check failed at line " +
                             std::to_string(location.line()));
  }
}

// Schema 3 state of the opaque-state fixture: enabled, gain, bias, polarity.
Json state(float enabled, float gain, float bias, float polarity) {
  std::string hex;
  for (float value : {enabled, gain, bias, polarity}) {
    const auto bits = std::bit_cast<uint32_t>(value);
    for (int b = 0; b < 4; ++b) {
      char text[3];
      std::snprintf(text, sizeof text, "%02x", (bits >> (8 * b)) & 0xffu);
      hex += text;
    }
  }
  return {{"schema", 3}, {"data", hex}};
}

Json module(const std::string &binary, const char *slot, float gain, float bias, float polarity,
            float enabled = 1) {
  Json result{{"binary", binary},
              {"module", "dev.vespera.test.opaque-state"},
              {"version", "2.1.0"},
              {"state", state(enabled, gain, bias, polarity)},
              {"parameters", {{{"id", 1}, {"value", enabled}}, {{"id", 2}, {"value", gain}}}}};
  if (slot) {
    result["slot"] = slot;
  }
  return result;
}
}

int main(int argc, char **argv) {
  if (argc != 2) {
    return 1;
  }
  try {
    const std::string binary = argv[1];
    fake_ae::reset();
    const auto *api = vespera_app_audio_entry(1);
    void *instance = api->create();
    check(instance != nullptr);
    auto send = [&](const Json &request) { return settled_audio_request(api, instance, request); };
    Json config{{"audio",
                 {{"backend", "fake"},
                  {"inputDevice", "input-device"},
                  {"outputDevice", "output-device"},
                  {"inputChannels", {0}},
                  {"outputChannels", {0, 1}},
                  {"sampleRate", 48000},
                  {"bufferSize", 128},
                  {"monitor", true}}},
                {"globals", Json::object()},
                {"amp", module(binary, nullptr, 0.5f, 0.1f, -1)},
                {"preFX", Json::array()},
                {"postFX", Json::array()}};
    std::array<float, 128> input{}, left{}, right{};
    input.fill(0.25f);
    uint64_t position = 0;
    auto output = [&](std::source_location location = std::source_location::current()) {
      check(fake_ae::invoke(input.data(), left.data(), right.data(), 128, position) ==
                AE_PROCESS_OK,
            location);
      position += 128;
      return left[127];
    };
    auto near = [](float actual, float expected) { return std::abs(actual - expected) < 1e-5f; };

    // Opaque state reaches the live module: parameter values alone would give 0.125.
    check(send({{"op", "start"}, {"configuration", config}}).at("ok"));
    check(near(output(), 0.25f * 0.5f * -1 + 0.1f));

    // A parameter edit keeps the state the module was built from.
    check(send({{"op", "parameter"}, {"id", 2}, {"value", 2.0}}).at("ok"));
    check(near(output(), 0.25f * 2 * -1 + 0.1f));

    // A later rebuild (here of another section) reapplies the stored state, then the edit.
    auto pre = Json::array({module(binary, "pre-fx.0", 1, 0.05f, 1)});
    check(send({{"op", "preFX"}, {"modules", pre}}).at("ok"));
    position = 0;
    check(near(output(), (0.25f + 0.05f) * 2 * -1 + 0.1f));

    // One preset request replaces every section and the globals together.
    Json preset{{"op", "preset"},
                {"amp", module(binary, nullptr, 1, -0.2f, 1)},
                {"cabinet", Json::array({module(binary, "cabinet", 0.5f, 0, -1)})},
                {"eq", Json::array()},
                {"preFX", Json::array({module(binary, "pre-fx.0", 1, 0.1f, 1),
                                       module(binary, "pre-fx.X", 2, 0, 1, 0)})},
                {"postFX", Json::array({module(binary, "post-fx.0", 1, 0.3f, 1)})},
                {"specialFX", Json::array()},
                {"globals", {{"outputGain", -6.0}}}};
    preset["preFX"][1]["bypassed"] = true;
    check(send(preset).at("ok"));
    position = 0;
    const float expected = (((0.25f + 0.1f) - 0.2f) * 0.5f * -1) + 0.3f;
    const float attenuated = expected * std::pow(10.0f, -6.0f / 20.0f);
    for (int block = 0; block < 10; ++block) {
      output();
    }
    check(near(output(), attenuated));

    // Rejected presets leave the previous graph and globals audible.
    auto rejected = [&](Json request) {
      check(!send(request).at("ok").get<bool>());
      for (int block = 0; block < 4; ++block) {
        output();
      }
      check(near(output(), attenuated));
    };
    auto broken = preset;
    broken["postFX"][0]["binary"] = "/missing/module.so";
    broken["globals"] = {{"outputGain", 0.0}};
    rejected(broken);
    broken = preset;
    broken["amp"]["state"] = state(1, 1, 0.9f, 1);
    rejected(broken);
    broken = preset;
    broken["amp"]["state"]["schema"] = 2;
    rejected(broken);
    broken = preset;
    broken["amp"]["state"]["data"] = "zz";
    rejected(broken);
    broken = preset;
    broken["cabinet"][0]["module"] = "dev.vespera.test.other";
    rejected(broken);
    broken = preset;
    broken["preFX"][0]["version"] = "2.0.0";
    rejected(broken);
    broken = preset;
    broken["postFX"] = Json::array({module(binary, "pre-fx.0", 1, 0, 1)});
    rejected(broken);
    broken = preset;
    broken["globals"] = {{"outputGain", 99.0}};
    rejected(broken);
    broken = preset;
    broken.erase("specialFX");
    rejected(broken);

    // Re-enabling the bypassed instance builds it from its stored state.
    auto enabled = preset["preFX"];
    enabled[1] = module(binary, "pre-fx.X", 2, 0, -1, 1);
    check(send({{"op", "preFX"}, {"modules", enabled}}).at("ok"));
    position = 0;
    for (int block = 0; block < 4; ++block) {
      output();
    }
    const float reenabled =
        ((((0.25f + 0.1f) * 2 * -1) - 0.2f) * 0.5f * -1 + 0.3f) * std::pow(10.0f, -6.0f / 20.0f);
    check(near(output(), reenabled));

    // Transpose moving away from zero reopens the stream; the preset still applies atomically.
    auto transposed = preset;
    transposed["preFX"] = enabled;
    transposed["globals"] = {{"outputGain", -6.0}, {"transpose", 3.0}};
    const auto opens = fake_ae::control().open_calls;
    check(send(transposed).at("ok"));
    check(fake_ae::control().open_calls == opens + 1);
    check(send({{"op", "status"}}).at("value").at("state") == "running");
    check(send(preset).at("ok"));

    // Malformed state encodings and schemas are rejected without changing the live graph.
    for (const Json &schema : {Json(1.5), Json(-3), Json(true), Json(4294967296ull), Json("3")}) {
      broken = preset;
      broken["amp"]["state"]["schema"] = schema;
      rejected(broken);
    }
    broken = preset;
    std::string upper = broken["amp"]["state"]["data"].get<std::string>();
    for (auto &c : upper) {
      c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    check(upper != broken["amp"]["state"]["data"].get<std::string>());
    broken["amp"]["state"]["data"] = upper;
    rejected(broken);

    // Offline rendering builds a separate graph and leaves the live one alone. It is refused
    // unless the process opts in.
    Json render{{"op", "renderGraph"},
                {"configuration",
                 {{"amp", module(binary, nullptr, 1, 0.1f, -1)},
                  {"postFX", Json::array({module(binary, "post-fx.0", 0.5f, 0, 1)})}}},
                {"input", Json::array({std::vector<float>(300, 0.5f)})},
                {"blockSize", 64}};
    unsetenv("VESPERA_ENABLE_RENDER_GRAPH");
    check(!send(render).at("ok").get<bool>());
    setenv("VESPERA_ENABLE_RENDER_GRAPH", "0", 1);
    check(!send(render).at("ok").get<bool>());
    setenv("VESPERA_ENABLE_RENDER_GRAPH", "1", 1);
    const auto rendered = send(render);
    check(rendered.at("ok"));
    for (const Json &bpm :
         {Json(0), Json(-120), Json(39.9), Json(240.5), Json("120"), Json(true), Json(nullptr)}) {
      auto bad_tempo = render;
      bad_tempo["bpm"] = bpm;
      check(!send(bad_tempo).at("ok").get<bool>());
    }
    auto overflow = render.dump();
    overflow.insert(overflow.size() - 1, ",\"bpm\":1e999");
    check(!Json::parse(api->request(instance, overflow.c_str())).at("ok").get<bool>());
    auto tempo_render = render;
    tempo_render["bpm"] = 240;
    check(send(tempo_render).at("ok"));
    const auto samples = rendered.at("value").at("output").at(0).get<std::vector<float>>();
    check(samples.size() == 300);
    for (float sample : samples) {
      check(near(sample, (0.5f * -1 + 0.1f) * 0.5f));
    }
    position = 0;
    for (int block = 0; block < 4; ++block) {
      output();
    }
    check(near(output(), attenuated));
    auto bad_render = render;
    bad_render["configuration"]["amp"]["state"] = state(1, 1, 0, 0.5f);
    check(!send(bad_render).at("ok").get<bool>());
    bad_render = render;
    bad_render["input"] = Json::array();
    check(!send(bad_render).at("ok").get<bool>());
    bad_render = render;
    bad_render["input"] = Json::array({std::vector<float>(10), std::vector<float>(11)});
    check(!send(bad_render).at("ok").get<bool>());

    // Offline renders place Special FX like the live graph and refuse invalid placements.
    auto placed = [&](const Json &placement) {
      auto request = render;
      request["configuration"]["postFX"] = Json::array();
      auto special = module(binary, "specialFX.state", 0.5f, 0, 1);
      special["placement"] = placement;
      request["configuration"]["specialFX"] = Json::array({special});
      return send(request);
    };
    for (const auto &[placement, value] :
         {std::pair{Json("before-pre-fx"), 0.5f * 0.5f * -1 + 0.1f},
          std::pair{Json(nullptr), (0.5f * -1 + 0.1f) * 0.5f},
          std::pair{Json("after-post-fx"), (0.5f * -1 + 0.1f) * 0.5f}}) {
      const auto reply = placed(placement);
      check(reply.at("ok"));
      check(near(reply.at("value").at("output").at(0).at(299).get<float>(), value));
    }
    check(!placed("after-amp").at("ok").get<bool>());
    bad_render = render;
    bad_render["configuration"]["amp"]["placement"] = "after-cabinet";
    check(!send(bad_render).at("ok").get<bool>());

    // Stopped sessions accept preset requests without building anything.
    check(send({{"op", "stop"}}).at("ok"));
    check(send(preset).at("ok"));
    broken = preset;
    broken["globals"] = {{"outputGain", 99.0}};
    check(!send(broken).at("ok").get<bool>());
    api->destroy(instance);
    check(fake_ae::control().contexts_alive == 0);
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
