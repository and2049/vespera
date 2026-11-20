#ifndef VESPERA_DSP_BENCH_BENCH_HPP
#define VESPERA_DSP_BENCH_BENCH_HPP

#include <vespera/dsp/delay.hpp>
#include <vespera/dsp/dynamics.hpp>
#include <vespera/dsp/filters.hpp>
#include <vespera/dsp/oscillator.hpp>
#include <vespera/dsp/smoothing.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vespera::dsp::bench {

namespace detail {

inline void require_rate(double sample_rate) {
  if (!std::isfinite(sample_rate) || sample_rate <= 0.0) {
    throw std::invalid_argument("sample rate must be positive and finite");
  }
}

inline void require_frequency(double sample_rate, double frequency) {
  require_rate(sample_rate);
  if (!std::isfinite(frequency) || frequency <= 0.0 || frequency >= sample_rate * 0.5) {
    throw std::invalid_argument("frequency must be positive and below Nyquist");
  }
}

inline void require_amplitude(double amplitude) {
  if (!std::isfinite(amplitude) || amplitude < 0.0) {
    throw std::invalid_argument("amplitude must be finite and non-negative");
  }
}

}

inline std::vector<float> sine(double sample_rate, double frequency, double amplitude,
                               std::size_t frames, double phase = 0.0) {
  detail::require_frequency(sample_rate, frequency);
  detail::require_amplitude(amplitude);
  std::vector<float> out(frames);
  const double step = 2.0 * std::numbers::pi * frequency / sample_rate;
  for (std::size_t n = 0; n < frames; ++n) {
    out[n] = static_cast<float>(amplitude * std::sin(step * static_cast<double>(n) + phase));
  }
  return out;
}

// Exponential sweep whose instantaneous frequency is start * (end / start)^(t / duration).
inline std::vector<float> log_sweep(double sample_rate, double start_frequency,
                                    double end_frequency, double amplitude, std::size_t frames) {
  detail::require_frequency(sample_rate, start_frequency);
  detail::require_frequency(sample_rate, end_frequency);
  detail::require_amplitude(amplitude);
  if (start_frequency == end_frequency || frames < 2) {
    return sine(sample_rate, start_frequency, amplitude, frames);
  }
  std::vector<float> out(frames);
  const double duration = static_cast<double>(frames) / sample_rate;
  const double rate = std::log(end_frequency / start_frequency) / duration;
  const double scale = 2.0 * std::numbers::pi * start_frequency / rate;
  for (std::size_t n = 0; n < frames; ++n) {
    const double t = static_cast<double>(n) / sample_rate;
    out[n] = static_cast<float>(amplitude * std::sin(scale * std::expm1(rate * t)));
  }
  return out;
}

inline std::vector<float> impulse(std::size_t frames, std::size_t position, double amplitude) {
  detail::require_amplitude(amplitude);
  if (position >= frames) {
    throw std::invalid_argument("impulse position must be inside the signal");
  }
  std::vector<float> out(frames, 0.0f);
  out[position] = static_cast<float>(amplitude);
  return out;
}

// Deterministic uniform white noise in [-amplitude, amplitude); RMS is amplitude / sqrt(3).
inline std::vector<float> white_noise(std::size_t frames, double amplitude, std::uint64_t seed) {
  detail::require_amplitude(amplitude);
  std::vector<float> out(frames);
  std::uint64_t state = seed;
  for (float &sample : out) {
    state += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    const double unit = static_cast<double>(z >> 11) * 0x1.0p-53;
    sample = static_cast<float>(amplitude * (2.0 * unit - 1.0));
  }
  return out;
}

inline std::vector<float> interleave(std::span<const float> mono, std::size_t channels) {
  if (channels == 0) {
    throw std::invalid_argument("channel count must be positive");
  }
  std::vector<float> out(mono.size() * channels);
  for (std::size_t frame = 0; frame < mono.size(); ++frame) {
    std::fill_n(out.begin() + static_cast<std::ptrdiff_t>(frame * channels), channels, mono[frame]);
  }
  return out;
}

struct SignalStats {
  double rms = 0.0;
  double peak = 0.0;
  double dc = 0.0;
};

inline SignalStats measure(std::span<const float> signal) {
  SignalStats stats;
  if (signal.empty()) {
    return stats;
  }
  double sum = 0.0;
  double squares = 0.0;
  for (float sample : signal) {
    const double value = sample;
    sum += value;
    squares += value * value;
    stats.peak = std::max(stats.peak, std::abs(value));
  }
  const auto count = static_cast<double>(signal.size());
  stats.dc = sum / count;
  stats.rms = std::sqrt(squares / count);
  return stats;
}

inline double to_db(double amplitude) {
  return amplitude > 0.0 ? 20.0 * std::log10(amplitude) : -std::numeric_limits<double>::infinity();
}

enum class Window { rectangular, hann };

inline std::vector<double> window_coefficients(Window window, std::size_t size) {
  std::vector<double> out(size, 1.0);
  if (window == Window::hann) {
    for (std::size_t n = 0; n < size; ++n) {
      out[n] = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(n) /
                                    static_cast<double>(size));
    }
  }
  return out;
}

inline bool is_power_of_two(std::size_t n) { return n != 0 && (n & (n - 1)) == 0; }

inline void fft_in_place(std::span<std::complex<double>> data) {
  const std::size_t n = data.size();
  if (!is_power_of_two(n)) {
    throw std::invalid_argument("FFT size must be a power of two");
  }
  for (std::size_t i = 1, j = 0; i < n; ++i) {
    std::size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) {
      j ^= bit;
    }
    j ^= bit;
    if (i < j) {
      std::swap(data[i], data[j]);
    }
  }
  for (std::size_t length = 2; length <= n; length <<= 1) {
    const double angle = -2.0 * std::numbers::pi / static_cast<double>(length);
    for (std::size_t k = 0; k < length / 2; ++k) {
      const std::complex<double> twiddle = std::polar(1.0, angle * static_cast<double>(k));
      for (std::size_t start = 0; start < n; start += length) {
        const std::complex<double> even = data[start + k];
        const std::complex<double> odd = data[start + k + length / 2] * twiddle;
        data[start + k] = even + odd;
        data[start + k + length / 2] = even - odd;
      }
    }
  }
}

// One-sided spectrum scaled so a sinusoid centred on a bin reads its peak amplitude.
inline std::vector<double> amplitude_spectrum(std::span<const float> signal, Window window) {
  const std::size_t n = signal.size();
  if (!is_power_of_two(n) || n < 2) {
    throw std::invalid_argument("spectrum length must be a power of two of at least 2");
  }
  const std::vector<double> weights = window_coefficients(window, n);
  double weight_sum = 0.0;
  std::vector<std::complex<double>> bins(n);
  for (std::size_t i = 0; i < n; ++i) {
    bins[i] = static_cast<double>(signal[i]) * weights[i];
    weight_sum += weights[i];
  }
  fft_in_place(bins);
  std::vector<double> out(n / 2 + 1);
  for (std::size_t k = 0; k < out.size(); ++k) {
    const double one_sided = (k == 0 || k == n / 2) ? 1.0 : 2.0;
    out[k] = one_sided * std::abs(bins[k]) / weight_sum;
  }
  return out;
}

inline double bin_frequency(std::size_t bin, std::size_t fft_size, double sample_rate) {
  return static_cast<double>(bin) * sample_rate / static_cast<double>(fft_size);
}

// Nearest frequency with a whole number of cycles in `frames`, so DFT measurements are leak-free.
inline double coherent_frequency(double sample_rate, std::size_t frames, double target) {
  detail::require_frequency(sample_rate, target);
  if (frames < 3) {
    throw std::invalid_argument("coherent frequency needs at least three frames");
  }
  const double n = static_cast<double>(frames);
  const double max_cycles = std::ceil(n / 2.0) - 1.0;
  const double cycles = std::clamp(std::round(target * n / sample_rate), 1.0, max_cycles);
  return cycles * sample_rate / n;
}

// Amplitude estimate from a windowed DFT evaluated at exactly `frequency`. This is a quick spectral
// probe, not a fit: every other component (DC, harmonics, noise and the tone's own
// negative-frequency image) leaks in through the window's response. With the periodic Hann window a
// lone tone with k whole cycles reads exactly when 2k is not within one of 0 or N (mod N); other
// components cancel only if they also complete whole cycles. Off-bin tones, short signals and
// frequencies within a few bins of DC or Nyquist are biased; use fit_harmonics() for those. Needs
// at least two samples.
inline double tone_amplitude(std::span<const float> signal, double sample_rate, double frequency,
                             Window window = Window::hann) {
  detail::require_frequency(sample_rate, frequency);
  if (signal.size() < 2) {
    throw std::invalid_argument("tone amplitude needs at least two samples");
  }
  const std::vector<double> weights = window_coefficients(window, signal.size());
  const double step = 2.0 * std::numbers::pi * frequency / sample_rate;
  double re = 0.0;
  double im = 0.0;
  double weight_sum = 0.0;
  for (std::size_t n = 0; n < signal.size(); ++n) {
    const double phase = step * static_cast<double>(n);
    const double value = static_cast<double>(signal[n]) * weights[n];
    re += value * std::cos(phase);
    im -= value * std::sin(phase);
    weight_sum += weights[n];
  }
  return 2.0 * std::hypot(re, im) / weight_sum;
}

struct HarmonicFit {
  double dc = 0.0;
  // Peak amplitude of each fitted harmonic; index 0 is the fundamental.
  std::vector<double> amplitudes;
  // RMS of the signal minus the fitted DC and harmonics.
  double residual_rms = 0.0;
};

// Least-squares fit of DC plus cosine/sine pairs at fundamental * k for k = 1..harmonics, valid for
// any length and phase without windowing. Harmonics closer than one DFT bin (sample_rate / N) to
// Nyquist are left out of the model. Throws std::invalid_argument when the fundamental completes
// less than one cycle, the model has no harmonic left, the signal has fewer than twice as many
// samples as model terms, or the basis is numerically degenerate.
inline HarmonicFit fit_harmonics(std::span<const float> signal, double sample_rate,
                                 double fundamental, std::size_t harmonics = 1) {
  detail::require_frequency(sample_rate, fundamental);
  if (harmonics == 0) {
    throw std::invalid_argument("harmonic fit needs at least the fundamental");
  }
  const std::size_t n = signal.size();
  const double bin = n == 0 ? sample_rate : sample_rate / static_cast<double>(n);
  if (fundamental < bin) {
    throw std::invalid_argument("harmonic fit needs at least one fundamental cycle");
  }
  std::size_t count = 0;
  while (count < harmonics &&
         fundamental * static_cast<double>(count + 1) < sample_rate * 0.5 - bin) {
    ++count;
  }
  if (count == 0) {
    throw std::invalid_argument("harmonic fit fundamental is within one bin of Nyquist");
  }
  const std::size_t terms = 2 * count + 1;
  if (n < 2 * terms) {
    throw std::invalid_argument("harmonic fit signal is too short for the model");
  }

  const double step = 2.0 * std::numbers::pi * fundamental / sample_rate;
  const double centre = 0.5 * static_cast<double>(n - 1);
  std::vector<double> basis(terms);
  const auto evaluate = [&](std::size_t index) {
    const std::complex<double> rotation =
        std::polar(1.0, step * (static_cast<double>(index) - centre));
    std::complex<double> harmonic = rotation;
    basis[0] = 1.0;
    for (std::size_t k = 0; k < count; ++k) {
      basis[1 + 2 * k] = harmonic.real();
      basis[2 + 2 * k] = harmonic.imag();
      harmonic *= rotation;
    }
  };

  std::vector<double> gram(terms * terms, 0.0);
  std::vector<double> coefficients(terms, 0.0);
  for (std::size_t index = 0; index < n; ++index) {
    evaluate(index);
    const double x = signal[index];
    for (std::size_t i = 0; i < terms; ++i) {
      coefficients[i] += basis[i] * x;
      for (std::size_t j = 0; j <= i; ++j) {
        gram[i * terms + j] += basis[i] * basis[j];
      }
    }
  }

  // Cholesky factorization; a pivot that loses almost all of its column's energy means that
  // basis function is (numerically) a combination of the earlier ones.
  for (std::size_t i = 0; i < terms; ++i) {
    for (std::size_t j = 0; j <= i; ++j) {
      double sum = gram[i * terms + j];
      for (std::size_t k = 0; k < j; ++k) {
        sum -= gram[i * terms + k] * gram[j * terms + k];
      }
      if (i == j) {
        const double original = gram[i * terms + i];
        if (!(sum > original * 1e-9)) {
          throw std::invalid_argument("harmonic fit basis is degenerate for this signal length");
        }
        gram[i * terms + i] = std::sqrt(sum);
      } else {
        gram[i * terms + j] = sum / gram[j * terms + j];
      }
    }
  }
  for (std::size_t i = 0; i < terms; ++i) {
    for (std::size_t k = 0; k < i; ++k) {
      coefficients[i] -= gram[i * terms + k] * coefficients[k];
    }
    coefficients[i] /= gram[i * terms + i];
  }
  for (std::size_t i = terms; i-- > 0;) {
    for (std::size_t k = i + 1; k < terms; ++k) {
      coefficients[i] -= gram[k * terms + i] * coefficients[k];
    }
    coefficients[i] /= gram[i * terms + i];
  }

  HarmonicFit fit;
  fit.dc = coefficients[0];
  for (std::size_t k = 0; k < count; ++k) {
    fit.amplitudes.push_back(std::hypot(coefficients[1 + 2 * k], coefficients[2 + 2 * k]));
  }
  double squares = 0.0;
  for (std::size_t index = 0; index < n; ++index) {
    evaluate(index);
    double model = 0.0;
    for (std::size_t i = 0; i < terms; ++i) {
      model += coefficients[i] * basis[i];
    }
    const double error = static_cast<double>(signal[index]) - model;
    squares += error * error;
  }
  fit.residual_rms = std::sqrt(squares / static_cast<double>(n));
  return fit;
}

// Ratio of the RSS of fitted harmonics 2..N to the fitted fundamental amplitude; infinite when the
// fundamental is absent or the fit is empty.
inline double harmonic_distortion(const HarmonicFit &fit) {
  if (fit.amplitudes.empty() || !(fit.amplitudes[0] > 0.0)) {
    return std::numeric_limits<double>::infinity();
  }
  double power = 0.0;
  for (std::size_t k = 1; k < fit.amplitudes.size(); ++k) {
    power += fit.amplitudes[k] * fit.amplitudes[k];
  }
  return std::sqrt(power) / fit.amplitudes[0];
}

// Same validity limits as fit_harmonics().
inline double harmonic_distortion(std::span<const float> signal, double sample_rate,
                                  double fundamental, std::size_t harmonics = 5) {
  return harmonic_distortion(fit_harmonics(signal, sample_rate, fundamental, harmonics));
}

// THD+N from a fit that models only DC and the fundamental (harmonics = 1): the residual RMS
// relative to the fundamental's RMS; infinite when the fundamental is absent. A fit that also
// models harmonics would exclude them from the residual, so it is rejected.
inline double thd_plus_noise(const HarmonicFit &fundamental_fit) {
  if (fundamental_fit.amplitudes.size() != 1) {
    throw std::invalid_argument("THD+N needs a fit of the fundamental only");
  }
  if (!(fundamental_fit.amplitudes[0] > 0.0)) {
    return std::numeric_limits<double>::infinity();
  }
  return fundamental_fit.residual_rms / (fundamental_fit.amplitudes[0] / std::numbers::sqrt2);
}

// THD+N as the RMS of everything left after removing the best-fit DC and fundamental sinusoid,
// relative to the fundamental's RMS. Same validity limits as fit_harmonics().
inline double thd_plus_noise(std::span<const float> signal, double sample_rate,
                             double fundamental) {
  return thd_plus_noise(fit_harmonics(signal, sample_rate, fundamental, 1));
}

struct SpectralPeak {
  double frequency = 0.0;
  double amplitude = 0.0;
};

inline std::vector<SpectralPeak> spectral_peaks(std::span<const double> spectrum,
                                                double sample_rate, std::size_t count,
                                                double floor = 1e-7) {
  if (spectrum.size() < 2) {
    return {};
  }
  const std::size_t fft_size = (spectrum.size() - 1) * 2;
  std::vector<SpectralPeak> peaks;
  for (std::size_t k = 0; k < spectrum.size(); ++k) {
    const double left = k == 0 ? 0.0 : spectrum[k - 1];
    const double right = k + 1 == spectrum.size() ? 0.0 : spectrum[k + 1];
    if (spectrum[k] < floor || spectrum[k] < left || spectrum[k] <= right) {
      continue;
    }
    double offset = 0.0;
    if (k > 0 && k + 1 < spectrum.size() && left > 0.0 && right > 0.0) {
      const double a = std::log(left);
      const double b = std::log(spectrum[k]);
      const double c = std::log(right);
      const double denominator = a - 2.0 * b + c;
      if (denominator < 0.0) {
        offset = std::clamp(0.5 * (a - c) / denominator, -0.5, 0.5);
      }
    }
    peaks.push_back(
        {(static_cast<double>(k) + offset) * sample_rate / static_cast<double>(fft_size),
         spectrum[k]});
  }
  std::sort(peaks.begin(), peaks.end(),
            [](const SpectralPeak &a, const SpectralPeak &b) { return a.amplitude > b.amplitude; });
  if (peaks.size() > count) {
    peaks.resize(count);
  }
  return peaks;
}

enum class StageKind { gain, lowpass, tanh, delay, limit, tremolo, fade };

inline constexpr double lowpass_q = 1.0 / std::numbers::sqrt2;
inline constexpr double limit_attack_seconds = 0.001;
inline constexpr double limit_release_seconds = 0.1;

struct Stage {
  StageKind kind = StageKind::gain;
  double value = 0.0;
};

// Processing chain built from library primitives plus trivial gain and tanh arithmetic. Every
// stage keeps independent per-channel state and processes interleaved frames in place.
class Chain {
public:
  Chain(std::vector<Stage> stages, double sample_rate, std::size_t channels)
      : stages_(std::move(stages)), channels_(channels) {
    detail::require_rate(sample_rate);
    if (channels == 0) {
      throw std::invalid_argument("chain channel count must be positive");
    }
    for (const Stage &stage : stages_) {
      nodes_.push_back(prepare(stage, sample_rate, channels));
    }
    reset();
  }

  // Comma-separated stages: gain:<dB>, lowpass:<Hz>, tanh:<drive>, delay:<samples>,
  // limit:<threshold dB>, tremolo:<Hz>, fade:<seconds>, or identity.
  static Chain parse(std::string_view spec, double sample_rate, std::size_t channels) {
    static constexpr std::pair<std::string_view, StageKind> names[] = {
        {"gain", StageKind::gain},   {"lowpass", StageKind::lowpass},
        {"tanh", StageKind::tanh},   {"delay", StageKind::delay},
        {"limit", StageKind::limit}, {"tremolo", StageKind::tremolo},
        {"fade", StageKind::fade}};
    std::vector<Stage> stages;
    while (!spec.empty()) {
      const std::size_t comma = spec.find(',');
      const std::string_view item = spec.substr(0, comma);
      spec = comma == std::string_view::npos ? std::string_view{} : spec.substr(comma + 1);
      if (item == "identity") {
        continue;
      }
      const std::size_t colon = item.find(':');
      if (colon == std::string_view::npos) {
        throw std::invalid_argument("chain stage needs name:value: " + std::string(item));
      }
      const std::string_view name = item.substr(0, colon);
      const std::string text(item.substr(colon + 1));
      char *end = nullptr;
      const double value = std::strtod(text.c_str(), &end);
      if (text.empty() || end != text.c_str() + text.size() || !std::isfinite(value)) {
        throw std::invalid_argument("chain stage value is not a finite number: " +
                                    std::string(item));
      }
      const auto found = std::find_if(std::begin(names), std::end(names),
                                      [&](const auto &entry) { return entry.first == name; });
      if (found == std::end(names)) {
        throw std::invalid_argument("unknown chain stage: " + std::string(name));
      }
      stages.push_back({found->second, value});
    }
    return Chain(std::move(stages), sample_rate, channels);
  }

  std::size_t channels() const { return channels_; }
  const std::vector<Stage> &stages() const { return stages_; }

  std::string describe() const {
    if (stages_.empty()) {
      return "identity";
    }
    std::string text;
    for (const Stage &stage : stages_) {
      if (!text.empty()) {
        text += " -> ";
      }
      const std::string value = std::to_string(stage.value);
      switch (stage.kind) {
      case StageKind::gain:
        text += "gain " + value + " dB";
        break;
      case StageKind::lowpass:
        text += "Biquad lowpass " + value + " Hz";
        break;
      case StageKind::tanh:
        text += "tanh drive " + value;
        break;
      case StageKind::delay:
        text += "FractionalDelay " + value + " samples";
        break;
      case StageKind::limit:
        text += "EnvelopeFollower limit " + value + " dB";
        break;
      case StageKind::tremolo:
        text += "Lfo tremolo " + value + " Hz";
        break;
      case StageKind::fade:
        text += "LinearSmoother fade-in " + value + " s";
        break;
      }
    }
    return text;
  }

  void reset() {
    for (Node &node : nodes_) {
      for (auto &filter : node.filters) {
        filter.reset();
      }
      for (auto &line : node.delays) {
        line.reset();
      }
      for (auto &follower : node.envelopes) {
        follower.reset();
      }
      for (auto &lfo : node.lfos) {
        lfo.reset();
      }
      for (auto &smoother : node.smoothers) {
        smoother.reset(0.0);
        smoother.set_target(1.0);
      }
    }
  }

  void process(std::span<float> interleaved) {
    if (interleaved.size() % channels_ != 0) {
      throw std::invalid_argument("chain block is not a whole number of frames");
    }
    for (Node &node : nodes_) {
      switch (node.kind) {
      case StageKind::gain:
        for (float &sample : interleaved) {
          sample = static_cast<float>(sample * node.scale);
        }
        break;
      case StageKind::tanh:
        for (float &sample : interleaved) {
          sample = static_cast<float>(std::tanh(sample * node.drive) * node.scale);
        }
        break;
      case StageKind::lowpass:
        for (std::size_t i = 0; i < interleaved.size(); ++i) {
          interleaved[i] = node.filters[i % channels_].process(interleaved[i]);
        }
        break;
      case StageKind::delay:
        for (std::size_t i = 0; i < interleaved.size(); ++i) {
          interleaved[i] = node.delays[i % channels_].process(interleaved[i]);
        }
        break;
      case StageKind::limit:
        for (std::size_t i = 0; i < interleaved.size(); ++i) {
          const double envelope = node.envelopes[i % channels_].process(interleaved[i]);
          if (envelope > node.scale) {
            interleaved[i] = static_cast<float>(interleaved[i] * (node.scale / envelope));
          }
        }
        break;
      case StageKind::tremolo:
        for (std::size_t i = 0; i < interleaved.size(); ++i) {
          const double lfo = node.lfos[i % channels_].process();
          interleaved[i] = static_cast<float>(interleaved[i] * 0.5 * (1.0 + lfo));
        }
        break;
      case StageKind::fade:
        for (std::size_t i = 0; i < interleaved.size(); ++i) {
          interleaved[i] =
              static_cast<float>(interleaved[i] * node.smoothers[i % channels_].process());
        }
        break;
      }
    }
  }

private:
  struct Node {
    StageKind kind = StageKind::gain;
    double scale = 1.0;
    double drive = 1.0;
    std::vector<Biquad> filters;
    std::vector<FractionalDelay> delays;
    std::vector<EnvelopeFollower> envelopes;
    std::vector<Lfo> lfos;
    std::vector<LinearSmoother> smoothers;
  };

  static Node prepare(const Stage &stage, double sample_rate, std::size_t channels) {
    Node node;
    node.kind = stage.kind;
    switch (stage.kind) {
    case StageKind::gain:
      node.scale = std::pow(10.0, stage.value / 20.0);
      break;
    case StageKind::tanh:
      if (!(stage.value > 0.0)) {
        throw std::invalid_argument("tanh drive must be positive");
      }
      node.drive = stage.value;
      node.scale = 1.0 / std::tanh(stage.value);
      break;
    case StageKind::lowpass:
      node.filters.assign(channels,
                          Biquad(BiquadCoefficients::lowpass(sample_rate, stage.value, lowpass_q)));
      break;
    case StageKind::delay: {
      if (!(stage.value >= 0.0) || stage.value > static_cast<double>(maximum_delay_samples)) {
        throw std::invalid_argument("delay must be within the maximum delay length");
      }
      const DelayConfig config{sample_rate, static_cast<std::size_t>(std::ceil(stage.value)),
                               stage.value};
      node.delays.assign(channels, FractionalDelay(config));
      break;
    }
    case StageKind::limit:
      if (stage.value > 0.0) {
        throw std::invalid_argument("limit threshold must be at most 0 dB");
      }
      node.scale = std::pow(10.0, stage.value / 20.0);
      node.envelopes.assign(
          channels, EnvelopeFollower({sample_rate, limit_attack_seconds, limit_release_seconds}));
      break;
    case StageKind::tremolo:
      node.lfos.assign(channels, Lfo({sample_rate, stage.value, LfoWaveform::sine, 0.0}));
      break;
    case StageKind::fade: {
      LinearSmoother smoother;
      smoother.configure(sample_rate, stage.value);
      node.smoothers.assign(channels, smoother);
      break;
    }
    }
    return node;
  }

  std::vector<Stage> stages_;
  std::vector<Node> nodes_;
  std::size_t channels_;
};

inline void process_blocks(Chain &chain, std::span<float> interleaved, std::size_t block_frames) {
  if (block_frames == 0) {
    throw std::invalid_argument("block size must be positive");
  }
  const std::size_t block = block_frames * chain.channels();
  for (std::size_t start = 0; start < interleaved.size(); start += block) {
    chain.process(interleaved.subspan(start, std::min(block, interleaved.size() - start)));
  }
}

struct ThroughputResult {
  std::size_t frames = 0;
  std::size_t repeats = 0;
  double best_seconds = 0.0;
  double median_seconds = 0.0;
  double realtime_factor = 0.0;
  double nanoseconds_per_sample = 0.0;
  double checksum = 0.0;
};

inline ThroughputResult measure_throughput(Chain &chain, std::span<const float> input,
                                           double sample_rate, std::size_t block_frames,
                                           std::size_t repeats) {
  detail::require_rate(sample_rate);
  if (repeats == 0 || input.empty() || input.size() % chain.channels() != 0) {
    throw std::invalid_argument("throughput needs repeats and whole, non-empty frames");
  }
  std::vector<float> work(input.size());
  std::vector<double> timings;
  ThroughputResult result;
  result.frames = input.size() / chain.channels();
  result.repeats = repeats;
  for (std::size_t r = 0; r < repeats; ++r) {
    std::copy(input.begin(), input.end(), work.begin());
    chain.reset();
    const auto start = std::chrono::steady_clock::now();
    process_blocks(chain, work, block_frames);
    const auto stop = std::chrono::steady_clock::now();
    timings.push_back(std::chrono::duration<double>(stop - start).count());
    result.checksum += measure(work).rms;
  }
  std::sort(timings.begin(), timings.end());
  result.best_seconds = timings.front();
  result.median_seconds = timings[timings.size() / 2];
  const double audio_seconds = static_cast<double>(result.frames) / sample_rate;
  const double best = std::max(result.best_seconds, 1e-12);
  result.realtime_factor = audio_seconds / best;
  result.nanoseconds_per_sample = best * 1e9 / static_cast<double>(input.size());
  return result;
}

}

#endif
