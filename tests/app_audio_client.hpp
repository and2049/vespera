#ifndef VESPERA_TEST_APP_AUDIO_CLIENT_HPP
#define VESPERA_TEST_APP_AUDIO_CLIENT_HPP

#include <nlohmann/json.hpp>
#include <vespera/app_audio.h>

#include <chrono>
#include <stdexcept>
#include <thread>

inline nlohmann::json settled_audio_request(const vs_app_audio_api *api, void *instance,
                                            const nlohmann::json &request) {
  using Json = nlohmann::json;
  auto reply = Json::parse(api->request(instance, request.dump().c_str()));
  if (!reply.at("ok").get<bool>() || request.at("op") == "status" ||
      request.at("op") == "renderGraph" || request.at("op") == "devices" ||
      request.at("op") == "midiDevices" || request.at("op") == "tempo" ||
      request.at("op") == "tap") {
    return reply;
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  for (;;) {
    const auto status = Json::parse(api->request(instance, R"({"op":"status"})"));
    if (!status.at("ok").get<bool>()) {
      return status;
    }
    const auto &value = status.at("value");
    if (!value.contains("candidate")) {
      return reply;
    }
    const auto &candidate = value["candidate"];
    const auto state = candidate["state"].get<std::string>();
    if (state == "failed") {
      api->request(instance, R"({"op":"cancelCandidate","discard":true})");
      return {{"ok", false}, {"error", candidate["error"]}};
    }
    if (state == "idle" || state == "cancelled" || state == "stopped" ||
        (state == "ready" && candidate["generation"] == candidate["activeGeneration"])) {
      if (reply["value"].is_object() && reply["value"].contains("state")) {
        return status;
      }
      return reply;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      throw std::runtime_error("candidate did not settle within test deadline");
    }
    std::this_thread::yield();
  }
}
#endif
