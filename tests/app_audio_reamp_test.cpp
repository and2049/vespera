#include "app_audio_client.hpp"
#include "fake_audio_engine.h"
#include "offline_render.hpp"
#include "wav_file.hpp"

#include <vespera/app_audio.h>
#include <vespera/host_services.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <source_location>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using Json = nlohmann::json;
using Channels = std::vector<std::vector<float>>;
namespace fs = std::filesystem;

std::string gain_module;
fs::path directory;

void check(bool condition, std::source_location location = std::source_location::current()) {
  if (!condition) {
    throw std::runtime_error("reamp check failed at line " + std::to_string(location.line()));
  }
}

struct Bridge {
  const vs_app_audio_api *api = vespera_app_audio_entry(1);
  void *instance = api->create();
  Bridge() { check(instance != nullptr); }
  ~Bridge() { close(); }
  Bridge(const Bridge &) = delete;
  Bridge &operator=(const Bridge &) = delete;
  void close() {
    if (instance) {
      api->destroy(instance);
      instance = nullptr;
    }
  }
  Json call(const Json &request) const {
    return Json::parse(api->request(instance, request.dump().c_str()));
  }
  Json settled(const Json &request) const { return settled_audio_request(api, instance, request); }
  Json reamp() const {
    const auto reply = call({{"op", "status"}});
    check(reply.at("ok").get<bool>());
    return reply.at("value").at("reamp");
  }
  Json finished(std::chrono::seconds limit = std::chrono::seconds(60)) const {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    for (;;) {
      auto job = reamp();
      if (job.at("state") != "rendering") {
        return job;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        throw std::runtime_error("reamp render did not finish within the test deadline");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
};

Json amp(float gain) {
  return {{"binary", gain_module}, {"parameters", {{{"id", 1}, {"value", gain}}}}};
}

Json configuration(const Json &globals, float gain = 0.5f) {
  return {{"globals", globals}, {"amp", amp(gain)}};
}

Json start(const fs::path &input, const fs::path &output, const Json &config,
           const Json &extra = Json::object()) {
  Json request{{"op", "reamp"},
               {"action", "start"},
               {"input", input.string()},
               {"output", output.string()},
               {"configuration", config}};
  request.update(extra);
  return request;
}

std::vector<float> tone(size_t frames, double rate, double hz, double level) {
  std::vector<float> result(frames);
  for (size_t i = 0; i < frames; ++i) {
    const double gate = (i / 3000) % 3 == 2 ? 0.001 : 1.0;
    result[i] = static_cast<float>(
        level * gate * std::sin(2.0 * 3.141592653589793 * hz * static_cast<double>(i) / rate));
  }
  return result;
}

void write_wav(const fs::path &path, uint32_t rate, const Channels &channels) {
  vespera::WavWriter writer(path, rate, static_cast<uint32_t>(channels.size()));
  std::array<const float *, 2> planes{channels[0].data(), channels.back().data()};
  writer.write(planes.data(), channels[0].size());
  writer.close();
}

Channels read_wav(const fs::path &path, vespera::WavFormat &format) {
  vespera::WavReader reader(path);
  format = reader.format();
  Channels result(format.channels, std::vector<float>(format.frames));
  std::array<float *, 2> planes{result[0].data(), result.back().data()};
  check(reader.read(planes.data(), format.frames) == format.frames);
  return result;
}

// What the bridge should produce: a fresh gain graph through the live signal path.
Channels direct_render(const Channels &input, uint32_t rate, const vespera::CoreIoSettings &io,
                       float gain, uint32_t block, double bpm) {
  vespera::Graph graph;
  graph.set_sharing(true);
  graph.set_host_services(
      vespera::HostServices::create({vespera::HostWorkMode::synchronous, 0, false}));
  graph.set_parameter_value(graph.add_module(gain_module), 1, gain);
  vespera::OfflineRenderer renderer(
      graph, {static_cast<double>(rate), block, static_cast<uint32_t>(input.size()), 2, bpm, io});
  Channels output(2);
  size_t position = 0;
  vespera::OfflineStream stream;
  stream.read = [&](float *const *channels, uint64_t frames) {
    const auto count = std::min<uint64_t>(frames, input[0].size() - position);
    for (size_t c = 0; c < input.size(); ++c) {
      std::copy_n(input[c].begin() + static_cast<std::ptrdiff_t>(position), count, channels[c]);
    }
    position += count;
    return count;
  };
  stream.write = [&](const float *const *channels, uint64_t frames) {
    for (size_t c = 0; c < 2; ++c) {
      output[c].insert(output[c].end(), channels[c], channels[c] + frames);
    }
  };
  check(vespera::render_aligned(renderer, stream) == vespera::OfflineRenderOutcome::finished);
  return output;
}

fs::path partial(const fs::path &output) {
  return output.parent_path() / ("." + output.filename().string() + ".partial");
}

void write_text(const fs::path &path, const std::string &text) {
  std::ofstream(path, std::ios::binary) << text;
}

std::string read_text(const fs::path &path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

float peak_of(const Channels &channels) {
  float peak = 0.0f;
  for (const auto &channel : channels) {
    for (const float sample : channel) {
      peak = std::max(peak, std::abs(sample));
    }
  }
  return peak;
}

void test_idle_status() {
  Bridge bridge;
  const auto job = bridge.reamp();
  check(job == Json{{"state", "idle"}, {"progress", 0.0}});
  check(bridge.call({{"op", "reamp"}, {"action", "cancel"}}).at("value") == job);
  check(!bridge.call({{"op", "reamp"}, {"action", "resume"}}).at("ok").get<bool>());
}

void test_conditioned_mono_render_matches_direct_render() {
  const auto input_path = directory / "mono.wav";
  const auto output_path = directory / "mono-out.wav";
  const Channels input{tone(20011, 44100.0, 210.0, 0.4)};
  write_wav(input_path, 44100, input);
  const Json globals{{"inputGain", 3.0},       {"outputGain", -1.5},     {"gateEnabled", true},
                     {"gateThreshold", -40.0}, {"doublerEnabled", true}, {"doublerTime", 5.0},
                     {"transpose", 2.0}};
  Bridge bridge;
  const auto started = bridge.call(start(input_path, output_path, configuration(globals, 0.75f),
                                         {{"bpm", 97.0}, {"blockSize", 64}}));
  check(started.at("ok").get<bool>());
  check(started.at("value").at("state") == "rendering");
  check(started.at("value").at("output") == output_path.string());
  const auto job = bridge.finished();
  check(job.at("state") == "done");
  check(job.at("progress") == 1.0);
  check(job.at("output") == output_path.string() && !job.contains("error"));
  check(fs::exists(output_path) && !fs::exists(partial(output_path)));

  vespera::WavFormat format;
  const auto rendered = read_wav(output_path, format);
  vespera::CoreIoSettings io;
  io.input_gain_db = 3.0f;
  io.output_gain_db = -1.5f;
  io.gate_enabled = true;
  io.gate_threshold_db = -40.0f;
  io.doubler_enabled = true;
  io.doubler_delay_ms = 5.0f;
  io.transpose_semitones = 2.0f;
  const auto expected = direct_render(input, 44100, io, 0.75f, 64, 97.0);
  check(format.sample_rate == 44100 && format.channels == 2);
  check(format.frames == input[0].size() + 221 + 664);
  check(rendered == expected);
  check(job.at("peak").get<float>() == peak_of(expected) && peak_of(expected) > 0.1f);
  check(bridge.reamp() == job);
}

void test_transparent_render_matches_render_graph() {
  const auto input_path = directory / "short.wav";
  const auto output_path = directory / "short-out.wav";
  const Channels input{tone(3001, 48000.0, 440.0, 0.8)};
  write_wav(input_path, 48000, input);
  Bridge bridge;
  check(bridge.call(start(input_path, output_path, configuration(Json::object()))).at("ok"));
  check(bridge.finished().at("state") == "done");
  setenv("VESPERA_ENABLE_RENDER_GRAPH", "1", 1);
  const auto graph = bridge.call({{"op", "renderGraph"},
                                  {"configuration", configuration(Json::object())},
                                  {"sampleRate", 48000},
                                  {"input", {input[0]}}});
  unsetenv("VESPERA_ENABLE_RENDER_GRAPH");
  check(graph.at("ok").get<bool>());
  const auto reference = graph.at("value").at("output").at(0).get<std::vector<float>>();
  vespera::WavFormat format;
  const auto rendered = read_wav(output_path, format);
  check(format.frames == reference.size());
  check(rendered[0] == reference && rendered[1] == reference);
  check(reference[7] == input[0][7] * 0.5f);
}

void test_stereo_take_keeps_channels() {
  const auto input_path = directory / "stereo.wav";
  const auto output_path = directory / "stereo-out.wav";
  const Channels input{tone(5000, 96000.0, 300.0, 0.5), tone(5000, 96000.0, 450.0, 0.25)};
  write_wav(input_path, 96000, input);
  Bridge bridge;
  check(bridge.call(start(input_path, output_path, configuration(Json::object(), 2.0f))).at("ok"));
  check(bridge.finished().at("state") == "done");
  vespera::WavFormat format;
  const auto rendered = read_wav(output_path, format);
  check(format.sample_rate == 96000 && format.frames == 5000);
  for (size_t i = 0; i < 5000; i += 7) {
    check(rendered[0][i] == input[0][i] * 2.0f && rendered[1][i] == input[1][i] * 2.0f);
  }
}

void test_synchronous_failures_keep_previous_state() {
  const auto input_path = directory / "valid.wav";
  const auto output_path = directory / "valid-out.wav";
  write_wav(input_path, 48000, {tone(1000, 48000.0, 100.0, 0.5)});
  const auto empty_path = directory / "empty.wav";
  write_wav(empty_path, 48000, {std::vector<float>{}});
  const auto eight_bit = directory / "eight-bit.wav";
  {
    const unsigned char header[] = {'R',  'I',  'F', 'F', 40,   0,    0, 0, 'W', 'A', 'V', 'E',
                                    'f',  'm',  't', ' ', 16,   0,    0, 0, 1,   0,   1,   0,
                                    0x80, 0xbb, 0,   0,   0x80, 0xbb, 0, 0, 1,   0,   8,   0,
                                    'd',  'a',  't', 'a', 4,    0,    0, 0, 1,   2,   3,   4};
    std::ofstream(eight_bit, std::ios::binary)
        .write(reinterpret_cast<const char *>(header), sizeof header);
  }
  write_text(directory / "existing.wav", "keep");
  Bridge bridge;
  const auto refused = [&](const Json &request, const char *message) {
    const auto reply = bridge.call(request);
    if (reply.at("ok").get<bool>()) {
      return false;
    }
    const auto error = reply.at("error").get<std::string>();
    if (error.find(message) == std::string::npos) {
      std::fprintf(stderr, "unexpected error: %s\n", error.c_str());
      return false;
    }
    return true;
  };
  const auto config = configuration(Json::object());
  check(refused(start(directory / "missing.wav", output_path, config), "missing.wav"));
  check(refused(start(eight_bit, output_path, config), "unsupported"));
  check(refused(start(empty_path, output_path, config), "no audio"));
  check(refused(start("relative.wav", output_path, config), "absolute"));
  check(refused(start(input_path, "out.wav", config), "absolute"));
  check(refused(start(input_path, input_path, config), "differ"));
  check(refused(start(input_path, directory / "no-such-directory" / "out.wav", config),
                "cannot be created"));
  check(refused(start(input_path, output_path, configuration({{"inputGain", 100.0}})),
                "Invalid global controls"));
  check(
      refused(start(input_path, output_path,
                    {{"amp", {{"binary", "/nonexistent/amp.so"}, {"parameters", Json::array()}}}}),
              "Module binary is missing"));
  check(refused(start(input_path, output_path, config, {{"bpm", 300.0}}), "tempo"));
  check(refused(start(input_path, output_path, config, {{"blockSize", 0}}), "block size"));
  check(refused(start(input_path, output_path, config, {{"blockSize", 4097}}), "block size"));
  if (geteuid() != 0) {
    const auto locked = directory / "locked";
    fs::create_directory(locked);
    fs::permissions(locked, fs::perms::owner_read | fs::perms::owner_exec);
    check(refused(start(input_path, locked / "out.wav", config), "cannot be created"));
    fs::permissions(locked, fs::perms::owner_all);
  }
  check(bridge.reamp().at("state") == "idle");
  check(!fs::exists(output_path) && !fs::exists(partial(output_path)));
  check(read_text(directory / "existing.wav") == "keep");
}

// Non-finite input samples render as zero, as the live path processes them.
void test_non_finite_input_renders_as_silence() {
  auto samples = tone(4000, 48000.0, 100.0, 0.5);
  samples[3000] = std::numeric_limits<float>::quiet_NaN();
  samples[3001] = std::numeric_limits<float>::infinity();
  samples[3002] = -std::numeric_limits<float>::infinity();
  write_wav(directory / "non-finite.wav", 48000, {samples});
  Bridge bridge;
  const Json config = configuration({{"doublerEnabled", true}});
  vespera::WavFormat format;
  check(bridge.call(start(directory / "non-finite.wav", directory / "non-finite-out.wav", config))
            .at("ok"));
  check(bridge.finished().at("state") == "done");
  const auto rendered = read_wav(directory / "non-finite-out.wav", format);
  for (const auto &channel : rendered) {
    check(std::all_of(channel.begin(), channel.end(), [](float s) { return std::isfinite(s); }));
  }
  samples[3000] = samples[3001] = samples[3002] = 0.0f;
  write_wav(directory / "zeroed.wav", 48000, {samples});
  check(
      bridge.call(start(directory / "zeroed.wav", directory / "zeroed-out.wav", config)).at("ok"));
  check(bridge.finished().at("state") == "done");
  check(read_wav(directory / "zeroed-out.wav", format) == rendered);
}

void test_render_failure_cleans_up() {
  const auto input_path = directory / "overflow.wav";
  const auto output_path = directory / "overflow-out.wav";
  auto samples = tone(4000, 48000.0, 100.0, 0.5);
  samples[3000] = std::numeric_limits<float>::max();
  write_wav(input_path, 48000, {samples});
  write_text(output_path, "previous take");
  Bridge bridge;
  check(bridge.call(start(input_path, output_path, configuration({{"inputGain", 24.0}}))).at("ok"));
  const auto job = bridge.finished();
  check(job.at("state") == "failed");
  check(job.at("error").get<std::string>().find("conditioning failed at frame 2944") !=
        std::string::npos);
  check(job.at("output") == output_path.string() && !job.contains("peak"));
  check(read_text(output_path) == "previous take" && !fs::exists(partial(output_path)));
  check(bridge.reamp() == job);

  const auto good = directory / "after-failure.wav";
  write_wav(good, 48000, {tone(1000, 48000.0, 100.0, 0.5)});
  check(bridge.call(start(good, output_path, configuration(Json::object()))).at("ok"));
  check(bridge.finished().at("state") == "done");
  vespera::WavFormat format;
  read_wav(output_path, format);
  check(format.frames == 1000);
}

// A long take rendered one frame per block, so cancellation reliably lands mid-render, while the
// live session keeps processing.
void test_cancel_mid_render_and_live_audio() {
  const auto input_path = directory / "long.wav";
  const auto output_path = directory / "long-out.wav";
  write_wav(input_path, 48000, {tone(48000 * 60, 48000.0, 100.0, 0.5)});
  write_text(output_path, "previous take");
  fake_ae::reset();
  Bridge bridge;
  const Json live{{"audio",
                   {{"backend", "fake"},
                    {"inputDevice", "input-device"},
                    {"outputDevice", "output-device"},
                    {"inputChannels", {0}},
                    {"outputChannels", {0, 1}},
                    {"sampleRate", 48000},
                    {"bufferSize", 128},
                    {"monitor", true}}},
                  {"globals", Json::object()},
                  {"amp", amp(0.5f)}};
  check(bridge.settled({{"op", "start"}, {"configuration", live}}).at("ok"));
  const auto config = configuration(Json::object());
  check(bridge.call(start(input_path, output_path, config, {{"blockSize", 1}})).at("ok"));
  const auto again = bridge.call(start(input_path, directory / "other.wav", config));
  check(!again.at("ok").get<bool>() &&
        again.at("error").get<std::string>().find("already running") != std::string::npos);
  check(!fs::exists(directory / "other.wav"));

  std::array<float, 128> in{}, left{}, right{};
  in.fill(0.25f);
  uint64_t position = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  for (;;) {
    check(fake_ae::invoke(in.data(), left.data(), right.data(), 128, position) == AE_PROCESS_OK);
    position += 128;
    const auto job = bridge.reamp();
    check(job.at("state") == "rendering");
    if (job.at("progress").get<double>() > 0.0) {
      check(job.contains("peak") && fs::exists(partial(output_path)));
      break;
    }
    check(std::chrono::steady_clock::now() < deadline);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  check(left[5] == 0.125f && right[5] == 0.125f);
  check(bridge.call({{"op", "reamp"}, {"action", "cancel"}}).at("ok"));
  const auto job = bridge.finished();
  check(job.at("state") == "cancelled");
  check(job.at("progress").get<double>() < 1.0);
  check(job.at("output") == output_path.string() && !job.contains("error") &&
        !job.contains("peak"));
  check(read_text(output_path) == "previous take" && !fs::exists(partial(output_path)));
  check(bridge.call({{"op", "status"}}).at("value").at("state") == "running");
}

// A crash can leave a render's temporary file behind; the next render to that destination
// replaces it, and removes it again if that render does not finish.
void test_stale_partial_output_is_replaced() {
  const auto input_path = directory / "stale.wav";
  const auto output_path = directory / "stale-out.wav";
  const Channels input{tone(4000, 48000.0, 330.0, 0.3)};
  write_wav(input_path, 48000, input);
  write_text(partial(output_path), "left by a crashed render");
  Bridge bridge;
  check(bridge.call(start(input_path, output_path, configuration(Json::object())))
            .at("ok")
            .get<bool>());
  check(bridge.finished().at("state") == "done");
  check(fs::exists(output_path) && !fs::exists(partial(output_path)));
  vespera::WavFormat format;
  check(read_wav(output_path, format)[0].size() == input[0].size());

  write_text(partial(output_path), "left by a crashed render");
  check(bridge.call(start(input_path, output_path, configuration(Json::object())))
            .at("ok")
            .get<bool>());
  bridge.call({{"op", "reamp"}, {"action", "cancel"}});
  const auto job = bridge.finished();
  check(job.at("state") == "cancelled" || job.at("state") == "done");
  check(!fs::exists(partial(output_path)));
}

void test_teardown_cancels_and_removes_partial_output() {
  const auto input_path = directory / "long.wav";
  const auto output_path = directory / "teardown-out.wav";
  Bridge bridge;
  check(
      bridge.call(start(input_path, output_path, configuration(Json::object()), {{"blockSize", 1}}))
          .at("ok"));
  const auto begun = std::chrono::steady_clock::now();
  bridge.close();
  check(std::chrono::steady_clock::now() - begun < std::chrono::seconds(10));
  check(!fs::exists(output_path) && !fs::exists(partial(output_path)));
}

}

int main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s GAIN_MODULE\n", argv[0]);
    return 2;
  }
  gain_module = argv[1];
  directory = fs::temp_directory_path() / ("vespera-reamp-test-" + std::to_string(getpid()));
  fs::remove_all(directory);
  fs::create_directories(directory);
  int status = 0;
  try {
    test_idle_status();
    test_conditioned_mono_render_matches_direct_render();
    test_transparent_render_matches_render_graph();
    test_stereo_take_keeps_channels();
    test_synchronous_failures_keep_previous_state();
    test_non_finite_input_renders_as_silence();
    test_render_failure_cleans_up();
    test_cancel_mid_render_and_live_audio();
    test_stale_partial_output_is_replaced();
    test_teardown_cancels_and_removes_partial_output();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    status = 1;
  }
  fs::remove_all(directory);
  return status;
}
