#include "baseline_bench.hpp"
#include "bench.hpp"
#include "convolution_bench.hpp"
#include "delay_bench.hpp"
#include "filters_bench.hpp"
#include "wav.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace bench = vespera::dsp::bench;

constexpr const char *usage_text = R"(usage: vespera_dsp_bench <command> [options]

commands:
  generate --signal sine|sweep|impulse|noise --out PATH
      [--rate 48000] [--seconds 1] [--channels 1] [--format pcm16|pcm24|pcm32|float32]
      [--amplitude 0.5] [--frequency 1000] [--end-frequency 20000] [--seed 1]
      [--coherent 1]   round sine frequency to a whole number of cycles
  analyze PATH [--fft 8192] [--window hann|rectangular] [--peaks 5]
      [--probe HZ[,HZ...]] [--tone HZ] [--harmonics 5]
      Prints level metrics, the averaged spectrum's in-band range, strongest peaks and
      probed levels and, with --tone, a least-squares DC + harmonic fit per channel:
      tone amplitude, THD over the fitted harmonics, and THD+N as the residual after
      removing the fitted DC and fundamental. Needs at least one full cycle of the tone.
      Use --window rectangular for impulse responses (Hann zeroes an impulse at sample 0).
  process IN OUT [--chain SPEC] [--block 256] [--format FORMAT]
      Runs the chain over a WAV file and writes the result.
  benchmark [--chain SPEC] [--rate 48000] [--channels 2] [--seconds 10] [--block 256]
      [--repeats 5] [--input PATH]
      Measures chain throughput on white noise or on the given WAV file.
  convolution [--rates 44100,48000,96000] [--channels 1,2] [--blocks 16,64,128,512]
      [--lengths 0.02,0.1,0.2,0.5,2] [--seconds 2] [--repeats 3] [--switching 0]
      [--direct-limit 0.2]
      Times vespera::dsp::Convolver on white noise with synthetic responses of each
      length in seconds, as a percentage of real time: the fastest repeat's average, the
      99.9th percentile of its block times and its slowest single block (which includes
      preemption by other processes). --switching 1 requests a new response whenever the last
      switch has finished, so most blocks are priming or crossfading. Responses up to
      --direct-limit seconds are also timed with vectorized time-domain convolution.
  baseline [--rates 44100,48000,96000] [--channels 1,2]
      [--blocks 1,16,64,128,512] [--mixed 1] [--scenarios steady,moving,silent,reset]
      [--lengths 0.02,0.2,2] [--seconds 2] [--repeats 2] [--chain 1] [--convolution 1]
      [--memory 1] [--reset-count 32]
      M1 repeatable baseline. Prints the compiler, CPU and build, then one row per case with
      the average, 99.9th percentile and slowest block as percentages of real time, plus the
      sampled load average. The chain rows run a fixed delay/biquad/limiter/gain/tremolo graph
      per channel; 'moving' retargets controls every block, 'silent' processes digital silence,
      'reset' resets every 32nd block. The convolution rows map the same scenario names to
      steady, continuously switching, silent input and reset cost (reset rows report seconds
      per reset instead of percentages). --mixed 1 adds a block size cycling through the
      listed sizes. --memory 1 measures allocator bytes retained by construction.
  delay [--rates 48000] [--capacities 0.2] [--taps 4] [--blocks 16,64,128] [--seconds 2]
      [--repeats 3] [--modes steady,modulated,crossfade] [--kernels linear,lagrange3]
      [--reset 1] [--allpasses 1]
      Times multi-tap delay reading on white noise as a percentage of real time. Each
      configuration is measured three ways: one shared HistoryBuffer with linear reads, one
      FractionalDelay instance per tap, and the shared buffer with third-order Lagrange reads
      (the separate instances only interpolate linearly, so --kernels filters their rows).
      steady reads fixed fractional delays, modulated sweeps every tap around three
      quarters of its base delay at 0.5 Hz, and crossfade blends two fixed taps with a slowly
      moving weight. --reset 1 adds a table of reset cost and storage: the shared row times
      HistoryBuffer::reset and the separate row times the identical std::fill over one
      max_delay + 2 buffer, because FractionalDelay's single-cursor API cannot make repeated
      full clears observable. --allpasses 1 adds steady Schroeder and first-order allpass
      rows. The modulation and blend signals are precomputed, so only the delay machinery is
      measured.
  filters [--rates 44100,48000,96000] [--channels 1,2] [--blocks 16,64,128,512]
      [--seconds 2] [--repeats 3] [--low-cutoff 100] [--high-cutoff 8000]
      Times the state-variable filter (steady and with cutoff/Q moving every sample) and
      the Linkwitz-Riley crossover on white noise, as a percentage of real time: the fastest
      repeat's average, the 99.9th percentile of its block times and its slowest single
      block. Also prints nanoseconds per sample with fixed controls and with an SVF
      try_set_controls or crossover configure before every sample, on distinct controls.
      These are measurements of this build and machine, not guarantees.

chain SPEC is comma-separated stages applied in order to every channel:
  identity            no processing
  gain:<dB>           static gain
  lowpass:<Hz>        vespera::dsp::Biquad Butterworth lowpass
  delay:<samples>     vespera::dsp::FractionalDelay
  limit:<dB>          vespera::dsp::EnvelopeFollower driven gain limiter (threshold <= 0)
  tremolo:<Hz>        vespera::dsp::Lfo sine amplitude modulation
  fade:<seconds>      vespera::dsp::LinearSmoother fade-in from silence
  tanh:<drive>        tanh saturation normalized to unity at full scale
  default: gain:-6,lowpass:8000,delay:12.5,limit:-12,tremolo:4,fade:0.01,tanh:2
)";

constexpr std::string_view default_chain =
    "gain:-6,lowpass:8000,delay:12.5,limit:-12,tremolo:4,fade:0.01,tanh:2";

class UsageError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

struct Arguments {
  std::vector<std::string> positional;
  std::map<std::string, std::string, std::less<>> options;

  std::string text(std::string_view name, std::string_view fallback) const {
    const auto found = options.find(name);
    return found == options.end() ? std::string(fallback) : found->second;
  }

  double number(std::string_view name, double fallback) const {
    const auto found = options.find(name);
    if (found == options.end()) {
      return fallback;
    }
    char *end = nullptr;
    const double value = std::strtod(found->second.c_str(), &end);
    if (found->second.empty() || end != found->second.c_str() + found->second.size() ||
        !std::isfinite(value)) {
      throw UsageError("--" + std::string(name) + " needs a finite number");
    }
    return value;
  }

  std::size_t count(std::string_view name, std::size_t fallback, std::size_t minimum,
                    std::size_t maximum) const {
    const double value = number(name, static_cast<double>(fallback));
    if (value != std::floor(value) || value < static_cast<double>(minimum) ||
        value > static_cast<double>(maximum)) {
      throw UsageError("--" + std::string(name) + " must be an integer in [" +
                       std::to_string(minimum) + ", " + std::to_string(maximum) + "]");
    }
    return static_cast<std::size_t>(value);
  }
};

Arguments parse_arguments(int argc, char **argv, const std::set<std::string_view> &allowed) {
  Arguments arguments;
  for (int i = 2; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (!arg.starts_with("--")) {
      arguments.positional.emplace_back(arg);
      continue;
    }
    const std::string_view name = arg.substr(2);
    if (!allowed.contains(name)) {
      throw UsageError("unknown option " + std::string(arg));
    }
    if (i + 1 >= argc) {
      throw UsageError(std::string(arg) + " needs a value");
    }
    arguments.options[std::string(name)] = argv[++i];
  }
  return arguments;
}

std::size_t frames_for(double rate, double seconds) {
  if (seconds <= 0.0) {
    throw UsageError("--seconds must be positive");
  }
  const double frames = std::round(rate * seconds);
  if (frames > static_cast<double>(bench::WavLimits{}.max_frames)) {
    throw UsageError("signal is longer than the frame limit");
  }
  return std::max<std::size_t>(1, static_cast<std::size_t>(frames));
}

std::uint32_t sample_rate_option(const Arguments &arguments) {
  return static_cast<std::uint32_t>(arguments.count("rate", 48000, bench::wav_minimum_sample_rate,
                                                    bench::wav_maximum_sample_rate));
}

bench::SampleFormat format_option(const Arguments &arguments, bench::SampleFormat fallback) {
  const std::string name = arguments.text("format", bench::format_name(fallback));
  const auto format = bench::parse_format_name(name);
  if (!format) {
    throw UsageError("unknown format " + name);
  }
  return *format;
}

std::vector<double> number_list(const Arguments &arguments, std::string_view name,
                                std::string_view fallback) {
  const std::string list = arguments.text(name, fallback);
  std::vector<double> values;
  std::size_t start = 0;
  while (start <= list.size()) {
    const std::size_t comma = std::min(list.find(',', start), list.size());
    const std::string item = list.substr(start, comma - start);
    char *end = nullptr;
    const double value = std::strtod(item.c_str(), &end);
    if (item.empty() || end != item.c_str() + item.size() || !(value > 0.0) ||
        !std::isfinite(value)) {
      throw UsageError("--" + std::string(name) + " needs comma-separated positive numbers");
    }
    values.push_back(value);
    start = comma + 1;
  }
  return values;
}

std::vector<std::string> split_list(const std::string &list) {
  std::vector<std::string> items;
  std::size_t start = 0;
  while (start <= list.size()) {
    const std::size_t comma = std::min(list.find(',', start), list.size());
    const std::string item = list.substr(start, comma - start);
    if (!item.empty()) {
      items.push_back(item);
    }
    start = comma + 1;
  }
  return items;
}

std::string db_text(double amplitude) {
  if (!(amplitude > 0.0)) {
    return "-inf";
  }
  char buffer[32];
  std::snprintf(buffer, sizeof buffer, "%.2f", bench::to_db(amplitude));
  return buffer;
}

std::vector<double> probe_frequencies(const std::string &list, double sample_rate) {
  std::vector<double> frequencies;
  std::size_t start = 0;
  while (start < list.size()) {
    const std::size_t comma = std::min(list.find(',', start), list.size());
    const std::string item = list.substr(start, comma - start);
    char *end = nullptr;
    const double value = std::strtod(item.c_str(), &end);
    if (item.empty() || end != item.c_str() + item.size() || !(value >= 0.0) ||
        value > sample_rate * 0.5) {
      throw UsageError("--probe needs comma-separated frequencies in [0, Nyquist]");
    }
    frequencies.push_back(value);
    start = comma + 1;
  }
  return frequencies;
}

int generate(const Arguments &arguments) {
  if (!arguments.positional.empty()) {
    throw UsageError("generate takes no positional arguments");
  }
  const std::string out = arguments.text("out", "");
  if (out.empty()) {
    throw UsageError("generate needs --out PATH");
  }
  const std::uint32_t rate = sample_rate_option(arguments);
  const double sample_rate = rate;
  const std::size_t frames = frames_for(sample_rate, arguments.number("seconds", 1.0));
  const double amplitude = arguments.number("amplitude", 0.5);
  const std::string signal = arguments.text("signal", "sine");
  double frequency = arguments.number("frequency", 1000.0);

  std::vector<float> mono;
  if (signal == "sine") {
    if (arguments.number("coherent", 0.0) != 0.0) {
      frequency = bench::coherent_frequency(sample_rate, frames, frequency);
    }
    mono = bench::sine(sample_rate, frequency, amplitude, frames);
  } else if (signal == "sweep") {
    mono = bench::log_sweep(sample_rate, arguments.number("frequency", 20.0),
                            arguments.number("end-frequency", 20000.0), amplitude, frames);
  } else if (signal == "impulse") {
    mono = bench::impulse(frames, 0, amplitude);
  } else if (signal == "noise") {
    mono =
        bench::white_noise(frames, amplitude, arguments.count("seed", 1, 0, std::size_t{1} << 52));
  } else {
    throw UsageError("unknown signal " + signal);
  }

  bench::WavAudio audio;
  audio.sample_rate = rate;
  audio.channels = static_cast<std::uint16_t>(arguments.count("channels", 1, 1, 2));
  audio.format = format_option(arguments, bench::SampleFormat::float32);
  audio.samples = bench::interleave(mono, audio.channels);
  bench::write_wav_file(out, audio);
  std::printf("wrote %s: %s %s, %u Hz, %u channel(s), %zu frames", out.c_str(), signal.c_str(),
              std::string(bench::format_name(audio.format)).c_str(), rate, audio.channels, frames);
  if (signal == "sine") {
    std::printf(", %.6f Hz", frequency);
  }
  std::printf("\n");
  return 0;
}

int analyze(const Arguments &arguments) {
  if (arguments.positional.size() != 1) {
    throw UsageError("analyze needs exactly one WAV path");
  }
  const bench::WavAudio audio = bench::read_wav_file(arguments.positional[0]);
  const double sample_rate = audio.sample_rate;
  const std::size_t requested = arguments.count("fft", 8192, 2, std::size_t{1} << 22);
  if (!bench::is_power_of_two(requested)) {
    throw UsageError("--fft must be a power of two");
  }
  const std::string window_name = arguments.text("window", "hann");
  if (window_name != "hann" && window_name != "rectangular") {
    throw UsageError("unknown window " + window_name);
  }
  const bench::Window window =
      window_name == "hann" ? bench::Window::hann : bench::Window::rectangular;
  const std::size_t peak_count = arguments.count("peaks", 5, 0, 100);
  const double tone = arguments.number("tone", 0.0);
  const std::size_t harmonics = arguments.count("harmonics", 5, 2, 64);
  const std::vector<double> probes = probe_frequencies(arguments.text("probe", ""), sample_rate);

  std::printf("%s: %s, %u Hz, %u channel(s), %zu frames (%.6f s)\n",
              arguments.positional[0].c_str(),
              std::string(bench::format_name(audio.format)).c_str(), audio.sample_rate,
              audio.channels, audio.frames(), static_cast<double>(audio.frames()) / sample_rate);

  for (std::size_t c = 0; c < audio.channels; ++c) {
    const std::vector<float> samples = audio.channel(c);
    const bench::SignalStats stats = bench::measure(samples);
    std::printf("channel %zu: rms %s dBFS, peak %s dBFS, dc %+.6g, crest %.2f dB\n", c,
                db_text(stats.rms).c_str(), db_text(stats.peak).c_str(), stats.dc,
                stats.rms > 0.0 ? bench::to_db(stats.peak / stats.rms) : 0.0);

    std::size_t fft = requested;
    while (fft > samples.size()) {
      fft >>= 1;
    }
    if (fft >= 2) {
      std::vector<double> power(fft / 2 + 1, 0.0);
      const std::size_t segments = samples.size() / fft;
      for (std::size_t s = 0; s < segments; ++s) {
        const auto spectrum =
            bench::amplitude_spectrum(std::span(samples).subspan(s * fft, fft), window);
        for (std::size_t k = 0; k < power.size(); ++k) {
          power[k] += spectrum[k] * spectrum[k] / static_cast<double>(segments);
        }
      }
      for (double &value : power) {
        value = std::sqrt(value);
      }
      std::printf("  spectrum: fft %zu %s, %zu segment(s), %.3f Hz/bin\n", fft, window_name.c_str(),
                  segments, sample_rate / static_cast<double>(fft));
      if (power.size() > 2) {
        const auto [low, high] = std::minmax_element(power.begin() + 1, power.end() - 1);
        std::printf("    range %s .. %s dBFS excluding DC and Nyquist\n", db_text(*low).c_str(),
                    db_text(*high).c_str());
      }
      for (const auto &peak : bench::spectral_peaks(power, sample_rate, peak_count)) {
        std::printf("    peak  %12.3f Hz  %8s dBFS\n", peak.frequency,
                    db_text(peak.amplitude).c_str());
      }
      for (double frequency : probes) {
        const auto bin = static_cast<std::size_t>(
            std::lround(frequency * static_cast<double>(fft) / sample_rate));
        std::printf("    probe %12.3f Hz  %8s dBFS (bin %.3f Hz)\n", frequency,
                    db_text(power[std::min(bin, power.size() - 1)]).c_str(),
                    bench::bin_frequency(bin, fft, sample_rate));
      }
    }

    if (tone > 0.0) {
      try {
        const bench::HarmonicFit fit = bench::fit_harmonics(samples, sample_rate, tone, harmonics);
        const double thd = bench::harmonic_distortion(fit);
        const double thd_n = bench::thd_plus_noise(
            fit.amplitudes.size() == 1 ? fit : bench::fit_harmonics(samples, sample_rate, tone, 1));
        std::printf("  tone %.3f Hz (least-squares fit, %zu harmonic(s)): amplitude %s dBFS, "
                    "dc %+.6g, thd %s dB (%.6f%%), thd+n %s dB\n",
                    tone, fit.amplitudes.size(), db_text(fit.amplitudes[0]).c_str(), fit.dc,
                    db_text(thd).c_str(), thd * 100.0, db_text(thd_n).c_str());
      } catch (const std::invalid_argument &error) {
        std::printf("  tone %.3f Hz: not measurable: %s\n", tone, error.what());
      }
    }
  }
  return 0;
}

int process(const Arguments &arguments) {
  if (arguments.positional.size() != 2) {
    throw UsageError("process needs IN and OUT WAV paths");
  }
  bench::WavAudio audio = bench::read_wav_file(arguments.positional[0]);
  bench::Chain chain = bench::Chain::parse(arguments.text("chain", default_chain),
                                           audio.sample_rate, audio.channels);
  const std::size_t block = arguments.count("block", 256, 1, std::size_t{1} << 20);
  const auto start = std::chrono::steady_clock::now();
  bench::process_blocks(chain, audio.samples, block);
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  audio.format = format_option(arguments, audio.format);
  bench::write_wav_file(arguments.positional[1], audio);
  std::printf("processed %zu frames through %s in %.6f s (%.1fx realtime) -> %s\n", audio.frames(),
              chain.describe().c_str(), seconds,
              static_cast<double>(audio.frames()) / audio.sample_rate / std::max(seconds, 1e-12),
              arguments.positional[1].c_str());
  return 0;
}

int benchmark(const Arguments &arguments) {
  if (!arguments.positional.empty()) {
    throw UsageError("benchmark takes no positional arguments; use --input PATH");
  }
  std::vector<float> input;
  std::uint32_t rate = 0;
  std::size_t channels = 0;
  const std::string path = arguments.text("input", "");
  if (path.empty()) {
    rate = sample_rate_option(arguments);
    channels = arguments.count("channels", 2, 1, 2);
    const std::size_t frames = frames_for(rate, arguments.number("seconds", 10.0));
    input.resize(frames * channels);
    const std::vector<float> left = bench::white_noise(frames, 0.5, 1);
    const std::vector<float> right = bench::white_noise(frames, 0.5, 2);
    for (std::size_t n = 0; n < frames; ++n) {
      input[n * channels] = left[n];
      if (channels == 2) {
        input[n * channels + 1] = right[n];
      }
    }
  } else {
    bench::WavAudio audio = bench::read_wav_file(path);
    rate = audio.sample_rate;
    channels = audio.channels;
    input = std::move(audio.samples);
    if (input.empty()) {
      throw UsageError("benchmark input has no frames");
    }
  }
  bench::Chain chain = bench::Chain::parse(arguments.text("chain", default_chain), rate, channels);
  const std::size_t block = arguments.count("block", 256, 1, std::size_t{1} << 20);
  const std::size_t repeats = arguments.count("repeats", 5, 1, 1000);
  const auto result = bench::measure_throughput(chain, input, rate, block, repeats);
  std::printf("chain: %s\n", chain.describe().c_str());
  std::printf("input: %s, %u Hz, %zu channel(s), %zu frames, block %zu, %zu repeat(s)\n",
              path.empty() ? "white noise" : path.c_str(), rate, channels, result.frames, block,
              repeats);
  std::printf("best %.6f s, median %.6f s, %.3g ns/sample, %.1fx realtime (checksum %.6f)\n",
              result.best_seconds, result.median_seconds, result.nanoseconds_per_sample,
              result.realtime_factor, result.checksum);
  return 0;
}

int convolution(const Arguments &arguments) {
  if (!arguments.positional.empty()) {
    throw UsageError("convolution takes no positional arguments");
  }
  const std::vector<double> rates = number_list(arguments, "rates", "44100,48000,96000");
  const std::vector<double> channel_counts = number_list(arguments, "channels", "1,2");
  const std::vector<double> blocks = number_list(arguments, "blocks", "16,64,128,512");
  const std::vector<double> lengths = number_list(arguments, "lengths", "0.02,0.1,0.2,0.5,2");
  const double seconds = arguments.number("seconds", 2.0);
  const std::size_t repeats = arguments.count("repeats", 3, 1, 1000);
  const bool switching = arguments.count("switching", 0, 0, 1) == 1;
  const double direct_limit = arguments.number("direct-limit", 0.2);
  if (!(seconds > 0.0) || seconds > 600.0) {
    throw UsageError("--seconds must be within (0, 600]");
  }
  for (double count : channel_counts) {
    if (count != 1.0 && count != 2.0) {
      throw UsageError("--channels takes 1 and/or 2");
    }
  }
  for (double block : blocks) {
    if (block != std::floor(block) || block > 65536.0) {
      throw UsageError("--blocks takes whole frame counts up to 65536");
    }
  }
  std::printf("%s, %.3g s of white noise per case, best of %zu\n",
              switching ? "switching continuously" : "steady response", seconds, repeats);
  std::printf("%7s %2s %7s %7s %5s %8s %8s %8s %8s  %s\n", "rate", "ch", "IR s", "frames", "block",
              "avg %", "p99.9 %", "worst %", "direct %", "layout (head, partition x count)");
  for (double rate : rates) {
    for (double count : channel_counts) {
      for (double length : lengths) {
        for (double block : blocks) {
          bench::ConvolutionCase test;
          test.sample_rate = rate;
          test.channels = static_cast<std::size_t>(count);
          test.response_seconds = length;
          test.block_frames = static_cast<std::size_t>(block);
          test.audio_seconds = seconds;
          test.repeats = repeats;
          test.switching = switching;
          const bench::ConvolutionMeasurement result = bench::measure_convolution(test);
          std::string direct = "-";
          if (length <= direct_limit && block == blocks.front()) {
            char text[32];
            std::snprintf(text, sizeof text, "%.2f",
                          bench::measure_direct_convolution(test).average_percent);
            direct = text;
          }
          std::string layout = std::to_string(result.layout.head_frames);
          for (const auto &level : result.layout.active_levels()) {
            layout += " " + std::to_string(level.partition_frames) + "x" +
                      std::to_string(level.partitions);
          }
          std::printf("%7.0f %2zu %7.3g %7zu %5zu %8.2f %8.2f %8.2f %8s  %s\n", rate, test.channels,
                      length, result.response_frames, test.block_frames, result.average_percent,
                      result.percentile_block_percent, result.worst_block_percent, direct.c_str(),
                      layout.c_str());
          std::fflush(stdout);
        }
      }
    }
  }
  return 0;
}

int baseline(const Arguments &arguments) {
  if (!arguments.positional.empty()) {
    throw UsageError("baseline takes no positional arguments");
  }
  const std::vector<double> rates = number_list(arguments, "rates", "44100,48000,96000");
  const std::vector<double> channel_counts = number_list(arguments, "channels", "1,2");
  const std::vector<double> blocks = number_list(arguments, "blocks", "1,16,64,128,512");
  const std::vector<double> lengths = number_list(arguments, "lengths", "0.02,0.2,2");
  const double seconds = arguments.number("seconds", 2.0);
  const std::size_t repeats = arguments.count("repeats", 2, 1, 1000);
  const bool with_chain = arguments.count("chain", 1, 0, 1) == 1;
  const bool with_convolution = arguments.count("convolution", 1, 0, 1) == 1;
  const bool with_memory = arguments.count("memory", 1, 0, 1) == 1;
  const bool mixed = arguments.count("mixed", 1, 0, 1) == 1;
  const std::size_t reset_count = arguments.count("reset-count", 32, 1, 100000);
  if (!(seconds > 0.0) || seconds > 600.0) {
    throw UsageError("--seconds must be within (0, 600]");
  }
  for (double count : channel_counts) {
    if (count != 1.0 && count != 2.0) {
      throw UsageError("--channels takes 1 and/or 2");
    }
  }
  for (double block : blocks) {
    if (block != std::floor(block) || block > 65536.0 || block < 1.0) {
      throw UsageError("--blocks takes whole frame counts in [1, 65536]");
    }
  }
  const std::map<std::string, bench::ToolkitWorkload> workload_names = {
      {"steady", bench::ToolkitWorkload::steady},
      {"moving", bench::ToolkitWorkload::moving},
      {"silent", bench::ToolkitWorkload::silent},
      {"reset", bench::ToolkitWorkload::reset}};
  std::vector<bench::ToolkitWorkload> workloads;
  for (const std::string &name :
       split_list(arguments.text("scenarios", "steady,moving,silent,reset"))) {
    const auto found = workload_names.find(name);
    if (found == workload_names.end()) {
      throw UsageError("unknown scenario " + name);
    }
    workloads.push_back(found->second);
  }

  std::printf("vespera_dsp_bench baseline: %s\n", bench::environment_report().c_str());
  std::printf("load average at start: %.2f\n", bench::detail::baseline_load_average());

  if (with_chain) {
    std::printf("\ntoolkit chain scenarios (%.3g s audio, best of %zu)\n", seconds, repeats);
    std::printf("%7s %2s %6s %8s %8s %8s %8s %8s %5s  %s\n", "rate", "ch", "block", "scenario",
                "avg %", "p99.9 %", "max %", "blocks", "load", "checksum");
    for (double rate : rates) {
      for (double count : channel_counts) {
        for (double block : blocks) {
          for (bool mixed_case :
               (mixed ? std::vector<bool>{false, true} : std::vector<bool>{false})) {
            for (bench::ToolkitWorkload workload : workloads) {
              bench::ToolkitCase test;
              test.sample_rate = rate;
              test.channels = static_cast<std::size_t>(count);
              test.block_frames = static_cast<std::size_t>(block);
              test.audio_seconds = seconds;
              test.repeats = repeats;
              test.workload = workload;
              test.mixed_blocks = mixed_case;
              const bench::ToolkitMeasurement result = bench::measure_toolkit_case(test);
              const auto name =
                  std::find_if(workload_names.begin(), workload_names.end(),
                               [workload](const auto &entry) { return entry.second == workload; });
              char block_text[16];
              if (mixed_case) {
                std::snprintf(block_text, sizeof block_text, "mixed");
              } else {
                std::snprintf(block_text, sizeof block_text, "%zu", test.block_frames);
              }
              std::printf("%7.0f %2zu %6s %8s %8.3f %8.3f %8.3f %8zu %5.2f  %.6f\n", rate,
                          test.channels, block_text, name->first.c_str(), result.average_percent,
                          result.percentile_block_percent, result.worst_block_percent,
                          result.blocks, bench::detail::baseline_load_average(), result.checksum);
              std::fflush(stdout);
            }
          }
        }
      }
    }
  }

  if (with_convolution) {
    std::printf("\nconvolution scenarios (%.3g s audio, best of %zu)\n", seconds, repeats);
    std::printf("%7s %2s %7s %6s %9s %8s %8s %8s %5s  %s\n", "rate", "ch", "IR s", "block",
                "scenario", "avg %", "p99.9 %", "max %", "load", "layout");
    for (double rate : rates) {
      for (double count : channel_counts) {
        for (double length : lengths) {
          for (double block : blocks) {
            for (bool mixed_case :
                 (mixed ? std::vector<bool>{false, true} : std::vector<bool>{false})) {
              for (bench::ToolkitWorkload workload : workloads) {
                bench::ConvolutionCase test;
                test.sample_rate = rate;
                test.channels = static_cast<std::size_t>(count);
                test.response_seconds = length;
                test.block_frames = static_cast<std::size_t>(block);
                test.audio_seconds = seconds;
                test.repeats = repeats;
                test.mixed_blocks = mixed_case;
                const char *scenario = "steady";
                if (workload == bench::ToolkitWorkload::moving) {
                  test.switching = true;
                  scenario = "switch";
                } else if (workload == bench::ToolkitWorkload::silent) {
                  test.silent_input = true;
                  scenario = "silent";
                } else if (workload == bench::ToolkitWorkload::reset) {
                  continue;
                }
                const bench::ConvolutionMeasurement result = bench::measure_convolution(test);
                std::string layout = std::to_string(result.layout.head_frames);
                for (const auto &level : result.layout.active_levels()) {
                  layout += " " + std::to_string(level.partition_frames) + "x" +
                            std::to_string(level.partitions);
                }
                char block_text[16];
                if (mixed_case) {
                  std::snprintf(block_text, sizeof block_text, "mixed");
                } else {
                  std::snprintf(block_text, sizeof block_text, "%zu", test.block_frames);
                }
                std::printf("%7.0f %2zu %7.3g %6s %9s %8.2f %8.2f %8.2f %5.2f  %s\n", rate,
                            test.channels, length, block_text, scenario, result.average_percent,
                            result.percentile_block_percent, result.worst_block_percent,
                            bench::detail::baseline_load_average(), layout.c_str());
                std::fflush(stdout);
              }
            }
          }
        }
      }
    }

    bool has_reset = false;
    for (bench::ToolkitWorkload workload : workloads) {
      has_reset = has_reset || workload == bench::ToolkitWorkload::reset;
    }
    if (has_reset) {
      std::printf("\nconvolution reset work (%zu resets per case)\n", reset_count);
      std::printf("%7s %2s %7s %7s %12s %12s %5s\n", "rate", "ch", "IR s", "capacity", "us/reset",
                  "us/1kframe", "load");
      for (double rate : rates) {
        for (double count : channel_counts) {
          for (double length : lengths) {
            bench::ConvolutionCase test;
            test.sample_rate = rate;
            test.channels = static_cast<std::size_t>(count);
            test.response_seconds = length;
            const bench::ResetMeasurement result =
                bench::measure_convolution_reset(test, reset_count);
            std::printf("%7.0f %2zu %7.3g %7zu %12.1f %12.1f %5.2f\n", rate, test.channels, length,
                        result.capacity_frames, 1e6 * result.seconds_per_reset,
                        1e6 * result.seconds_per_reset * 1000.0 /
                            static_cast<double>(result.capacity_frames),
                        bench::detail::baseline_load_average());
            std::fflush(stdout);
          }
        }
      }
    }
  }

  if (with_memory) {
    std::printf("\nconstruction memory (allocator bytes in use)\n");
    std::printf("%7s %2s %7s %14s %14s\n", "rate", "ch", "IR s", "2 responses", "convolver");
    for (double rate : rates) {
      for (double count : channel_counts) {
        for (double length : lengths) {
          bench::ConvolutionCase test;
          test.sample_rate = rate;
          test.channels = static_cast<std::size_t>(count);
          test.response_seconds = length;
          const bench::ConstructionMemory result = bench::measure_construction_memory(test);
          if (!result.measured) {
            std::printf("%7.0f %2zu %7.3g %14s %14s\n", rate, test.channels, length, "unmeasured",
                        "unmeasured");
          } else {
            std::printf("%7.0f %2zu %7.3g %14zu %14zu\n", rate, test.channels, length,
                        static_cast<std::size_t>(result.response_bytes),
                        static_cast<std::size_t>(result.convolver_bytes));
          }
          std::fflush(stdout);
        }
      }
    }
  }
  std::printf("\nload average at end: %.2f\n", bench::detail::baseline_load_average());
  return 0;
}

int delay(const Arguments &arguments) {
  if (!arguments.positional.empty()) {
    throw UsageError("delay takes no positional arguments");
  }
  const std::vector<double> rates = number_list(arguments, "rates", "48000");
  const std::vector<double> capacities = number_list(arguments, "capacities", "0.2");
  const std::vector<double> tap_counts = number_list(arguments, "taps", "4");
  const std::vector<double> blocks = number_list(arguments, "blocks", "16,64,128");
  const std::vector<double> seconds = number_list(arguments, "seconds", "2");
  const std::size_t repeats = arguments.count("repeats", 3, 1, 1000);
  const std::string modes_text = arguments.text("modes", "steady,modulated,crossfade");
  const std::string kernels_text = arguments.text("kernels", "linear,lagrange3");
  const bool reset = arguments.count("reset", 1, 0, 1) == 1;
  const bool allpasses = arguments.count("allpasses", 1, 0, 1) == 1;
  const double audio_seconds = seconds.front();
  if (seconds.size() != 1 || !(audio_seconds > 0.0) || audio_seconds > 600.0) {
    throw UsageError("--seconds takes one value within (0, 600]");
  }
  for (double taps : tap_counts) {
    if (taps != std::floor(taps) || taps < 1.0 || taps > 16.0) {
      throw UsageError("--taps takes whole counts within [1, 16]");
    }
  }
  for (double block : blocks) {
    if (block != std::floor(block) || block < 1.0 || block > 65536.0) {
      throw UsageError("--blocks takes whole frame counts up to 65536");
    }
  }
  const auto names = [](const std::string &list, std::string_view option,
                        const std::set<std::string_view> &allowed) {
    std::set<std::string> chosen;
    std::size_t start = 0;
    while (start <= list.size()) {
      const std::size_t comma = std::min(list.find(',', start), list.size());
      const std::string item = list.substr(start, comma - start);
      if (!allowed.contains(item)) {
        throw UsageError("--" + std::string(option) + " has unknown item '" + item + "'");
      }
      chosen.insert(item);
      start = comma + 1;
    }
    return chosen;
  };
  const std::set<std::string> mode_names =
      names(modes_text, "modes", {"steady", "modulated", "crossfade"});
  std::vector<bench::DelayMode> modes;
  if (mode_names.contains("steady")) {
    modes.push_back(bench::DelayMode::steady);
  }
  if (mode_names.contains("modulated")) {
    modes.push_back(bench::DelayMode::modulated);
  }
  if (mode_names.contains("crossfade")) {
    modes.push_back(bench::DelayMode::crossfade);
  }
  const std::set<std::string> kernel_names =
      names(kernels_text, "kernels", {"linear", "lagrange3"});
  const bool linear = kernel_names.contains("linear");
  const bool lagrange = kernel_names.contains("lagrange3");

  std::printf("delay reads (crossfade rows always use 2 taps), %.3g s of white noise per case, "
              "best of %zu\n",
              audio_seconds, repeats);
  std::printf("%7s %7s %4s %5s %9s %8s %8s %8s %11s  %s\n", "rate", "cap s", "taps", "block",
              "mode", "avg %", "p99.9 %", "worst %", "bytes", "configuration");
  for (double rate : rates) {
    for (double capacity : capacities) {
      for (double taps : tap_counts) {
        for (double block : blocks) {
          for (const bench::DelayMode mode : modes) {
            const auto mode_name = mode == bench::DelayMode::steady      ? "steady"
                                   : mode == bench::DelayMode::modulated ? "modulated"
                                                                         : "crossfade";
            const auto run = [&](bool shared, bool lagrange_kernel) {
              bench::DelayCase test;
              test.sample_rate = rate;
              test.capacity_seconds = capacity;
              test.taps = static_cast<std::size_t>(taps);
              test.block_frames = static_cast<std::size_t>(block);
              test.audio_seconds = audio_seconds;
              test.repeats = repeats;
              test.mode = mode;
              test.shared = shared;
              test.lagrange = lagrange_kernel;
              return bench::measure_delay(test);
            };
            const auto print = [&](const bench::DelayMeasurement &result,
                                   const std::string &configuration) {
              const auto effective_taps =
                  static_cast<std::size_t>(mode == bench::DelayMode::crossfade ? 2.0 : taps);
              std::printf("%7.0f %7.3g %4zu %5zu %9s %8.2f %8.2f %8.2f %11zu  %s\n", rate, capacity,
                          effective_taps, static_cast<std::size_t>(block), mode_name,
                          result.average_percent, result.percentile_block_percent,
                          result.worst_block_percent, result.bytes, configuration.c_str());
              std::fflush(stdout);
            };
            if (linear) {
              print(run(true, false), "shared linear");
              print(run(false, false), "separate linear");
            }
            if (lagrange) {
              print(run(true, true), "shared lagrange3");
            }
          }
        }
      }
    }
  }

  if (reset) {
    std::printf("\nreset work, best of %zu repeats of %d in-place clears\n", repeats, 64);
    std::printf("%7s %9s %14s %18s\n", "rate", "cap s", "shared reset", "one separate clear");
    for (double rate : rates) {
      for (double capacity : capacities) {
        const bench::DelayResetMeasurement result =
            bench::measure_delay_reset(rate, capacity, repeats, 64);
        std::printf("%7.0f %9.3g %11.1f ns %17.1f ns  (%zu samples: %zu B shared, %zu B per "
                    "separate tap)\n",
                    rate, capacity, result.shared_nanoseconds, result.separate_nanoseconds,
                    result.capacity_samples, result.shared_bytes, result.separate_bytes_per_tap);
        std::fflush(stdout);
      }
    }
  }

  if (allpasses) {
    std::printf("\nsteady allpasses, %.3g s of white noise, best of %zu\n", audio_seconds, repeats);
    std::printf("%7s %18s %8s %11s\n", "rate", "section", "avg %", "rms");
    for (double rate : rates) {
      for (const std::size_t block :
           {static_cast<std::size_t>(blocks.front()), static_cast<std::size_t>(blocks.back())}) {
        for (const std::size_t allpass_delay :
             {std::size_t{1}, std::size_t{480}, std::size_t{4800}}) {
          const bench::AllpassMeasurement result =
              bench::measure_schroeder_allpass(rate, allpass_delay, block, audio_seconds, repeats);
          std::printf("%7.0f %13s D=%-4zu %8.3f %11.6f\n", rate, "schroeder", allpass_delay,
                      result.average_percent, result.checksum);
        }
        const bench::AllpassMeasurement first_order =
            bench::measure_first_order_allpass(rate, block, audio_seconds, repeats);
        std::printf("%7.0f %18s %8.3f %11.6f\n", rate, "first-order", first_order.average_percent,
                    first_order.checksum);
      }
      std::fflush(stdout);
    }
  }
  return 0;
}

int filters(const Arguments &arguments) {
  if (!arguments.positional.empty()) {
    throw UsageError("filters takes no positional arguments");
  }
  const std::vector<double> rates = number_list(arguments, "rates", "44100,48000,96000");
  const std::vector<double> channel_counts = number_list(arguments, "channels", "1,2");
  const std::vector<double> blocks = number_list(arguments, "blocks", "16,64,128,512");
  const double seconds = arguments.number("seconds", 2.0);
  const std::size_t repeats = arguments.count("repeats", 3, 1, 1000);
  const double low_cutoff = arguments.number("low-cutoff", 100.0);
  const double high_cutoff = arguments.number("high-cutoff", 8000.0);
  if (!(seconds > 0.0) || seconds > 600.0) {
    throw UsageError("--seconds must be within (0, 600]");
  }
  for (double count : channel_counts) {
    if (count != 1.0 && count != 2.0) {
      throw UsageError("--channels takes 1 and/or 2");
    }
  }
  for (double block : blocks) {
    if (block != std::floor(block) || block > 65536.0) {
      throw UsageError("--blocks takes whole frame counts up to 65536");
    }
  }
  std::printf("white noise per case, best of %zu\n", repeats);
  std::printf("%9s %2s %5s %-16s %8s %8s %8s  %s\n", "rate", "ch", "block", "mode", "avg %",
              "p99.9 %", "worst %", "checksum");
  for (double rate : rates) {
    for (double count : channel_counts) {
      for (double block : blocks) {
        const std::pair<const char *, bench::FilterMotion> modes[] = {
            {"svf steady", bench::FilterMotion::steady},
            {"svf moving", bench::FilterMotion::moving}};
        for (const auto &[name, motion] : modes) {
          bench::FilterCase test;
          test.sample_rate = rate;
          test.channels = static_cast<std::size_t>(count);
          test.block_frames = static_cast<std::size_t>(block);
          test.audio_seconds = seconds;
          test.repeats = repeats;
          test.motion = motion;
          test.low_cutoff = low_cutoff;
          test.high_cutoff = high_cutoff;
          const bench::FilterMeasurement result = bench::measure_svf(test);
          std::printf("%9.0f %2zu %5zu %-16s %8.3f %8.3f %8.3f  %.6f\n", rate, test.channels,
                      test.block_frames, name, result.average_percent,
                      result.percentile_block_percent, result.worst_block_percent, result.checksum);
          std::fflush(stdout);
        }
        bench::FilterCase test;
        test.sample_rate = rate;
        test.channels = static_cast<std::size_t>(count);
        test.block_frames = static_cast<std::size_t>(block);
        test.audio_seconds = seconds;
        test.repeats = repeats;
        test.low_cutoff = low_cutoff;
        test.high_cutoff = high_cutoff;
        const bench::FilterMeasurement result = bench::measure_crossover(test);
        std::printf("%9.0f %2zu %5zu %-16s %8.3f %8.3f %8.3f  %.6f\n", rate, test.channels,
                    test.block_frames, "lr4 steady", result.average_percent,
                    result.percentile_block_percent, result.worst_block_percent, result.checksum);
        std::fflush(stdout);
      }
    }
    const bench::FilterUpdateCost cost =
        bench::measure_filter_update_cost(rate, low_cutoff, high_cutoff);
    std::printf("%9.0f  per call: svf sample %.1f ns, svf try_set_controls + sample %.1f ns, "
                "lr4 sample %.1f ns, lr4 configure + sample %.1f ns\n",
                rate, cost.svf_sample_nanoseconds, cost.svf_update_and_sample_nanoseconds,
                cost.crossover_sample_nanoseconds, cost.crossover_configure_and_sample_nanoseconds);
  }
  return 0;
}

}

int main(int argc, char **argv) {
  if (argc < 2 || std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h" ||
      std::string_view(argv[1]) == "help") {
    std::fputs(usage_text, argc < 2 ? stderr : stdout);
    return argc < 2 ? 2 : 0;
  }
  for (int i = 2; i < argc; ++i) {
    if (std::string_view(argv[i]) == "--help") {
      std::fputs(usage_text, stdout);
      return 0;
    }
  }
  const std::string_view command = argv[1];
  try {
    if (command == "generate") {
      return generate(
          parse_arguments(argc, argv,
                          {"signal", "out", "rate", "seconds", "channels", "format", "amplitude",
                           "frequency", "end-frequency", "seed", "coherent"}));
    }
    if (command == "analyze") {
      return analyze(
          parse_arguments(argc, argv, {"fft", "window", "peaks", "probe", "tone", "harmonics"}));
    }
    if (command == "process") {
      return process(parse_arguments(argc, argv, {"chain", "block", "format"}));
    }
    if (command == "benchmark") {
      return benchmark(parse_arguments(
          argc, argv, {"chain", "rate", "channels", "seconds", "block", "repeats", "input"}));
    }
    if (command == "convolution") {
      return convolution(parse_arguments(argc, argv,
                                         {"rates", "channels", "blocks", "lengths", "seconds",
                                          "repeats", "switching", "direct-limit"}));
    }
    if (command == "baseline") {
      return baseline(
          parse_arguments(argc, argv,
                          {"rates", "channels", "blocks", "scenarios", "lengths", "seconds",
                           "repeats", "chain", "convolution", "memory", "mixed", "reset-count"}));
    }
    if (command == "delay") {
      return delay(parse_arguments(argc, argv,
                                   {"rates", "capacities", "taps", "blocks", "seconds", "repeats",
                                    "modes", "kernels", "reset", "allpasses"}));
    }
    if (command == "filters") {
      return filters(parse_arguments(
          argc, argv,
          {"rates", "channels", "blocks", "seconds", "repeats", "low-cutoff", "high-cutoff"}));
    }
    throw UsageError("unknown command " + std::string(command));
  } catch (const UsageError &error) {
    std::fprintf(stderr, "error: %s\n\n%s", error.what(), usage_text);
    return 2;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "error: %s\n", error.what());
    return 1;
  }
}
