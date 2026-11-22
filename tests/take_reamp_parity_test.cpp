// Acceptance gate for take recording and reamping: a take recorded from a live bridge session and
// reamped through the same configuration must reproduce the live device output.
//
// The live session runs on the fake backend; its output is captured from the device buffers the
// callback fills. The reamp export is aligned to its input: export frame i corresponds to live
// output frame take_start + latency + i, where latency is the graph latency plus the core IO input
// latency the session reports. Every export frame, including the tail, is compared: after the
// take, the device input is silence, which is exactly what the offline render pads with.
//
// Samples compare equal when their bits match or when both are zero. The live path adds the
// metronome click buffer, silent here, to every output sample, which turns -0.0 into +0.0; any
// other difference fails.
#include "app_audio_client.hpp"
#include "fake_audio_engine.h"
#include "take_recorder.hpp"
#include "wav_file.hpp"

#include <nlohmann/json.hpp>
#include <vespera/app_audio.h>

#include <unistd.h>

#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;
using Channels = std::vector<std::vector<float>>;

void check(bool condition, std::source_location location = std::source_location::current()) {
  if (!condition) {
    throw std::runtime_error("take reamp parity check failed at line " +
                             std::to_string(location.line()));
  }
}

fs::path scratch;
std::string gain_module;
std::string delay_module;

constexpr uint32_t rate = 48000;
// Varying device block sizes up to the fake backend's 256-frame maximum; the reamp renders in
// the negotiated 128-frame blocks, so parity also shows block-size independence.
constexpr std::array<uint32_t, 10> block_sizes{128, 64, 1, 255, 256, 37, 128, 200, 17, 96};

// A deterministic two-tone signal that drops for one 500 ms section in three to about -76 dBFS,
// long enough for the envelope decay and hold, so the -50 dB gate closes and reopens repeatedly.
// The right channel differs in phase and level so stereo channels are distinguishable.
float signal(uint64_t frame, uint32_t channel) {
  const double t = static_cast<double>(frame) / rate;
  const double level = (frame / 24000) % 3 == 2 ? 0.0003 : 1.0;
  const double phase = channel == 0 ? 0.0 : 1.3;
  return static_cast<float>(
      level * (0.3 * std::sin(2.0 * M_PI * 220.0 * t + phase) +
               (channel == 0 ? 0.2 : 0.15) * std::sin(2.0 * M_PI * 1375.0 * t + 2.0 * phase)));
}

bool same(float live, float reamp) {
  return std::bit_cast<uint32_t>(live) == std::bit_cast<uint32_t>(reamp) ||
         (live == 0.0f && reamp == 0.0f);
}

struct Mismatch {
  uint64_t frame;
  uint32_t channel;
  float live;
  float reamp;
};

// The first export frame at or after `from` that differs from the aligned live output.
std::optional<Mismatch> first_mismatch(const Channels &live, uint64_t offset, const Channels &reamp,
                                       uint64_t from = 0) {
  for (uint64_t i = from; i < reamp[0].size(); ++i) {
    for (uint32_t c = 0; c < 2; ++c) {
      if (!same(live[c][offset + i], reamp[c][i])) {
        return Mismatch{i, c, live[c][offset + i], reamp[c][i]};
      }
    }
  }
  return std::nullopt;
}

void report(const char *label, const std::optional<Mismatch> &mismatch) {
  if (mismatch) {
    std::fprintf(stderr, "%s: export frame %llu channel %u: live %.9g reamp %.9g\n", label,
                 static_cast<unsigned long long>(mismatch->frame), mismatch->channel,
                 static_cast<double>(mismatch->live), static_cast<double>(mismatch->reamp));
  }
}

Channels read_wav(const fs::path &path, vespera::WavFormat &format) {
  vespera::WavReader reader(path);
  format = reader.format();
  Channels result(format.channels, std::vector<float>(format.frames));
  std::array<float *, 2> planes{result[0].data(), result.back().data()};
  check(reader.read(planes.data(), format.frames) == format.frames);
  return result;
}

Json read_json(fs::path path) {
  path.replace_extension(".json");
  std::ifstream file(path);
  return Json::parse(file);
}

struct Bridge {
  const vs_app_audio_api *api = vespera_app_audio_entry(1);
  void *instance = api->create();
  uint32_t inputs = 1;
  uint64_t position = 0;
  uint64_t produced = 0;
  Channels output{{}, {}};
  std::array<std::vector<float>, 2> block{std::vector<float>(256), std::vector<float>(256)};
  std::array<std::vector<float>, 2> out{std::vector<float>(256), std::vector<float>(256)};

  Bridge() { check(instance != nullptr); }
  ~Bridge() { api->destroy(instance); }
  Bridge(const Bridge &) = delete;
  Bridge &operator=(const Bridge &) = delete;

  Json send(const Json &request) { return settled_audio_request(api, instance, request); }
  Json status() { return send({{"op", "status"}}).at("value"); }

  // Delivers one device block, keeping its output; input is the signal, or silence when quiet.
  void deliver(uint32_t frames, bool quiet = false) {
    for (uint32_t c = 0; c < inputs; ++c) {
      for (uint32_t f = 0; f < frames; ++f) {
        block[c][f] = quiet ? 0.0f : signal(produced + f, c);
      }
    }
    auto &control = fake_ae::control();
    const float *in[] = {block[0].data(), block[1].data()};
    float *device[] = {out[0].data(), out[1].data()};
    ae_audio_block audio{};
    audio.struct_size = sizeof(audio);
    audio.inputs = in;
    audio.input_channels = inputs;
    audio.outputs = device;
    audio.output_channels = 2;
    audio.frames = frames;
    audio.stream_id = control.stream_id;
    audio.configuration_generation = control.configuration_generation;
    audio.run_generation = control.run_generation;
    audio.frame_position = position;
    check(control.process(control.user, &audio) == AE_PROCESS_OK);
    for (uint32_t c = 0; c < 2; ++c) {
      output[c].insert(output[c].end(), out[c].begin(), out[c].begin() + frames);
    }
    position += frames;
    produced += frames;
  }

  // Delivers at least `frames` frames in the varying block sizes.
  void play(uint64_t frames, bool quiet = false) {
    for (uint64_t end = produced + frames, i = 0; produced < end; ++i) {
      deliver(block_sizes[i % block_sizes.size()], quiet);
    }
  }

  void wait_take() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (status().at("recording").at("state") != "idle") {
      check(std::chrono::steady_clock::now() < deadline);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  // Renders `input` through `configuration` and returns the export.
  Channels reamp(const fs::path &input, const fs::path &destination, const Json &configuration,
                 const Json &sidecar) {
    const auto reply = send({{"op", "reamp"},
                             {"action", "start"},
                             {"input", input.string()},
                             {"output", destination.string()},
                             {"configuration", configuration},
                             {"bpm", sidecar.at("bpm")},
                             {"blockSize", sidecar.at("blockSize")}});
    if (!reply.at("ok").get<bool>()) {
      throw std::runtime_error("reamp refused: " + reply.dump());
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    for (;;) {
      const auto job = status().at("reamp");
      if (job.at("state") == "done") {
        break;
      }
      if (job.at("state") != "rendering") {
        throw std::runtime_error("reamp did not finish: " + job.dump());
      }
      check(std::chrono::steady_clock::now() < deadline);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    vespera::WavFormat format;
    auto result = read_wav(destination, format);
    check(format.channels == 2 && format.sample_rate == rate);
    return result;
  }
};

Json pedal(const std::string &binary, const std::string &slot, float value) {
  return {{"slot", slot}, {"binary", binary}, {"parameters", {{{"id", 1}, {"value", value}}}}};
}

Json globals(float transpose = 0.0f) {
  return {{"inputGain", 4.5},      {"outputGain", -3.0},     {"gateEnabled", true},
          {"gateThreshold", -50},  {"doublerEnabled", true}, {"doublerTime", 7.5},
          {"transpose", transpose}};
}

// Gain amp, delay Post FX (100 frames of latency, a 32-frame echo, a 64-frame tail) and a
// second gain as Pre FX.
Json configuration(uint32_t inputs, const Json &global) {
  Json channels = inputs == 2 ? Json{0, 1} : Json{0};
  return {{"audio",
           {{"backend", "fake"},
            {"inputDevice", "input-device"},
            {"outputDevice", "output-device"},
            {"inputChannels", channels},
            {"outputChannels", {0, 1}},
            {"sampleRate", rate},
            {"bufferSize", 128},
            {"monitor", true}}},
          {"globals", global},
          {"tempo", {{"bpm", 97.0}, {"enabled", false}, {"level", -18.0}}},
          {"amp", {{"binary", gain_module}, {"parameters", {{{"id", 1}, {"value", 1.5}}}}}},
          {"preFX", Json::array({pedal(gain_module, "pre-fx.0", 0.8f)})},
          {"postFX", Json::array({pedal(delay_module, "post-fx.0", 0.9f)})}};
}

struct Take {
  fs::path path;
  Json sidecar;
  uint64_t start = 0;
  uint64_t frames = 0;
  uint64_t latency = 0;
};

// Starts a session, optionally plays `before` frames, records `frames` frames, stops, then plays
// enough silence to cover the latency and tail of the export.
Take record(Bridge &bridge, const Json &config, const std::string &name, uint64_t before,
            uint64_t frames) {
  check(bridge.send({{"op", "start"}, {"configuration", config}}).at("ok"));
  const auto status = bridge.status();
  check(status.at("state") == "running" && status.at("blocks") == 0);
  bridge.play(before);
  Take take;
  take.path = scratch / (name + ".wav");
  take.latency = status.at("latencyFrames").get<uint64_t>();
  check(
      bridge.send({{"op", "record"}, {"action", "start"}, {"path", take.path.string()}}).at("ok"));
  take.start = bridge.produced;
  bridge.play(frames);
  take.frames = bridge.produced - take.start;
  check(bridge.send({{"op", "record"}, {"action", "stop"}}).at("ok"));
  bridge.play(take.latency + rate, true);
  bridge.wait_take();
  check(bridge.status().at("recording").at("frames") == take.frames);
  take.sidecar = read_json(take.path);
  check(take.sidecar.at("frames") == take.frames);
  check(take.sidecar.at("droppedFrames") == 0 && take.sidecar.at("discontinuities") == 0);
  check(take.sidecar.at("blockSize") == 128 && take.sidecar.at("sampleRate") == rate);
  check(take.sidecar.at("channels") == bridge.inputs);
  check(take.sidecar.at("bpm") == 97.0);
  // The sidecar carries the configuration the bridge was running, ready to feed back to reamp.
  check(take.sidecar.at("configuration") == config);
  vespera::WavFormat format;
  const auto recorded = read_wav(take.path, format);
  for (uint64_t f = 0; f < take.frames; ++f) {
    for (uint32_t c = 0; c < bridge.inputs; ++c) {
      check(std::bit_cast<uint32_t>(recorded[c][f]) ==
            std::bit_cast<uint32_t>(signal(take.start + f, c)));
    }
  }
  return take;
}

// Recording from the session's first block: the live core IO and graph start from the same
// reset state as the offline render, so every export frame must match.
void parity_from_first_block(uint32_t inputs, float transpose) {
  fake_ae::reset();
  Bridge bridge;
  bridge.inputs = inputs;
  const auto config = configuration(inputs, globals(transpose));
  const std::string name = "first-" + std::to_string(inputs) + (transpose != 0 ? "-t" : "");
  const auto take = record(bridge, config, name, 0, 3 * rate);
  check(take.start == 0);
  const auto reamp = bridge.reamp(take.path, scratch / (name + "-reamp.wav"),
                                  take.sidecar.at("configuration"), take.sidecar);
  check(reamp[0].size() > take.frames);
  check(take.start + take.latency + reamp[0].size() <= bridge.output[0].size());
  const auto mismatch = first_mismatch(bridge.output, take.start + take.latency, reamp);
  report(name.c_str(), mismatch);
  check(!mismatch);
  // The gate really closed: late in the quiet section from 1.0 s to 1.5 s the export is near
  // silence. The window ends 1500 frames early because the transposer's grains read up to its
  // latency ahead of the aligned position, where the loud section resumes.
  for (uint64_t f = rate + rate / 2 - 4000; f < rate + rate / 2 - 1500; ++f) {
    check(std::fabs(reamp[0][f]) < 1e-5f && std::fabs(reamp[1][f]) < 1e-5f);
  }

  // Negative controls: one changed rig parameter or global must break parity.
  auto changed = config;
  changed["amp"]["parameters"][0]["value"] = 1.49;
  const auto other_rig =
      bridge.reamp(take.path, scratch / (name + "-rig.wav"), changed, take.sidecar);
  check(first_mismatch(bridge.output, take.start + take.latency, other_rig).has_value());
  changed = config;
  changed["globals"]["outputGain"] = -3.1;
  const auto other_global =
      bridge.reamp(take.path, scratch / (name + "-global.wav"), changed, take.sidecar);
  check(first_mismatch(bridge.output, take.start + take.latency, other_global).has_value());
  check(bridge.send({{"op", "stop"}}).at("ok"));
}

// A take that starts mid-session meets live core IO and graph state built from audio before the
// take, while the render starts from reset, so parity cannot hold from export frame zero. That
// history leaves the output after a bounded settle, asserted exactly rather than loosely:
//  - the delay fixture: its direct path is 100 frames of reported latency, already removed by
//    the alignment, but its echo reaches a further 32 frames back;
//  - the doubler adds a copy 7.5 ms (360 frames) late, after the graph.
// Export frames [0, 392) may therefore mix in pre-take input; from frame 392 on they must match.
// The remaining state converges inside that window or never diverges: the take starts 8000+
// frames into a loud section, so the gain ramps have reached their targets and both gates are
// open, and the gate's peak envelope follows the input again at the first waveform peak, long
// before the first quiet section.
//
// Transposition is left out on purpose: the shifter's grain schedule and splice positions depend
// on everything it has processed since reset, so a mid-session take reamped from reset never
// realigns with the live output (measured: differences through the end of a 3 s take). Its
// parity holds only for takes that start with the session, which parity_from_first_block covers.
void parity_after_settling_mid_session(uint32_t inputs) {
  fake_ae::reset();
  Bridge bridge;
  bridge.inputs = inputs;
  const auto config = configuration(inputs, globals());
  const std::string name = "mid-" + std::to_string(inputs);
  const auto take = record(bridge, config, name, 8200, 3 * rate);
  check(take.start >= 8200 && take.start < 24000);
  const auto reamp = bridge.reamp(take.path, scratch / (name + "-reamp.wav"), config, take.sidecar);
  constexpr uint64_t settle = 360 + 32;
  const auto early = first_mismatch(bridge.output, take.start + take.latency, reamp);
  check(early.has_value() && early->frame < settle);
  const auto late = first_mismatch(bridge.output, take.start + take.latency, reamp, settle);
  report(name.c_str(), late);
  check(!late);
  check(bridge.send({{"op", "stop"}}).at("ok"));
}

// A take with dropped frames holds silence where they were dropped; its sidecar configuration
// renders as is, and the silence comes out as silence.
void dropped_take_renders() {
  fake_ae::reset();
  const auto config =
      Json{{"globals", Json::object()},
           {"amp", {{"binary", gain_module}, {"parameters", {{{"id", 1}, {"value", 2.0}}}}}}};
  fs::path path;
  Json sidecar;
  for (int attempt = 0; attempt < 20 && (sidecar.is_null() || sidecar.at("droppedFrames") == 0);
       ++attempt) {
    path = scratch / ("dropped-" + std::to_string(attempt) + ".wav");
    vespera::TakeRecorder recorder;
    vespera::TakeOptions options;
    options.path = path;
    options.configuration = config.dump();
    recorder.start(options, {rate, 1, 128, 256});
    std::vector<float> samples(256);
    for (uint64_t offset = 0; offset < 8 * rate; offset += samples.size()) {
      for (size_t f = 0; f < samples.size(); ++f) {
        samples[f] = signal(offset + f, 0) + 0.5f;
      }
      const float *channels[] = {samples.data()};
      recorder.capture(channels, 256, 0, 120, false);
    }
    recorder.finish();
    sidecar = read_json(path);
  }
  const auto dropped = sidecar.at("droppedFrames").get<uint64_t>();
  check(dropped > 0);
  Bridge bridge;
  const auto reamp =
      bridge.reamp(path, scratch / "dropped-reamp.wav", sidecar.at("configuration"), sidecar);
  vespera::WavFormat format;
  const auto take = read_wav(path, format);
  check(reamp[0].size() == format.frames);
  uint64_t silent = 0;
  for (uint64_t f = 0; f < format.frames; ++f) {
    if (take[0][f] == 0.0f) {
      ++silent;
      check(reamp[0][f] == 0.0f && reamp[1][f] == 0.0f);
    } else {
      check(reamp[0][f] == take[0][f] * 2.0f && reamp[1][f] == reamp[0][f]);
    }
  }
  check(silent == dropped);
}

}

int main(int argc, char **argv) {
  if (argc != 3) {
    return 2;
  }
  gain_module = argv[1];
  delay_module = argv[2];
  scratch = fs::temp_directory_path() /
            ("vespera-take-reamp-" + std::to_string(static_cast<long>(getpid())));
  fs::remove_all(scratch);
  fs::create_directories(scratch);
  int result = 0;
  try {
    parity_from_first_block(1, 0.0f);
    parity_from_first_block(2, 0.0f);
    parity_from_first_block(1, 3.0f);
    parity_from_first_block(2, -5.0f);
    parity_after_settling_mid_session(1);
    parity_after_settling_mid_session(2);
    dropped_take_renders();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    result = 1;
  }
  std::error_code ignored;
  fs::remove_all(scratch, ignored);
  return result;
}
