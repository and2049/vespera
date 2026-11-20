#ifndef VESPERA_DSP_EQUALIZER_HPP
#define VESPERA_DSP_EQUALIZER_HPP

#include <vespera/dsp/common.hpp>
#include <vespera/dsp/filters.hpp>
#include <vespera/dsp/smoothing.hpp>

#include <array>
#include <cassert>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <numbers>
#include <span>
#include <stdexcept>

// Multichannel graphic equalizer: an optional high-pass stage, a series of peak bands at fixed
// centre frequencies, and an optional low-pass stage, with coefficients shared by all channels.
//
// The equalizer does not smooth controls. The owner smooths band gains and corner frequencies
// and calls the setters at a modest rate; each setter designs new coefficients, which keeps the
// filter history (see Biquad). Switching a pass stage on or off is different: it crossfades the
// stage's output with its input over stage_fade_seconds, so a stage can be switched off at a
// control's end stop without a click and without having to design an extreme corner frequency.
//
// Signal contract, threading and denormal behaviour are those of filters.hpp.

namespace vespera::dsp {

template <std::size_t Bands> struct GraphicEqualizerConfig {
  double sample_rate = 48000.0;
  std::array<double, Bands> band_frequencies{};
  double band_q = std::numbers::sqrt2;
  double highpass_q = std::numbers::sqrt2 / 2.0;
  double lowpass_q = std::numbers::sqrt2 / 2.0;
  double stage_fade_seconds = 0.02;
};

// Each band is a peak filter while its gain is nonzero; at exactly 0 dB it is the identity. A band
// whose centre is at or above maximum_normalized_filter_frequency times the sample rate stays the
// identity at every gain. Identity stages still run, so a band returning to 0 dB lets its history
// drain instead of discarding it.
//
// The pass stages always run, so their history stays current while they are faded out. A stage
// starts switched off with identity coefficients; until a corner frequency is set, switching it on
// changes nothing. While a stage's weight is w, its contribution is x + (filtered - x) * w; at
// w = 0 that is x, apart from the sign of a zero, while the stage's history is finite.
template <std::size_t Bands, std::size_t Channels = 2> class GraphicEqualizer {
  static_assert(Bands > 0, "a graphic equalizer needs at least one band");
  static_assert(Channels > 0, "a graphic equalizer needs at least one channel");

public:
  using Config = GraphicEqualizerConfig<Bands>;

  // Throws std::invalid_argument for an invalid sample rate, Q or fade time, or for a band centre
  // that is non-finite, not positive, or below the minimum filter frequency.
  explicit GraphicEqualizer(const Config &config) : config_(config) {
    validate_sample_rate(config.sample_rate);
    validate_filter_q(config.band_q);
    validate_filter_q(config.highpass_q);
    validate_filter_q(config.lowpass_q);
    for (std::size_t band = 0; band < Bands; ++band) {
      const double frequency = config.band_frequencies[band];
      if (!std::isfinite(frequency) || frequency <= 0.0) {
        throw std::invalid_argument("Equalizer band frequencies must be finite and positive");
      }
      band_active_[band] = frequency < config.sample_rate * maximum_normalized_filter_frequency;
      if (band_active_[band]) {
        validate_filter_frequency(config.sample_rate, frequency);
      }
    }
    highpass_weight_.configure(config.sample_rate, config.stage_fade_seconds);
    lowpass_weight_.configure(config.sample_rate, config.stage_fade_seconds);
  }

  [[nodiscard]] const Config &config() const noexcept { return config_; }
  static constexpr std::size_t band_count() noexcept { return Bands; }
  static constexpr std::size_t channel_count() noexcept { return Channels; }

  // Whether the band's centre lies below the frequency limit, so a gain can take effect.
  [[nodiscard]] bool band_active(std::size_t band) const noexcept {
    assert(band < Bands);
    return band_active_[band];
  }

  // Throws std::invalid_argument, changing nothing, for band >= band_count() or a gain that is
  // non-finite or beyond +/-maximum_filter_gain_db, even for an inactive band.
  void set_band_gain(std::size_t band, double gain_db) {
    if (band >= Bands) {
      throw std::invalid_argument("Equalizer band index out of range");
    }
    const BiquadCoefficients coefficients = design_band(band, gain_db);
    for (BiquadCascade<Bands> &channel : bands_) {
      channel.set_coefficients(band, coefficients);
    }
  }

  // All-or-nothing: every gain is validated and designed before any band changes.
  void set_band_gains(const std::array<double, Bands> &gains_db) {
    typename BiquadCascade<Bands>::Coefficients coefficients;
    for (std::size_t band = 0; band < Bands; ++band) {
      coefficients[band] = design_band(band, gains_db[band]);
    }
    for (BiquadCascade<Bands> &channel : bands_) {
      channel.set_coefficients(coefficients);
    }
  }

  // Designs the stage's corner with BiquadCoefficients::highpass or lowpass and the configured Q;
  // throws std::invalid_argument, changing nothing, where that factory would.
  void set_highpass_frequency(double frequency) {
    const BiquadCoefficients coefficients =
        BiquadCoefficients::highpass(config_.sample_rate, frequency, config_.highpass_q);
    for (Biquad &filter : highpass_) {
      filter.set_coefficients(coefficients);
    }
  }

  void set_lowpass_frequency(double frequency) {
    const BiquadCoefficients coefficients =
        BiquadCoefficients::lowpass(config_.sample_rate, frequency, config_.lowpass_q);
    for (Biquad &filter : lowpass_) {
      filter.set_coefficients(coefficients);
    }
  }

  // Starts a linear fade of the stage's weight towards 1 or 0 from its current value. Repeating
  // the current target does not restart the fade.
  void set_highpass_enabled(bool enabled) noexcept {
    highpass_weight_.set_target(enabled ? 1.0 : 0.0);
  }
  void set_lowpass_enabled(bool enabled) noexcept {
    lowpass_weight_.set_target(enabled ? 1.0 : 0.0);
  }

  [[nodiscard]] double highpass_weight() const noexcept { return highpass_weight_.current(); }
  [[nodiscard]] double lowpass_weight() const noexcept { return lowpass_weight_.current(); }

  // Clears all filter history and completes any stage fade at its target. Coefficients are kept.
  void reset() noexcept {
    highpass_weight_.reset(highpass_weight_.target());
    lowpass_weight_.reset(lowpass_weight_.target());
    for (std::size_t c = 0; c < Channels; ++c) {
      highpass_[c].reset();
      bands_[c].reset();
      lowpass_[c].reset();
    }
  }

  // Processes one frame in place, channel c of the frame through channel c's filters. The stage
  // fades advance once per call, so call it exactly once per frame. Requires
  // frame.size() <= channel_count(); unused channels keep their history.
  template <std::floating_point Sample> void process_frame(std::span<Sample> frame) noexcept {
    assert(frame.size() <= Channels);
    const double highpass = highpass_weight_.process();
    const double lowpass = lowpass_weight_.process();
    for (std::size_t c = 0; c < frame.size(); ++c) {
      double sample = static_cast<double>(frame[c]);
      const double highpassed = highpass_[c].process(sample);
      sample += (highpassed - sample) * highpass;
      sample = bands_[c].process(sample);
      const double lowpassed = lowpass_[c].process(sample);
      sample += (lowpassed - sample) * lowpass;
      frame[c] = static_cast<Sample>(sample);
    }
  }

private:
  BiquadCoefficients design_band(std::size_t band, double gain_db) const {
    validate_filter_gain_db(gain_db);
    if (!band_active_[band] || gain_db == 0.0) {
      return {};
    }
    return BiquadCoefficients::peak(config_.sample_rate, config_.band_frequencies[band],
                                    config_.band_q, gain_db);
  }

  Config config_;
  std::array<bool, Bands> band_active_{};
  LinearSmoother highpass_weight_;
  LinearSmoother lowpass_weight_;
  std::array<Biquad, Channels> highpass_{};
  std::array<BiquadCascade<Bands>, Channels> bands_{};
  std::array<Biquad, Channels> lowpass_{};
};

}

#endif
