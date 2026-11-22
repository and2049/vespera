#ifndef VESPERA_OFFLINE_RENDER_HPP
#define VESPERA_OFFLINE_RENDER_HPP

#include <vespera/audio_session.hpp>
#include <vespera/core_io.hpp>
#include <vespera/runtime.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace vespera {

inline constexpr uint32_t offline_render_max_block_size = 4096;
inline constexpr double offline_render_max_tail_seconds = 10.0;
// Output past the declared tail continues while it is audible, above about -100 dBFS (below
// 16-bit resolution), and ends at its last audible frame once it has stayed inaudible for the
// hold: longer than the silent gaps between the repeats of a delay set as long as 2 s.
inline constexpr float offline_render_audible_level = 1.0e-5f;
inline constexpr double offline_render_tail_hold_seconds = 2.0;

struct OfflineRenderSettings {
  double sample_rate = 48000.0;
  uint32_t block_size = 128;
  // A stereo input requires a stereo graph; a mono input feeds every graph channel, as in the
  // live session.
  uint32_t input_channels = 1;
  uint32_t graph_channels = 2;
  double bpm = 120.0;
  // Present: the live signal path, with the core IO input stage before the graph and its output
  // stage after it, producing two output channels. Absent: the graph alone, producing one output
  // channel per graph channel.
  std::optional<CoreIoSettings> core_io;
};

// Processes a populated graph block by block offline, reproducing the live session's signal
// path apart from device I/O, metronome clicks, parameter and MIDI events. Every block is flagged
// VS_PROCESS_OFFLINE, the first one also VS_PROCESS_DISCONTINUITY, and carries a tempo context
// whose beat starts at zero. Non-finite input samples are processed as zero, as live input is.
//
// The constructor prepares the graph, which must be editing; the destructor releases it.
// Construction throws std::invalid_argument for invalid settings and whatever Graph::prepare
// throws.
class OfflineRenderer {
public:
  OfflineRenderer(Graph &graph, const OfflineRenderSettings &settings);
  ~OfflineRenderer();
  OfflineRenderer(const OfflineRenderer &) = delete;
  OfflineRenderer &operator=(const OfflineRenderer &) = delete;

  [[nodiscard]] const OfflineRenderSettings &settings() const noexcept { return settings_; }
  [[nodiscard]] uint32_t output_channels() const noexcept;
  // The core IO input latency plus the graph latency: how far output lags its input.
  [[nodiscard]] uint64_t latency_frames() const noexcept;
  // How long output continues after its input ends: the graph tail plus the core IO doubler
  // delay and, while transposing, the input stage latency, at most
  // offline_render_max_tail_seconds. An unbounded graph tail yields that maximum.
  [[nodiscard]] uint64_t tail_frames() const noexcept;
  [[nodiscard]] uint64_t position() const noexcept { return position_; }

  // Planar; `frames` is within [1, block_size]. Throws std::runtime_error, naming the frame,
  // when processing fails or, on the live signal path, when a conditioned or output sample is not
  // finite.
  void process(const float *const *inputs, float *const *outputs, uint32_t frames);

private:
  Graph &graph_;
  OfflineRenderSettings settings_;
  std::optional<MonoStereoAdapter> adapter_;
  std::optional<CoreIoProcessor> core_io_;
  std::vector<float> conditioned_;
  std::vector<float> sanitised_;
  uint64_t position_ = 0;
};

// Pulls input and pushes output in planar blocks of at most the renderer's block size. `read`
// fills every input channel and returns the frames it produced, fewer than requested only at
// the end of the input.
struct OfflineStream {
  std::function<uint64_t(float *const *channels, uint64_t frames)> read;
  std::function<void(const float *const *channels, uint64_t frames)> write;
  // Polled before every block; may be empty.
  std::function<bool()> cancelled;
  // Receives the output frames written so far after every write; may be empty.
  std::function<void(uint64_t written)> progress;
};

enum class OfflineRenderOutcome { finished, cancelled };

// Renders the whole input followed by the renderer's tail through a renderer that has not
// processed anything yet, discarding the first latency_frames() of output so the result is
// sample-aligned with its input: input frames plus tail_frames() frames, then any further output
// up to its last audible frame (see offline_render_tail_hold_seconds), within
// offline_render_max_tail_seconds of the input's end. The result does not depend on the block
// size. Progress reports the frames written so far; while the render looks past the declared
// tail, frames held back as possibly trailing silence are not yet reported.
OfflineRenderOutcome render_aligned(OfflineRenderer &renderer, const OfflineStream &stream);

}

#endif
