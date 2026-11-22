#include "take_recorder.hpp"
#include "wav_file.hpp"

#include <nlohmann/json.hpp>

#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using vespera::FrameRing;
using vespera::TakeEnd;
using vespera::TakeFormat;
using vespera::TakeOptions;
using vespera::TakeRecorder;
using vespera::TakeState;
using Json = nlohmann::json;

int failures = 0;
const char *current_test = "";
std::filesystem::path scratch;

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

bool eventually(const std::function<bool()> &condition,
                std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!condition()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

bool same_bits(float a, float b) {
  return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b);
}

// Planar signal whose every sample is distinct and nonzero: channel c of frame f holds
// (f + 1) * (c ? -1 : 1) plus `base`.
struct Signal {
  std::vector<std::vector<float>> channels;

  Signal(uint32_t count, uint64_t frames, float base = 0.0f) : channels(count) {
    for (uint32_t c = 0; c < count; ++c) {
      channels[c].resize(frames);
      for (uint64_t f = 0; f < frames; ++f) {
        const auto value = static_cast<float>(f + 1) + base;
        channels[c][f] = c == 0 ? value : -value;
      }
    }
  }

  std::vector<const float *> at(uint64_t offset) const {
    std::vector<const float *> pointers;
    for (const auto &channel : channels) {
      pointers.push_back(channel.data() + offset);
    }
    return pointers;
  }
};

std::vector<std::vector<float>> read_wav(const std::filesystem::path &path,
                                         vespera::WavFormat *format = nullptr) {
  vespera::WavReader reader(path);
  if (format) {
    *format = reader.format();
  }
  std::vector<std::vector<float>> channels(reader.format().channels,
                                           std::vector<float>(reader.format().frames));
  std::vector<float *> pointers;
  for (auto &channel : channels) {
    pointers.push_back(channel.data());
  }
  if (reader.read(pointers.data(), reader.format().frames) != reader.format().frames) {
    throw std::runtime_error("short WAV read");
  }
  return channels;
}

Json read_json(const std::filesystem::path &path) {
  std::ifstream file(path);
  return Json::parse(file);
}

std::filesystem::path sidecar(std::filesystem::path path) {
  path.replace_extension(".json");
  return path;
}

TakeFormat format(uint32_t channels, uint32_t rate = 48000, uint32_t block = 64,
                  uint32_t max_block = 256) {
  return TakeFormat{rate, channels, block, max_block};
}

// Plays the audio callback: delivers blocks until the recorder settles out of finalizing.
bool acknowledge(TakeRecorder &recorder, uint32_t channels) {
  const Signal silence(channels, 16);
  return eventually([&] {
    const auto state = recorder.status().state;
    if (state != TakeState::recording && state != TakeState::finalizing) {
      return true;
    }
    recorder.capture(silence.at(0).data(), 16, 0, 120, false);
    return false;
  });
}

void test_ring_wraparound() {
  FrameRing ring;
  ring.allocate(2, 8);
  CHECK(ring.capacity() == 8 && ring.channels() == 2);
  CHECK(ring.readable().frames == 0);
  const Signal first(2, 5);
  CHECK(ring.push(first.at(0).data(), 5));
  auto region = ring.readable();
  CHECK(region.frames == 5);
  for (uint64_t f = 0; f < 5; ++f) {
    CHECK(region.samples[f * 2] == static_cast<float>(f + 1));
    CHECK(region.samples[f * 2 + 1] == -static_cast<float>(f + 1));
  }
  ring.consume(5);
  CHECK(ring.readable().frames == 0);

  const Signal second(2, 6, 100.0f);
  CHECK(ring.push(second.at(0).data(), 6));
  region = ring.readable();
  CHECK(region.frames == 3);
  for (uint64_t f = 0; f < 3; ++f) {
    CHECK(region.samples[f * 2] == static_cast<float>(f + 101));
  }
  ring.consume(3);
  region = ring.readable();
  CHECK(region.frames == 3);
  for (uint64_t f = 0; f < 3; ++f) {
    CHECK(region.samples[f * 2] == static_cast<float>(f + 104));
    CHECK(region.samples[f * 2 + 1] == -static_cast<float>(f + 104));
  }
  ring.consume(3);
  CHECK(ring.dropped_frames() == 0 && ring.pending_gap() == 0);

  // A block exactly the size of the free space fits.
  const Signal whole(2, 8, 200.0f);
  CHECK(ring.push(whole.at(0).data(), 8));
  CHECK(!ring.push(whole.at(0).data(), 1));
  CHECK(ring.dropped_frames() == 1);
}

void test_ring_full_drops_keep_timeline() {
  FrameRing ring;
  ring.allocate(1, 8);
  const Signal data(1, 6);
  CHECK(ring.push(data.at(0).data(), 6));
  const Signal late(1, 4, 50.0f);
  CHECK(!ring.push(late.at(0).data(), 4));
  CHECK(ring.dropped_frames() == 4 && ring.pending_gap() == 4);
  // Two frames of the gap fit now; the block behind it is dropped too.
  CHECK(!ring.push(late.at(0).data(), 1));
  CHECK(ring.dropped_frames() == 5 && ring.pending_gap() == 3);
  auto region = ring.readable();
  CHECK(region.frames == 8);
  for (uint64_t f = 0; f < 6; ++f) {
    CHECK(region.samples[f] == static_cast<float>(f + 1));
  }
  CHECK(region.samples[6] == 0.0f && region.samples[7] == 0.0f);
  ring.consume(8);
  const Signal next(1, 2, 70.0f);
  CHECK(ring.push(next.at(0).data(), 2));
  CHECK(ring.pending_gap() == 0 && ring.dropped_frames() == 5);
  region = ring.readable();
  CHECK(region.frames == 5);
  CHECK(region.samples[0] == 0.0f && region.samples[1] == 0.0f && region.samples[2] == 0.0f);
  CHECK(region.samples[3] == 71.0f && region.samples[4] == 72.0f);
  ring.consume(5);

  // A block larger than the ring can never fit and is owed whole.
  const Signal huge(1, 9);
  CHECK(!ring.push(huge.at(0).data(), 9));
  CHECK(ring.dropped_frames() == 14 && ring.pending_gap() == 9);
  // Inserted silence is the zeros consumed regions were cleared to.
  CHECK(!ring.push(next.at(0).data(), 2));
  region = ring.readable();
  CHECK(region.frames == 3);
  CHECK(std::all_of(region.samples, region.samples + region.frames,
                    [](float sample) { return sample == 0.0f; }));
}

void test_ring_concurrent_stress() {
  FrameRing ring;
  ring.allocate(2, 997);
  constexpr uint64_t total = 400000;
  std::atomic<bool> produced{false};
  std::thread producer([&] {
    std::vector<float> left(512), right(512);
    uint64_t next = 0;
    uint32_t size = 1;
    while (next < total) {
      const auto frames = static_cast<uint32_t>(std::min<uint64_t>(size, total - next));
      for (uint32_t f = 0; f < frames; ++f) {
        left[f] = static_cast<float>(next + f + 1);
        right[f] = -left[f];
      }
      const float *channels[] = {left.data(), right.data()};
      static_cast<void>(ring.push(channels, frames));
      next += frames;
      size = size * 7 % 509 + 1;
      if (next % 64 == 0) {
        std::this_thread::yield();
      }
    }
    produced.store(true, std::memory_order_release);
  });
  uint64_t received = 0;
  uint64_t zeros = 0;
  bool ordered = true;
  for (;;) {
    const bool done = produced.load(std::memory_order_acquire);
    for (auto region = ring.readable(); region.frames != 0; region = ring.readable()) {
      for (uint64_t f = 0; f < region.frames; ++f) {
        const float left = region.samples[f * 2];
        const float right = region.samples[f * 2 + 1];
        if (left == 0.0f && right == 0.0f) {
          ++zeros;
        } else if (left != static_cast<float>(received + f + 1) || right != -left) {
          ordered = false;
        }
      }
      received += region.frames;
      ring.consume(region.frames);
    }
    if (done) {
      break;
    }
  }
  producer.join();
  received += ring.pending_gap();
  zeros += ring.pending_gap();
  CHECK(ordered);
  CHECK(received == total);
  CHECK(zeros == ring.dropped_frames());
}

void test_start_stop_records_exact_blocks() {
  const auto path = scratch / "exact" / "take.wav";
  TakeRecorder recorder;
  CHECK(recorder.status().state == TakeState::idle && !recorder.status().has_take);
  Signal signal(2, 64 + 100 + 128);
  signal.channels[0][3] = std::numeric_limits<float>::quiet_NaN();
  signal.channels[1][4] = -std::numeric_limits<float>::infinity();
  signal.channels[0][5] = -0.0f;
  signal.channels[1][6] = std::numeric_limits<float>::denorm_min();
  const Signal before(2, 32, 1000.0f);
  recorder.capture(before.at(0).data(), 32, 0, 120, false);

  TakeOptions options;
  options.path = path;
  options.configuration = R"({"audio":{"sampleRate":48000},"amp":null})";
  options.bpm = 111;
  options.beat = 9;
  recorder.start(options, format(2, 48000, 64, 256));
  auto status = recorder.status();
  CHECK(status.state == TakeState::recording && status.has_take);
  CHECK(status.path == path && status.sample_rate == 48000 && status.channels == 2);
  CHECK(status.frames == 0);
  CHECK(std::filesystem::exists(path));

  recorder.capture(signal.at(0).data(), 64, 3.25, 97, true);
  recorder.capture(signal.at(64).data(), 100, 3.5, 98, true);
  recorder.capture(signal.at(164).data(), 128, 4, 98, false);
  CHECK(recorder.status().frames == 292);
  CHECK(recorder.status().discontinuities == 1);
  recorder.stop();
  CHECK(recorder.status().state == TakeState::finalizing);
  recorder.stop();
  CHECK(recorder.status().state == TakeState::finalizing);
  const Signal after(2, 32, 2000.0f);
  recorder.capture(after.at(0).data(), 32, 5, 98, false);
  recorder.capture(after.at(0).data(), 32, 5, 98, false);
  CHECK(eventually([&] { return recorder.status().state == TakeState::idle; }));
  status = recorder.status();
  CHECK(status.end == TakeEnd::stopped && status.error.empty());
  CHECK(status.frames == 292 && status.dropped_frames == 0 && status.discontinuities == 1);
  CHECK(status.path == path);

  vespera::WavFormat wav;
  const auto channels = read_wav(path, &wav);
  CHECK(wav.sample_rate == 48000 && wav.channels == 2 && wav.frames == 292);
  bool identical = true;
  for (uint32_t c = 0; c < 2; ++c) {
    for (uint64_t f = 0; f < 292; ++f) {
      identical = identical && same_bits(channels[c][f], signal.channels[c][f]);
    }
  }
  CHECK(identical);

  const auto json = read_json(sidecar(path));
  CHECK(json.at("version") == 1);
  CHECK(json.at("sampleRate") == 48000 && json.at("channels") == 2);
  CHECK(json.at("blockSize") == 64);
  CHECK(json.at("frames") == 292 && json.at("droppedFrames") == 0);
  CHECK(json.at("discontinuities") == 1);
  CHECK(json.at("bpm") == 97.0 && json.at("beat") == 3.25);
  CHECK(json.at("configuration") == Json::parse(options.configuration));
  CHECK(!std::filesystem::exists(sidecar(path).string() + ".tmp"));
  // Stopping an idle recorder changes nothing.
  recorder.stop();
  CHECK(recorder.status().state == TakeState::idle && recorder.status().frames == 292);
}

void test_start_is_rejected_while_busy() {
  TakeRecorder recorder;
  TakeOptions options;
  options.path = scratch / "busy.wav";
  recorder.start(options, format(1));
  CHECK(throws<std::logic_error>([&] { recorder.start(options, format(1)); }));
  CHECK(recorder.status().state == TakeState::recording);
  recorder.stop();
  CHECK(throws<std::logic_error>([&] { recorder.start(options, format(1)); }));
  CHECK(recorder.status().state == TakeState::finalizing);
  CHECK(acknowledge(recorder, 1));
  CHECK(recorder.status().state == TakeState::idle);
  // The same recorder takes again once idle, replacing the last take's report.
  options.path = scratch / "busy-again.wav";
  recorder.start(options, format(1));
  CHECK(recorder.status().path == options.path);
  recorder.finish();
  CHECK(recorder.status().state == TakeState::idle);
}

void test_rejected_starts_change_nothing() {
  TakeRecorder recorder;
  TakeOptions options;
  const auto blocker = scratch / "blocker";
  std::ofstream(blocker) << "file";
  options.path = blocker / "take.wav";
  CHECK(throws<std::runtime_error>([&] { recorder.start(options, format(1)); }));
  CHECK(recorder.status().state == TakeState::idle && !recorder.status().has_take);

  if (geteuid() != 0) {
    const auto locked = scratch / "locked";
    std::filesystem::create_directories(locked);
    std::filesystem::permissions(locked, std::filesystem::perms::owner_read |
                                             std::filesystem::perms::owner_exec);
    options.path = locked / "take.wav";
    CHECK(throws<std::runtime_error>([&] { recorder.start(options, format(1)); }));
    CHECK(!std::filesystem::exists(options.path));
    std::filesystem::permissions(locked, std::filesystem::perms::owner_all);
  }

  options.path = scratch / "collide.json";
  CHECK(throws<std::invalid_argument>([&] { recorder.start(options, format(1)); }));
  CHECK(!std::filesystem::exists(options.path));
  options.path = scratch / "bad-config.wav";
  options.configuration = "{not json";
  CHECK(throws<std::invalid_argument>([&] { recorder.start(options, format(1)); }));
  CHECK(!std::filesystem::exists(options.path));
  options.configuration.clear();
  CHECK(throws<std::invalid_argument>([&] { recorder.start(options, format(3)); }));
  CHECK(throws<std::invalid_argument>([&] { recorder.start(options, format(1, 4000)); }));
  CHECK(recorder.status().state == TakeState::idle && !recorder.status().has_take);

  // Missing parent folders are created.
  options.path = scratch / "nested" / "deeper" / "take.wav";
  recorder.start(options, format(1));
  recorder.finish();
  CHECK(std::filesystem::exists(options.path) && std::filesystem::exists(sidecar(options.path)));
  CHECK(read_json(sidecar(options.path)).at("configuration").is_null());
}

void test_frame_limit_ends_take_as_full() {
  TakeRecorder recorder;
  TakeOptions options;
  options.path = scratch / "full.wav";
  options.frame_limit = 100;
  recorder.start(options, format(1, 48000, 64, 64));
  const Signal signal(1, 64 * 4);
  for (uint64_t block = 0; block < 3; ++block) {
    recorder.capture(signal.at(block * 64).data(), 64, 0, 120, false);
  }
  CHECK(recorder.status().frames == 100);
  CHECK(eventually([&] { return recorder.status().state != TakeState::recording; }));
  CHECK(recorder.status().state == TakeState::finalizing);
  // The callback acknowledges the stop the writer requested, at a block boundary.
  recorder.capture(signal.at(192).data(), 64, 0, 120, false);
  CHECK(eventually([&] { return recorder.status().state == TakeState::idle; }));
  const auto status = recorder.status();
  CHECK(status.end == TakeEnd::full && status.error.empty() && status.frames == 100);
  const auto channels = read_wav(options.path);
  CHECK(channels[0].size() == 100);
  CHECK(std::equal(channels[0].begin(), channels[0].end(), signal.channels[0].begin()));
  CHECK(read_json(sidecar(options.path)).at("frames") == 100);

  // A limit reached exactly at a stop is still reported as full.
  options.path = scratch / "full-exact.wav";
  options.frame_limit = 64;
  recorder.start(options, format(1, 48000, 64, 64));
  recorder.capture(signal.at(0).data(), 64, 0, 120, false);
  CHECK(acknowledge(recorder, 1));
  CHECK(recorder.status().end == TakeEnd::full && recorder.status().frames == 64);
}

void test_disk_failure_reports_failed() {
  rlimit original{};
  CHECK(getrlimit(RLIMIT_FSIZE, &original) == 0);
  const auto previous = std::signal(SIGXFSZ, SIG_IGN);
  rlimit limited = original;
  limited.rlim_cur = 16384;
  CHECK(setrlimit(RLIMIT_FSIZE, &limited) == 0);

  TakeRecorder recorder;
  TakeOptions options;
  options.path = scratch / "disk-full.wav";
  recorder.start(options, format(2, 48000, 1024, 1024));
  const Signal signal(2, 1024);
  CHECK(eventually([&] {
    if (recorder.status().state == TakeState::failed) {
      return true;
    }
    recorder.capture(signal.at(0).data(), 1024, 0, 120, false);
    return false;
  }));
  CHECK(setrlimit(RLIMIT_FSIZE, &original) == 0);
  std::signal(SIGXFSZ, previous);
  const auto status = recorder.status();
  CHECK(status.state == TakeState::failed && status.end == TakeEnd::failed);
  CHECK(status.error.find("could not be written") != std::string::npos);
  CHECK(!std::filesystem::exists(sidecar(options.path)));
  // Failure persists through stop and is cleared only by the next take.
  recorder.stop();
  CHECK(recorder.status().state == TakeState::failed);
  options.path = scratch / "after-failure.wav";
  recorder.start(options, format(2));
  CHECK(recorder.status().state == TakeState::recording && recorder.status().error.empty());
  recorder.capture(signal.at(0).data(), 64, 0, 120, false);
  recorder.finish();
  CHECK(recorder.status().state == TakeState::idle && recorder.status().frames == 64);
}

void test_quiescent_settlement() {
  TakeRecorder recorder;
  TakeOptions options;
  options.path = scratch / "quiescent.wav";
  recorder.start(options, format(1));
  const Signal signal(1, 192);
  recorder.capture(signal.at(0).data(), 64, 0, 120, false);
  // A stream stop without a stop request keeps the take; the restart is a discontinuity.
  recorder.settle_quiescent();
  CHECK(recorder.status().state == TakeState::recording);
  recorder.capture(signal.at(64).data(), 64, 0, 120, true);
  recorder.stop();
  recorder.settle_quiescent();
  CHECK(eventually([&] { return recorder.status().state == TakeState::idle; }));
  // The stream resuming after the stop records nothing.
  recorder.capture(signal.at(128).data(), 64, 0, 120, false);
  const auto channels = read_wav(options.path);
  CHECK(channels[0].size() == 128);
  CHECK(std::equal(channels[0].begin(), channels[0].end(), signal.channels[0].begin()));
  CHECK(read_json(sidecar(options.path)).at("discontinuities") == 1);
}

void test_empty_take_uses_start_tempo() {
  TakeRecorder recorder;
  TakeOptions options;
  options.path = scratch / "empty.wav";
  options.bpm = 133;
  options.beat = 17.5;
  recorder.start(options, format(1));
  recorder.stop();
  CHECK(acknowledge(recorder, 1));
  const auto json = read_json(sidecar(options.path));
  CHECK(json.at("frames") == 0 && json.at("bpm") == 133.0 && json.at("beat") == 17.5);
  CHECK(read_wav(options.path)[0].empty());
}

// Captures far more than the ring holds faster than the writer drains: whatever is dropped, the
// file spans every captured frame, in place, with silence exactly where frames were dropped.
void test_burst_keeps_timeline() {
  TakeRecorder recorder;
  TakeOptions options;
  options.path = scratch / "burst.wav";
  recorder.start(options, format(1, 8000, 256, 256));
  constexpr uint64_t total = 256 * 300;
  const Signal signal(1, total);
  for (uint64_t offset = 0; offset < total; offset += 256) {
    recorder.capture(signal.at(offset).data(), 256, 0, 120, false);
  }
  recorder.stop();
  CHECK(acknowledge(recorder, 1));
  const auto status = recorder.status();
  CHECK(status.frames == total);
  const auto channels = read_wav(options.path);
  CHECK(channels[0].size() == total);
  uint64_t zeros = 0;
  bool placed = true;
  for (uint64_t f = 0; f < channels[0].size(); ++f) {
    if (channels[0][f] == 0.0f) {
      ++zeros;
    } else {
      placed = placed && channels[0][f] == signal.channels[0][f];
    }
  }
  CHECK(placed);
  CHECK(zeros == status.dropped_frames);
  CHECK(read_json(sidecar(options.path)).at("droppedFrames") == status.dropped_frames);
}

void test_periodic_sync_keeps_file_readable() {
  TakeRecorder recorder;
  TakeOptions options;
  options.path = scratch / "sync.wav";
  recorder.start(options, format(1));
  const Signal signal(1, 4800);
  recorder.capture(signal.at(0).data(), 4800, 0, 120, false);
  CHECK(eventually([&] { return vespera::WavReader(options.path).format().frames == 4800; },
                   std::chrono::seconds(5)));
  recorder.finish();
}

void test_destruction_finalizes() {
  TakeOptions options;
  options.path = scratch / "destroyed.wav";
  const Signal signal(2, 64);
  {
    TakeRecorder recorder;
    recorder.start(options, format(2));
    recorder.capture(signal.at(0).data(), 64, 0, 120, false);
  }
  CHECK(read_wav(options.path)[1].size() == 64);
  CHECK(read_json(sidecar(options.path)).at("frames") == 64);
}

// A callback thread captures continuously while the control thread starts and stops takes.
void test_concurrent_takes() {
  TakeRecorder recorder;
  std::atomic<bool> running{true};
  std::atomic<uint64_t> counter{0};
  std::thread callback([&] {
    std::vector<float> block(48);
    while (running.load(std::memory_order_relaxed)) {
      const uint64_t first = counter.load(std::memory_order_relaxed);
      for (uint32_t f = 0; f < 48; ++f) {
        block[f] = static_cast<float>(first + f + 1);
      }
      const float *channels[] = {block.data()};
      recorder.capture(channels, 48, 0, 120, false);
      counter.store(first + 48, std::memory_order_relaxed);
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  });
  for (int take = 0; take < 5; ++take) {
    TakeOptions options;
    options.path = scratch / ("concurrent-" + std::to_string(take) + ".wav");
    recorder.start(options, format(1, 48000, 48, 48));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    recorder.stop();
    CHECK(eventually([&] { return recorder.status().state == TakeState::idle; }));
    const auto status = recorder.status();
    const auto channels = read_wav(options.path);
    CHECK(channels[0].size() == status.frames);
    CHECK(status.frames % 48 == 0);
    bool contiguous = !channels[0].empty();
    for (size_t f = 1; f < channels[0].size(); ++f) {
      contiguous = contiguous && channels[0][f] == channels[0][f - 1] + 1.0f;
    }
    CHECK(contiguous);
    CHECK(channels[0].empty() || std::fmod(channels[0][0] - 1.0f, 48.0f) == 0.0f);
  }
  running.store(false);
  callback.join();
}

}

int main() {
  scratch = std::filesystem::temp_directory_path() /
            ("vespera-take-recorder-" + std::to_string(static_cast<long>(getpid())));
  std::filesystem::remove_all(scratch);
  std::filesystem::create_directories(scratch);
  const std::pair<const char *, std::function<void()>> tests[] = {
      {"ring_wraparound", test_ring_wraparound},
      {"ring_full_drops_keep_timeline", test_ring_full_drops_keep_timeline},
      {"ring_concurrent_stress", test_ring_concurrent_stress},
      {"start_stop_records_exact_blocks", test_start_stop_records_exact_blocks},
      {"start_is_rejected_while_busy", test_start_is_rejected_while_busy},
      {"rejected_starts_change_nothing", test_rejected_starts_change_nothing},
      {"frame_limit_ends_take_as_full", test_frame_limit_ends_take_as_full},
      {"disk_failure_reports_failed", test_disk_failure_reports_failed},
      {"quiescent_settlement", test_quiescent_settlement},
      {"empty_take_uses_start_tempo", test_empty_take_uses_start_tempo},
      {"burst_keeps_timeline", test_burst_keeps_timeline},
      {"periodic_sync_keeps_file_readable", test_periodic_sync_keeps_file_readable},
      {"destruction_finalizes", test_destruction_finalizes},
      {"concurrent_takes", test_concurrent_takes},
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
    std::fprintf(stderr, "%d take recorder check(s) failed\n", failures);
    return 1;
  }
  return 0;
}
