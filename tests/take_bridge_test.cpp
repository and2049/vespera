#include "app_audio_client.hpp"
#include "fake_audio_engine.h"
#include "wav_file.hpp"

#include <nlohmann/json.hpp>
#include <vespera/app_audio.h>

#include <unistd.h>

#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <source_location>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using Json = nlohmann::json;

void check(bool condition, std::source_location location = std::source_location::current()) {
  if (!condition) {
    throw std::runtime_error("take bridge check failed at line " + std::to_string(location.line()));
  }
}

std::filesystem::path scratch;

std::set<std::string> keys(const Json &object) {
  std::set<std::string> result;
  for (const auto &[key, value] : object.items()) {
    result.insert(key);
  }
  return result;
}

const std::set<std::string> take_keys{"state",      "path",     "frames",
                                      "sampleRate", "channels", "droppedFrames"};

std::vector<float> read_mono(const std::filesystem::path &path) {
  vespera::WavReader reader(path);
  check(reader.format().channels == 1 && reader.format().sample_rate == 48000);
  std::vector<float> samples(reader.format().frames);
  float *channels[] = {samples.data()};
  reader.read(channels, samples.size());
  return samples;
}

Json read_sidecar(std::filesystem::path path) {
  path.replace_extension(".json");
  std::ifstream file(path);
  return Json::parse(file);
}

struct Bridge {
  const vs_app_audio_api *api = vespera_app_audio_entry(1);
  void *instance = api->create();
  uint64_t position = 0;
  uint64_t produced = 0;
  std::vector<float> history;
  std::array<float, 128> input{}, left{}, right{};

  Bridge() { check(instance != nullptr); }
  ~Bridge() {
    if (instance) {
      api->destroy(instance);
    }
  }

  Json send(const Json &request) { return settled_audio_request(api, instance, request); }
  Json recording() { return send({{"op", "status"}}).at("value").at("recording"); }

  ae_process_result block(float poison = 0.0f) {
    for (size_t f = 0; f < input.size(); ++f) {
      input[f] = static_cast<float>((produced + f) % 4096) / 4096.0f - 0.5f;
    }
    if (poison != 0.0f) {
      input[5] = std::numeric_limits<float>::max();
    }
    history.insert(history.end(), input.begin(), input.end());
    produced += input.size();
    const auto result = fake_ae::invoke(input.data(), left.data(), right.data(), 128, position);
    position += 128;
    return result;
  }

  bool settle() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
      const auto state = recording().at("state").get<std::string>();
      if (state != "recording" && state != "finalizing") {
        return true;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  bool matches(const std::filesystem::path &path, uint64_t begin, uint64_t end) {
    const auto recorded = read_mono(path);
    if (recorded.size() != end - begin) {
      return false;
    }
    for (uint64_t f = 0; f < recorded.size(); ++f) {
      if (std::bit_cast<uint32_t>(recorded[f]) != std::bit_cast<uint32_t>(history[begin + f])) {
        return false;
      }
    }
    return true;
  }
};

Json configuration(const char *gain, float amp_gain = 2.0f, uint32_t buffer = 128) {
  return Json{{"audio",
               {{"backend", "fake"},
                {"inputDevice", "input-device"},
                {"outputDevice", "output-device"},
                {"inputChannels", {0}},
                {"outputChannels", {0, 1}},
                {"sampleRate", 48000},
                {"bufferSize", buffer},
                {"monitor", true}}},
              {"globals", Json::object()},
              {"amp", {{"binary", gain}, {"parameters", {{{"id", 1}, {"value", amp_gain}}}}}}};
}

Json record(const std::filesystem::path &path) {
  return {{"op", "record"}, {"action", "start"}, {"path", path.string()}};
}

const Json stop_recording{{"op", "record"}, {"action", "stop"}};

void contract_and_rejections(const char *gain) {
  fake_ae::reset();
  Bridge bridge;
  auto recording = bridge.recording();
  check(recording == Json{{"state", "idle"}});
  auto reply = bridge.send(record(scratch / "early.wav"));
  check(!reply.at("ok").get<bool>());
  check(reply.at("error") == "Recording requires running audio");
  check(!std::filesystem::exists(scratch / "early.wav"));
  check(bridge.send(stop_recording).at("ok"));
  check(bridge.recording() == Json{{"state", "idle"}});

  const auto config = configuration(gain);
  check(bridge.send({{"op", "start"}, {"configuration", config}}).at("ok"));
  check(!bridge.send(record("relative/take.wav")).at("ok").get<bool>());
  check(!bridge.send({{"op", "record"}, {"action", "pause"}}).at("ok").get<bool>());
  check(!bridge.send({{"op", "record"}, {"action", "start"}}).at("ok").get<bool>());
  std::ofstream(scratch / "plain-file") << "x";
  reply = bridge.send(record(scratch / "plain-file" / "take.wav"));
  check(!reply.at("ok").get<bool>());
  check(reply.at("error").get<std::string>().find("cannot be created") != std::string::npos);
  check(bridge.recording() == Json{{"state", "idle"}});

  const auto path = scratch / "contract" / "missing" / "take.wav";
  reply = bridge.send(record(path));
  check(reply.at("ok"));
  check(reply.at("value").at("recording").at("state") == "recording");
  recording = bridge.recording();
  check(keys(recording) == take_keys);
  check(recording.at("path") == path.string() && recording.at("frames") == 0);
  check(recording.at("sampleRate") == 48000 && recording.at("channels") == 1);
  check(recording.at("droppedFrames") == 0);
  check(!bridge.send(record(scratch / "second.wav")).at("ok").get<bool>());
  check(!std::filesystem::exists(scratch / "second.wav"));

  const uint64_t begin = bridge.produced;
  for (int block = 0; block < 3; ++block) {
    check(bridge.block() == AE_PROCESS_OK);
  }
  const uint64_t end = bridge.produced;
  check(bridge.recording().at("frames") == 384);
  reply = bridge.send(stop_recording);
  check(reply.at("ok"));
  check(reply.at("value").at("recording").at("state") == "finalizing");
  check(keys(bridge.recording()) == take_keys);
  check(bridge.block() == AE_PROCESS_OK);
  check(bridge.settle());
  recording = bridge.recording();
  check(keys(recording) == take_keys);
  check(recording.at("state") == "idle" && recording.at("frames") == 384);
  check(recording.at("path") == path.string());
  check(bridge.matches(path, begin, end));
  const auto sidecar = read_sidecar(path);
  check(sidecar.at("version") == 1 && sidecar.at("frames") == 384);
  check(sidecar.at("sampleRate") == 48000 && sidecar.at("channels") == 1);
  check(sidecar.at("blockSize") == 128 && sidecar.at("droppedFrames") == 0);
  check(sidecar.at("discontinuities") == 0 && sidecar.at("bpm") == 120.0);
  check(sidecar.at("beat").is_number());
  check(sidecar.at("configuration") == config);
  check(bridge.send(stop_recording).at("ok"));
  check(bridge.recording().at("state") == "idle");
}

// A graph candidate swap keeps the stream open, so the take continues; the sidecar keeps the
// configuration from when recording started.
void survives_candidate_swap(const char *gain) {
  fake_ae::reset();
  Bridge bridge;
  const auto config = configuration(gain);
  check(bridge.send({{"op", "start"}, {"configuration", config}}).at("ok"));
  const auto path = scratch / "swap.wav";
  check(bridge.send(record(path)).at("ok"));
  const uint64_t begin = bridge.produced;
  check(bridge.block() == AE_PROCESS_OK);
  check(bridge.block() == AE_PROCESS_OK);
  check(fake_ae::control().open_calls == 1);
  const auto amp = Json{{"binary", gain}, {"parameters", {{{"id", 1}, {"value", 3.0}}}}};
  check(bridge.send({{"op", "amp"}, {"amp", amp}}).at("ok"));
  check(fake_ae::control().open_calls == 1);
  check(bridge.recording().at("state") == "recording");
  check(bridge.block() == AE_PROCESS_OK);
  check(bridge.left[127] == 3.0f * bridge.input[127]);
  check(bridge.block() == AE_PROCESS_OK);
  const uint64_t end = bridge.produced;
  check(bridge.send(stop_recording).at("ok"));
  check(bridge.block() == AE_PROCESS_OK);
  check(bridge.settle());
  check(bridge.matches(path, begin, end));
  const auto sidecar = read_sidecar(path);
  check(sidecar.at("frames") == 512);
  check(sidecar.at("discontinuities") == 1);
  check(sidecar.at("configuration") == config);
}

// Anything that closes or reopens the stream ends the take with a readable file and sidecar.
void stream_changes_finalize(const char *gain) {
  fake_ae::reset();
  {
    Bridge bridge;
    check(bridge.send({{"op", "start"}, {"configuration", configuration(gain)}}).at("ok"));
    const auto stopped = scratch / "stop-op.wav";
    check(bridge.send(record(stopped)).at("ok"));
    const uint64_t begin = bridge.produced;
    check(bridge.block() == AE_PROCESS_OK);
    check(bridge.send({{"op", "stop"}}).at("ok"));
    check(bridge.recording().at("state") == "idle");
    check(bridge.matches(stopped, begin, bridge.produced));
    check(read_sidecar(stopped).at("frames") == 128);

    check(bridge.send({{"op", "start"}, {"configuration", configuration(gain)}}).at("ok"));
    const auto reopened = scratch / "reopen.wav";
    check(bridge.send(record(reopened)).at("ok"));
    const uint64_t second = bridge.produced;
    check(bridge.block() == AE_PROCESS_OK);
    const uint64_t second_end = bridge.produced;
    const auto opens = fake_ae::control().open_calls;
    check(
        bridge.send({{"op", "start"}, {"configuration", configuration(gain, 2.0f, 64)}}).at("ok"));
    check(fake_ae::control().open_calls == opens + 1);
    check(bridge.recording().at("state") == "idle");
    check(bridge.recording().at("path") == reopened.string());
    check(bridge.matches(reopened, second, second_end));
    check(read_sidecar(reopened).at("frames") == 128);

    // Processing failure enters recovery, which ends the take after the failed block.
    const auto failed = scratch / "recovery.wav";
    bridge.position = 0;
    check(bridge.send(record(failed)).at("ok"));
    const uint64_t third = bridge.produced;
    check(bridge.block() == AE_PROCESS_OK);
    check(bridge.block(1.0f) == AE_PROCESS_FAILED);
    const uint64_t third_end = bridge.produced;
    const auto status = bridge.send({{"op", "status"}}).at("value");
    check(status.at("state") == "recovery_required");
    check(status.at("recording").at("state") != "recording");
    check(!bridge.send(record(scratch / "during-recovery.wav")).at("ok").get<bool>());
    check(bridge.send({{"op", "stop"}}).at("ok"));
    check(bridge.recording().at("state") == "idle");
    check(bridge.matches(failed, third, third_end));

    // Teardown while recording finalizes.
    check(bridge.send({{"op", "start"}, {"configuration", configuration(gain)}}).at("ok"));
    bridge.position = 0;
    check(bridge.send(record(scratch / "teardown.wav")).at("ok"));
    check(bridge.block() == AE_PROCESS_OK);
    check(bridge.block() == AE_PROCESS_OK);
  }
  check(read_sidecar(scratch / "teardown.wav").at("frames") == 256);
  check(read_mono(scratch / "teardown.wav").size() == 256);
  check(fake_ae::control().streams_alive == 0);
}

}

int main(int argc, char **argv) {
  if (argc != 2) {
    return 2;
  }
  scratch = std::filesystem::temp_directory_path() /
            ("vespera-take-bridge-" + std::to_string(static_cast<long>(getpid())));
  std::filesystem::remove_all(scratch);
  std::filesystem::create_directories(scratch);
  int result = 0;
  try {
    contract_and_rejections(argv[1]);
    survives_candidate_swap(argv[1]);
    stream_changes_finalize(argv[1]);
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    result = 1;
  }
  std::error_code ignored;
  std::filesystem::remove_all(scratch, ignored);
  return result;
}
