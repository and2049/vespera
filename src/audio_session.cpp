#include <vespera/audio_session.hpp>

#include "samples.hpp"
#include "take_recorder.hpp"

#include <audio_engine/engine.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <optional>
#include <type_traits>
#include <utility>

namespace vespera {

namespace {

constexpr uint32_t max_output_channels = 2;

bool valid_graph_channels(uint32_t channels) noexcept { return channels == 1 || channels == 2; }

uint32_t input_channel_count(InputMode mode) noexcept { return mode == InputMode::stereo ? 2 : 1; }

// The callback always conditions two output channels and averages them for a mono device, where a
// doubled copy would only comb-filter the dry signal, so the doubler is withheld from mono devices.
CoreIoSettings device_core_io(CoreIoSettings settings, uint32_t output_channels) noexcept {
  if (output_channels == 1) {
    settings.doubler_enabled = false;
  }
  return settings;
}

std::string describe(int32_t code, const std::string &operation, const std::string &detail) {
  std::string text = operation + ": " + audio_result_name(code);
  if (!detail.empty()) {
    text += " (" + detail + ")";
  }
  return text;
}

std::string diagnostic_detail(const ae_context *context) {
  if (context == nullptr) {
    return {};
  }
  ae_diagnostic diagnostic{};
  diagnostic.struct_size = sizeof(diagnostic);
  if (ae_context_get_diagnostic(context, &diagnostic) != AE_OK || diagnostic.result == AE_OK) {
    return {};
  }
  diagnostic.operation[sizeof(diagnostic.operation) - 1] = '\0';
  diagnostic.message[sizeof(diagnostic.message) - 1] = '\0';
  std::string detail = diagnostic.operation;
  if (diagnostic.message[0] != '\0') {
    detail += detail.empty() ? "" : ": ";
    detail += diagnostic.message;
  }
  return detail;
}

std::string copy_text(const char *text) { return text == nullptr ? std::string() : text; }

std::string copy_name(const char (&name)[64]) {
  return std::string(name, strnlen(name, sizeof(name)));
}

DeviceStreamState to_device_state(ae_stream_state state) {
  switch (state) {
  case AE_STREAM_STATE_READY:
    return DeviceStreamState::ready;
  case AE_STREAM_STATE_RUNNING:
    return DeviceStreamState::running;
  default:
    return DeviceStreamState::recovery_required;
  }
}

struct DeviceSnapshot {
  ae_device_snapshot *handle = nullptr;
  ~DeviceSnapshot() {
    if (handle != nullptr) {
      ae_device_snapshot_release(handle);
    }
  }
};

struct CapabilitySnapshot {
  ae_capability_snapshot *handle = nullptr;
  ~CapabilitySnapshot() {
    if (handle != nullptr) {
      ae_capability_snapshot_release(handle);
    }
  }
};

float block_peak(const float *samples, uint32_t frames) noexcept {
  float peak = 0.0f;
  for (uint32_t i = 0; i < frames; ++i) {
    peak = std::max(peak, std::fabs(samples[i]));
  }
  return peak;
}

// Meter energy is accumulated as squared magnitude in fixed point so control-thread interval
// differences of the monotonic totals stay exact however long the stream runs. Modular uint64
// arithmetic keeps them exact across wraparound while one interval holds fewer than 2^24
// ceiling-level frames (about 5.8 minutes at 48 kHz).
constexpr double meter_energy_scale = 68719476736.0;
constexpr uint32_t meter_channels = 4;
constexpr uint32_t output_meter_base = 2;

struct BlockLevel {
  float peak = 0.0f;
  uint64_t energy = 0;
  uint64_t clipped = 0;
};

BlockLevel measure(const float *samples, uint32_t frames) noexcept {
  BlockLevel level;
  double energy = 0.0;
  for (uint32_t i = 0; i < frames; ++i) {
    const float sample = samples[i];
    const float magnitude =
        std::isfinite(sample) ? std::min(std::fabs(sample), meter_ceiling) : meter_ceiling;
    if (!(std::fabs(sample) < 1.0f)) {
      ++level.clipped;
    }
    level.peak = std::max(level.peak, magnitude);
    energy += static_cast<double>(magnitude) * magnitude;
  }
  level.energy = static_cast<uint64_t>(energy * meter_energy_scale + 0.5);
  return level;
}

void raise_peak(std::atomic<float> &peak, float value) noexcept {
  float current = peak.load(std::memory_order_relaxed);
  while (value > current &&
         !peak.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
  }
}

}

AudioError::AudioError(int32_t code, std::string operation, const std::string &detail)
    : std::runtime_error(describe(code, operation, detail)), code_(code),
      operation_(std::move(operation)) {}

const char *audio_result_name(int32_t code) noexcept {
  switch (code) {
  case AE_OK:
    return "ok";
  case AE_ERROR_INVALID_ARGUMENT:
    return "invalid argument";
  case AE_ERROR_INCOMPATIBLE_ABI:
    return "incompatible ABI";
  case AE_ERROR_INVALID_STATE:
    return "invalid state";
  case AE_ERROR_NOT_FOUND:
    return "not found";
  case AE_ERROR_BUSY:
    return "busy";
  case AE_ERROR_UNSUPPORTED:
    return "unsupported";
  case AE_ERROR_UNSUPPORTED_CONFIGURATION:
    return "unsupported configuration";
  case AE_ERROR_DEVICE_UNAVAILABLE:
    return "device unavailable";
  case AE_ERROR_DRIVER_FAILURE:
    return "driver failure";
  case AE_ERROR_PROCESSOR_FAILURE:
    return "processor failure";
  case AE_ERROR_ALLOCATION_FAILURE:
    return "allocation failure";
  case AE_ERROR_QUIESCENCE_FAILURE:
    return "quiescence failure";
  case AE_ERROR_CONFIGURATION_CHANGED:
    return "configuration changed";
  case AE_ERROR_PERMISSION_DENIED:
    return "permission denied";
  default:
    return "unknown error";
  }
}

const char *to_string(SessionState state) noexcept {
  switch (state) {
  case SessionState::stopped:
    return "stopped";
  case SessionState::prepared:
    return "prepared";
  case SessionState::running:
    return "running";
  case SessionState::recovery_required:
    return "recovery_required";
  }
  return "unknown";
}

const char *to_string(RecoveryReason reason) noexcept {
  switch (reason) {
  case RecoveryReason::none:
    return "none";
  case RecoveryReason::stream_fault:
    return "stream_fault";
  case RecoveryReason::processing_failure:
    return "processing_failure";
  case RecoveryReason::quiescence_failure:
    return "quiescence_failure";
  case RecoveryReason::close_failure:
    return "close_failure";
  }
  return "unknown";
}

const char *to_string(DeviceStreamState state) noexcept {
  switch (state) {
  case DeviceStreamState::ready:
    return "ready";
  case DeviceStreamState::running:
    return "running";
  case DeviceStreamState::recovery_required:
    return "recovery_required";
  }
  return "unknown";
}

const char *to_string(GraphTransitionState state) noexcept {
  switch (state) {
  case GraphTransitionState::idle:
    return "idle";
  case GraphTransitionState::queued:
    return "queued";
  case GraphTransitionState::crossfading:
    return "crossfading";
  case GraphTransitionState::awaiting_retirement:
    return "awaiting_retirement";
  }
  return "unknown";
}

const char *to_string(InputMode mode) noexcept {
  switch (mode) {
  case InputMode::mono:
    return "mono";
  case InputMode::stereo:
    return "stereo";
  }
  return "unknown";
}

MonoStereoAdapter::MonoStereoAdapter(Graph &graph, uint32_t max_frames, uint32_t graph_channels)
    : graph_(graph), max_frames_(max_frames), graph_channels_(graph_channels) {
  if (max_frames == 0) {
    throw std::invalid_argument("MonoStereoAdapter requires a nonzero maximum block size");
  }
  if (!valid_graph_channels(graph_channels)) {
    throw std::invalid_argument("MonoStereoAdapter requires a mono or stereo graph");
  }
  const size_t frames = max_frames;
  storage_.assign(frames * graph_channels * 2, 0.0f);
  float *base = storage_.data();
  for (uint32_t channel = 0; channel < graph_channels; ++channel) {
    inputs_[channel] = base + channel * frames;
    outputs_[channel] = base + (graph_channels + channel) * frames;
  }
}

vs_result MonoStereoAdapter::process(const float *input, uint32_t frames, uint64_t frame_position,
                                     uint32_t flags, std::span<const Event> events,
                                     const vs_tempo_context *tempo) noexcept {
  const float *const source[] = {input};
  return process(std::span<const float *const>(source), frames, frame_position, flags, events,
                 tempo);
}

vs_result MonoStereoAdapter::process(std::span<const float *const> inputs, uint32_t frames,
                                     uint64_t frame_position, uint32_t flags,
                                     std::span<const Event> events,
                                     const vs_tempo_context *tempo) noexcept {
  if (frames == 0 || frames > max_frames_ || inputs.empty() || inputs.size() > graph_channels_) {
    for (uint32_t channel = 0; channel < graph_channels_; ++channel) {
      std::fill_n(outputs_[channel], max_frames_, 0.0f);
    }
    return VS_ERROR_ARGUMENT;
  }
  for (uint32_t channel = 0; channel < graph_channels_; ++channel) {
    const float *source = inputs[std::min<size_t>(channel, inputs.size() - 1)];
    if (source == nullptr) {
      std::fill_n(inputs_[channel], frames, 0.0f);
    } else {
      std::copy_n(source, frames, inputs_[channel]);
    }
  }

  Block block;
  block.inputs = inputs_.data();
  block.outputs = outputs_.data();
  block.channels = graph_channels_;
  block.frames = frames;
  block.frame_position = frame_position;
  block.flags = flags;
  block.tempo = tempo;
  return graph_.process(block, events);
}

const float *MonoStereoAdapter::output(uint32_t channel) const noexcept {
  if (channel >= max_output_channels) {
    return nullptr;
  }
  return outputs_[std::min(channel, graph_channels_ - 1)];
}

struct AudioSession::CallbackState {
  static_assert(std::atomic<uint64_t>::is_always_lock_free);
  static_assert(std::atomic<float>::is_always_lock_free);

  // Ownership of the replacement slot and of `active` passes between threads only through
  // `stage`, or while the callback is confirmed quiescent:
  //   idle -> queued                 control, after filling the inactive slot
  //   queued -> crossfading          callback, adopting at a block boundary
  //   crossfading -> awaiting_retirement
  //                                  callback, after selecting the new slot as active
  //   awaiting_retirement -> idle    control, after destroying the old slot
  // Under quiescence the control thread settles any stage directly back to idle.
  enum class ReplacementStage : uint32_t { idle, queued, crossfading, awaiting_retirement };
  static_assert(std::atomic<ReplacementStage>::is_always_lock_free);
  enum class SettingsStage : uint32_t { idle, queued };
  static_assert(std::atomic<SettingsStage>::is_always_lock_free);

  struct Slot {
    // Declared first so it is destroyed after the adapter that refers to it.
    std::unique_ptr<Graph> graph;
    std::optional<MonoStereoAdapter> adapter;

    void destroy() noexcept {
      adapter.reset();
      if (graph) {
        try {
          graph->release();
        } catch (...) {
        }
        graph.reset();
      }
    }
  };

  CallbackState() { slots[0].graph = std::make_unique<Graph>(); }

  std::array<Slot, 2> slots;
  ParameterQueue parameters;
  MidiQueue midi;
  double midi_sample_rate = 48000;
  std::array<Event, Graph::max_events_per_block> events{};
  static_assert(ParameterQueue::capacity + MidiQueue::capacity <= Graph::max_events_per_block);
  std::atomic<ReplacementStage> stage{ReplacementStage::idle};

  // Written by the control thread only while no callback is installed.
  OutputMode output_mode = OutputMode::silent;
  uint32_t input_channels = 1;
  uint32_t output_channels = 2;
  uint32_t stream_id = 0;
  uint32_t configuration_generation = 0;
  uint32_t graph_channels = 2;
  // Planar crossfade output, graph_channels * the adapters' max_frames.
  std::vector<float> mix;
  CoreIoProcessor core_io;
  std::vector<float> conditioned_input;
  std::vector<float> conditioned_output;
  CoreIoSettings pending_settings;
  TempoClock tempo;
  TempoSettings pending_tempo;
  std::atomic<SettingsStage> tempo_stage{SettingsStage::idle};
  static_assert(std::atomic<double>::is_always_lock_free);
  std::atomic<double> beat_position{0};
  std::vector<float> clicks;
  std::atomic<SettingsStage> settings_stage{SettingsStage::idle};

  // Written by the control thread before publishing `queued`.
  uint32_t transition_frames = 0;
  uint64_t incoming_generation = 0;

  // Selects the slot processed outside a crossfade. Written by the control thread while
  // quiescent and by the callback when it completes a crossfade.
  uint32_t active = 0;

  // Owned by the callback thread between start and confirmed quiescence.
  bool has_position = false;
  uint32_t run_generation = 0;
  uint64_t next_position = 0;
  uint32_t fade_position = 0;
  bool incoming_started = false;

  std::atomic<uint32_t> faded_frames{0};

  std::atomic<uint64_t> blocks{0};
  std::atomic<uint64_t> frames{0};
  std::atomic<uint64_t> discontinuities{0};
  std::atomic<uint64_t> processing_failures{0};
  std::atomic<uint64_t> contract_violations{0};
  std::atomic<float> input_peak{0.0f};
  std::atomic<float> output_peak{0.0f};
  std::atomic<uint64_t> next_frame_position{0};

  // Monotonic meter totals since the last reset_run, in the order input_frames, output_frames,
  // energy[meter_channels], clipped[meter_channels]. Meter channels 0 and 1 are inputs, 2 and 3
  // outputs.
  static constexpr size_t meter_total_count = 2 + 2 * meter_channels;
  using MeterTotals = std::array<uint64_t, meter_total_count>;
  static constexpr size_t energy_base = 2;
  static constexpr size_t clipped_base = 2 + meter_channels;

  // Callback-owned running totals, published as a whole through a sequence lock: odd while the
  // callback is writing, so the control thread observes totals of one block boundary.
  MeterTotals meter_totals{};
  std::atomic<uint32_t> meter_sequence{0};
  std::array<std::atomic<uint64_t>, meter_total_count> meter_published{};
  // Interval peaks, raised by the callback and exchanged with zero by read_meters.
  std::array<std::atomic<float>, meter_channels> meter_peaks{};
  // Control-owned totals at the start of the current read_meters interval.
  MeterTotals meter_baseline{};

  TakeRecorder take;

  void meter(uint32_t channel, const float *samples, uint32_t block_frames) noexcept {
    const BlockLevel level = measure(samples, block_frames);
    raise_peak(meter_peaks[channel], level.peak);
    meter_totals[energy_base + channel] += level.energy;
    meter_totals[clipped_base + channel] += level.clipped;
  }

  void publish_meters() noexcept {
    const uint32_t sequence = meter_sequence.load(std::memory_order_relaxed);
    meter_sequence.store(sequence + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    for (size_t i = 0; i < meter_total_count; ++i) {
      meter_published[i].store(meter_totals[i], std::memory_order_relaxed);
    }
    meter_sequence.store(sequence + 2, std::memory_order_release);
  }

  // Bounded so a callback suspended mid-publication can never stall the control thread.
  std::optional<MeterTotals> published_meters() const noexcept {
    for (int attempt = 0; attempt < 1024; ++attempt) {
      const uint32_t before = meter_sequence.load(std::memory_order_acquire);
      if ((before & 1) != 0) {
        continue;
      }
      MeterTotals totals;
      for (size_t i = 0; i < meter_total_count; ++i) {
        totals[i] = meter_published[i].load(std::memory_order_relaxed);
      }
      std::atomic_thread_fence(std::memory_order_acquire);
      if (meter_sequence.load(std::memory_order_relaxed) == before) {
        return totals;
      }
    }
    return std::nullopt;
  }

  // Requires quiescence. Invalidates every previously issued parameter token.
  void reset_run(uint64_t graph_generation) noexcept {
    core_io.reset();
    settings_stage.store(SettingsStage::idle, std::memory_order_relaxed);
    parameters.clear(graph_generation);
    midi.reset();
    parameters.reset_counters();
    next_frame_position.store(0, std::memory_order_relaxed);
    has_position = false;
    run_generation = 0;
    next_position = 0;
    fade_position = 0;
    incoming_started = false;
    blocks.store(0, std::memory_order_relaxed);
    frames.store(0, std::memory_order_relaxed);
    discontinuities.store(0, std::memory_order_relaxed);
    processing_failures.store(0, std::memory_order_relaxed);
    contract_violations.store(0, std::memory_order_relaxed);
    input_peak.store(0.0f, std::memory_order_relaxed);
    output_peak.store(0.0f, std::memory_order_relaxed);
    meter_totals.fill(0);
    meter_baseline.fill(0);
    for (std::atomic<float> &peak : meter_peaks) {
      peak.store(0.0f, std::memory_order_relaxed);
    }
    publish_meters();
  }

  CallbackTelemetry telemetry() const noexcept {
    CallbackTelemetry result;
    result.blocks = blocks.load(std::memory_order_relaxed);
    result.frames = frames.load(std::memory_order_relaxed);
    result.discontinuities = discontinuities.load(std::memory_order_relaxed);
    result.processing_failures = processing_failures.load(std::memory_order_relaxed);
    result.contract_violations = contract_violations.load(std::memory_order_relaxed);
    result.input_peak = input_peak.load(std::memory_order_relaxed);
    result.output_peak = output_peak.load(std::memory_order_relaxed);
    result.next_frame_position = next_frame_position.load(std::memory_order_relaxed);
    result.parameters = parameters.telemetry();
    return result;
  }
};

namespace {

void silence(const ae_audio_block &block) noexcept {
  if (block.outputs == nullptr) {
    return;
  }
  for (uint32_t channel = 0; channel < block.output_channels; ++channel) {
    if (block.outputs[channel] != nullptr) {
      std::fill_n(block.outputs[channel], block.frames, 0.0f);
    }
  }
}

using ReplacementStage = AudioSession::CallbackState::ReplacementStage;

bool valid_block(const AudioSession::CallbackState &state, const MonoStereoAdapter &adapter,
                 const ae_audio_block &block) noexcept {
  if (block.struct_size < sizeof(ae_audio_block) || block.input_channels != state.input_channels ||
      block.output_channels != state.output_channels || block.frames == 0 ||
      block.frames > adapter.max_frames() || block.inputs == nullptr || block.outputs == nullptr) {
    return false;
  }
  for (uint32_t channel = 0; channel < block.input_channels; ++channel) {
    if (block.inputs[channel] == nullptr) {
      return false;
    }
  }
  for (uint32_t channel = 0; channel < block.output_channels; ++channel) {
    if (block.outputs[channel] == nullptr) {
      return false;
    }
  }
  return block.stream_id == state.stream_id &&
         block.configuration_generation == state.configuration_generation;
}

// Linear crossfade with weight min(1, (faded + frame + 1) / transition_frames) on the incoming
// graph. Weights sum to one, and mixing in double keeps finite inputs finite after narrowing.
void crossfade(AudioSession::CallbackState &state, const MonoStereoAdapter &outgoing,
               const MonoStereoAdapter &incoming, uint32_t frames) noexcept {
  const size_t stride = outgoing.max_frames();
  const double length = state.transition_frames;
  for (uint32_t channel = 0; channel < state.graph_channels; ++channel) {
    const float *from = outgoing.output(channel);
    const float *to = incoming.output(channel);
    float *mixed = state.mix.data() + channel * stride;
    for (uint32_t i = 0; i < frames; ++i) {
      const uint64_t elapsed = static_cast<uint64_t>(state.fade_position) + i + 1;
      const double weight =
          elapsed >= state.transition_frames ? 1.0 : static_cast<double>(elapsed) / length;
      mixed[i] = static_cast<float>(static_cast<double>(from[i]) * (1.0 - weight) +
                                    static_cast<double>(to[i]) * weight);
    }
  }
}

const float *mixed_output(const AudioSession::CallbackState &state, uint32_t channel,
                          size_t stride) noexcept {
  return state.mix.data() + std::min(channel, state.graph_channels - 1) * stride;
}

// Applies the settings the control thread queued while the stream was running. The staged
// atomics make each application single-shot; a discontinuity resets the core IO state so the
// conditioning never resumes mid-stream state.
void apply_pending_settings(AudioSession::CallbackState &state, uint32_t flags) noexcept {
  if (state.settings_stage.load(std::memory_order_acquire) ==
      AudioSession::CallbackState::SettingsStage::queued) {
    static_cast<void>(state.core_io.set_settings(state.pending_settings));
    state.settings_stage.store(AudioSession::CallbackState::SettingsStage::idle,
                               std::memory_order_release);
  }
  if ((flags & VS_PROCESS_DISCONTINUITY) != 0) {
    state.core_io.reset();
  }
  if (state.tempo_stage.load(std::memory_order_acquire) ==
      AudioSession::CallbackState::SettingsStage::queued) {
    state.tempo.set_settings(state.pending_tempo);
    state.tempo_stage.store(AudioSession::CallbackState::SettingsStage::idle,
                            std::memory_order_release);
  }
}

// Meters the block's inputs and raises the interval peak; returns whether every input sample
// is finite.
bool meter_inputs(AudioSession::CallbackState &state, const ae_audio_block &block) noexcept {
  float input_peak = 0.0f;
  bool input_finite = true;
  for (uint32_t channel = 0; channel < block.input_channels; ++channel) {
    input_peak = std::max(input_peak, block_peak(block.inputs[channel], block.frames));
    input_finite =
        input_finite && vespera::detail::finite_samples(&block.inputs[channel], 1, block.frames);
  }
  if (input_peak > state.input_peak.load(std::memory_order_relaxed)) {
    state.input_peak.store(input_peak, std::memory_order_relaxed);
  }
  for (uint32_t channel = 0; channel < block.input_channels; ++channel) {
    state.meter(channel, block.inputs[channel], block.frames);
  }
  state.meter_totals[0] += block.frames;
  return input_finite;
}

// Runs the graph output through the core IO output stage, adds the metronome clicks and
// replaces `output` with the conditioned channels; false leaves the failure to the caller.
bool condition_output(AudioSession::CallbackState &state,
                      std::array<const float *, max_output_channels> &output, uint32_t frames,
                      size_t stride) noexcept {
  std::array<float *, max_output_channels> final_dest{};
  for (uint32_t channel = 0; channel < max_output_channels; ++channel) {
    final_dest[channel] = state.conditioned_output.data() + channel * stride;
  }
  if (!state.core_io.process_output(output.data(), state.graph_channels, final_dest.data(),
                                    max_output_channels, frames)) {
    return false;
  }
  for (uint32_t channel = 0; channel < max_output_channels; ++channel) {
    for (uint32_t f = 0; f < frames; ++f) {
      final_dest[channel][f] += state.clicks[f];
    }
    output[channel] = final_dest[channel];
  }
  return true;
}

ae_process_result process_stream(void *user, const ae_audio_block *block) noexcept {
  auto &state = *static_cast<AudioSession::CallbackState *>(user);
  if (block == nullptr || !state.slots[state.active].adapter.has_value()) {
    state.contract_violations.fetch_add(1, std::memory_order_relaxed);
    return AE_PROCESS_FAILED;
  }
  MonoStereoAdapter &active = *state.slots[state.active].adapter;
  if (!valid_block(state, active, *block)) {
    state.contract_violations.fetch_add(1, std::memory_order_relaxed);
    silence(*block);
    return AE_PROCESS_FAILED;
  }

  uint32_t flags = 0;
  if (!state.has_position || block->run_generation != state.run_generation ||
      block->frame_position != state.next_position) {
    flags |= VS_PROCESS_DISCONTINUITY;
    state.discontinuities.fetch_add(1, std::memory_order_relaxed);
    if (state.has_position) {
      // Scheduled positions refer to the broken timeline; the first block of a run defines it.
      state.parameters.invalidate_timeline();
      state.midi.reset();
    }
  }
  state.has_position = true;
  state.run_generation = block->run_generation;
  state.next_position = block->frame_position + block->frames;
  state.next_frame_position.store(state.next_position, std::memory_order_relaxed);
  state.blocks.fetch_add(1, std::memory_order_relaxed);
  state.frames.fetch_add(block->frames, std::memory_order_relaxed);

  apply_pending_settings(state, flags);
  const auto tempo =
      state.tempo.context((flags & VS_PROCESS_DISCONTINUITY) ? VS_TEMPO_DISCONTINUITY : 0);
  for (uint32_t f = 0; f < block->frames; ++f) {
    state.clicks[f] = state.tempo.sample();
  }
  state.beat_position.store(state.tempo.beat_position(), std::memory_order_relaxed);

  const uint32_t frames = block->frames;
  std::array<const float *, max_output_channels> conditioned{};
  std::array<float *, max_output_channels> conditioned_dest{};
  const size_t stride = active.max_frames();
  for (uint32_t channel = 0; channel < block->input_channels; ++channel) {
    conditioned_dest[channel] = state.conditioned_input.data() + channel * stride;
    conditioned[channel] = conditioned_dest[channel];
  }

  // The meters show the raw input, counting non-finite samples as clipped; everything after them,
  // the take included, sees those samples as zero, so they never stop the stream.
  const float *const *device_input = block->inputs;
  if (!meter_inputs(state, *block)) {
    vespera::detail::copy_zeroing_non_finite(block->inputs, conditioned_dest.data(),
                                             block->input_channels, frames);
    device_input = conditioned.data();
  }
  state.take.capture(device_input, frames, tempo.beat_position, tempo.bpm,
                     (flags & VS_PROCESS_DISCONTINUITY) != 0);
  if (!state.core_io.process_input(device_input, conditioned_dest.data(), block->input_channels,
                                   frames) ||
      !vespera::detail::finite_samples(conditioned.data(), block->input_channels, frames)) {
    state.processing_failures.fetch_add(1, std::memory_order_relaxed);
    state.publish_meters();
    silence(*block);
    return AE_PROCESS_FAILED;
  }
  const std::span<const float *const> graph_input(conditioned.data(), block->input_channels);

  ReplacementStage stage = state.stage.load(std::memory_order_acquire);
  if (stage == ReplacementStage::queued) {
    // Requests issued against the old graph must never reach the new one.
    state.parameters.activate_graph(state.incoming_generation);
    state.midi.reset();
    static_cast<void>(state.parameters.collect(block->frame_position, frames));
    state.fade_position = 0;
    state.incoming_started = false;
    stage = ReplacementStage::crossfading;
    state.stage.store(stage, std::memory_order_relaxed);
  }

  std::array<const float *, max_output_channels> output{};
  if (stage == ReplacementStage::crossfading) {
    flags |= VS_PROCESS_MIDI_PANIC;
    const uint32_t incoming_slot = 1 - state.active;
    MonoStereoAdapter &incoming = *state.slots[incoming_slot].adapter;
    const uint32_t incoming_flags =
        state.incoming_started ? flags : flags | VS_PROCESS_DISCONTINUITY;
    state.incoming_started = true;
    const vs_result outgoing_result =
        active.process(graph_input, frames, block->frame_position, flags, {}, &tempo);
    const vs_result incoming_result =
        incoming.process(graph_input, frames, block->frame_position, incoming_flags, {}, &tempo);
    if (outgoing_result != VS_OK || incoming_result != VS_OK) {
      state.processing_failures.fetch_add(1, std::memory_order_relaxed);
      state.publish_meters();
      silence(*block);
      return AE_PROCESS_FAILED;
    }
    crossfade(state, active, incoming, frames);
    for (uint32_t channel = 0; channel < max_output_channels; ++channel) {
      output[channel] = mixed_output(state, channel, stride);
    }
    const uint64_t faded = static_cast<uint64_t>(state.fade_position) + frames;
    state.fade_position = static_cast<uint32_t>(std::min<uint64_t>(faded, state.transition_frames));
    state.faded_frames.store(state.fade_position, std::memory_order_relaxed);
    if (state.fade_position == state.transition_frames) {
      state.active = incoming_slot;
      state.stage.store(ReplacementStage::awaiting_retirement, std::memory_order_release);
    }
  } else {
    const auto parameters = state.parameters.collect(block->frame_position, frames);
    bool panic = false;
    const uint64_t now = midi_clock_ns();
    const auto duration = static_cast<uint64_t>(frames * 1e9 / state.midi_sample_rate);
    const auto midi = state.midi.collect(now > duration ? now - duration : 0, frames,
                                         state.midi_sample_rate, panic);
    if (panic) {
      flags |= VS_PROCESS_MIDI_PANIC;
    }
    const auto end = std::merge(
        parameters.begin(), parameters.end(), midi.begin(), midi.end(), state.events.begin(),
        [](const Event &a, const Event &b) { return a.frame_offset() < b.frame_offset(); });
    const std::span<const Event> events(state.events.begin(), end);
    if (active.process(graph_input, frames, block->frame_position, flags, events, &tempo) !=
        VS_OK) {
      state.processing_failures.fetch_add(1, std::memory_order_relaxed);
      state.publish_meters();
      silence(*block);
      return AE_PROCESS_FAILED;
    }
    state.parameters.count_applied(parameters.size());
    for (uint32_t channel = 0; channel < max_output_channels; ++channel) {
      output[channel] = active.output(channel);
    }
  }

  if (!condition_output(state, output, frames, stride)) {
    state.processing_failures.fetch_add(1, std::memory_order_relaxed);
    state.publish_meters();
    silence(*block);
    return AE_PROCESS_FAILED;
  }
  if (!vespera::detail::finite_samples(output.data(), max_output_channels, frames)) {
    state.processing_failures.fetch_add(1, std::memory_order_relaxed);
    state.publish_meters();
    silence(*block);
    return AE_PROCESS_FAILED;
  }

  float output_peak = 0.0f;
  for (uint32_t channel = 0; channel < max_output_channels; ++channel) {
    output_peak = std::max(output_peak, block_peak(output[channel], frames));
  }
  if (output_peak > state.output_peak.load(std::memory_order_relaxed)) {
    state.output_peak.store(output_peak, std::memory_order_relaxed);
  }
  for (uint32_t channel = 0; channel < max_output_channels; ++channel) {
    state.meter(output_meter_base + channel, output[channel], frames);
  }
  state.meter_totals[1] += frames;
  state.publish_meters();

  if (state.output_mode != OutputMode::monitor) {
    silence(*block);
    return AE_PROCESS_SILENCE;
  }
  if (block->output_channels == 1) {
    for (uint32_t frame = 0; frame < frames; ++frame) {
      block->outputs[0][frame] = output[0][frame] * 0.5f + output[1][frame] * 0.5f;
    }
  } else {
    for (uint32_t channel = 0; channel < max_output_channels; ++channel) {
      std::copy_n(output[channel], frames, block->outputs[channel]);
    }
  }
  return AE_PROCESS_OK;
}

}

AudioSession::AudioSession() : callback_(std::make_unique<CallbackState>()) {
  ae_context_options options{};
  options.struct_size = sizeof(options);
  options.api_version_major = AE_API_VERSION_MAJOR;
  options.api_version_minor = AE_API_VERSION_MINOR;
  const ae_result result = ae_context_create(&options, &context_);
  if (result != AE_OK) {
    context_ = nullptr;
    throw AudioError(result, "ae_context_create", {});
  }
}

AudioSession::~AudioSession() {
  const CloseResult result = close();
  if (result == CloseResult::stop_failed) {
    // Deliberate leak: the upstream callback may still dereference this state and run module
    // code, and the context remains referenced by the unclosed stream.
    static_cast<void>(callback_.release());
    return;
  }
  if (ae_context_destroy(context_) != AE_OK) {
    // The context still owns an unclosed stream; leaking it is the only safe outcome.
    context_ = nullptr;
  }
}

std::vector<AudioDevice> AudioSession::devices() const {
  DeviceSnapshot snapshot;
  const ae_result result = ae_context_enumerate_devices(context_, &snapshot.handle);
  if (result != AE_OK) {
    fail(result, "ae_context_enumerate_devices");
  }
  const uint32_t count = ae_device_snapshot_count(snapshot.handle);
  std::vector<AudioDevice> devices;
  devices.reserve(count);
  for (uint32_t index = 0; index < count; ++index) {
    ae_device_info info{};
    info.struct_size = sizeof(info);
    const ae_result get = ae_device_snapshot_get(snapshot.handle, index, &info);
    if (get != AE_OK) {
      fail(get, "ae_device_snapshot_get");
    }
    AudioDevice device;
    device.backend_id = copy_text(info.backend_id);
    device.device_id = copy_text(info.device_id);
    device.display_name = copy_text(info.display_name);
    device.input_channels = info.input_channels;
    device.output_channels = info.output_channels;
    device.channel_counts_known = info.channel_counts_known != 0;
    device.pairable = (info.flags & AE_DEVICE_PAIRABLE) != 0;
    devices.push_back(std::move(device));
  }
  return devices;
}

void AudioSession::refresh_devices() {
  require_state(SessionState::stopped, "refresh_devices");
  const ae_result result = ae_context_refresh_devices(context_);
  if (result != AE_OK) {
    fail(result, "ae_context_refresh_devices");
  }
}

DeviceCapabilities AudioSession::probe(const AudioDevice &device) {
  require_state(SessionState::stopped, "probe");
  CapabilitySnapshot snapshot;
  const ae_result result = ae_context_probe_device(context_, device.backend_id.c_str(),
                                                   device.device_id.c_str(), &snapshot.handle);
  if (result != AE_OK) {
    fail(result, "ae_context_probe_device");
  }
  ae_device_capabilities info{};
  info.struct_size = sizeof(info);
  const ae_result get = ae_capability_snapshot_get(snapshot.handle, &info);
  if (get != AE_OK) {
    fail(get, "ae_capability_snapshot_get");
  }

  DeviceCapabilities capabilities;
  capabilities.input_channels = info.input_channels;
  capabilities.output_channels = info.output_channels;
  capabilities.current_sample_rate = info.current_sample_rate;
  capabilities.min_frames = info.min_frames;
  capabilities.max_frames = info.max_frames;
  capabilities.preferred_frames = info.preferred_frames;
  capabilities.frame_granularity = info.frame_granularity;
  if (info.tested_rates != nullptr) {
    capabilities.tested_rates.assign(info.tested_rates, info.tested_rates + info.tested_rate_count);
  }

  const auto copy_channels = [&](ae_direction direction, uint32_t count,
                                 std::vector<AudioChannel> &channels) {
    channels.reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
      ae_channel_info channel{};
      channel.struct_size = sizeof(channel);
      const ae_result status =
          ae_capability_snapshot_get_channel(snapshot.handle, direction, index, &channel);
      if (status == AE_ERROR_INVALID_ARGUMENT) {
        break;
      }
      if (status != AE_OK) {
        fail(status, "ae_capability_snapshot_get_channel");
      }
      channels.push_back(
          {channel.index, copy_name(channel.name), channel.native_format, channel.supported != 0});
    }
  };
  copy_channels(AE_DIRECTION_INPUT, info.input_channels, capabilities.inputs);
  copy_channels(AE_DIRECTION_OUTPUT, info.output_channels, capabilities.outputs);
  return capabilities;
}

Graph &AudioSession::graph() {
  require_state(SessionState::stopped, "graph");
  return *callback_->slots[active_slot_].graph;
}

std::vector<NodeMetadata> AudioSession::nodes() const {
  return callback_->slots[active_slot_].graph->nodes();
}

bool AudioSession::wait_for_module_work(std::chrono::steady_clock::time_point deadline) const {
  return callback_->slots[active_slot_].graph->wait_for_work(deadline);
}

std::vector<ModuleNotice> AudioSession::take_module_notices() {
  return callback_->slots[active_slot_].graph->take_notices();
}

std::optional<ModuleStatus> AudioSession::module_status(NodeId instance_id) const {
  return callback_->slots[active_slot_].graph->module_status(instance_id);
}

Preset AudioSession::capture_preset() const {
  if (state_ != SessionState::stopped && state_ != SessionState::prepared) {
    throw std::logic_error("capture_preset requires a stopped or prepared session");
  }
  return callback_->slots[active_slot_].graph->capture_preset();
}

void AudioSession::restore_preset(const Preset &preset, const ModuleCatalog &catalog,
                                  const StreamConfig &config,
                                  std::shared_ptr<HostServices> services) {
  require_state(SessionState::stopped, "restore_preset");
  auto replacement = Graph::restore_preset(preset, catalog, std::move(services));
  prepare_graph(config, std::move(replacement));
}

float AudioSession::parameter_value(NodeId instance_id, uint32_t parameter_id) const {
  if (state_ != SessionState::stopped && state_ != SessionState::prepared) {
    throw std::logic_error("parameter_value requires a stopped or prepared session");
  }
  return callback_->slots[active_slot_].graph->parameter_value(instance_id, parameter_id);
}

void AudioSession::set_parameter_value(NodeId instance_id, uint32_t parameter_id, float value) {
  if (state_ != SessionState::stopped && state_ != SessionState::prepared) {
    throw std::logic_error("set_parameter_value requires a stopped or prepared session");
  }
  callback_->slots[active_slot_].graph->set_parameter_value(instance_id, parameter_id, value);
}

void AudioSession::set_core_io_settings(const CoreIoSettings &settings) {
  if (state_ != SessionState::prepared && state_ != SessionState::running) {
    throw std::logic_error("set_core_io_settings requires a prepared or running session");
  }
  if (!validate_core_io_settings(settings) ||
      (settings.transpose_semitones != 0.0f &&
       callback_->core_io.transpose_latency() == TransposeLatency::none)) {
    throw std::invalid_argument("core IO settings are invalid for this prepared stream");
  }
  CallbackState &callback = *callback_;
  if (state_ == SessionState::prepared) {
    if (!callback.core_io.set_settings(device_core_io(settings, callback.output_channels))) {
      throw std::invalid_argument("core IO settings were rejected");
    }
    callback.settings_stage.store(CallbackState::SettingsStage::idle, std::memory_order_release);
  } else {
    if (callback.settings_stage.load(std::memory_order_acquire) !=
        CallbackState::SettingsStage::idle) {
      throw std::logic_error("a core IO settings update is already queued");
    }
    callback.pending_settings = device_core_io(settings, callback.output_channels);
    callback.settings_stage.store(CallbackState::SettingsStage::queued, std::memory_order_release);
  }
  core_io_settings_ = settings;
}

void AudioSession::set_tempo_settings(const TempoSettings &settings) {
  if (!validate_tempo_settings(settings)) {
    throw std::invalid_argument("invalid tempo settings");
  }
  if (state_ != SessionState::prepared && state_ != SessionState::running) {
    throw std::logic_error("tempo updates require a prepared or running session");
  }
  if (state_ == SessionState::prepared) {
    callback_->tempo.set_settings(settings);
    callback_->tempo_stage.store(CallbackState::SettingsStage::idle, std::memory_order_release);
  } else {
    if (callback_->tempo_stage.load(std::memory_order_acquire) !=
        CallbackState::SettingsStage::idle) {
      throw std::logic_error("a tempo update is already queued");
    }
    callback_->pending_tempo = settings;
    callback_->tempo_stage.store(CallbackState::SettingsStage::queued, std::memory_order_release);
  }
}

void AudioSession::reset_tempo(const TempoSettings &settings, double beat) {
  require_state(SessionState::prepared, "reset_tempo");
  callback_->tempo.prepare(info_.sample_rate, settings, beat);
  callback_->beat_position.store(beat, std::memory_order_relaxed);
  callback_->tempo_stage.store(CallbackState::SettingsStage::idle, std::memory_order_release);
}

double AudioSession::beat_position() const noexcept {
  return callback_->beat_position.load(std::memory_order_relaxed);
}

MidiQueue &AudioSession::midi_input() noexcept { return callback_->midi; }

ParameterToken AudioSession::parameter_token() const noexcept {
  if (state_ != SessionState::running || replacement_in_flight()) {
    return {};
  }
  return callback_->parameters.token();
}

ParameterResult AudioSession::set_parameter(ParameterToken token, NodeId instance_id,
                                            uint32_t parameter_id, float value,
                                            std::optional<uint64_t> frame_position) noexcept {
  ParameterQueue &queue = callback_->parameters;
  const auto reject = [&](ParameterResult result) {
    queue.count_rejected();
    return result;
  };
  if (state_ != SessionState::running) {
    return reject(ParameterResult::not_running);
  }
  if (replacement_in_flight() || token.graph_generation != graph_generation_ ||
      token != queue.token()) {
    return reject(ParameterResult::stale_token);
  }
  const auto node =
      std::find_if(prepared_nodes_.begin(), prepared_nodes_.end(),
                   [&](const NodeMetadata &n) { return n.instance_id == instance_id; });
  if (node == prepared_nodes_.end()) {
    return reject(ParameterResult::unknown_node);
  }
  const auto &parameters = node->module.parameters;
  const auto parameter =
      std::find_if(parameters.begin(), parameters.end(),
                   [&](const ParameterMetadata &p) { return p.id == parameter_id; });
  if (parameter == parameters.end()) {
    return reject(ParameterResult::unknown_parameter);
  }
  if (!std::isfinite(value) || value < parameter->minimum || value > parameter->maximum) {
    return reject(ParameterResult::invalid_value);
  }
  switch (queue.push(token, instance_id, parameter_id, value, frame_position)) {
  case ParameterQueue::PushResult::accepted:
    return ParameterResult::accepted;
  case ParameterQueue::PushResult::stale_token:
    return ParameterResult::stale_token;
  case ParameterQueue::PushResult::queue_full:
    return ParameterResult::queue_full;
  }
  return ParameterResult::stale_token;
}

void AudioSession::request_graph_replacement(std::unique_ptr<Graph> candidate,
                                             uint32_t transition_frames) {
  require_state(SessionState::running, "request_graph_replacement");
  if (replacement_in_flight()) {
    throw std::logic_error("request_graph_replacement: a graph replacement is already in flight");
  }
  if (!candidate) {
    throw std::invalid_argument("request_graph_replacement requires a graph");
  }
  if (transition_frames == 0 || transition_frames > max_transition_frames()) {
    throw std::invalid_argument("request_graph_replacement: transition_frames must be within [1, " +
                                std::to_string(max_transition_frames()) + "]");
  }

  CallbackState &state = *callback_;
  const uint32_t incoming_slot = 1 - active_slot_;
  CallbackState::Slot &slot = state.slots[incoming_slot];
  if (slot.graph || slot.adapter) {
    throw std::logic_error("request_graph_replacement: replacement slot is still occupied");
  }
  candidate->prepare(Format{static_cast<double>(info_.sample_rate), info_.max_frames_per_block,
                            info_.graph_channels});
  std::optional<MonoStereoAdapter> adapter;
  std::vector<NodeMetadata> metadata;
  try {
    if (candidate->latency_frames() != info_.graph_latency_frames) {
      throw std::invalid_argument("request_graph_replacement: replacement latency of " +
                                  std::to_string(candidate->latency_frames()) +
                                  " frames differs from the active " +
                                  std::to_string(info_.graph_latency_frames) + " frames");
    }
    metadata = candidate->nodes();
    adapter.emplace(*candidate, info_.max_frames_per_block, info_.graph_channels);
  } catch (...) {
    try {
      candidate->release();
    } catch (...) {
    }
    throw;
  }

  const uint64_t generation = candidate->generation();
  slot.graph = std::move(candidate);
  slot.adapter.emplace(std::move(*adapter));
  state.transition_frames = transition_frames;
  state.incoming_generation = generation;
  state.faded_frames.store(0, std::memory_order_relaxed);
  incoming_generation_ = generation;
  incoming_nodes_.swap(metadata);
  transition_frames_ = transition_frames;
  state.stage.store(CallbackState::ReplacementStage::queued, std::memory_order_release);
}

AudioSession::StagedGraph AudioSession::stage_graph(std::unique_ptr<Graph> candidate) const {
  if (state_ != SessionState::prepared && state_ != SessionState::running) {
    throw std::logic_error("stage_graph requires a prepared or running session");
  }
  if (!candidate || candidate->state() != GraphState::prepared) {
    throw std::invalid_argument("stage_graph requires a prepared graph");
  }
  const Format format = candidate->prepared_format();
  if (format.sample_rate != static_cast<double>(info_.sample_rate) ||
      format.max_frames != info_.max_frames_per_block || format.channels != info_.graph_channels) {
    throw std::invalid_argument("stage_graph: the candidate was prepared for a different stream "
                                "format");
  }
  StagedGraph staged;
  staged.adapter_.emplace(*candidate, info_.max_frames_per_block, info_.graph_channels);
  staged.metadata_ = candidate->nodes();
  staged.graph_ = std::move(candidate);
  return staged;
}

AudioSession::StagedGraph AudioSession::swap_staged_graph(StagedGraph &&next) {
  require_state(SessionState::prepared, "swap_staged_graph");
  if (replacement_in_flight()) {
    throw std::logic_error("swap_staged_graph: a graph replacement is still in flight");
  }
  if (!next.graph_ || !next.adapter_) {
    throw std::logic_error("swap_staged_graph requires a staged graph");
  }
  static_assert(std::is_nothrow_move_constructible_v<MonoStereoAdapter>);
  CallbackState &state = *callback_;
  CallbackState::Slot &slot = state.slots[active_slot_];
  StagedGraph previous;
  if (slot.adapter) {
    previous.adapter_.emplace(std::move(*slot.adapter));
    slot.adapter.reset();
  }
  previous.graph_ = std::move(slot.graph);
  previous.metadata_.swap(prepared_nodes_);
  slot.graph = std::move(next.graph_);
  slot.adapter.emplace(std::move(*next.adapter_));
  next.adapter_.reset();
  prepared_nodes_.swap(next.metadata_);
  graph_generation_ = slot.graph->generation();
  info_.graph_latency_frames = slot.graph->latency_frames();
  state.parameters.clear(graph_generation_);
  state.midi.reset();
  return previous;
}

GraphTransitionState AudioSession::graph_transition_state() const noexcept {
  switch (callback_->stage.load(std::memory_order_acquire)) {
  case CallbackState::ReplacementStage::idle:
    return GraphTransitionState::idle;
  case CallbackState::ReplacementStage::queued:
    return GraphTransitionState::queued;
  case CallbackState::ReplacementStage::crossfading:
    return GraphTransitionState::crossfading;
  case CallbackState::ReplacementStage::awaiting_retirement:
    return GraphTransitionState::awaiting_retirement;
  }
  return GraphTransitionState::idle;
}

bool AudioSession::replacement_in_flight() const noexcept {
  return callback_->stage.load(std::memory_order_acquire) != CallbackState::ReplacementStage::idle;
}

void AudioSession::retire_replacement() noexcept {
  CallbackState &state = *callback_;
  const uint32_t outgoing = active_slot_;
  active_slot_ = 1 - outgoing;
  state.slots[outgoing].destroy();
  prepared_nodes_.swap(incoming_nodes_);
  incoming_nodes_.clear();
  graph_generation_ = incoming_generation_;
  incoming_generation_ = 0;
  transition_frames_ = 0;
  ++committed_transitions_;
  state.stage.store(CallbackState::ReplacementStage::idle, std::memory_order_release);
}

void AudioSession::settle_replacement() noexcept {
  CallbackState &state = *callback_;
  switch (state.stage.load(std::memory_order_acquire)) {
  case CallbackState::ReplacementStage::idle:
    return;
  case CallbackState::ReplacementStage::queued:
    state.slots[1 - active_slot_].destroy();
    incoming_nodes_.clear();
    incoming_generation_ = 0;
    transition_frames_ = 0;
    ++cancelled_transitions_;
    state.stage.store(CallbackState::ReplacementStage::idle, std::memory_order_relaxed);
    return;
  case CallbackState::ReplacementStage::crossfading:
    state.active = 1 - active_slot_;
    [[fallthrough]];
  case CallbackState::ReplacementStage::awaiting_retirement:
    retire_replacement();
    return;
  }
}

void AudioSession::prepare(const StreamConfig &config) { prepare_graph(config, nullptr); }

void AudioSession::prepare_graph(const StreamConfig &config, std::unique_ptr<Graph> replacement) {
  require_state(SessionState::stopped, "prepare");
  if (config.backend_id.empty() || config.device_id.empty()) {
    throw std::invalid_argument("prepare requires a backend and device identifier");
  }
  if (config.output_channel_count < 1 || config.output_channel_count > 2) {
    throw std::invalid_argument("prepare requires one or two output channels");
  }
  if (config.output_channel_count == 2 && config.output_channels[0] == config.output_channels[1]) {
    throw std::invalid_argument("prepare requires two distinct output channels");
  }
  if (config.sample_rate == 0 || config.frames_per_block == 0) {
    throw std::invalid_argument("prepare requires an explicit sample rate and block size");
  }
  if (!valid_graph_channels(config.graph_channels)) {
    throw std::invalid_argument("prepare requires a mono or stereo graph");
  }
  if (!validate_core_io_settings(config.core_io)) {
    throw std::invalid_argument("prepare requires valid core IO settings");
  }
  switch (config.input_mode) {
  case InputMode::mono:
    if (config.second_input_channel) {
      throw std::invalid_argument("prepare: mono input mode does not take a second input channel");
    }
    break;
  case InputMode::stereo:
    if (!config.second_input_channel) {
      throw std::invalid_argument("prepare: stereo input mode requires a second input channel");
    }
    if (*config.second_input_channel == config.input_channel) {
      throw std::invalid_argument("prepare requires two distinct input channels");
    }
    if (config.graph_channels != 2) {
      throw std::invalid_argument("prepare: stereo input mode requires a stereo graph");
    }
    break;
  default:
    throw std::invalid_argument("prepare requires a mono or stereo input mode");
  }

  const uint32_t input_count = input_channel_count(config.input_mode);
  const std::array<uint32_t, 2> inputs{config.input_channel,
                                       config.second_input_channel.value_or(0)};
  const std::array<uint32_t, 2> outputs{
      config.output_channels[0], config.output_channel_count == 2 ? config.output_channels[1] : 0};
  ae_stream_request request{};
  request.struct_size = sizeof(request);
  request.backend_id = config.backend_id.c_str();
  request.input_device_id = config.device_id.c_str();
  request.output_device_id =
      config.output_device_id.empty() ? config.device_id.c_str() : config.output_device_id.c_str();
  request.input_channels = {AE_CHANNEL_MODE_EXPLICIT, input_count, inputs.data()};
  request.output_channels = {AE_CHANNEL_MODE_EXPLICIT, config.output_channel_count, outputs.data()};
  request.sample_rate = config.sample_rate;
  request.frames_per_block = config.frames_per_block;
  request.negotiation =
      config.allow_adjustment ? AE_NEGOTIATION_ALLOW_ADJUSTMENT : AE_NEGOTIATION_EXACT;

  ae_stream_format format{};
  format.struct_size = sizeof(format);
  ae_stream *stream = nullptr;
  const ae_result opened = ae_stream_open(context_, &request, &stream, &format);
  if (opened != AE_OK) {
    fail(opened, "ae_stream_open");
  }
  stream_ = stream;
  state_ = SessionState::prepared;

  try {
    if (format.input_channels != input_count ||
        format.output_channels != config.output_channel_count || format.frames_per_block == 0 ||
        format.max_frames_per_block < format.frames_per_block) {
      throw AudioError(AE_ERROR_UNSUPPORTED_CONFIGURATION, "ae_stream_open",
                       "negotiated channel layout or block size is not usable");
    }
    StreamInfo info;
    info.sample_rate = format.sample_rate;
    info.frames_per_block = format.frames_per_block;
    info.max_frames_per_block = format.max_frames_per_block;
    info.input_mode = config.input_mode;
    info.input_channels = input_count;
    info.output_channels = config.output_channel_count;
    info.stream_id = format.stream_id;
    info.configuration_generation = format.configuration_generation;
    info.output_mode = config.output_mode;
    info.graph_channels = config.graph_channels;
    ae_result mapped = AE_OK;
    for (uint32_t channel = 0; mapped == AE_OK && channel < input_count; ++channel) {
      mapped = ae_stream_get_channel(stream_, AE_DIRECTION_INPUT, channel,
                                     &info.physical_inputs[channel]);
    }
    for (uint32_t channel = 0; mapped == AE_OK && channel < config.output_channel_count;
         ++channel) {
      mapped = ae_stream_get_channel(stream_, AE_DIRECTION_OUTPUT, channel,
                                     &info.physical_outputs[channel]);
    }
    if (mapped != AE_OK) {
      fail(mapped, "ae_stream_get_channel");
    }
    info.physical_input = info.physical_inputs[0];
    if (info.physical_inputs != inputs || info.physical_outputs != outputs) {
      throw AudioError(AE_ERROR_UNSUPPORTED_CONFIGURATION, "ae_stream_get_channel",
                       "stream channel mapping differs from the explicit request");
    }

    CallbackState &state = *callback_;
    state.core_io.prepare(static_cast<double>(format.sample_rate), format.max_frames_per_block,
                          config.core_io.transpose_semitones == 0.0f ? TransposeLatency::none
                                                                     : TransposeLatency::fixed,
                          device_core_io(config.core_io, config.output_channel_count));
    state.tempo.prepare(format.sample_rate, config.tempo, config.initial_beat);
    state.midi_sample_rate = format.sample_rate;
    state.beat_position.store(config.initial_beat, std::memory_order_relaxed);
    state.clicks.resize(format.max_frames_per_block);
    state.tempo_stage.store(CallbackState::SettingsStage::idle, std::memory_order_relaxed);
    info.core_io_latency_frames = state.core_io.input_latency_frames();
    std::vector<float> conditioned_input(static_cast<size_t>(format.max_frames_per_block) *
                                         info.input_channels);
    std::vector<float> conditioned_output(static_cast<size_t>(format.max_frames_per_block) *
                                          max_output_channels);
    CallbackState::Slot &slot = state.slots[active_slot_];
    Graph &graph = replacement ? *replacement : *slot.graph;
    graph.prepare(Format{static_cast<double>(format.sample_rate), format.max_frames_per_block,
                         config.graph_channels});
    MonoStereoAdapter adapter(graph, format.max_frames_per_block, config.graph_channels);
    std::vector<float> mix(static_cast<size_t>(format.max_frames_per_block) * config.graph_channels,
                           0.0f);
    auto metadata = graph.nodes();
    info.graph_latency_frames = graph.latency_frames();
    static_assert(std::is_nothrow_move_constructible_v<MonoStereoAdapter>);
    if (replacement) {
      slot.graph.swap(replacement);
    }
    slot.adapter.emplace(std::move(adapter));
    state.mix.swap(mix);
    state.conditioned_input.swap(conditioned_input);
    state.conditioned_output.swap(conditioned_output);
    state.settings_stage.store(CallbackState::SettingsStage::idle, std::memory_order_relaxed);
    state.graph_channels = config.graph_channels;
    state.input_channels = input_count;
    state.output_channels = config.output_channel_count;
    state.active = active_slot_;
    state.output_mode = config.output_mode;
    state.stream_id = format.stream_id;
    state.configuration_generation = format.configuration_generation;
    prepared_nodes_.swap(metadata);
    graph_generation_ = slot.graph->generation();
    core_io_settings_ = config.core_io;
    state.reset_run(graph_generation_);
    info_ = info;
  } catch (...) {
    static_cast<void>(close());
    throw;
  }
}

void AudioSession::start() {
  require_state(SessionState::prepared, "start");
  callback_->reset_run(graph_generation_);
  const ae_result result = ae_stream_start(stream_, &process_stream, callback_.get());
  if (result == AE_ERROR_QUIESCENCE_FAILURE) {
    callback_installed_ = true;
    enter_recovery(RecoveryReason::quiescence_failure, result);
    fail(result, "ae_stream_start");
  }
  if (result != AE_OK) {
    last_error_ = result;
    fail(result, "ae_stream_start");
  }
  callback_installed_ = true;
  state_ = SessionState::running;
}

void AudioSession::stop() {
  if (state_ == SessionState::recovery_required) {
    if (!callback_installed_) {
      return;
    }
  } else {
    require_state(SessionState::running, "stop");
  }
  const ae_result result = ae_stream_stop(stream_);
  if (result != AE_OK) {
    enter_recovery(RecoveryReason::quiescence_failure, result);
    fail(result, "ae_stream_stop");
  }
  callback_installed_ = false;
  callback_->take.settle_quiescent();
  if (callback_->settings_stage.load(std::memory_order_acquire) ==
      CallbackState::SettingsStage::queued) {
    static_cast<void>(callback_->core_io.set_settings(callback_->pending_settings));
    callback_->settings_stage.store(CallbackState::SettingsStage::idle, std::memory_order_relaxed);
  }
  settle_replacement();
  callback_->parameters.clear(graph_generation_);
  callback_->midi.reset();
  if (state_ == SessionState::recovery_required) {
    if (recovery_ == RecoveryReason::quiescence_failure) {
      recovery_ = RecoveryReason::stream_fault;
    }
    return;
  }
  ae_stream_status status{};
  status.struct_size = sizeof(status);
  if (ae_stream_get_status(stream_, &status) == AE_OK &&
      status.state == AE_STREAM_STATE_RECOVERY_REQUIRED) {
    enter_recovery(status.last_result == AE_ERROR_PROCESSOR_FAILURE
                       ? RecoveryReason::processing_failure
                       : RecoveryReason::stream_fault,
                   status.last_result);
    return;
  }
  if (callback_->processing_failures.load(std::memory_order_relaxed) != 0 ||
      callback_->contract_violations.load(std::memory_order_relaxed) != 0) {
    enter_recovery(RecoveryReason::processing_failure, AE_ERROR_PROCESSOR_FAILURE);
    return;
  }
  state_ = SessionState::prepared;
}

CloseResult AudioSession::close() noexcept {
  if (stream_ == nullptr) {
    callback_->take.finish();
    release_prepared_resources();
    state_ = SessionState::stopped;
    recovery_ = RecoveryReason::none;
    return CloseResult::closed;
  }
  if (callback_installed_) {
    const ae_result stopped = ae_stream_stop(stream_);
    if (stopped != AE_OK) {
      enter_recovery(RecoveryReason::quiescence_failure, stopped);
      return CloseResult::stop_failed;
    }
    callback_installed_ = false;
  }
  callback_->take.finish();
  release_prepared_resources();
  const ae_result closed = ae_stream_close(stream_);
  if (closed != AE_OK) {
    enter_recovery(RecoveryReason::close_failure, closed);
    return CloseResult::close_failed;
  }
  stream_ = nullptr;
  info_ = {};
  state_ = SessionState::stopped;
  recovery_ = RecoveryReason::none;
  return CloseResult::closed;
}

SessionStatus AudioSession::poll() {
  SessionStatus status;
  if (stream_ != nullptr) {
    ae_stream_status upstream{};
    upstream.struct_size = sizeof(upstream);
    const ae_result result = ae_stream_get_status(stream_, &upstream);
    if (result != AE_OK) {
      fail(result, "ae_stream_get_status");
    }
    status.stream_open = true;
    status.stream.state = to_device_state(upstream.state);
    status.stream.last_result = upstream.last_result;
    status.stream.stream_id = upstream.stream_id;
    status.stream.configuration_generation = upstream.configuration_generation;
    status.stream.run_generation = upstream.run_generation;
    status.stream.delivered_frames = upstream.delivered_frames;
    status.stream.fault_flags = upstream.fault_flags;
    status.stream.latency_valid = upstream.latency_valid != 0;
    status.stream.input_latency_frames = upstream.input_latency_frames;
    status.stream.output_latency_frames = upstream.output_latency_frames;
    status.stream.callback_count = upstream.callback_count;
    status.stream.max_callback_nanoseconds = upstream.max_callback_nanoseconds;
    status.stream.deadline_overruns = upstream.deadline_overruns;
    status.stream.driver_overloads = upstream.driver_overloads;

    if ((state_ == SessionState::prepared || state_ == SessionState::running) &&
        (upstream.state == AE_STREAM_STATE_RECOVERY_REQUIRED || upstream.fault_flags != 0)) {
      enter_recovery(upstream.last_result == AE_ERROR_PROCESSOR_FAILURE
                         ? RecoveryReason::processing_failure
                         : RecoveryReason::stream_fault,
                     upstream.last_result);
    }
  }
  CallbackState &state = *callback_;
  if (state.stage.load(std::memory_order_acquire) ==
      CallbackState::ReplacementStage::awaiting_retirement) {
    retire_replacement();
  }
  status.transition.state = graph_transition_state();
  if (status.transition.state != GraphTransitionState::idle) {
    status.transition.transition_frames = transition_frames_;
    status.transition.faded_frames = state.faded_frames.load(std::memory_order_relaxed);
  }
  status.transition.committed = committed_transitions_;
  status.transition.cancelled = cancelled_transitions_;
  status.telemetry = state.telemetry();
  status.state = state_;
  status.recovery = recovery_;
  status.last_error = last_error_;
  return status;
}

MeterReading AudioSession::read_meters() noexcept {
  CallbackState &state = *callback_;
  using Totals = CallbackState::MeterTotals;
  MeterReading reading;
  reading.input_channels = info_.input_channels;
  std::array<float, meter_channels> peaks{};
  for (uint32_t channel = 0; channel < meter_channels; ++channel) {
    peaks[channel] = state.meter_peaks[channel].exchange(0.0f, std::memory_order_relaxed);
  }
  const std::optional<Totals> totals = state.published_meters();
  Totals interval{};
  if (totals) {
    for (size_t i = 0; i < CallbackState::meter_total_count; ++i) {
      interval[i] = (*totals)[i] - state.meter_baseline[i];
    }
    state.meter_baseline = *totals;
  }
  reading.input_frames = interval[0];
  reading.output_frames = interval[1];
  const auto fill = [&](ChannelMeter &meter, uint32_t channel, uint64_t frames) {
    meter.peak = peaks[channel];
    meter.clipped_samples = interval[CallbackState::clipped_base + channel];
    if (frames != 0) {
      const double energy =
          static_cast<double>(interval[CallbackState::energy_base + channel]) / meter_energy_scale;
      meter.rms = static_cast<float>(std::sqrt(energy / static_cast<double>(frames)));
    }
  };
  for (uint32_t channel = 0; channel < max_output_channels; ++channel) {
    fill(reading.inputs[channel], channel, reading.input_frames);
    fill(reading.outputs[channel], output_meter_base + channel, reading.output_frames);
  }
  return reading;
}

void AudioSession::start_take(const TakeOptions &options) {
  require_state(SessionState::running, "start_take");
  callback_->take.start(options, TakeFormat{info_.sample_rate, info_.input_channels,
                                            info_.frames_per_block, info_.max_frames_per_block});
}

void AudioSession::stop_take() noexcept {
  callback_->take.stop();
  if (!callback_installed_) {
    callback_->take.settle_quiescent();
  }
}

TakeStatus AudioSession::take_status() const { return callback_->take.status(); }

const StreamInfo &AudioSession::stream_info() const {
  if (state_ != SessionState::prepared && state_ != SessionState::running) {
    throw std::logic_error("stream_info requires a prepared or running session");
  }
  return info_;
}

void AudioSession::require_state(SessionState expected, const char *operation) const {
  if (state_ != expected) {
    throw std::logic_error(std::string(operation) + " requires the " + to_string(expected) +
                           " state but the session is " + to_string(state_));
  }
}

void AudioSession::enter_recovery(RecoveryReason reason, int32_t code) noexcept {
  callback_->take.stop();
  if (!callback_installed_) {
    callback_->take.settle_quiescent();
  }
  state_ = SessionState::recovery_required;
  if (recovery_ != RecoveryReason::quiescence_failure || !callback_installed_) {
    recovery_ = reason;
  }
  last_error_ = code;
}

void AudioSession::fail(int32_t code, const char *operation) const {
  throw AudioError(code, operation, diagnostic_detail(context_));
}

void AudioSession::release_prepared_resources() noexcept {
  settle_replacement();
  CallbackState &state = *callback_;
  state.parameters.clear(0);
  state.midi.reset();
  state.settings_stage.store(CallbackState::SettingsStage::idle, std::memory_order_relaxed);
  state.conditioned_input.clear();
  state.conditioned_output.clear();
  graph_generation_ = 0;
  prepared_nodes_.clear();
  CallbackState::Slot &slot = state.slots[active_slot_];
  slot.adapter.reset();
  try {
    slot.graph->release();
  } catch (...) {
  }
}

}
