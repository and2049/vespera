#ifndef VESPERA_TAKE_RECORDER_HPP
#define VESPERA_TAKE_RECORDER_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vespera {

class WavWriter;

// Wait-free single-producer, single-consumer ring of interleaved float frames. The producer
// appends whole planar blocks; the consumer reads contiguous interleaved regions.
//
// A block that does not fit is dropped whole and counted. The ring keeps the timeline intact:
// the dropped frames are owed as a gap of silence that the producer inserts ahead of the next
// block that fits, so audio after a dropout keeps its position. Consumed regions are cleared
// before they are released, which lets the producer insert a gap by advancing its index alone.
class FrameRing {
public:
  struct Region {
    const float *samples = nullptr;
    uint64_t frames = 0;
  };

  FrameRing() = default;
  FrameRing(const FrameRing &) = delete;
  FrameRing &operator=(const FrameRing &) = delete;

  // Allocates a cleared ring and resets every index and count. Requires that neither side is
  // running.
  void allocate(uint32_t channels, uint64_t capacity_frames);
  uint32_t channels() const noexcept { return channels_; }
  uint64_t capacity() const noexcept { return capacity_; }

  // Producer side. Never allocates, locks or blocks. `channels` holds channels() planar
  // pointers. Returns false when the block was dropped.
  bool push(const float *const *channels, uint32_t frames) noexcept;
  // Frames of dropped blocks, including those already represented by inserted silence.
  uint64_t dropped_frames() const noexcept { return dropped_.load(std::memory_order_relaxed); }
  // Frames of dropped blocks not yet represented in the ring. Exact once the producer has
  // stopped and that has been synchronised with the reader.
  uint64_t pending_gap() const noexcept { return gap_.load(std::memory_order_relaxed); }

  // Consumer side. The region is the longest contiguous run of published frames.
  Region readable() const noexcept;
  // Releases the first `frames` frames of readable() to the producer.
  void consume(uint64_t frames) noexcept;

private:
  std::vector<float> samples_;
  uint32_t channels_ = 0;
  uint64_t capacity_ = 0;
  alignas(64) std::atomic<uint64_t> write_{0};
  std::atomic<uint64_t> gap_{0};
  std::atomic<uint64_t> dropped_{0};
  alignas(64) std::atomic<uint64_t> read_{0};
};

enum class TakeState { idle, recording, finalizing, failed };

const char *to_string(TakeState state) noexcept;

// Seconds of stream audio the ring between the audio callback and the file holds.
inline constexpr uint32_t take_buffer_seconds = 4;

struct TakeFormat {
  uint32_t sample_rate = 0;
  uint32_t channels = 0;
  uint32_t block_size = 0;
  uint32_t max_block_frames = 0;
};

struct TakeOptions {
  std::filesystem::path path;
  // JSON text copied into the sidecar's "configuration"; empty writes null.
  std::string configuration;
  // Written to the sidecar when the take ends before its first block.
  double bpm = 120;
  double beat = 0;
  // Ends the take once the file holds this many frames; zero means the WAV size limit.
  uint64_t frame_limit = 0;
};

// Why the most recent take ended.
enum class TakeEnd { none, stopped, full, failed };

struct TakeStatus {
  TakeState state = TakeState::idle;
  // False until the first take starts; the fields below describe the current take, or the most
  // recent one while idle or failed.
  bool has_take = false;
  std::filesystem::path path;
  uint32_t sample_rate = 0;
  uint32_t channels = 0;
  // Frames captured so far while recording or finalizing; frames in the file once it is closed.
  uint64_t frames = 0;
  uint64_t dropped_frames = 0;
  uint64_t discontinuities = 0;
  TakeEnd end = TakeEnd::none;
  std::string error;
};

// Records the raw device input of a running stream to a binary32 WAV file and a JSON sidecar.
//
// The audio callback calls capture() once per accepted block; a take contains whole blocks
// only, from the first block the callback observes after start() to the last one before it
// observes stop(). A dedicated writer thread drains the ring into the file, refreshes the
// header about once a second so an interrupted file stays readable, ends the take at the frame
// limit, and reports disk errors as failed. Dropped blocks are written as silence of the same
// length and counted in dropped_frames, so the file always spans every frame the callback
// accepted while armed, unless it ended early at the frame limit. Samples are stored bit for bit
// as passed to capture(); the session passes the device input with non-finite samples replaced by
// zero, as the live path processes them, so an offline replay of the take meets the same input. A
// block the live path fails after capture() stays in the take.
//
// Lifecycle: idle -> recording (start) -> finalizing (stop, frame limit or disk error) -> idle,
// or failed after a disk error, which persists until the next start(); start() is accepted from
// idle or failed. Finalizing waits
// for the callback to acknowledge the stop at a block boundary, or for settle_quiescent() or
// finish() to acknowledge it on the callback's behalf, then drains, closes the file and writes
// the sidecar.
//
// Every member except capture() belongs to one control thread.
class TakeRecorder {
public:
  TakeRecorder();
  // Finishes any take as finish() does, so the callback must no longer run.
  ~TakeRecorder();
  TakeRecorder(const TakeRecorder &) = delete;
  TakeRecorder &operator=(const TakeRecorder &) = delete;

  // Creates the parent directory and the file, allocates the ring and arms the callback.
  // Throws std::logic_error unless idle or failed, std::invalid_argument for an unsupported
  // format or a path whose sidecar would replace it, and std::runtime_error when the file
  // cannot be created; nothing changes on failure.
  void start(const TakeOptions &options, const TakeFormat &format);
  // Asks the callback to stop at its next block boundary. Does nothing unless recording.
  void stop() noexcept;
  // Acknowledges a pending stop for a callback that is confirmed not to be running. A take that
  // has not been asked to stop keeps recording when the callback resumes.
  void settle_quiescent() noexcept;
  // Stops, drains and closes the take and joins the writer. Requires that the callback is
  // confirmed not to be running.
  void finish() noexcept;
  TakeStatus status() const;

  // Audio callback. While disarmed this is a single acquire load. `beat` and `bpm` describe the
  // block's first frame.
  void capture(const float *const *inputs, uint32_t frames, double beat, double bpm,
               bool discontinuity) noexcept {
    const CaptureStage stage = capture_.load(std::memory_order_acquire);
    if (stage != CaptureStage::disarmed) {
      capture_armed(stage, inputs, frames, beat, bpm, discontinuity);
    }
  }

private:
  // Ownership of the ring's producer side passes between threads only through `capture_`:
  //   disarmed -> armed       control, after preparing the ring and writer
  //   armed -> stopping       control or writer
  //   stopping -> disarmed    callback, at a block boundary, after its last ring access
  // While the callback is confirmed quiescent, the control thread settles any stage to
  // disarmed directly.
  enum class CaptureStage : uint32_t { disarmed, armed, stopping };
  static_assert(std::atomic<CaptureStage>::is_always_lock_free);

  void capture_armed(CaptureStage stage, const float *const *inputs, uint32_t frames, double beat,
                     double bpm, bool discontinuity) noexcept;
  void run(TakeOptions options, TakeFormat format, uint64_t frame_limit) noexcept;
  void join() noexcept;

  FrameRing ring_;
  std::atomic<CaptureStage> capture_{CaptureStage::disarmed};
  // Callback-owned while armed; reset by the control thread before arming.
  bool started_ = false;
  std::atomic<uint64_t> captured_{0};
  std::atomic<uint64_t> discontinuities_{0};
  std::atomic<double> first_beat_{0};
  std::atomic<double> first_bpm_{0};

  mutable std::mutex mutex_;
  std::condition_variable wake_;
  TakeState state_ = TakeState::idle;
  TakeStatus last_;
  uint64_t frame_limit_ = 0;
  // Owned by the writer thread while it runs.
  std::unique_ptr<WavWriter> file_;
  std::thread writer_;
};

}

#endif
