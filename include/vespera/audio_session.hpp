#ifndef VESPERA_AUDIO_SESSION_HPP
#define VESPERA_AUDIO_SESSION_HPP

#include <vespera/core_io.hpp>
#include <vespera/midi_queue.hpp>
#include <vespera/parameter_queue.hpp>
#include <vespera/runtime.hpp>
#include <vespera/tempo.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

struct ae_context;
struct ae_stream;

namespace vespera {

class AudioError : public std::runtime_error {
public:
  AudioError(int32_t code, std::string operation, const std::string &detail);

  int32_t code() const noexcept { return code_; }
  const std::string &operation() const noexcept { return operation_; }

private:
  int32_t code_;
  std::string operation_;
};

const char *audio_result_name(int32_t code) noexcept;

struct AudioDevice {
  std::string backend_id;
  std::string device_id;
  std::string display_name;
  uint32_t input_channels = 0;
  uint32_t output_channels = 0;
  bool channel_counts_known = false;
  bool pairable = false;
};

struct AudioChannel {
  uint32_t index = 0;
  std::string name;
  int32_t native_format = 0;
  bool supported = false;
};

struct DeviceCapabilities {
  uint32_t input_channels = 0;
  uint32_t output_channels = 0;
  uint32_t current_sample_rate = 0;
  uint32_t min_frames = 0;
  uint32_t max_frames = 0;
  uint32_t preferred_frames = 0;
  int32_t frame_granularity = 0;
  std::vector<uint32_t> tested_rates;
  std::vector<AudioChannel> inputs;
  std::vector<AudioChannel> outputs;
};

// Live output is silent unless monitoring is requested; the graph still processes every block.
enum class OutputMode { silent, monitor };

// Physical capture layout. mono opens one hardware input; stereo opens two distinct hardware
// inputs that feed a stereo graph's left and right channels independently. Stereo capture
// requires a stereo graph: there is no implicit downmix into a mono graph.
enum class InputMode { mono, stereo };

struct StreamConfig {
  std::string backend_id;
  std::string device_id;
  // Empty selects the input device for output; otherwise the backend pairs the two devices.
  std::string output_device_id;
  // The only input in mono mode; the left input in stereo mode.
  uint32_t input_channel = 0;
  // The right input; required in stereo mode, distinct from input_channel, and rejected in mono
  // mode so a stale selection is never silently ignored.
  std::optional<uint32_t> second_input_channel;
  InputMode input_mode = InputMode::mono;
  std::array<uint32_t, 2> output_channels{0, 1};
  uint32_t output_channel_count = 2;
  uint32_t sample_rate = 48000;
  uint32_t frames_per_block = 128;
  bool allow_adjustment = false;
  OutputMode output_mode = OutputMode::silent;
  // Whole-graph layout: 1 processes a mono graph whose output is duplicated to both hardware
  // outputs; 2 processes a stereo graph fed the mono input on both channels in mono input mode,
  // or the two physical inputs as left and right in stereo input mode.
  uint32_t graph_channels = 2;
  CoreIoSettings core_io;
  TempoSettings tempo;
  double initial_beat = 0;
};

struct StreamInfo {
  uint32_t sample_rate = 0;
  uint32_t frames_per_block = 0;
  uint32_t max_frames_per_block = 0;
  InputMode input_mode = InputMode::mono;
  // Negotiated hardware input count: 1 for mono, 2 for stereo.
  uint32_t input_channels = 0;
  uint32_t output_channels = 2;
  // Equal to physical_inputs[0].
  uint32_t physical_input = 0;
  // Only the first input_channels entries are meaningful; the rest are zero.
  std::array<uint32_t, 2> physical_inputs{};
  std::array<uint32_t, 2> physical_outputs{};
  uint32_t stream_id = 0;
  uint32_t configuration_generation = 0;
  uint32_t graph_latency_frames = 0;
  uint32_t core_io_latency_frames = 0;
  uint32_t graph_channels = 0;
  OutputMode output_mode = OutputMode::silent;
};

// Progress of a live graph replacement. At most one replacement is in flight at a time.
enum class GraphTransitionState {
  idle,
  // Published to the callback, which adopts it at the start of its next block.
  queued,
  // Both graphs process the same live input while the output crossfades between them.
  crossfading,
  // The callback processes only the new graph; poll() retires the old one and promotes the new
  // graph's metadata and parameter generation.
  awaiting_retirement
};

struct GraphTransitionStatus {
  GraphTransitionState state = GraphTransitionState::idle;
  uint32_t transition_frames = 0;
  // Output frames already crossfaded toward the new graph; approximate while crossfading.
  uint32_t faded_frames = 0;
  // Counted since the session was created.
  uint64_t committed = 0;
  uint64_t cancelled = 0;
};

enum class SessionState { stopped, prepared, running, recovery_required };

enum class RecoveryReason {
  none,
  stream_fault,
  processing_failure,
  // The callback may still be executing; its state and the graph are retained until a stop
  // succeeds, or leaked if the session is destroyed first.
  quiescence_failure,
  close_failure
};

enum class DeviceStreamState { ready, running, recovery_required };

struct DeviceStreamStatus {
  DeviceStreamState state = DeviceStreamState::ready;
  int32_t last_result = 0;
  uint32_t stream_id = 0;
  uint32_t configuration_generation = 0;
  uint32_t run_generation = 0;
  uint64_t delivered_frames = 0;
  uint32_t fault_flags = 0;
  bool latency_valid = false;
  uint32_t input_latency_frames = 0;
  uint32_t output_latency_frames = 0;
  uint64_t callback_count = 0;
  uint64_t max_callback_nanoseconds = 0;
  uint64_t deadline_overruns = 0;
  uint64_t driver_overloads = 0;
};

struct CallbackTelemetry {
  uint64_t blocks = 0;
  uint64_t frames = 0;
  uint64_t discontinuities = 0;
  uint64_t processing_failures = 0;
  uint64_t contract_violations = 0;
  float input_peak = 0.0f;
  float output_peak = 0.0f;
  // Frame position expected at the start of the next block; meaningful once blocks > 0.
  uint64_t next_frame_position = 0;
  ParameterTelemetry parameters;
};

// Meter levels are magnitudes saturated at this ceiling (about +12 dBFS); non-finite samples
// read as the ceiling.
inline constexpr float meter_ceiling = 4.0f;

struct ChannelMeter {
  float peak = 0.0f;
  float rms = 0.0f;
  // Samples at or above full scale (|x| >= 1) or non-finite.
  uint64_t clipped_samples = 0;

  bool clipped() const noexcept { return clipped_samples != 0; }
};

// Levels accumulated over one read_meters() interval. Input meters describe the physical inputs
// in StreamInfo::physical_inputs order, before the graph; output meters describe the stereo pair
// the graph presents after core output gain and doubler, including any crossfade, before
// output-mode silencing. Unused channels and blocks that were never delivered read as zero.
struct MeterReading {
  uint32_t input_channels = 0;
  uint64_t input_frames = 0;
  // Fewer than input_frames when processing failed for some blocks.
  uint64_t output_frames = 0;
  std::array<ChannelMeter, 2> inputs{};
  std::array<ChannelMeter, 2> outputs{};
};

struct SessionStatus {
  SessionState state = SessionState::stopped;
  RecoveryReason recovery = RecoveryReason::none;
  int32_t last_error = 0;
  bool stream_open = false;
  DeviceStreamStatus stream;
  CallbackTelemetry telemetry;
  GraphTransitionStatus transition;
};

enum class CloseResult { closed, stop_failed, close_failed };

const char *to_string(SessionState state) noexcept;
const char *to_string(RecoveryReason reason) noexcept;
const char *to_string(DeviceStreamState state) noexcept;
const char *to_string(GraphTransitionState state) noexcept;
const char *to_string(InputMode mode) noexcept;

// Feeds a mono or planar stereo source into a mono or stereo graph through preallocated planar
// buffers and presents the result as a stereo pair. A stereo graph receives a mono source on both
// channels, or a stereo source as distinct left and right channels; a mono graph accepts only a
// mono source, and its single output is presented as both output channels. Shared by the live
// callback and offline rendering so both observe the same block contract.
class MonoStereoAdapter {
public:
  MonoStereoAdapter(Graph &graph, uint32_t max_frames, uint32_t graph_channels = 2);

  // A null input is treated as silence. Never allocates; frames must be in [1, max_frames()].
  // Events are forwarded to Graph::process unchanged.
  vs_result process(const float *input, uint32_t frames, uint64_t frame_position, uint32_t flags,
                    std::span<const Event> events = {},
                    const vs_tempo_context *tempo = nullptr) noexcept;
  // Planar source of one or two channels; a null channel is treated as silence. Two channels
  // into a mono graph, or any other channel count, fails with VS_ERROR_ARGUMENT and silenced
  // outputs without processing the graph.
  vs_result process(std::span<const float *const> inputs, uint32_t frames, uint64_t frame_position,
                    uint32_t flags, std::span<const Event> events = {},
                    const vs_tempo_context *tempo = nullptr) noexcept;
  // Channels 0 and 1 are the stereo pair; both name the same buffer for a mono graph.
  const float *output(uint32_t channel) const noexcept;
  uint32_t max_frames() const noexcept { return max_frames_; }
  uint32_t graph_channels() const noexcept { return graph_channels_; }

private:
  Graph &graph_;
  uint32_t max_frames_;
  uint32_t graph_channels_;
  std::vector<float> storage_;
  std::array<float *, 2> inputs_{};
  std::array<float *, 2> outputs_{};
};

// Owns one audio-io-core context, at most one duplex stream on a device or device pair, and the
// graph processed by its callback. All members must be called from one control thread.
//
// Callback state, including the graph and its loaded modules, lives in a separate heap
// allocation. If the stream cannot be confirmed quiescent when the session is destroyed, that
// allocation and the context are intentionally leaked so a still-running callback never
// observes freed memory.
class AudioSession {
public:
  AudioSession();
  void set_tempo_settings(const TempoSettings &settings);
  double beat_position() const noexcept;
  ~AudioSession();
  AudioSession(const AudioSession &) = delete;
  AudioSession &operator=(const AudioSession &) = delete;

  std::vector<AudioDevice> devices() const;
  void refresh_devices();
  DeviceCapabilities probe(const AudioDevice &device);

  // Editing is only permitted while stopped. Successful preset restoration and committed graph
  // replacement invalidate this reference.
  Graph &graph();
  void reset_tempo(const TempoSettings &settings, double beat);
  // Copies immutable node metadata; safe in any state, including while streaming. During a live
  // replacement it describes the old graph until poll() retires it.
  std::vector<NodeMetadata> nodes() const;
  // Graph::wait_for_work, take_notices and module_status for the active graph; safe in any state,
  // including while streaming, and like nodes() during a live replacement.
  bool wait_for_module_work(std::chrono::steady_clock::time_point deadline) const;
  std::vector<ModuleNotice> take_module_notices();
  std::optional<ModuleStatus> module_status(NodeId instance_id) const;
  // State capture requires confirmed quiescence; restoration prepares a candidate while stopped.
  Preset capture_preset() const;
  // The restored graph attaches services, when given, to the modules that use them.
  void restore_preset(const Preset &preset, const ModuleCatalog &catalog,
                      const StreamConfig &config, std::shared_ptr<HostServices> services = nullptr);

  // Graph::parameter_value and set_parameter_value for a stopped or prepared session, where
  // processing is confirmed quiescent. Reads report values modules actually applied; requests a
  // successful stop discarded are never reflected. There is no live readback while running.
  float parameter_value(NodeId instance_id, uint32_t parameter_id) const;
  void set_parameter_value(NodeId instance_id, uint32_t parameter_id, float value);

  // Host-owned input/output conditioning. A running change is accepted at the next block
  // boundary; at most one change may be pending. A new nonzero transpose requires the fixed
  // latency policy chosen during prepare, so enabling it after a zero-transpose prepare requires
  // stop/close/prepare. Updates never change the graph's module or parameter generation.
  void set_core_io_settings(const CoreIoSettings &settings);
  CoreIoSettings core_io_settings() const noexcept { return core_io_settings_; }

  // Token for set_parameter, valid until the session stops, is re-prepared, requests a graph
  // replacement, or the callback observes an unexpected timeline discontinuity. Zero when not
  // running or while a graph replacement is in flight.
  ParameterToken parameter_token() const noexcept;
  // Only the returned queue's producer methods may be called from MIDI callbacks. Bind its
  // generation while configuring a route on the control thread; rebind after timeline changes.
  MidiQueue &midi_input() noexcept;
  // Queues a native-unit parameter change for the next block, or for an absolute frame of the
  // stream timeline reported by CallbackTelemetry::next_frame_position. `accepted` means
  // queued, not applied; poll() reports applied, stale and late counts. Never blocks.
  // Rejected as stale_token while a graph replacement is in flight.
  //
  // A replacement discards every request still queued or pending for the old graph when the
  // callback adopts the new one; neither graph receives events while crossfading.
  ParameterResult set_parameter(ParameterToken token, NodeId instance_id, uint32_t parameter_id,
                                float value,
                                std::optional<uint64_t> frame_position = std::nullopt) noexcept;

  // Longest accepted replacement crossfade: one second at the negotiated sample rate.
  uint32_t max_transition_frames() const noexcept { return info_.sample_rate; }

  // Replaces the running graph without stopping the stream. `candidate` must be an editing
  // graph; it is prepared here, on the control thread, at the session's negotiated rate, maximum
  // block size and graph channel count, and must report the same latency as the active graph
  // (latency compensation across a transition is not supported). Requires the running state,
  // no replacement already in flight, and transition_frames in [1, max_transition_frames()].
  // Any rejection or preparation failure throws, destroys the candidate and leaves the active
  // graph, its metadata and parameter tokens untouched.
  //
  // The callback adopts the candidate at its next block boundary; the candidate's first block is
  // flagged as a discontinuity. Both graphs then process the same live input while the output
  // crossfades linearly, sample by sample across blocks of any size, from the old graph to the
  // new one over transition_frames. The old contribution reaches zero at the fade's end; after
  // that block the old graph stops processing. Reverb or delay tails do not spill past the fade.
  // Both graphs consume processing time while crossfading. To bypass modules, replace the graph
  // with one that omits them; an empty graph passes the input through dry.
  //
  // poll() retires the old graph off the audio thread once the fade completes and then promotes
  // the new graph's metadata and parameter generation. Until then nodes() describes the old
  // graph and parameter_token() is zero.
  //
  // A failure of either graph while crossfading follows the ordinary policy: the block is
  // silenced and the session enters recovery_required. A successful stop() or close() settles
  // the replacement deterministically: one the callback had not yet adopted is cancelled and
  // destroyed; one it had adopted, completely or partly faded, is committed and the old graph
  // destroyed. If the stream cannot be confirmed quiescent, both graphs are retained.
  void request_graph_replacement(std::unique_ptr<Graph> candidate, uint32_t transition_frames);
  GraphTransitionState graph_transition_state() const noexcept;

  // A prepared graph together with the callback resources the session needs to process it.
  class StagedGraph {
  public:
    StagedGraph() = default;
    explicit operator bool() const noexcept { return graph_ != nullptr; }
    Graph *graph() const noexcept { return graph_.get(); }
    std::unique_ptr<Graph> take_graph() noexcept {
      adapter_.reset();
      metadata_.clear();
      return std::move(graph_);
    }

  private:
    friend class AudioSession;
    std::unique_ptr<Graph> graph_;
    std::optional<MonoStereoAdapter> adapter_;
    std::vector<NodeMetadata> metadata_;
  };

  // Validates and allocates everything needed to adopt `candidate`, which the caller prepared at
  // prepared_format() equal to the session's negotiated rate, maximum block size and graph
  // channel count. Allowed while prepared or running, so the work happens before the stream
  // stops. On failure it throws and destroys the candidate.
  StagedGraph stage_graph(std::unique_ptr<Graph> candidate) const;
  // Adopts a staged graph in the prepared state, with the stream open but stopped so processing
  // is quiescent, and returns the previous graph staged so it can be swapped back without
  // allocating. Unlike request_graph_replacement this accepts a different latency:
  // StreamInfo::graph_latency_frames reports the adopted graph's latency on return and the change
  // takes effect when start() resumes the stream. Parameter tokens are invalidated and queued
  // requests discarded. Throws std::logic_error, changing nothing, unless the session is
  // prepared with no replacement in flight and `next` holds a graph; otherwise it cannot fail.
  StagedGraph swap_staged_graph(StagedGraph &&next);

  void prepare(const StreamConfig &config);
  void start();
  void stop();
  [[nodiscard]] CloseResult close() noexcept;

  // Observes upstream stream faults and transitions to recovery_required when necessary, and
  // retires the old graph of a completed replacement.
  SessionStatus poll();
  // Returns levels accumulated since the previous read, prepare or start, and begins a new
  // interval. Safe in any state and never blocks the callback. Peaks and the remaining fields
  // are each exact and lossless across intervals, but a block in flight during the read may be
  // counted in the peak of one interval and the RMS and clip counts of the next.
  MeterReading read_meters() noexcept;
  SessionState state() const noexcept { return state_; }
  RecoveryReason recovery_reason() const noexcept { return recovery_; }
  const StreamInfo &stream_info() const;

  struct CallbackState;

private:
  void prepare_graph(const StreamConfig &config, std::unique_ptr<Graph> replacement);
  void require_state(SessionState expected, const char *operation) const;
  void enter_recovery(RecoveryReason reason, int32_t code) noexcept;
  [[noreturn]] void fail(int32_t code, const char *operation) const;
  void release_prepared_resources() noexcept;
  bool replacement_in_flight() const noexcept;
  void retire_replacement() noexcept;
  void settle_replacement() noexcept;

  ae_context *context_ = nullptr;
  ae_stream *stream_ = nullptr;
  std::unique_ptr<CallbackState> callback_;
  bool callback_installed_ = false;
  SessionState state_ = SessionState::stopped;
  RecoveryReason recovery_ = RecoveryReason::none;
  int32_t last_error_ = 0;
  StreamInfo info_;
  // Copied at prepare so control-thread validation never reads graph state during processing.
  uint64_t graph_generation_ = 0;
  CoreIoSettings core_io_settings_;
  std::vector<NodeMetadata> prepared_nodes_;
  // The callback slot whose graph the control thread treats as active. It lags the callback's
  // own selection until poll() or quiescent settlement retires a completed replacement.
  uint32_t active_slot_ = 0;
  // The in-flight replacement's values, promoted when it is retired.
  uint64_t incoming_generation_ = 0;
  std::vector<NodeMetadata> incoming_nodes_;
  uint32_t transition_frames_ = 0;
  uint64_t committed_transitions_ = 0;
  uint64_t cancelled_transitions_ = 0;
};

}

#endif
