#include <vespera/audio_session.hpp>

#include "fake_audio_engine.h"
#include "take_recorder.hpp"
#include "wav_file.hpp"

#include <nlohmann/json.hpp>

#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

// Only the thread that plays the audio callback counts, so the take writer's own allocations
// never register.
thread_local bool counting_allocations = false;
std::atomic<uint64_t> allocation_count{0};

}

void *operator new(std::size_t size) {
  if (counting_allocations) {
    allocation_count.fetch_add(1, std::memory_order_relaxed);
  }
  if (void *memory = std::malloc(size == 0 ? 1 : size)) {
    return memory;
  }
  throw std::bad_alloc();
}

void *operator new[](std::size_t size) { return operator new(size); }
void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete[](void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void *memory, std::size_t) noexcept { std::free(memory); }

namespace {

using vespera::AudioSession;
using vespera::SessionState;
using vespera::TakeEnd;
using vespera::TakeOptions;
using vespera::TakeState;
using Json = nlohmann::json;

int failures = 0;
const char *current_test = "";
std::filesystem::path scratch;
std::filesystem::path gain_module;

#define CHECK(condition)                                                                           \
  do {                                                                                             \
    if (!(condition)) {                                                                            \
      std::fprintf(stderr, "%s:%d: %s: CHECK(%s) failed\n", __FILE__, __LINE__, current_test,      \
                   #condition);                                                                    \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (false)

template <typename Exception, typename Function> bool throws(Function &&function) {
  try {
    function();
  } catch (const Exception &) {
    return true;
  } catch (...) {
    return false;
  }
  return false;
}

bool eventually(const std::function<bool()> &condition) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!condition()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

vespera::StreamConfig config(uint32_t inputs) {
  vespera::StreamConfig result;
  result.backend_id = "fake";
  result.device_id = "dev-a";
  result.input_channel = 3;
  result.output_channels = {0, 1};
  result.output_mode = vespera::OutputMode::monitor;
  result.core_io.input_gain_db = 6.0f;
  if (inputs == 2) {
    result.input_mode = vespera::InputMode::stereo;
    result.second_input_channel = 1;
  }
  return result;
}

// The device side of the stream: planar input with a sample pattern that is distinct per frame
// and channel, including signed zero, subnormals and values beyond full scale.
struct Device {
  uint32_t channels;
  uint64_t position = 0;
  uint64_t produced = 0;
  std::array<std::vector<float>, 2> history;
  std::array<std::vector<float>, 2> block;
  std::vector<float> left = std::vector<float>(512), right = std::vector<float>(512);

  explicit Device(uint32_t count) : channels(count) {
    for (auto &channel : block) {
      channel.resize(512);
    }
    for (auto &channel : history) {
      channel.reserve(1 << 16);
    }
  }

  float sample(uint64_t frame, uint32_t channel) const {
    switch (frame % 7) {
    case 0:
      return channel == 0 ? -0.0f : 0.0f;
    case 1:
      return std::numeric_limits<float>::denorm_min() * static_cast<float>(channel + 1);
    case 2:
      return 3.5f + static_cast<float>(channel);
    default:
      return (static_cast<float>(frame % 1000) * 0.001f - 0.5f) * (channel == 0 ? 1.0f : -0.75f);
    }
  }

  // Delivers one block; returns the callback result. The block's input is kept in `history`.
  ae_process_result deliver(uint32_t frames, float poison = 0.0f) {
    for (uint32_t c = 0; c < channels; ++c) {
      for (uint32_t f = 0; f < frames; ++f) {
        block[c][f] = sample(produced + f, c);
      }
    }
    if (poison != 0.0f) {
      block[0][frames / 2] = poison;
    }
    for (uint32_t c = 0; c < channels; ++c) {
      history[c].insert(history[c].end(), block[c].begin(), block[c].begin() + frames);
    }
    produced += frames;
    const ae_process_result result = run(frames);
    position += frames;
    return result;
  }

  // Mirrors fake_ae::invoke, including latching recovery on failure, for one or two inputs.
  ae_process_result run(uint32_t frames) {
    auto &control = fake_ae::control();
    const float *inputs[] = {block[0].data(), block[1].data()};
    float *outputs[] = {left.data(), right.data()};
    ae_audio_block audio{};
    audio.struct_size = sizeof(audio);
    audio.inputs = inputs;
    audio.input_channels = channels;
    audio.outputs = outputs;
    audio.output_channels = 2;
    audio.frames = frames;
    audio.stream_id = control.stream_id;
    audio.configuration_generation = control.configuration_generation;
    audio.run_generation = control.run_generation;
    audio.frame_position = position;
    const ae_process_result result = control.process(control.user, &audio);
    if (result == AE_PROCESS_FAILED) {
      control.last_result = AE_ERROR_PROCESSOR_FAILURE;
      control.state = AE_STREAM_STATE_RECOVERY_REQUIRED;
    }
    return result;
  }

  // The input delivered in frames [begin, end) of the device history.
  std::vector<std::vector<float>> span(uint64_t begin, uint64_t end) const {
    std::vector<std::vector<float>> result(channels);
    for (uint32_t c = 0; c < channels; ++c) {
      result[c].assign(history[c].begin() + static_cast<std::ptrdiff_t>(begin),
                       history[c].begin() + static_cast<std::ptrdiff_t>(end));
    }
    return result;
  }
};

std::vector<std::vector<float>> read_wav(const std::filesystem::path &path,
                                         vespera::WavFormat &format) {
  vespera::WavReader reader(path);
  format = reader.format();
  std::vector<std::vector<float>> channels(format.channels, std::vector<float>(format.frames));
  std::vector<float *> pointers;
  for (auto &channel : channels) {
    pointers.push_back(channel.data());
  }
  reader.read(pointers.data(), format.frames);
  return channels;
}

bool identical(const std::vector<std::vector<float>> &a, const std::vector<std::vector<float>> &b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t c = 0; c < a.size(); ++c) {
    if (a[c].size() != b[c].size()) {
      return false;
    }
    for (size_t f = 0; f < a[c].size(); ++f) {
      if (std::bit_cast<uint32_t>(a[c][f]) != std::bit_cast<uint32_t>(b[c][f])) {
        return false;
      }
    }
  }
  return true;
}

Json read_sidecar(std::filesystem::path path) {
  path.replace_extension(".json");
  std::ifstream file(path);
  return Json::parse(file);
}

TakeOptions take(const std::string &name) {
  TakeOptions options;
  options.path = scratch / (name + ".wav");
  options.configuration = R"({"rig":")" + name + R"("})";
  return options;
}

bool settled(AudioSession &session) {
  return eventually([&] { return session.take_status().state == TakeState::idle; });
}

// The take holds exactly the blocks the callback accepted between the start and stop requests,
// bit for bit as the device delivered them, before input gain, in any block sizes.
void test_take_matches_device_input() {
  const std::vector<std::vector<uint32_t>> patterns{{128, 128, 128}, {1, 255, 64, 256, 17, 128}};
  for (const uint32_t channels : {1u, 2u}) {
    for (const auto &pattern : patterns) {
      fake_ae::reset();
      AudioSession session;
      session.prepare(config(channels));
      session.start();
      Device device(channels);
      CHECK(device.deliver(128) == AE_PROCESS_OK);
      CHECK(device.deliver(64) == AE_PROCESS_OK);
      // Input gain applies to the graph, never to the take.
      CHECK(device.left[0] != device.block[0][0] || device.block[0][0] == 0.0f);
      const auto options =
          take("exact-" + std::to_string(channels) + "-" + std::to_string(pattern.size()));
      session.start_take(options);
      const uint64_t begin = device.produced;
      for (const uint32_t frames : pattern) {
        CHECK(device.deliver(frames) == AE_PROCESS_OK);
      }
      const uint64_t end = device.produced;
      session.stop_take();
      CHECK(session.take_status().state == TakeState::finalizing);
      CHECK(device.deliver(32) == AE_PROCESS_OK);
      CHECK(device.deliver(200) == AE_PROCESS_OK);
      CHECK(settled(session));
      const auto status = session.take_status();
      CHECK(status.frames == end - begin && status.dropped_frames == 0);
      CHECK(status.channels == channels && status.sample_rate == 48000);
      vespera::WavFormat format;
      const auto recorded = read_wav(options.path, format);
      CHECK(format.channels == channels && format.sample_rate == 48000);
      CHECK(identical(recorded, device.span(begin, end)));
      const auto sidecar = read_sidecar(options.path);
      CHECK(sidecar.at("frames") == end - begin);
      CHECK(sidecar.at("channels") == channels);
      CHECK(sidecar.at("blockSize") == 128);
      CHECK(sidecar.at("discontinuities") == 0);
      CHECK(sidecar.at("configuration") == Json::parse(options.configuration));
      CHECK(session.close() == vespera::CloseResult::closed);
    }
  }
}

void test_take_requires_running_session() {
  fake_ae::reset();
  AudioSession session;
  CHECK(throws<std::logic_error>([&] { session.start_take(take("stopped")); }));
  session.prepare(config(1));
  CHECK(throws<std::logic_error>([&] { session.start_take(take("prepared")); }));
  CHECK(!std::filesystem::exists(scratch / "prepared.wav"));
  session.stop_take();
  CHECK(session.take_status().state == TakeState::idle && !session.take_status().has_take);
  session.start();
  session.start_take(take("running"));
  CHECK(throws<std::logic_error>([&] { session.start_take(take("second")); }));
  CHECK(session.take_status().path == scratch / "running.wav");
}

// The live callback never allocates while it records, acknowledges a stop or is disarmed.
void test_callback_does_not_allocate() {
  fake_ae::reset();
  AudioSession session;
  session.prepare(config(2));
  session.start();
  Device device(2);
  device.deliver(128);
  session.start_take(take("allocation"));
  allocation_count.store(0);
  counting_allocations = true;
  for (int block = 0; block < 64; ++block) {
    device.run(128);
    device.position += 128;
  }
  counting_allocations = false;
  session.stop_take();
  counting_allocations = true;
  device.run(128);
  device.position += 128;
  device.run(128);
  device.position += 128;
  counting_allocations = false;
  CHECK(allocation_count.load() == 0);
  CHECK(settled(session));
  CHECK(session.take_status().frames == 64 * 128);
}

// A stream stop and restart that keeps the stream open, as a staged graph swap does, keeps the
// take and counts the restart as a discontinuity.
void test_restart_keeps_take() {
  fake_ae::reset();
  AudioSession session;
  session.prepare(config(1));
  session.start();
  Device device(1);
  const auto options = take("restart");
  session.start_take(options);
  const uint64_t begin = device.produced;
  device.deliver(128);
  device.deliver(128);
  session.stop();
  CHECK(session.state() == SessionState::prepared);
  CHECK(session.take_status().state == TakeState::recording);
  session.start();
  device.position = 0;
  device.deliver(128);
  device.deliver(64);
  const uint64_t end = device.produced;
  session.stop_take();
  device.deliver(128);
  CHECK(settled(session));
  vespera::WavFormat format;
  CHECK(identical(read_wav(options.path, format), device.span(begin, end)));
  CHECK(read_sidecar(options.path).at("discontinuities") == 1);
  CHECK(session.take_status().discontinuities == 1);
}

// A stop request against a stopped stream finalizes without waiting for a callback.
void test_stop_while_stream_stopped() {
  fake_ae::reset();
  AudioSession session;
  session.prepare(config(1));
  session.start();
  Device device(1);
  session.start_take(take("stopped-stream"));
  device.deliver(128);
  session.stop();
  session.stop_take();
  CHECK(settled(session));
  CHECK(session.take_status().frames == 128);
}

void test_close_and_destruction_finalize() {
  fake_ae::reset();
  const auto closed = take("closed");
  {
    AudioSession session;
    session.prepare(config(2));
    session.start();
    Device device(2);
    session.start_take(closed);
    device.deliver(100);
    CHECK(session.close() == vespera::CloseResult::closed);
    CHECK(session.take_status().state == TakeState::idle);
    CHECK(session.take_status().end == TakeEnd::stopped);
    CHECK(session.take_status().frames == 100);
  }
  CHECK(read_sidecar(closed.path).at("frames") == 100);

  fake_ae::reset();
  const auto destroyed = take("destroyed");
  {
    AudioSession session;
    session.prepare(config(1));
    session.start();
    Device device(1);
    session.start_take(destroyed);
    device.deliver(77);
  }
  CHECK(read_sidecar(destroyed.path).at("frames") == 77);
  vespera::WavFormat format;
  CHECK(read_wav(destroyed.path, format)[0].size() == 77);
}

// A block that fails processing after capture stays in the take; recovery ends the take.
void test_failed_block_is_captured_and_recovery_ends_take() {
  fake_ae::reset();
  AudioSession session;
  session.prepare(config(1));
  session.start();
  Device device(1);
  const auto options = take("recovery");
  session.start_take(options);
  CHECK(device.deliver(128) == AE_PROCESS_OK);
  CHECK(device.deliver(128, std::numeric_limits<float>::max()) == AE_PROCESS_FAILED);
  const uint64_t end = device.produced;
  CHECK(session.poll().state == SessionState::recovery_required);
  CHECK(session.take_status().state == TakeState::finalizing);
  session.stop();
  CHECK(settled(session));
  vespera::WavFormat format;
  const auto recorded = read_wav(options.path, format);
  CHECK(identical(recorded, device.span(0, end)));
  CHECK(recorded[0][128 + 64] == std::numeric_limits<float>::max());
}

// Non-finite device samples are recorded as zero, as the live path processes them, and the take
// continues; every other sample of the block stays bit for bit.
void test_non_finite_input_is_recorded_as_zero() {
  for (const uint32_t channels : {1u, 2u}) {
    fake_ae::reset();
    AudioSession session;
    session.prepare(config(channels));
    session.start();
    Device device(channels);
    const auto options = take(channels == 1 ? "non-finite-mono" : "non-finite-stereo");
    session.start_take(options);
    CHECK(device.deliver(128) == AE_PROCESS_OK);
    CHECK(device.deliver(128, std::numeric_limits<float>::quiet_NaN()) == AE_PROCESS_OK);
    for (uint32_t c = 0; c < channels; ++c) {
      for (uint32_t f = 0; f < 64; ++f) {
        device.block[c][f] = device.sample(device.produced + f, c);
      }
    }
    device.block[channels - 1][7] = -std::numeric_limits<float>::infinity();
    for (uint32_t c = 0; c < channels; ++c) {
      device.history[c].insert(device.history[c].end(), device.block[c].begin(),
                               device.block[c].begin() + 64);
    }
    device.produced += 64;
    CHECK(device.run(64) == AE_PROCESS_OK);
    device.position += 64;
    CHECK(device.deliver(128) == AE_PROCESS_OK);
    const uint64_t end = device.history[0].size();
    CHECK(session.poll().state == SessionState::running);
    CHECK(session.poll().telemetry.processing_failures == 0);
    session.stop_take();
    device.deliver(128);
    CHECK(settled(session));
    auto expected = device.span(0, end);
    for (auto &channel : expected) {
      for (float &sample : channel) {
        sample = std::isfinite(sample) ? sample : 0.0f;
      }
    }
    vespera::WavFormat format;
    const auto recorded = read_wav(options.path, format);
    CHECK(identical(recorded, expected));
    CHECK(recorded[0][128 + 64] == 0.0f && !std::signbit(recorded[0][128 + 64]));
    CHECK(recorded[channels - 1][256 + 7] == 0.0f);
    CHECK(read_sidecar(options.path).at("frames") == end);
  }
}

// A rejected block is never recorded.
void test_contract_violation_is_not_recorded() {
  fake_ae::reset();
  AudioSession session;
  session.prepare(config(1));
  session.start();
  Device device(1);
  const auto options = take("violation");
  session.start_take(options);
  CHECK(device.deliver(128) == AE_PROCESS_OK);
  CHECK(device.run(300) == AE_PROCESS_FAILED);
  CHECK(session.take_status().frames == 128);
  CHECK(session.close() == vespera::CloseResult::closed);
  CHECK(session.take_status().frames == 128);
}

}

int main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s GAIN_MODULE\n", argv[0]);
    return 2;
  }
  gain_module = std::filesystem::canonical(argv[1]);
  scratch = std::filesystem::temp_directory_path() /
            ("vespera-take-session-" + std::to_string(static_cast<long>(getpid())));
  std::filesystem::remove_all(scratch);
  std::filesystem::create_directories(scratch);
  const std::pair<const char *, std::function<void()>> tests[] = {
      {"take_matches_device_input", test_take_matches_device_input},
      {"take_requires_running_session", test_take_requires_running_session},
      {"callback_does_not_allocate", test_callback_does_not_allocate},
      {"restart_keeps_take", test_restart_keeps_take},
      {"stop_while_stream_stopped", test_stop_while_stream_stopped},
      {"close_and_destruction_finalize", test_close_and_destruction_finalize},
      {"non_finite_input_is_recorded_as_zero", test_non_finite_input_is_recorded_as_zero},
      {"failed_block_is_captured_and_recovery_ends_take",
       test_failed_block_is_captured_and_recovery_ends_take},
      {"contract_violation_is_not_recorded", test_contract_violation_is_not_recorded},
  };
  for (const auto &[name, test] : tests) {
    current_test = name;
    try {
      test();
    } catch (const std::exception &error) {
      std::fprintf(stderr, "%s: unexpected exception: %s\n", name, error.what());
      ++failures;
    }
  }
  std::error_code ignored;
  std::filesystem::remove_all(scratch, ignored);
  if (failures != 0) {
    std::fprintf(stderr, "%d take session check(s) failed\n", failures);
    return 1;
  }
  return 0;
}
