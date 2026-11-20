#ifndef VESPERA_CORE_IO_HPP
#define VESPERA_CORE_IO_HPP

#include <cstdint>
#include <vector>

namespace vespera {

inline constexpr float core_io_min_gain_db = -60.0f;
inline constexpr float core_io_max_gain_db = 24.0f;
inline constexpr float core_io_min_gate_threshold_db = -120.0f;
inline constexpr float core_io_max_gate_threshold_db = 0.0f;
inline constexpr float core_io_max_transpose_semitones = 12.0f;
inline constexpr float core_io_min_doubler_delay_ms = 1.0f;
inline constexpr float core_io_max_doubler_delay_ms = 40.0f;

// Host-side input/output conditioning applied around the graph. The defaults are transparent:
// unity gains, gate and doubler off, no transposition.
struct CoreIoSettings {
  float input_gain_db = 0.0f;
  float output_gain_db = 0.0f;
  bool gate_enabled = false;
  float gate_threshold_db = -80.0f;
  float transpose_semitones = 0.0f;
  bool doubler_enabled = false;
  float doubler_delay_ms = 6.0f;

  friend bool operator==(const CoreIoSettings &, const CoreIoSettings &) = default;
};

// Every field must be finite and within the core_io_* bounds; transposition is within
// [-core_io_max_transpose_semitones, core_io_max_transpose_semitones].
[[nodiscard]] bool validate_core_io_settings(const CoreIoSettings &settings) noexcept;

// Chosen at prepare() and constant until the next prepare(), so the input path latency never
// depends on settings. `none` (the default) gives a zero-latency, bit-transparent input path and
// rejects any non-zero transposition. `fixed` reserves the transposition latency permanently:
// the input path is delayed by input_latency_frames() even at zero transposition (an exact,
// uncolored integer delay), so transposition can be engaged and released while running without
// shifting timing. Enabling transposition under `none` requires preparing again with `fixed`.
enum class TransposeLatency { none, fixed };

// Input path, before the graph: input gain, per-channel noise gate, pitch transposition.
// Output path, after the graph: stereo doubler (and mono-to-stereo duplication), output gain.
//
// Gains ramp linearly over 20 ms. The gate opens when a peak envelope (30 ms decay) reaches the
// threshold and closes once it stays 6 dB below for a 50 ms hold, with 0.5 ms attack and 20 ms
// release time constants; it starts open after reset. Transposition is a duration-preserving
// two-tap delay-line shifter with 20 ms grains whose splice points are aligned by normalized
// cross-correlation over a 10 ms search range; engaging, releasing and retuning crossfade or
// glide over 20-30 ms. The doubler crossfades delay changes over 20 ms.
//
// Channels are independent: a stereo stream yields, per channel, exactly what a mono stream with
// that channel's signal would.
//
// Threading: prepare(), set_settings() and reset() must not run concurrently with processing; they
// may run on the control thread while the stream is quiescent or on the audio thread between
// blocks. Only prepare() allocates. Processing is allocation-free, lock-free and noexcept.
class CoreIoProcessor {
public:
  static constexpr uint32_t max_channels = 2;
  static constexpr uint32_t max_frame_capacity = 65536;

  CoreIoProcessor() = default;
  CoreIoProcessor(const CoreIoProcessor &) = delete;
  CoreIoProcessor &operator=(const CoreIoProcessor &) = delete;

  // Throws std::invalid_argument for a sample rate outside [8000, 768000] Hz, a capacity outside
  // [1, max_frame_capacity], or `none` latency while the current settings transpose; the
  // processor is unchanged on failure. Keeps the current settings and resets all signal state.
  void prepare(double sample_rate, uint32_t max_frames,
               TransposeLatency latency = TransposeLatency::none);

  // Prepares and replaces the settings together, so a re-prepare can change the latency policy and
  // transposition in one step regardless of the previous policy. Throws std::invalid_argument for
  // the conditions above evaluated against `settings`, or for settings that fail
  // validate_core_io_settings(); all checks and allocations happen before any state changes, so
  // the processor, including its settings and signal state, is unchanged on failure.
  void prepare(double sample_rate, uint32_t max_frames, TransposeLatency latency,
               const CoreIoSettings &settings);

  // Returns false and changes nothing when the settings are invalid, or transpose while prepared
  // with TransposeLatency::none. Accepted settings ramp in from the current state.
  [[nodiscard]] bool set_settings(const CoreIoSettings &settings) noexcept;

  // Clears delay memory, envelopes and ramps, jumping straight to the current settings. Call on
  // any stream discontinuity. Changing the channel layout between calls resets the affected path.
  void reset() noexcept;

  // Planar, out-of-place; outputs[c] may also alias inputs[c]. `channels` is 1 or 2 and
  // `frames` at most the prepared capacity. Non-finite input samples are processed as zero.
  // Returns false when unprepared or arguments are invalid, writing silence to the outputs when
  // they are addressable.
  bool process_input(const float *const *inputs, float *const *outputs, uint32_t channels,
                     uint32_t frames) noexcept;

  // Planar, out-of-place; outputs[c] may also alias inputs[c]. Supported layouts are 1->1, 1->2
  // and 2->2. A mono input to stereo output is duplicated; with the doubler enabled the right
  // channel is the delayed signal, and for stereo input the right channel is delayed while the
  // left stays dry. The doubler has no effect on mono output.
  bool process_output(const float *const *inputs, uint32_t input_channels, float *const *outputs,
                      uint32_t output_channels, uint32_t frames) noexcept;

  [[nodiscard]] bool prepared() const noexcept { return sample_rate_ != 0.0; }
  [[nodiscard]] double sample_rate() const noexcept { return sample_rate_; }
  [[nodiscard]] uint32_t max_frames() const noexcept { return max_frames_; }
  [[nodiscard]] TransposeLatency transpose_latency() const noexcept { return latency_mode_; }
  // Nominal delay of process_input() in frames, fixed by the prepared rate and latency policy and
  // independent of settings: 0 for `none`; for `fixed`,
  // 2 + round(0.01 * sample_rate) + round(0.005 * sample_rate) (722 at 48 kHz, 664 at 44.1 kHz).
  // At zero transposition the input path is exactly this integer delay. While transposing it is
  // the centre of the grain delay range rather than an exact group delay: the shifter reads two
  // crossfaded taps whose delays sweep continuously through [2, 2 + 2 * (latency - 2)] and are
  // re-spliced at correlation-chosen points, so the delay seen by any given transient varies
  // within that range and a transposed impulse does not peak at this latency.
  // process_output() adds no latency.
  [[nodiscard]] uint32_t input_latency_frames() const noexcept { return pitch_latency_; }
  [[nodiscard]] const CoreIoSettings &settings() const noexcept { return settings_; }

  // Upper bound, at every supported sample rate, on the correlation terms evaluated when one tap
  // of one channel is re-spliced. Splices of a channel are at least 0.01 s of input apart, apart
  // from one extra splice when transposition engages or a glide crosses unison, so the search
  // adds bounded work per callback regardless of the sample rate.
  static constexpr uint32_t max_splice_search_terms = 24576;
  // Diagnostic: the most correlation terms a single channel's splice has evaluated since prepare().
  [[nodiscard]] uint32_t peak_splice_search_terms() const noexcept { return peak_splice_terms_; }

private:
  class Ramp {
  public:
    void configure(uint32_t frames) noexcept { frames_ = frames == 0 ? 1 : frames; }
    void snap(double value) noexcept {
      current_ = target_ = value;
      remaining_ = 0;
    }
    void set_target(double value) noexcept;
    double next() noexcept;
    [[nodiscard]] double current() const noexcept { return current_; }
    [[nodiscard]] double target() const noexcept { return target_; }
    [[nodiscard]] bool active() const noexcept { return remaining_ != 0; }

  private:
    double current_ = 0.0;
    double target_ = 0.0;
    double step_ = 0.0;
    uint32_t frames_ = 1;
    uint32_t remaining_ = 0;
  };

  enum class GateState { open, closed };
  // Whether transposition currently shortens (pitch up) or lengthens (pitch down) tap delays.
  enum class PitchDirection { shortening, lengthening };

  struct GateChannel {
    GateState state = GateState::open;
    float envelope = 0.0f;
    float gain = 1.0f;
    uint32_t hold_remaining = 0;
  };

  void apply_targets(bool snap) noexcept;
  void reset_input() noexcept;
  void reset_output() noexcept;
  float gate(GateChannel &channel, float sample) noexcept;
  float read_pitch(uint32_t channel, double delay) const noexcept;
  static PitchDirection direction_for(double ratio) noexcept;
  void engage_pitch() noexcept;
  void splice_tap(uint32_t channels, uint32_t tap) noexcept;
  double splice_score(const float *ring, uint32_t reference, uint32_t candidate,
                      uint32_t step) noexcept;
  static void silence(float *const *outputs, uint32_t channels, uint32_t frames) noexcept;

  CoreIoSettings settings_{};
  double sample_rate_ = 0.0;
  uint32_t max_frames_ = 0;
  TransposeLatency latency_mode_ = TransposeLatency::none;

  Ramp input_gain_;
  Ramp output_gain_;

  float gate_open_level_ = 0.0f;
  float gate_close_level_ = 0.0f;
  float envelope_release_ = 0.0f;
  float gate_attack_ = 0.0f;
  float gate_release_ = 0.0f;
  uint32_t gate_hold_frames_ = 0;
  GateChannel gates_[max_channels]{};

  std::vector<float> pitch_buffer_;
  uint32_t pitch_mask_ = 0;
  uint32_t pitch_write_ = 0;
  uint32_t pitch_window_ = 0;
  uint32_t pitch_search_ = 0;
  uint32_t pitch_correlation_ = 0;
  uint32_t pitch_search_stride_ = 1;
  uint32_t pitch_refine_term_step_ = 1;
  uint32_t splice_terms_ = 0;
  uint32_t peak_splice_terms_ = 0;
  uint32_t pitch_latency_ = 0;
  double pitch_phase_ = 0.5;
  double pitch_ratio_ = 1.0;
  PitchDirection pitch_direction_ = PitchDirection::shortening;
  double pitch_delays_[max_channels][2]{};
  bool pitch_splice_pending_[2]{};
  Ramp semitones_;
  Ramp pitch_mix_;
  uint32_t input_channels_ = 0;

  std::vector<float> doubler_buffer_;
  uint32_t doubler_mask_ = 0;
  uint32_t doubler_write_ = 0;
  uint32_t doubler_active_delay_ = 0;
  uint32_t doubler_next_delay_ = 0;
  uint32_t doubler_target_delay_ = 0;
  uint32_t doubler_fade_frames_ = 1;
  uint32_t doubler_fade_remaining_ = 0;
  uint32_t output_input_channels_ = 0;
  uint32_t output_output_channels_ = 0;
};

}

#endif
