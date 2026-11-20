#include <vespera/core_io.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace vespera {

namespace {

constexpr double minimum_sample_rate = 8000.0;
constexpr double maximum_sample_rate = 768000.0;
constexpr double gain_ramp_seconds = 0.02;
constexpr double pitch_window_seconds = 0.02;
constexpr double pitch_search_seconds = 0.01;
constexpr double pitch_correlation_seconds = 0.008;
constexpr double pitch_search_rate = 16000.0;
constexpr uint32_t pitch_refine_terms = 128;
constexpr double pitch_mix_seconds = 0.02;
constexpr double semitone_glide_seconds = 0.03;
constexpr uint32_t pitch_min_delay = 2;
constexpr double gate_hysteresis_db = 6.0;
constexpr double gate_envelope_release_seconds = 0.03;
constexpr double gate_attack_seconds = 0.0005;
constexpr double gate_release_seconds = 0.02;
constexpr double gate_hold_seconds = 0.05;
constexpr double doubler_fade_seconds = 0.02;
constexpr double pi = 3.14159265358979323846;

bool within(float value, float low, float high) noexcept {
  return std::isfinite(value) && value >= low && value <= high;
}

double db_to_linear(double db) noexcept { return std::pow(10.0, db / 20.0); }

uint32_t frames_for(double sample_rate, double seconds) noexcept {
  return static_cast<uint32_t>(std::lround(sample_rate * seconds));
}

uint32_t ring_size(uint32_t minimum) noexcept {
  uint32_t size = 1;
  while (size < minimum) {
    size <<= 1;
  }
  return size;
}

float one_pole(double sample_rate, double seconds) noexcept {
  return static_cast<float>(-std::expm1(-1.0 / (sample_rate * seconds)));
}

float finite_or_zero(float sample) noexcept { return std::isfinite(sample) ? sample : 0.0f; }

uint32_t doubler_delay_frames(const CoreIoSettings &settings, double sample_rate) noexcept {
  if (!settings.doubler_enabled) {
    return 0;
  }
  return std::max<uint32_t>(1, frames_for(sample_rate, settings.doubler_delay_ms / 1000.0));
}

}

bool validate_core_io_settings(const CoreIoSettings &settings) noexcept {
  return within(settings.input_gain_db, core_io_min_gain_db, core_io_max_gain_db) &&
         within(settings.output_gain_db, core_io_min_gain_db, core_io_max_gain_db) &&
         within(settings.gate_threshold_db, core_io_min_gate_threshold_db,
                core_io_max_gate_threshold_db) &&
         within(settings.transpose_semitones, -core_io_max_transpose_semitones,
                core_io_max_transpose_semitones) &&
         within(settings.doubler_delay_ms, core_io_min_doubler_delay_ms,
                core_io_max_doubler_delay_ms);
}

void CoreIoProcessor::Ramp::set_target(double value) noexcept {
  if (value == target_) {
    return;
  }
  target_ = value;
  step_ = (target_ - current_) / frames_;
  remaining_ = frames_;
}

double CoreIoProcessor::Ramp::next() noexcept {
  if (remaining_ != 0) {
    --remaining_;
    current_ = remaining_ == 0 ? target_ : current_ + step_;
  }
  return current_;
}

void CoreIoProcessor::prepare(double sample_rate, uint32_t max_frames, TransposeLatency latency) {
  prepare(sample_rate, max_frames, latency, settings_);
}

void CoreIoProcessor::prepare(double sample_rate, uint32_t max_frames, TransposeLatency latency,
                              const CoreIoSettings &settings) {
  const CoreIoSettings requested = settings;
  if (!validate_core_io_settings(requested)) {
    throw std::invalid_argument("Core IO settings are out of range");
  }
  if (!std::isfinite(sample_rate) || sample_rate < minimum_sample_rate ||
      sample_rate > maximum_sample_rate) {
    throw std::invalid_argument("Core IO sample rate must be within [8000, 768000] Hz");
  }
  if (max_frames == 0 || max_frames > max_frame_capacity) {
    throw std::invalid_argument("Core IO frame capacity must be within [1, 65536]");
  }
  if (latency != TransposeLatency::none && latency != TransposeLatency::fixed) {
    throw std::invalid_argument("Core IO transpose latency policy is invalid");
  }
  if (latency == TransposeLatency::none && requested.transpose_semitones != 0.0f) {
    throw std::invalid_argument("Core IO transposition requires fixed transpose latency");
  }

  const uint32_t window =
      2 * std::max<uint32_t>(1, frames_for(sample_rate, pitch_window_seconds / 2.0));
  const uint32_t search =
      2 * std::max<uint32_t>(1, frames_for(sample_rate, pitch_search_seconds / 2.0));
  const uint32_t correlation =
      std::max<uint32_t>(1, frames_for(sample_rate, pitch_correlation_seconds));
  uint32_t pitch_latency = 0;
  std::vector<float> pitch_buffer;
  uint32_t pitch_size = 0;
  if (latency == TransposeLatency::fixed) {
    pitch_latency = pitch_min_delay + (window + search) / 2;
    pitch_size = ring_size(pitch_min_delay + window + search + correlation + 8);
    pitch_buffer.assign(static_cast<std::size_t>(pitch_size) * max_channels, 0.0f);
  }
  const uint32_t doubler_size =
      ring_size(frames_for(sample_rate, core_io_max_doubler_delay_ms / 1000.0) + 2);
  std::vector<float> doubler_buffer(doubler_size, 0.0f);

  settings_ = requested;
  sample_rate_ = sample_rate;
  max_frames_ = max_frames;
  latency_mode_ = latency;
  pitch_buffer_ = std::move(pitch_buffer);
  pitch_mask_ = pitch_size == 0 ? 0 : pitch_size - 1;
  pitch_window_ = window;
  pitch_search_ = search;
  pitch_correlation_ = correlation;
  pitch_search_stride_ = static_cast<uint32_t>(std::ceil(sample_rate / pitch_search_rate));
  pitch_refine_term_step_ = (correlation + pitch_refine_terms - 1) / pitch_refine_terms;
  peak_splice_terms_ = 0;
  pitch_latency_ = pitch_latency;
  doubler_buffer_ = std::move(doubler_buffer);
  doubler_mask_ = doubler_size - 1;

  input_gain_.configure(frames_for(sample_rate, gain_ramp_seconds));
  output_gain_.configure(frames_for(sample_rate, gain_ramp_seconds));
  semitones_.configure(frames_for(sample_rate, semitone_glide_seconds));
  pitch_mix_.configure(frames_for(sample_rate, pitch_mix_seconds));
  envelope_release_ =
      static_cast<float>(std::exp(-1.0 / (sample_rate * gate_envelope_release_seconds)));
  gate_attack_ = one_pole(sample_rate, gate_attack_seconds);
  gate_release_ = one_pole(sample_rate, gate_release_seconds);
  gate_hold_frames_ = frames_for(sample_rate, gate_hold_seconds);
  doubler_fade_frames_ = std::max<uint32_t>(1, frames_for(sample_rate, doubler_fade_seconds));

  apply_targets(true);
  reset();
}

bool CoreIoProcessor::set_settings(const CoreIoSettings &settings) noexcept {
  if (!validate_core_io_settings(settings)) {
    return false;
  }
  if (prepared() && latency_mode_ == TransposeLatency::none &&
      settings.transpose_semitones != 0.0f) {
    return false;
  }
  settings_ = settings;
  if (prepared()) {
    apply_targets(false);
  }
  return true;
}

void CoreIoProcessor::apply_targets(bool snap) noexcept {
  const auto set = [snap](Ramp &ramp, double value) {
    if (snap) {
      ramp.snap(value);
    } else {
      ramp.set_target(value);
    }
  };
  set(input_gain_, db_to_linear(settings_.input_gain_db));
  set(output_gain_, db_to_linear(settings_.output_gain_db));
  gate_open_level_ = static_cast<float>(db_to_linear(settings_.gate_threshold_db));
  gate_close_level_ =
      static_cast<float>(db_to_linear(settings_.gate_threshold_db - gate_hysteresis_db));

  const double semitones = settings_.transpose_semitones;
  if (semitones != 0.0) {
    if (snap || (pitch_mix_.current() == 0.0 && pitch_mix_.target() == 0.0)) {
      semitones_.snap(semitones);
      pitch_ratio_ = std::exp2(semitones / 12.0);
      engage_pitch();
    } else {
      semitones_.set_target(semitones);
    }
    set(pitch_mix_, 1.0);
  } else {
    set(pitch_mix_, 0.0);
  }

  doubler_target_delay_ = doubler_delay_frames(settings_, sample_rate_);
}

void CoreIoProcessor::reset() noexcept {
  reset_input();
  reset_output();
}

void CoreIoProcessor::reset_input() noexcept {
  std::fill(pitch_buffer_.begin(), pitch_buffer_.end(), 0.0f);
  pitch_write_ = 0;
  for (GateChannel &channel : gates_) {
    channel = GateChannel{};
    channel.hold_remaining = gate_hold_frames_;
  }
  input_gain_.snap(input_gain_.target());
  semitones_.snap(semitones_.target());
  pitch_mix_.snap(pitch_mix_.target());
  pitch_ratio_ = std::exp2(semitones_.current() / 12.0);
  engage_pitch();
}

CoreIoProcessor::PitchDirection CoreIoProcessor::direction_for(double ratio) noexcept {
  return ratio >= 1.0 ? PitchDirection::shortening : PitchDirection::lengthening;
}

void CoreIoProcessor::engage_pitch() noexcept {
  pitch_phase_ = 0.5;
  pitch_direction_ = direction_for(pitch_ratio_);
  for (auto &delays : pitch_delays_) {
    delays[0] = delays[1] = pitch_latency_;
  }
  pitch_splice_pending_[0] = false;
  pitch_splice_pending_[1] = true;
}

double CoreIoProcessor::splice_score(const float *ring, uint32_t reference, uint32_t candidate,
                                     uint32_t step) noexcept {
  double cross = 0.0;
  double energy = 0.0;
  uint32_t terms = 0;
  for (uint32_t k = 0; k < pitch_correlation_; k += step) {
    const double a = ring[(pitch_write_ - reference - k) & pitch_mask_];
    const double b = ring[(pitch_write_ - candidate - k) & pitch_mask_];
    cross += a * b;
    energy += b * b;
    ++terms;
  }
  splice_terms_ += terms;
  return cross / std::sqrt(energy + 1e-30);
}

// Restarts a silent tap at the delay within its search range whose recent waveform best matches
// the audible tap, so the following crossfade sums coherent rather than arbitrarily phased grains.
// The search scans a grid of roughly 16 kHz resolution and then halves the lag step around the
// best candidate down to one frame, with decimated correlation sums, so its cost per splice is
// bounded independently of the sample rate.
void CoreIoProcessor::splice_tap(uint32_t channels, uint32_t tap) noexcept {
  const uint32_t low = pitch_direction_ == PitchDirection::shortening
                           ? pitch_min_delay + pitch_window_
                           : pitch_min_delay;
  const uint32_t high = low + pitch_search_;
  const uint32_t coarse_step = pitch_search_stride_;
  const uint32_t refine_step = pitch_refine_term_step_;
  for (uint32_t c = 0; c < channels; ++c) {
    splice_terms_ = 0;
    const float *ring = pitch_buffer_.data() + static_cast<std::size_t>(c) * (pitch_mask_ + 1);
    const double other = pitch_delays_[c][1 - tap];
    const double whole = std::floor(other);
    const auto reference = static_cast<uint32_t>(whole);

    uint32_t chosen = low + pitch_search_ / 2;
    double best = splice_score(ring, reference, chosen, coarse_step);
    for (uint32_t candidate = low; candidate <= high; candidate += coarse_step) {
      const double score = splice_score(ring, reference, candidate, coarse_step);
      if (score > best) {
        best = score;
        chosen = candidate;
      }
    }
    if (coarse_step > 1) {
      best = splice_score(ring, reference, chosen, refine_step);
      for (uint32_t lag_step = coarse_step; lag_step > 1;) {
        lag_step = (lag_step + 1) / 2;
        const uint32_t center = chosen;
        for (const uint32_t candidate : {center - std::min(center, lag_step), center + lag_step}) {
          if (candidate < low || candidate > high) {
            continue;
          }
          const double score = splice_score(ring, reference, candidate, refine_step);
          if (score > best) {
            best = score;
            chosen = candidate;
          }
        }
      }
    }
    peak_splice_terms_ = std::max(peak_splice_terms_, splice_terms_);
    pitch_delays_[c][tap] = chosen + (other - whole);
  }
}

void CoreIoProcessor::reset_output() noexcept {
  std::fill(doubler_buffer_.begin(), doubler_buffer_.end(), 0.0f);
  doubler_write_ = 0;
  doubler_active_delay_ = doubler_next_delay_ = doubler_target_delay_;
  doubler_fade_remaining_ = 0;
  output_gain_.snap(output_gain_.target());
}

void CoreIoProcessor::silence(float *const *outputs, uint32_t channels, uint32_t frames) noexcept {
  if (outputs == nullptr) {
    return;
  }
  for (uint32_t c = 0; c < channels; ++c) {
    if (outputs[c] != nullptr) {
      std::fill_n(outputs[c], frames, 0.0f);
    }
  }
}

float CoreIoProcessor::gate(GateChannel &channel, float sample) noexcept {
  const float level = std::abs(sample);
  channel.envelope = level > channel.envelope ? level : channel.envelope * envelope_release_;
  if (channel.envelope < 1e-20f) {
    channel.envelope = 0.0f;
  }

  if (!settings_.gate_enabled) {
    channel.state = GateState::open;
    channel.hold_remaining = gate_hold_frames_;
  } else if (channel.state == GateState::open) {
    if (channel.envelope >= gate_close_level_) {
      channel.hold_remaining = gate_hold_frames_;
    } else if (channel.hold_remaining != 0) {
      --channel.hold_remaining;
    } else {
      channel.state = GateState::closed;
    }
  } else if (channel.envelope >= gate_open_level_) {
    channel.state = GateState::open;
    channel.hold_remaining = gate_hold_frames_;
  }

  if (channel.state == GateState::open) {
    channel.gain += (1.0f - channel.gain) * gate_attack_;
    if (channel.gain > 0.99999f) {
      channel.gain = 1.0f;
    }
  } else {
    channel.gain -= channel.gain * gate_release_;
    if (channel.gain < 1e-5f) {
      channel.gain = 0.0f;
    }
  }
  return sample * channel.gain;
}

float CoreIoProcessor::read_pitch(uint32_t channel, double delay) const noexcept {
  const float *ring = pitch_buffer_.data() + static_cast<std::size_t>(channel) * (pitch_mask_ + 1);
  const auto whole = static_cast<uint32_t>(delay);
  const auto t = static_cast<float>(delay - whole);
  const uint32_t base = pitch_write_ - whole;
  const float newer = ring[(base + 1) & pitch_mask_];
  const float y0 = ring[base & pitch_mask_];
  const float y1 = ring[(base - 1) & pitch_mask_];
  const float y2 = ring[(base - 2) & pitch_mask_];
  const float c1 = 0.5f * (y1 - newer);
  const float c2 = newer - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
  const float c3 = 0.5f * (y2 - newer) + 1.5f * (y0 - y1);
  return ((c3 * t + c2) * t + c1) * t + y0;
}

bool CoreIoProcessor::process_input(const float *const *inputs, float *const *outputs,
                                    uint32_t channels, uint32_t frames) noexcept {
  const bool layout_valid = channels >= 1 && channels <= max_channels;
  if (!prepared() || !layout_valid || frames > max_frames_ || inputs == nullptr ||
      outputs == nullptr) {
    if (layout_valid) {
      silence(outputs, channels, frames);
    }
    return false;
  }
  for (uint32_t c = 0; c < channels; ++c) {
    if (inputs[c] == nullptr || outputs[c] == nullptr) {
      silence(outputs, channels, frames);
      return false;
    }
  }
  if (channels != input_channels_) {
    reset_input();
    input_channels_ = channels;
  }

  const bool pitch_available = latency_mode_ == TransposeLatency::fixed;
  const uint32_t ring_stride = pitch_mask_ + 1;
  const double window = pitch_window_;
  const double longest_delay = pitch_min_delay + pitch_window_ + pitch_search_;
  for (uint32_t n = 0; n < frames; ++n) {
    const auto gain = static_cast<float>(input_gain_.next());

    const bool shifting =
        pitch_available && (pitch_mix_.current() > 0.0 || pitch_mix_.target() > 0.0);
    float mix = 0.0f;
    float weight_a = 0.0f;
    if (shifting) {
      mix = static_cast<float>(pitch_mix_.next());
      if (semitones_.active()) {
        pitch_ratio_ = std::exp2(semitones_.next() / 12.0);
        const PitchDirection direction = direction_for(pitch_ratio_);
        if (direction != pitch_direction_) {
          // Retracing each tap's travel keeps it inside its delay range and leaves the
          // symmetric crossfade weights unchanged.
          pitch_direction_ = direction;
          pitch_phase_ = pitch_phase_ == 0.0 ? 0.0 : 1.0 - pitch_phase_;
        }
      }
      for (uint32_t tap = 0; tap < 2; ++tap) {
        if (pitch_splice_pending_[tap]) {
          pitch_splice_pending_[tap] = false;
          splice_tap(channels, tap);
        }
      }
      const double s = std::sin(pi * pitch_phase_);
      weight_a = static_cast<float>(s * s);
    }

    for (uint32_t c = 0; c < channels; ++c) {
      const float sample = gate(gates_[c], finite_or_zero(inputs[c][n]) * gain);
      if (!pitch_available) {
        outputs[c][n] = sample;
        continue;
      }
      float *ring = pitch_buffer_.data() + static_cast<std::size_t>(c) * ring_stride;
      ring[pitch_write_] = sample;
      const float aligned = ring[(pitch_write_ - pitch_latency_) & pitch_mask_];
      if (mix > 0.0f) {
        const float shifted = weight_a * read_pitch(c, pitch_delays_[c][0]) +
                              (1.0f - weight_a) * read_pitch(c, pitch_delays_[c][1]);
        outputs[c][n] = aligned + mix * (shifted - aligned);
      } else {
        outputs[c][n] = aligned;
      }
    }

    if (shifting) {
      const double drift = pitch_ratio_ - 1.0;
      for (uint32_t c = 0; c < channels; ++c) {
        for (double &delay : pitch_delays_[c]) {
          delay = std::clamp(delay - drift, static_cast<double>(pitch_min_delay), longest_delay);
        }
      }
      const double previous = pitch_phase_;
      pitch_phase_ += std::abs(drift) / window;
      if (previous < 0.5 && pitch_phase_ >= 0.5) {
        pitch_splice_pending_[1] = true;
      }
      if (pitch_phase_ >= 1.0) {
        pitch_phase_ -= 1.0;
        pitch_splice_pending_[0] = true;
      }
    }
    pitch_write_ = (pitch_write_ + 1) & pitch_mask_;
  }
  return true;
}

bool CoreIoProcessor::process_output(const float *const *inputs, uint32_t input_channels,
                                     float *const *outputs, uint32_t output_channels,
                                     uint32_t frames) noexcept {
  const bool output_valid = output_channels >= 1 && output_channels <= max_channels;
  const bool layout_valid =
      output_valid && input_channels >= 1 && input_channels <= output_channels;
  if (!prepared() || !layout_valid || frames > max_frames_ || inputs == nullptr ||
      outputs == nullptr) {
    if (output_valid) {
      silence(outputs, output_channels, frames);
    }
    return false;
  }
  for (uint32_t c = 0; c < output_channels; ++c) {
    if ((c < input_channels && inputs[c] == nullptr) || outputs[c] == nullptr) {
      silence(outputs, output_channels, frames);
      return false;
    }
  }
  if (input_channels != output_input_channels_ || output_channels != output_output_channels_) {
    reset_output();
    output_input_channels_ = input_channels;
    output_output_channels_ = output_channels;
  }

  for (uint32_t n = 0; n < frames; ++n) {
    const auto gain = static_cast<float>(output_gain_.next());
    const float left = finite_or_zero(inputs[0][n]);
    if (output_channels == 1) {
      outputs[0][n] = left * gain;
      continue;
    }
    const float source = input_channels == 2 ? finite_or_zero(inputs[1][n]) : left;
    doubler_buffer_[doubler_write_] = source;
    if (doubler_fade_remaining_ == 0 && doubler_target_delay_ != doubler_active_delay_) {
      doubler_next_delay_ = doubler_target_delay_;
      doubler_fade_remaining_ = doubler_fade_frames_;
    }
    float right = doubler_buffer_[(doubler_write_ - doubler_active_delay_) & doubler_mask_];
    if (doubler_fade_remaining_ != 0) {
      --doubler_fade_remaining_;
      const float next = doubler_buffer_[(doubler_write_ - doubler_next_delay_) & doubler_mask_];
      const float weight = static_cast<float>(doubler_fade_frames_ - doubler_fade_remaining_) /
                           static_cast<float>(doubler_fade_frames_);
      right += weight * (next - right);
      if (doubler_fade_remaining_ == 0) {
        doubler_active_delay_ = doubler_next_delay_;
        right = next;
      }
    }
    doubler_write_ = (doubler_write_ + 1) & doubler_mask_;
    outputs[0][n] = left * gain;
    outputs[1][n] = right * gain;
  }
  return true;
}

}
