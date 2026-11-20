#ifndef VESPERA_TEMPO_HPP
#define VESPERA_TEMPO_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <vespera/module.h>

namespace vespera {

struct TempoSettings {
  double bpm = 120;
  bool metronome_enabled = false;
  float metronome_level_db = -18;
};

inline bool validate_tempo_settings(const TempoSettings &settings) noexcept {
  return std::isfinite(settings.bpm) && settings.bpm >= 40 && settings.bpm <= 240 &&
         std::isfinite(settings.metronome_level_db) && settings.metronome_level_db >= -60 &&
         settings.metronome_level_db <= 0;
}

class TempoClock {
public:
  void prepare(double sample_rate, TempoSettings settings = {}, double beat = 0) {
    if (!std::isfinite(sample_rate) || sample_rate < 8000 || !validate_tempo_settings(settings) ||
        !std::isfinite(beat) || beat < 0 || beat > 1e12) {
      throw std::invalid_argument("Invalid tempo clock configuration");
    }
    rate_ = sample_rate;
    settings_ = settings;
    anchor_ = beat;
    frames_ = 0;
    next_beat_ = std::ceil(beat);
    click_age_ = rate_;
    gain_ = settings.metronome_enabled ? std::pow(10.0, settings.metronome_level_db / 20.0) : 0;
    target_gain_ = gain_;
  }

  bool set_settings(TempoSettings settings) noexcept {
    if (!validate_tempo_settings(settings)) {
      return false;
    }
    anchor_ = beat_position();
    frames_ = 0;
    settings_ = settings;
    target_gain_ =
        settings.metronome_enabled ? std::pow(10.0, settings.metronome_level_db / 20.0) : 0;
    return true;
  }

  double beat_position() const noexcept {
    return anchor_ + static_cast<double>(frames_) * settings_.bpm / (60 * rate_);
  }
  vs_tempo_context context(uint32_t flags = 0) const noexcept {
    return {sizeof(vs_tempo_context), VS_TEMPO_VALID | flags, settings_.bpm, beat_position()};
  }

  float sample() noexcept {
    const double beat = beat_position();
    if (beat + 1e-10 >= next_beat_) {
      if (settings_.metronome_enabled) {
        click_age_ = 0;
        accented_ = std::fmod(next_beat_, 4) == 0;
      }
      next_beat_ += 1;
    }
    gain_ += std::clamp(target_gain_ - gain_, -1.0 / (rate_ * 0.005), 1.0 / (rate_ * 0.005));
    float output = 0;
    const double t = click_age_ / rate_;
    if (t < 0.04) {
      const double envelope =
          std::min(1.0, t / 0.001) * std::exp(-t * 180) * std::min(1.0, (0.04 - t) / 0.005);
      output = static_cast<float>(gain_ * envelope *
                                  std::sin(2 * std::numbers::pi * (accented_ ? 1760 : 1320) * t));
    }
    click_age_ += 1;
    ++frames_;
    return output;
  }

private:
  double rate_ = 48000;
  TempoSettings settings_;
  double anchor_ = 0;
  uint64_t frames_ = 0;
  double next_beat_ = 0;
  double click_age_ = 48000;
  double gain_ = 0;
  double target_gain_ = 0;
  bool accented_ = false;
};

class TapTempo {
public:
  std::optional<double> push(double seconds) noexcept {
    if (!std::isfinite(seconds)) {
      return std::nullopt;
    }
    auto previous = previous_;
    previous_ = seconds;
    if (!previous) {
      return std::nullopt;
    }
    const double interval = seconds - *previous;
    if (interval < 0.25 || interval > 1.5) {
      return std::nullopt;
    }
    return 60 / interval;
  }

private:
  std::optional<double> previous_;
};
}
#endif
