#include "take_recorder.hpp"

#include "wav_file.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <system_error>

namespace vespera {
namespace {

constexpr auto drain_interval = std::chrono::milliseconds(10);
constexpr auto sync_interval = std::chrono::seconds(1);
constexpr uint64_t silence_chunk_frames = 4096;

std::filesystem::path sidecar_path(const std::filesystem::path &take) {
  auto path = take;
  path.replace_extension(".json");
  return path;
}

void write_sidecar(const std::filesystem::path &path, const nlohmann::ordered_json &sidecar) {
  const std::string text = sidecar.dump(2) + "\n";
  auto temporary = path;
  temporary += ".tmp";
  std::FILE *file = std::fopen(temporary.c_str(), "wb");
  if (!file) {
    throw std::runtime_error("Take sidecar " + path.filename().string() + " cannot be created");
  }
  const bool written = std::fwrite(text.data(), 1, text.size(), file) == text.size();
  if (std::fclose(file) != 0 || !written) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    throw std::runtime_error("Take sidecar " + path.filename().string() + " could not be written");
  }
  std::error_code error;
  std::filesystem::rename(temporary, path, error);
  if (error) {
    std::filesystem::remove(temporary, error);
    throw std::runtime_error("Take sidecar " + path.filename().string() + " could not be written");
  }
}

}

void FrameRing::allocate(uint32_t channels, uint64_t capacity_frames) {
  if (channels == 0 || capacity_frames == 0) {
    throw std::invalid_argument("FrameRing requires channels and capacity");
  }
  samples_.assign(static_cast<size_t>(capacity_frames) * channels, 0.0f);
  channels_ = channels;
  capacity_ = capacity_frames;
  write_.store(0, std::memory_order_relaxed);
  gap_.store(0, std::memory_order_relaxed);
  dropped_.store(0, std::memory_order_relaxed);
  read_.store(0, std::memory_order_relaxed);
}

bool FrameRing::push(const float *const *channels, uint32_t frames) noexcept {
  uint64_t write = write_.load(std::memory_order_relaxed);
  uint64_t space = capacity_ - (write - read_.load(std::memory_order_acquire));
  uint64_t gap = gap_.load(std::memory_order_relaxed);
  if (gap != 0) {
    const uint64_t fill = std::min(gap, space);
    write += fill;
    space -= fill;
    gap -= fill;
  }
  if (gap != 0 || frames > space) {
    gap_.store(gap + frames, std::memory_order_relaxed);
    dropped_.store(dropped_.load(std::memory_order_relaxed) + frames, std::memory_order_relaxed);
    write_.store(write, std::memory_order_release);
    return false;
  }
  gap_.store(0, std::memory_order_relaxed);
  uint64_t position = write % capacity_;
  for (uint32_t frame = 0; frame < frames; ++frame) {
    float *destination = samples_.data() + position * channels_;
    for (uint32_t channel = 0; channel < channels_; ++channel) {
      destination[channel] = channels[channel][frame];
    }
    position = position + 1 == capacity_ ? 0 : position + 1;
  }
  write_.store(write + frames, std::memory_order_release);
  return true;
}

FrameRing::Region FrameRing::readable() const noexcept {
  if (capacity_ == 0) {
    return {};
  }
  const uint64_t read = read_.load(std::memory_order_relaxed);
  const uint64_t write = write_.load(std::memory_order_acquire);
  const uint64_t position = read % capacity_;
  return {samples_.data() + position * channels_, std::min(write - read, capacity_ - position)};
}

void FrameRing::consume(uint64_t frames) noexcept {
  const uint64_t read = read_.load(std::memory_order_relaxed);
  std::fill_n(samples_.data() + (read % capacity_) * channels_, frames * channels_, 0.0f);
  read_.store(read + frames, std::memory_order_release);
}

const char *to_string(TakeState state) noexcept {
  switch (state) {
  case TakeState::idle:
    return "idle";
  case TakeState::recording:
    return "recording";
  case TakeState::finalizing:
    return "finalizing";
  case TakeState::failed:
    return "failed";
  }
  return "unknown";
}

TakeRecorder::TakeRecorder() = default;

TakeRecorder::~TakeRecorder() { finish(); }

void TakeRecorder::start(const TakeOptions &options, const TakeFormat &format) {
  {
    std::lock_guard lock(mutex_);
    if (state_ == TakeState::recording) {
      throw std::logic_error("A take is already recording");
    }
    if (state_ == TakeState::finalizing) {
      throw std::logic_error("A take is still finalizing");
    }
  }
  join();
  if (capture_.load(std::memory_order_acquire) != CaptureStage::disarmed) {
    throw std::logic_error("A take is still finalizing");
  }
  if (format.channels == 0 || format.channels > wav_max_channels || format.block_size == 0 ||
      format.max_block_frames < format.block_size) {
    throw std::invalid_argument("Take format is not supported");
  }
  if (sidecar_path(options.path) == options.path) {
    throw std::invalid_argument("A take cannot share its path with its sidecar");
  }
  if (!options.configuration.empty() && !nlohmann::json::accept(options.configuration)) {
    throw std::invalid_argument("Take configuration is not valid JSON");
  }
  const auto folder = options.path.parent_path();
  std::error_code error;
  if (!folder.empty()) {
    std::filesystem::create_directories(folder, error);
  }
  if (error) {
    throw std::runtime_error("Recording folder " + folder.string() + " cannot be created");
  }
  auto file = std::make_unique<WavWriter>(options.path, format.sample_rate, format.channels);
  uint64_t limit = file->remaining_frames();
  if (options.frame_limit != 0) {
    limit = std::min(limit, options.frame_limit);
  }
  try {
    ring_.allocate(format.channels,
                   std::max<uint64_t>(uint64_t{take_buffer_seconds} * format.sample_rate,
                                      uint64_t{4} * format.max_block_frames));
  } catch (...) {
    file.reset();
    std::filesystem::remove(options.path, error);
    throw;
  }
  started_ = false;
  captured_.store(0, std::memory_order_relaxed);
  discontinuities_.store(0, std::memory_order_relaxed);
  first_beat_.store(options.beat, std::memory_order_relaxed);
  first_bpm_.store(options.bpm, std::memory_order_relaxed);
  file_ = std::move(file);
  {
    std::lock_guard lock(mutex_);
    last_ = {};
    last_.has_take = true;
    last_.path = options.path;
    last_.sample_rate = format.sample_rate;
    last_.channels = format.channels;
    frame_limit_ = limit;
    state_ = TakeState::recording;
  }
  capture_.store(CaptureStage::armed, std::memory_order_release);
  try {
    writer_ = std::thread(&TakeRecorder::run, this, options, format, limit);
  } catch (...) {
    auto armed = CaptureStage::armed;
    capture_.compare_exchange_strong(armed, CaptureStage::stopping, std::memory_order_acq_rel);
    file_.reset();
    std::filesystem::remove(options.path, error);
    std::lock_guard lock(mutex_);
    last_.has_take = false;
    state_ = TakeState::idle;
    throw;
  }
}

void TakeRecorder::stop() noexcept {
  auto armed = CaptureStage::armed;
  capture_.compare_exchange_strong(armed, CaptureStage::stopping, std::memory_order_acq_rel);
  std::lock_guard lock(mutex_);
  if (state_ == TakeState::recording) {
    state_ = TakeState::finalizing;
  }
  wake_.notify_all();
}

void TakeRecorder::settle_quiescent() noexcept {
  auto stopping = CaptureStage::stopping;
  if (capture_.compare_exchange_strong(stopping, CaptureStage::disarmed,
                                       std::memory_order_acq_rel)) {
    std::lock_guard lock(mutex_);
    wake_.notify_all();
  }
}

void TakeRecorder::finish() noexcept {
  stop();
  settle_quiescent();
  join();
}

void TakeRecorder::join() noexcept {
  if (writer_.joinable()) {
    writer_.join();
  }
}

TakeStatus TakeRecorder::status() const {
  std::lock_guard lock(mutex_);
  TakeStatus status = last_;
  status.state = state_;
  if (state_ == TakeState::recording || state_ == TakeState::finalizing) {
    status.frames = std::min(captured_.load(std::memory_order_relaxed), frame_limit_);
    status.dropped_frames = ring_.dropped_frames();
    status.discontinuities = discontinuities_.load(std::memory_order_relaxed);
  }
  return status;
}

void TakeRecorder::capture_armed(CaptureStage stage, const float *const *inputs, uint32_t frames,
                                 double beat, double bpm, bool discontinuity) noexcept {
  if (stage == CaptureStage::stopping) {
    capture_.store(CaptureStage::disarmed, std::memory_order_release);
    return;
  }
  if (!started_) {
    started_ = true;
    first_beat_.store(beat, std::memory_order_relaxed);
    first_bpm_.store(bpm, std::memory_order_relaxed);
  } else if (discontinuity) {
    discontinuities_.store(discontinuities_.load(std::memory_order_relaxed) + 1,
                           std::memory_order_relaxed);
  }
  static_cast<void>(ring_.push(inputs, frames));
  captured_.store(captured_.load(std::memory_order_relaxed) + frames, std::memory_order_relaxed);
}

void TakeRecorder::run(TakeOptions options, TakeFormat format, uint64_t limit) noexcept {
  WavWriter &file = *file_;
  TakeEnd end = TakeEnd::stopped;
  std::string error;
  bool writing = true;
  const auto end_early = [&](TakeEnd reason) {
    writing = false;
    end = reason;
    auto armed = CaptureStage::armed;
    capture_.compare_exchange_strong(armed, CaptureStage::stopping, std::memory_order_acq_rel);
    std::lock_guard lock(mutex_);
    if (state_ == TakeState::recording) {
      state_ = TakeState::finalizing;
    }
  };
  auto last_sync = std::chrono::steady_clock::now();
  for (;;) {
    const bool producer_done = capture_.load(std::memory_order_acquire) == CaptureStage::disarmed;
    try {
      for (auto region = ring_.readable(); region.frames != 0; region = ring_.readable()) {
        if (writing) {
          file.write_interleaved(region.samples, std::min(region.frames, limit - file.frames()));
          if (file.frames() == limit) {
            end_early(TakeEnd::full);
          }
        }
        ring_.consume(region.frames);
      }
      if (producer_done && writing && ring_.pending_gap() != 0) {
        uint64_t gap = ring_.pending_gap();
        if (gap >= limit - file.frames()) {
          gap = limit - file.frames();
          end = TakeEnd::full;
        }
        const std::vector<float> silence(
            static_cast<size_t>(std::min(gap, silence_chunk_frames)) * ring_.channels(), 0.0f);
        while (gap != 0) {
          const uint64_t count = std::min(gap, silence_chunk_frames);
          file.write_interleaved(silence.data(), count);
          gap -= count;
        }
      }
      if (writing && std::chrono::steady_clock::now() - last_sync >= sync_interval) {
        file.sync();
        last_sync = std::chrono::steady_clock::now();
      }
    } catch (const std::exception &failure) {
      error = failure.what();
      end_early(TakeEnd::failed);
    }
    if (producer_done) {
      break;
    }
    std::unique_lock lock(mutex_);
    wake_.wait_for(lock, drain_interval, [&] {
      return capture_.load(std::memory_order_acquire) == CaptureStage::disarmed;
    });
  }

  try {
    file.close();
  } catch (const std::exception &failure) {
    if (end != TakeEnd::failed) {
      error = failure.what();
      end = TakeEnd::failed;
    }
  }
  const uint64_t frames = file.frames();
  const uint64_t captured = captured_.load(std::memory_order_relaxed);
  const uint64_t dropped = ring_.dropped_frames();
  const uint64_t discontinuities = discontinuities_.load(std::memory_order_relaxed);
  if (end != TakeEnd::failed) {
    try {
      nlohmann::ordered_json sidecar;
      sidecar["version"] = 1;
      sidecar["sampleRate"] = format.sample_rate;
      sidecar["channels"] = format.channels;
      sidecar["blockSize"] = format.block_size;
      sidecar["frames"] = frames;
      sidecar["droppedFrames"] = dropped;
      sidecar["discontinuities"] = discontinuities;
      sidecar["bpm"] = captured != 0 ? first_bpm_.load(std::memory_order_relaxed) : options.bpm;
      sidecar["beat"] = captured != 0 ? first_beat_.load(std::memory_order_relaxed) : options.beat;
      sidecar["configuration"] = options.configuration.empty()
                                     ? nlohmann::ordered_json()
                                     : nlohmann::ordered_json::parse(options.configuration);
      write_sidecar(sidecar_path(options.path), sidecar);
    } catch (const std::exception &failure) {
      error = failure.what();
      end = TakeEnd::failed;
    }
  }
  std::lock_guard lock(mutex_);
  last_.frames = frames;
  last_.dropped_frames = dropped;
  last_.discontinuities = discontinuities;
  last_.end = end;
  last_.error = error;
  state_ = end == TakeEnd::failed ? TakeState::failed : TakeState::idle;
}

}
