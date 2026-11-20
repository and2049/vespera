#include "../bench/baseline_bench.hpp"
#include "../bench/bench.hpp"
#include "../bench/convolution_bench.hpp"
#include "../bench/filters_bench.hpp"
#include "../bench/wav.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <numbers>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace bench = vespera::dsp::bench;
using bench::SampleFormat;
using bench::WavAudio;
using bench::WavError;
using Bytes = std::vector<std::uint8_t>;

int failures = 0;
const char *current_test = "";

#define CHECK(condition)                                                                           \
  do {                                                                                             \
    if (!(condition)) {                                                                            \
      std::fprintf(stderr, "%s:%d: %s: CHECK(%s) failed\n", __FILE__, __LINE__, current_test,      \
                   #condition);                                                                    \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (false)

#define CHECK_NEAR(actual, expected, tolerance)                                                    \
  do {                                                                                             \
    const double check_actual = (actual);                                                          \
    const double check_expected = (expected);                                                      \
    if (!(std::abs(check_actual - check_expected) <= (tolerance))) {                               \
      std::fprintf(stderr, "%s:%d: %s: %s = %.12g, expected %.12g\n", __FILE__, __LINE__,          \
                   current_test, #actual, check_actual, check_expected);                           \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (false)

constexpr SampleFormat all_formats[] = {SampleFormat::pcm16, SampleFormat::pcm24,
                                        SampleFormat::pcm32, SampleFormat::float32};

std::string rejection(const Bytes &bytes, const bench::WavLimits &limits = {}) {
  try {
    bench::parse_wav(bytes, limits);
  } catch (const WavError &error) {
    return error.what();
  }
  return {};
}

bool rejects(const Bytes &bytes, std::string_view fragment = {},
             const bench::WavLimits &limits = {}) {
  const std::string message = rejection(bytes, limits);
  if (message.empty()) {
    std::fprintf(stderr, "%s: WAV unexpectedly accepted\n", current_test);
    return false;
  }
  if (message.find(fragment) == std::string::npos) {
    std::fprintf(stderr, "%s: rejection '%s' lacks '%.*s'\n", current_test, message.c_str(),
                 static_cast<int>(fragment.size()), fragment.data());
    return false;
  }
  return true;
}

void put_u16(Bytes &bytes, std::size_t at, std::uint16_t value) {
  bytes[at] = static_cast<std::uint8_t>(value & 0xFF);
  bytes[at + 1] = static_cast<std::uint8_t>(value >> 8);
}

void put_u32(Bytes &bytes, std::size_t at, std::uint32_t value) {
  for (std::size_t i = 0; i < 4; ++i) {
    bytes[at + i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF);
  }
}

void append_u16(Bytes &bytes, std::uint16_t value) {
  bytes.resize(bytes.size() + 2);
  put_u16(bytes, bytes.size() - 2, value);
}

void append_u32(Bytes &bytes, std::uint32_t value) {
  bytes.resize(bytes.size() + 4);
  put_u32(bytes, bytes.size() - 4, value);
}

void append_chunk(Bytes &bytes, std::string_view id, const Bytes &body, bool pad = true) {
  bytes.insert(bytes.end(), id.begin(), id.end());
  append_u32(bytes, static_cast<std::uint32_t>(body.size()));
  bytes.insert(bytes.end(), body.begin(), body.end());
  if (pad && body.size() % 2 != 0) {
    bytes.push_back(0);
  }
}

Bytes fmt_body(std::uint16_t tag, std::uint16_t channels, std::uint32_t rate, std::uint16_t bits) {
  Bytes body;
  const auto align = static_cast<std::uint16_t>(channels * bits / 8);
  append_u16(body, tag);
  append_u16(body, channels);
  append_u32(body, rate);
  append_u32(body, rate * align);
  append_u16(body, align);
  append_u16(body, bits);
  return body;
}

Bytes extensible_fmt(std::uint16_t subformat, std::uint16_t channels, std::uint16_t bits) {
  Bytes body = fmt_body(0xFFFE, channels, 48000, bits);
  append_u16(body, 22);
  append_u16(body, bits);
  append_u32(body, channels == 1 ? 0x4 : 0x3);
  append_u16(body, subformat);
  const Bytes guid_tail = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80,
                           0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
  body.insert(body.end(), guid_tail.begin(), guid_tail.end());
  return body;
}

Bytes riff(const std::vector<std::pair<std::string_view, Bytes>> &chunks) {
  Bytes bytes = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E'};
  for (const auto &[id, body] : chunks) {
    append_chunk(bytes, id, body);
  }
  put_u32(bytes, 4, static_cast<std::uint32_t>(bytes.size() - 8));
  return bytes;
}

Bytes pcm16_mono(std::initializer_list<std::int16_t> samples) {
  Bytes data;
  for (std::int16_t sample : samples) {
    append_u16(data, static_cast<std::uint16_t>(sample));
  }
  return riff({{"fmt ", fmt_body(1, 1, 48000, 16)}, {"data", data}});
}

WavAudio test_audio(SampleFormat format, std::uint16_t channels, std::size_t frames) {
  WavAudio audio;
  audio.sample_rate = 44100;
  audio.channels = channels;
  audio.format = format;
  const auto noise = bench::white_noise(frames * channels, 0.9, 7);
  audio.samples = noise;
  audio.samples[0] = 1.0f;
  if (audio.samples.size() > 1) {
    audio.samples[1] = -1.0f;
  }
  return audio;
}

double quantization_step(SampleFormat format) {
  switch (format) {
  case SampleFormat::pcm16:
    return 1.0 / 32768.0;
  case SampleFormat::pcm24:
    return 1.0 / 8388608.0;
  case SampleFormat::pcm32:
    return 1.0 / 16777216.0;
  case SampleFormat::float32:
    return 0.0;
  }
  return 1.0;
}

void test_roundtrip() {
  current_test = "roundtrip";
  for (SampleFormat format : all_formats) {
    for (std::uint16_t channels : {std::uint16_t{1}, std::uint16_t{2}}) {
      for (std::size_t frames : {std::size_t{0}, std::size_t{1}, std::size_t{257}}) {
        WavAudio original = test_audio(format, channels, std::max<std::size_t>(frames, 1));
        original.samples.resize(frames * channels);
        const Bytes bytes = bench::encode_wav(original);
        CHECK(bytes.size() % 2 == 0);
        const WavAudio decoded = bench::parse_wav(bytes);
        CHECK(decoded.sample_rate == original.sample_rate);
        CHECK(decoded.channels == original.channels);
        CHECK(decoded.format == format);
        CHECK(decoded.samples.size() == original.samples.size());
        double worst = 0.0;
        for (std::size_t i = 0; i < decoded.samples.size() && i < original.samples.size(); ++i) {
          worst = std::max(worst, std::abs(static_cast<double>(decoded.samples[i]) -
                                           static_cast<double>(original.samples[i])));
        }
        const double step = quantization_step(format);
        CHECK(worst <= step);
        CHECK(bench::encode_wav(decoded) == bytes);
      }
    }
  }
}

void test_pcm_decoding_and_clipping() {
  current_test = "pcm decoding";
  const WavAudio decoded = bench::parse_wav(pcm16_mono({0, 16384, -32768, 32767, -1}));
  CHECK(decoded.samples.size() == 5);
  CHECK(decoded.samples[0] == 0.0f);
  CHECK(decoded.samples[1] == 0.5f);
  CHECK(decoded.samples[2] == -1.0f);
  CHECK(decoded.samples[3] == 32767.0f / 32768.0f);
  CHECK(decoded.samples[4] == -1.0f / 32768.0f);

  Bytes data = {0x00, 0x00, 0x80, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x40};
  const WavAudio pcm24 =
      bench::parse_wav(riff({{"fmt ", fmt_body(1, 1, 48000, 24)}, {"data", data}}));
  CHECK(pcm24.samples.size() == 3);
  CHECK(pcm24.samples[0] == -1.0f);
  CHECK(pcm24.samples[1] == -1.0f / 8388608.0f);
  CHECK(pcm24.samples[2] == 0.5f);

  WavAudio loud;
  loud.channels = 1;
  loud.format = SampleFormat::pcm16;
  loud.samples = {2.0f, -2.0f, 1.0f};
  const WavAudio clipped = bench::parse_wav(bench::encode_wav(loud));
  CHECK(clipped.samples[0] == 32767.0f / 32768.0f);
  CHECK(clipped.samples[1] == -1.0f);
  CHECK(clipped.samples[2] == 32767.0f / 32768.0f);

  loud.format = SampleFormat::float32;
  CHECK(bench::parse_wav(bench::encode_wav(loud)).samples == loud.samples);
}

void test_chunk_layout() {
  current_test = "chunk layout";
  const Bytes data = {0x00, 0x40};
  const Bytes odd = {'a', 'b', 'c'};
  CHECK(
      rejection(
          riff({{"LIST", odd}, {"fmt ", fmt_body(1, 1, 48000, 16)}, {"data", data}, {"junk", odd}}))
          .empty());

  Bytes unpadded = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E'};
  append_chunk(unpadded, "fmt ", fmt_body(1, 1, 48000, 16));
  append_chunk(unpadded, "data", data);
  append_chunk(unpadded, "junk", odd, false);
  put_u32(unpadded, 4, static_cast<std::uint32_t>(unpadded.size() - 8));
  CHECK(rejects(unpadded, "pad byte"));

  Bytes misaligned_pad = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E'};
  append_chunk(misaligned_pad, "junk", odd, false);
  append_chunk(misaligned_pad, "fmt ", fmt_body(1, 1, 48000, 16));
  append_chunk(misaligned_pad, "data", data);
  put_u32(misaligned_pad, 4, static_cast<std::uint32_t>(misaligned_pad.size() - 8));
  CHECK(rejects(misaligned_pad));

  CHECK(rejection(riff({{"fmt ", extensible_fmt(1, 2, 24)}, {"data", Bytes(12, 0)}})).empty());
  CHECK(
      bench::parse_wav(riff({{"fmt ", extensible_fmt(3, 1, 32)}, {"data", Bytes(4, 0)}})).format ==
      SampleFormat::float32);

  Bytes fmt18 = fmt_body(3, 1, 48000, 32);
  append_u16(fmt18, 0);
  CHECK(rejection(riff({{"fmt ", fmt18}, {"fact", Bytes(4, 0)}, {"data", Bytes(4, 0)}})).empty());
}

void test_malformed_rejection() {
  current_test = "malformed";
  const Bytes data = {0x00, 0x40, 0x00, 0xC0};
  const Bytes fmt = fmt_body(1, 1, 48000, 16);
  const Bytes valid = riff({{"fmt ", fmt}, {"data", data}});
  CHECK(rejection(valid).empty());

  CHECK(rejects({}, "truncated"));
  Bytes bad_magic = valid;
  bad_magic[0] = 'X';
  CHECK(rejects(bad_magic, "RIFF/WAVE"));
  Bytes bad_form = valid;
  bad_form[8] = 'X';
  CHECK(rejects(bad_form, "RIFF/WAVE"));

  Bytes small_riff = valid;
  put_u32(small_riff, 4, 3);
  CHECK(rejects(small_riff, "smaller"));
  Bytes huge_riff = valid;
  put_u32(huge_riff, 4, 0xFFFFFFFF);
  CHECK(rejects(huge_riff, "truncated"));
  Bytes trailing = valid;
  trailing.push_back(0);
  CHECK(rejects(trailing, "after the declared RIFF"));

  Bytes huge_data = valid;
  put_u32(huge_data, 12 + 8 + 16 + 4, 0xFFFFFFF0);
  CHECK(rejects(huge_data, "extends past"));

  CHECK(rejects(riff({{"data", data}}), "precedes"));
  CHECK(rejects(riff({{"fmt ", fmt}}), "no data"));
  CHECK(rejects(riff({{"LIST", Bytes(4, 0)}}), "no fmt"));
  CHECK(rejects(riff({{"fmt ", fmt}, {"fmt ", fmt}, {"data", data}}), "more than one fmt"));
  CHECK(rejects(riff({{"fmt ", fmt}, {"data", data}, {"data", data}}), "more than one data"));
  CHECK(rejects(riff({{"fmt ", Bytes(14, 0)}, {"data", data}}), "shorter than 16"));
  CHECK(rejects(riff({{"fmt ", fmt}, {"data", Bytes(3, 0)}}), "whole number"));

  CHECK(rejects(riff({{"fmt ", fmt_body(2, 1, 48000, 16)}, {"data", data}}), "format tag"));
  CHECK(rejects(riff({{"fmt ", fmt_body(1, 1, 48000, 8)}, {"data", data}}), "bit depth"));
  CHECK(rejects(riff({{"fmt ", fmt_body(3, 1, 48000, 64)}, {"data", Bytes(8, 0)}}), "bit depth"));
  CHECK(rejects(riff({{"fmt ", fmt_body(1, 3, 48000, 16)}, {"data", Bytes(6, 0)}}), "channel"));
  CHECK(rejects(riff({{"fmt ", fmt_body(1, 0, 48000, 16)}, {"data", data}}), "channel"));
  CHECK(rejects(riff({{"fmt ", fmt_body(1, 1, 7999, 16)}, {"data", data}}), "sample rate"));
  CHECK(rejects(riff({{"fmt ", fmt_body(1, 1, 768001, 16)}, {"data", data}}), "sample rate"));

  Bytes bad_align = fmt;
  put_u16(bad_align, 12, 4);
  CHECK(rejects(riff({{"fmt ", bad_align}, {"data", data}}), "block alignment"));
  Bytes bad_rate = fmt;
  put_u32(bad_rate, 8, 1);
  CHECK(rejects(riff({{"fmt ", bad_rate}, {"data", data}}), "byte rate"));

  Bytes partial_extension = fmt;
  partial_extension.push_back(0);
  CHECK(rejects(riff({{"fmt ", partial_extension}, {"data", data}}), "partial extension"));
  Bytes long_extension = fmt;
  append_u16(long_extension, 4);
  CHECK(rejects(riff({{"fmt ", long_extension}, {"data", data}}), "exceeds fmt"));
  CHECK(rejects(riff({{"fmt ", fmt_body(0xFFFE, 1, 48000, 16)}, {"data", data}}),
                "missing its extension"));
  Bytes bad_guid = extensible_fmt(1, 1, 16);
  bad_guid[24 + 5] ^= 1;
  CHECK(rejects(riff({{"fmt ", bad_guid}, {"data", data}}), "subformat"));
  Bytes bad_valid_bits = extensible_fmt(1, 1, 24);
  put_u16(bad_valid_bits, 18, 20);
  CHECK(rejects(riff({{"fmt ", bad_valid_bits}, {"data", Bytes(3, 0)}}), "valid bits"));

  for (float bad : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                    -std::numeric_limits<float>::infinity()}) {
    Bytes body;
    append_u32(body, std::bit_cast<std::uint32_t>(bad));
    CHECK(rejects(riff({{"fmt ", fmt_body(3, 1, 48000, 32)}, {"data", body}}), "non-finite"));
  }
}

void test_truncation_and_mutation() {
  current_test = "truncation";
  WavAudio audio = test_audio(SampleFormat::pcm24, 2, 5);
  const Bytes valid = bench::encode_wav(audio);
  for (std::size_t length = 0; length < valid.size(); ++length) {
    const Bytes prefix(valid.begin(), valid.begin() + static_cast<std::ptrdiff_t>(length));
    if (rejection(prefix).empty()) {
      std::fprintf(stderr, "%s: prefix of %zu bytes accepted\n", current_test, length);
      ++failures;
    }
  }
  for (std::size_t length = 0; length < valid.size(); ++length) {
    Bytes consistent(valid.begin(), valid.begin() + static_cast<std::ptrdiff_t>(length));
    if (consistent.size() >= 8) {
      put_u32(consistent, 4, static_cast<std::uint32_t>(consistent.size() - 8));
      if (rejection(consistent).empty()) {
        std::fprintf(stderr, "%s: truncated file with fixed RIFF size %zu accepted\n", current_test,
                     length);
        ++failures;
      }
    }
  }

  current_test = "mutation";
  for (std::size_t at = 0; at < valid.size(); ++at) {
    for (std::uint8_t mask : {std::uint8_t{0x01}, std::uint8_t{0x80}, std::uint8_t{0xFF}}) {
      Bytes mutated = valid;
      mutated[at] ^= mask;
      try {
        const WavAudio parsed = bench::parse_wav(mutated);
        CHECK(parsed.samples.size() <= mutated.size());
        CHECK(parsed.samples.size() % parsed.channels == 0);
      } catch (const WavError &) {
      } catch (const std::exception &error) {
        std::fprintf(stderr, "%s: byte %zu raised non-WAV exception %s\n", current_test, at,
                     error.what());
        ++failures;
      }
    }
  }
}

// create_directory() fails rather than reuses an existing path, so each directory belongs to
// exactly one test process and removing it cannot disturb concurrent runs.
std::filesystem::path create_unique_temp_directory() {
  const auto base = std::filesystem::temp_directory_path();
  std::random_device entropy;
  for (int attempt = 0; attempt < 100; ++attempt) {
    const std::uint64_t token = (std::uint64_t{entropy()} << 32) ^ entropy();
    char name[64];
    std::snprintf(name, sizeof name, "vespera_dsp_bench_test-%016llx",
                  static_cast<unsigned long long>(token));
    const auto candidate = base / name;
    if (std::filesystem::create_directory(candidate)) {
      return candidate;
    }
  }
  throw std::runtime_error("cannot create a unique temporary directory");
}

void test_limits_and_files() {
  current_test = "limits";
  const Bytes valid = pcm16_mono({1, 2, 3, 4});
  CHECK(rejects(valid, "size limit", {.max_file_bytes = valid.size() - 1}));
  CHECK(rejection(valid, {.max_file_bytes = valid.size()}).empty());
  CHECK(rejects(valid, "frame limit", {.max_frames = 3}));
  CHECK(rejection(valid, {.max_frames = 4}).empty());

  WavAudio bad;
  bad.channels = 2;
  bad.samples = {0.0f};
  CHECK(rejection(Bytes{}).size() > 0);
  bool threw = false;
  try {
    bench::encode_wav(bad);
  } catch (const WavError &) {
    threw = true;
  }
  CHECK(threw);
  bad.samples = {0.0f, std::numeric_limits<float>::quiet_NaN()};
  threw = false;
  try {
    bench::encode_wav(bad);
  } catch (const WavError &) {
    threw = true;
  }
  CHECK(threw);

  current_test = "files";
  const auto directory = create_unique_temp_directory();
  const auto path = directory / "roundtrip.wav";
  const WavAudio audio = test_audio(SampleFormat::pcm16, 2, 1000);
  bench::write_wav_file(path, audio);
  const WavAudio read = bench::read_wav_file(path);
  CHECK(bench::encode_wav(read) == bench::encode_wav(audio));
  const auto size = static_cast<std::size_t>(std::filesystem::file_size(path));
  threw = false;
  try {
    bench::read_wav_file(path, {.max_file_bytes = size - 1});
  } catch (const WavError &error) {
    threw = std::string_view(error.what()).find("size limit") != std::string_view::npos;
  }
  CHECK(threw);
  threw = false;
  try {
    bench::read_wav_file(directory / "missing.wav");
  } catch (const WavError &) {
    threw = true;
  }
  CHECK(threw);
  std::filesystem::remove_all(directory);
}

void test_signals() {
  current_test = "signals";
  const double rate = 48000.0;
  const std::size_t frames = 48000;
  const auto tone = bench::sine(rate, 1000.0, 0.5, frames);
  const auto stats = bench::measure(tone);
  CHECK_NEAR(stats.rms, 0.5 / std::numbers::sqrt2, 1e-6);
  CHECK_NEAR(stats.peak, 0.5, 1e-6);
  CHECK_NEAR(stats.dc, 0.0, 1e-7);

  const auto noise = bench::white_noise(1 << 18, 0.6, 99);
  CHECK(noise == bench::white_noise(1 << 18, 0.6, 99));
  CHECK(noise != bench::white_noise(1 << 18, 0.6, 100));
  const auto noise_stats = bench::measure(noise);
  CHECK_NEAR(noise_stats.rms, 0.6 / std::sqrt(3.0), 0.003);
  CHECK(noise_stats.peak <= 0.6);
  CHECK_NEAR(noise_stats.dc, 0.0, 0.005);

  const auto click = bench::impulse(64, 3, 0.25);
  CHECK(click[3] == 0.25f);
  CHECK(bench::measure(click).rms == std::sqrt(0.25 * 0.25 / 64.0));
  const auto flat =
      bench::amplitude_spectrum(bench::impulse(64, 0, 1.0), bench::Window::rectangular);
  for (std::size_t k = 1; k + 1 < flat.size(); ++k) {
    CHECK_NEAR(flat[k], 2.0 / 64.0, 1e-12);
  }

  const auto sweep = bench::log_sweep(rate, 100.0, 10000.0, 0.8, frames);
  CHECK(bench::measure(sweep).peak <= 0.8 + 1e-6);
  CHECK(sweep[0] == 0.0f);
  const std::size_t segment = 2048;
  const auto head = bench::spectral_peaks(
      bench::amplitude_spectrum(std::span(sweep).first(segment), bench::Window::hann), rate, 1);
  const auto tail = bench::spectral_peaks(
      bench::amplitude_spectrum(std::span(sweep).last(segment), bench::Window::hann), rate, 1);
  const double head_mid = 100.0 * std::pow(100.0, (segment / 2.0) / frames);
  const double tail_mid = 100.0 * std::pow(100.0, (frames - segment / 2.0) / frames);
  CHECK(head.size() == 1 && std::abs(head[0].frequency - head_mid) < 2.0 * rate / segment);
  CHECK(tail.size() == 1 && std::abs(tail[0].frequency - tail_mid) / tail_mid < 0.02);

  bool threw = false;
  try {
    bench::sine(rate, rate / 2.0, 1.0, 4);
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  CHECK(threw);
}

void test_spectrum() {
  current_test = "spectrum";
  const std::size_t n = 256;
  const auto noise = bench::white_noise(n, 1.0, 5);
  std::vector<std::complex<double>> bins(noise.begin(), noise.end());
  bench::fft_in_place(bins);
  double worst = 0.0;
  for (std::size_t k = 0; k < n; ++k) {
    std::complex<double> expected = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
      expected += static_cast<double>(noise[i]) *
                  std::polar(1.0, -2.0 * std::numbers::pi * static_cast<double>(k * i % n) /
                                      static_cast<double>(n));
    }
    worst = std::max(worst, std::abs(bins[k] - expected));
  }
  CHECK(worst < 1e-9);

  const double rate = 48000.0;
  const double frequency = bench::bin_frequency(37, n, rate);
  CHECK_NEAR(bench::coherent_frequency(rate, n, frequency + 50.0), frequency, 1e-9);
  const auto tone = bench::sine(rate, frequency, 0.75, n, 0.3);
  for (bench::Window window : {bench::Window::rectangular, bench::Window::hann}) {
    const auto spectrum = bench::amplitude_spectrum(tone, window);
    CHECK_NEAR(spectrum[37], 0.75, 1e-6);
    const auto peaks = bench::spectral_peaks(spectrum, rate, 3);
    CHECK(!peaks.empty() && std::abs(peaks[0].frequency - frequency) < 1e-6);
    CHECK_NEAR(bench::tone_amplitude(tone, rate, frequency, window), 0.75, 1e-6);
  }
  const auto rectangular = bench::amplitude_spectrum(tone, bench::Window::rectangular);
  for (std::size_t k = 0; k < rectangular.size(); ++k) {
    if (k != 37) {
      CHECK(rectangular[k] < 1e-6);
    }
  }

  std::vector<float> dc_nyquist(n);
  for (std::size_t i = 0; i < n; ++i) {
    dc_nyquist[i] = static_cast<float>(0.25 + (i % 2 == 0 ? 0.5 : -0.5));
  }
  const auto edges = bench::amplitude_spectrum(dc_nyquist, bench::Window::rectangular);
  CHECK_NEAR(edges.front(), 0.25, 1e-12);
  CHECK_NEAR(edges.back(), 0.5, 1e-12);

  const auto off_bin = bench::sine(rate, 1234.567, 0.5, 9999);
  CHECK_NEAR(bench::tone_amplitude(off_bin, rate, 1234.567), 0.5, 1e-4);

  bool threw = false;
  try {
    bench::amplitude_spectrum(std::vector<float>(100), bench::Window::hann);
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  CHECK(threw);
}

void test_distortion() {
  current_test = "distortion";
  const double rate = 48000.0;
  const std::size_t frames = 48000;
  const double f0 = 1000.0;
  std::vector<float> signal(frames);
  for (std::size_t n = 0; n < frames; ++n) {
    const double t = static_cast<double>(n) / rate;
    signal[n] = static_cast<float>(0.5 * std::sin(2 * std::numbers::pi * f0 * t) +
                                   0.05 * std::sin(2 * std::numbers::pi * 2 * f0 * t) +
                                   0.025 * std::sin(2 * std::numbers::pi * 3 * f0 * t + 1.0));
  }
  CHECK_NEAR(bench::tone_amplitude(signal, rate, f0), 0.5, 1e-6);
  CHECK_NEAR(bench::tone_amplitude(signal, rate, 2 * f0), 0.05, 1e-6);
  CHECK_NEAR(bench::harmonic_distortion(signal, rate, f0), std::sqrt(0.01 + 0.0025), 1e-5);
  CHECK(bench::harmonic_distortion(bench::sine(rate, f0, 0.5, frames), rate, f0) < 1e-5);
  CHECK(std::isinf(bench::harmonic_distortion(std::vector<float>(frames), rate, f0)));
  CHECK(std::isinf(bench::thd_plus_noise(std::vector<float>(frames), rate, f0)));
}

template <typename Function> bool throws_invalid(Function function) {
  try {
    function();
  } catch (const std::invalid_argument &) {
    return true;
  }
  return false;
}

void test_noncoherent_fit() {
  current_test = "noncoherent fit";
  const double rate = 48000.0;
  const double f0 = 1000.3;
  const auto frames = static_cast<std::size_t>(rate * 0.013);
  std::vector<float> tone(frames);
  for (std::size_t n = 0; n < frames; ++n) {
    tone[n] = static_cast<float>(
        0.25 + 0.5 * std::sin(2 * std::numbers::pi * f0 * static_cast<double>(n) / rate + 0.7));
  }
  const bench::HarmonicFit fit = bench::fit_harmonics(tone, rate, f0);
  CHECK(fit.amplitudes.size() == 1);
  CHECK_NEAR(fit.amplitudes[0], 0.5, 1e-6);
  CHECK_NEAR(fit.dc, 0.25, 1e-6);
  const double float_floor = 0.5 * std::ldexp(1.0, -24);
  CHECK(fit.residual_rms <= float_floor);
  const double thd_n = bench::thd_plus_noise(tone, rate, f0);
  CHECK(std::isfinite(thd_n) && thd_n >= 0.0 && bench::to_db(thd_n) < -125.0);
  CHECK(bench::harmonic_distortion(tone, rate, f0) < 1e-6);

  current_test = "harmonic and noise reference";
  const double f1 = 997.3;
  const std::size_t length = 4801;
  const auto noise = bench::white_noise(length, 0.01, 11);
  std::vector<float> mix(length);
  double reference_squares = 0.0;
  for (std::size_t n = 0; n < length; ++n) {
    const double w = 2 * std::numbers::pi * f1 * static_cast<double>(n) / rate;
    const double rest = 0.05 * std::sin(2 * w + 1.3) + 0.025 * std::sin(3 * w + 0.4) + noise[n];
    mix[n] = static_cast<float>(-0.1 + 0.5 * std::sin(w + 2.0) + rest);
    reference_squares += rest * rest;
  }
  const double reference_thd_n =
      std::sqrt(reference_squares / static_cast<double>(length)) / (0.5 / std::numbers::sqrt2);
  CHECK_NEAR(bench::thd_plus_noise(mix, rate, f1) / reference_thd_n, 1.0, 0.01);
  const bench::HarmonicFit full = bench::fit_harmonics(mix, rate, f1, 3);
  CHECK(full.amplitudes.size() == 3);
  CHECK_NEAR(full.amplitudes[0], 0.5, 1e-3);
  CHECK_NEAR(full.amplitudes[1], 0.05, 1e-3);
  CHECK_NEAR(full.amplitudes[2], 0.025, 1e-3);
  CHECK_NEAR(full.dc, -0.1, 1e-3);
  CHECK_NEAR(full.residual_rms / bench::measure(noise).rms, 1.0, 0.01);
  CHECK_NEAR(bench::harmonic_distortion(mix, rate, f1, 3), std::hypot(0.05, 0.025) / 0.5, 2e-3);
  CHECK(bench::harmonic_distortion(full) == bench::harmonic_distortion(mix, rate, f1, 3));
  CHECK(throws_invalid([&] { bench::thd_plus_noise(full); }));
  CHECK(bench::thd_plus_noise(bench::fit_harmonics(mix, rate, f1, 1)) ==
        bench::thd_plus_noise(mix, rate, f1));
  CHECK(std::isinf(bench::harmonic_distortion(bench::HarmonicFit{})));
  CHECK(throws_invalid([] { bench::thd_plus_noise(bench::HarmonicFit{}); }));

  current_test = "fit limits";
  const auto short_tone = bench::sine(rate, 1000.0, 0.5, 40);
  CHECK(throws_invalid([&] { bench::fit_harmonics(short_tone, rate, 1000.0); }));
  CHECK(throws_invalid([&] { bench::fit_harmonics(std::vector<float>{}, rate, 1000.0); }));
  CHECK(throws_invalid([&] { bench::fit_harmonics(std::vector<float>(5), rate, 20000.0); }));
  CHECK(throws_invalid([&] { bench::fit_harmonics(tone, rate, 1000.0, 0); }));
  CHECK(throws_invalid([&] { bench::fit_harmonics(tone, rate, rate * 0.5 - 1.0); }));
  CHECK(throws_invalid([&] { bench::thd_plus_noise(std::vector<float>(47), rate, 1000.0); }));
  CHECK(bench::fit_harmonics(bench::sine(rate, 1200.0, 0.5, 48), rate, 1200.0).amplitudes.size() ==
        1);
  CHECK(bench::fit_harmonics(tone, rate, 0.3 * rate, 5).amplitudes.size() == 1);
  CHECK(throws_invalid([] { bench::tone_amplitude(std::vector<float>{1.0f}, 48000.0, 100.0); }));
  CHECK(throws_invalid([] { bench::tone_amplitude(std::vector<float>{}, 48000.0, 100.0); }));

  current_test = "hann low bin";
  const std::size_t n64 = 64;
  const double bin1 = bench::bin_frequency(1, n64, rate);
  CHECK_NEAR(bench::tone_amplitude(bench::sine(rate, bin1, 0.5, n64), rate, bin1), 0.5, 1e-6);
  std::vector<float> offset = bench::sine(rate, bin1, 0.5, n64);
  for (float &sample : offset) {
    sample += 0.25f;
  }
  CHECK(std::abs(bench::tone_amplitude(offset, rate, bin1) - 0.5) > 0.05);
  CHECK_NEAR(bench::fit_harmonics(offset, rate, bin1).amplitudes[0], 0.5, 1e-6);
}

void test_chain() {
  current_test = "chain";
  const double rate = 48000.0;
  const auto noise = bench::interleave(bench::white_noise(4096, 0.5, 3), 2);

  auto identity = bench::Chain::parse("identity", rate, 2);
  std::vector<float> work = noise;
  bench::process_blocks(identity, work, 64);
  CHECK(work == noise);
  CHECK(identity.describe() == "identity");

  auto gain = bench::Chain::parse("gain:-6.0206", rate, 2);
  work = noise;
  gain.process(work);
  CHECK_NEAR(bench::measure(work).rms, bench::measure(noise).rms * 0.5, 1e-6);

  const double cutoff = bench::coherent_frequency(rate, 48000, 2000.0);
  auto lowpass = bench::Chain::parse("lowpass:" + std::to_string(cutoff), rate, 1);
  std::vector<float> dc(48000, 0.5f);
  lowpass.process(dc);
  CHECK_NEAR(dc.back(), 0.5, 1e-6);
  lowpass.reset();
  auto tone = bench::sine(rate, cutoff, 0.5, 96000);
  lowpass.process(tone);
  const auto settled = std::span(tone).last(48000);
  CHECK_NEAR(bench::tone_amplitude(settled, rate, cutoff), 0.5 * bench::lowpass_q, 1e-4);
  lowpass.reset();
  auto high = bench::sine(rate, 16000.0, 0.5, 96000);
  lowpass.process(high);
  CHECK(bench::tone_amplitude(std::span(high).last(48000), rate, 16000.0) < 0.5 * 0.02);

  auto chain = bench::Chain::parse("gain:12,lowpass:5000,tanh:3", rate, 2);
  std::vector<float> single = noise;
  std::vector<float> blocked = noise;
  bench::process_blocks(chain, single, 1);
  chain.reset();
  bench::process_blocks(chain, blocked, 333);
  CHECK(single == blocked);
  CHECK(bench::measure(blocked).peak <= 1.0 / std::tanh(3.0) + 1e-6);

  auto drive = bench::Chain::parse("tanh:2", rate, 1);
  std::vector<float> unity = {1.0f, -1.0f, 0.0f};
  drive.process(unity);
  CHECK_NEAR(unity[0], 1.0, 1e-6);
  CHECK_NEAR(unity[1], -1.0, 1e-6);
  CHECK(unity[2] == 0.0f);

  auto delay = bench::Chain::parse("delay:2.5", rate, 2);
  std::vector<float> click = bench::interleave(bench::impulse(8, 0, 1.0), 2);
  delay.process(click);
  CHECK(click[4] == 0.5f && click[5] == 0.5f && click[6] == 0.5f && click[7] == 0.5f);
  CHECK(std::count(click.begin(), click.end(), 0.0f) == 12);

  auto limit = bench::Chain::parse("limit:-6.0206", rate, 1);
  auto loud = bench::sine(rate, 1000.0, 1.0, 48000);
  limit.process(loud);
  const double limited = bench::measure(std::span(loud).last(24000)).peak;
  CHECK(limited > 0.5 && limited < 0.52);
  limit.reset();
  auto quiet = bench::sine(rate, 1000.0, 0.25, 4800);
  const auto quiet_input = quiet;
  limit.process(quiet);
  CHECK(quiet == quiet_input);

  const double tremolo_rate = bench::coherent_frequency(rate, 48000, 5.0);
  auto tremolo = bench::Chain::parse("tremolo:" + std::to_string(tremolo_rate), rate, 1);
  std::vector<float> ones(48000, 1.0f);
  tremolo.process(ones);
  CHECK_NEAR(bench::measure(ones).dc, 0.5, 1e-6);
  CHECK_NEAR(bench::tone_amplitude(ones, rate, tremolo_rate, bench::Window::rectangular), 0.5,
             1e-6);

  auto fade = bench::Chain::parse("fade:0.001", rate, 1);
  std::vector<float> held(96, 1.0f);
  fade.process(held);
  CHECK_NEAR(held[0], 1.0 / 48.0, 1e-6);
  CHECK_NEAR(held[23], 0.5, 1e-6);
  CHECK(held[47] == 1.0f && held[95] == 1.0f);

  for (const char *spec :
       {"gain", "gain:x", "gain:nan", "boost:3", "lowpass:30000", "tanh:0", "lowpass:-1",
        "gain:1e999", "delay:-1", "delay:1e12", "limit:1", "tremolo:-2", "fade:-1", "fade:100"}) {
    bool threw = false;
    try {
      bench::Chain::parse(spec, rate, 1);
    } catch (const std::invalid_argument &) {
      threw = true;
    }
    if (!threw) {
      std::fprintf(stderr, "%s: chain spec '%s' accepted\n", current_test, spec);
      ++failures;
    }
  }

  current_test = "throughput";
  const auto result = bench::measure_throughput(chain, noise, rate, 128, 3);
  CHECK(result.frames == 4096);
  CHECK(result.repeats == 3);
  CHECK(result.best_seconds <= result.median_seconds);
  CHECK(result.realtime_factor > 0.0);
  CHECK(std::isfinite(result.checksum) && result.checksum > 0.0);
}

}

void test_convolution_measurement() {
  current_test = "convolution measurement";
  for (bool switching : {false, true}) {
    bench::ConvolutionCase test;
    test.channels = 2;
    test.response_seconds = 0.05;
    test.block_frames = 100;
    test.audio_seconds = 0.25;
    test.repeats = 1;
    test.switching = switching;
    const auto result = bench::measure_convolution(test);
    CHECK(result.response_frames == 2400);
    CHECK(result.layout.capacity_frames() >= 2400);
    CHECK(std::isfinite(result.average_percent) && result.average_percent > 0.0);
    CHECK(result.worst_block_percent >= result.percentile_block_percent);
    CHECK(result.percentile_block_percent > 0.0);
    CHECK(result.checksum > 0.0);
  }
  bench::ConvolutionCase test;
  test.response_seconds = 0.01;
  test.audio_seconds = 0.05;
  test.repeats = 1;
  const auto direct = bench::measure_direct_convolution(test);
  CHECK(direct.average_percent > 0.0 && direct.checksum > 0.0);
  test.channels = 3;
  bool rejected = false;
  try {
    (void)bench::measure_convolution(test);
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  CHECK(rejected);
}

void test_baseline_measurements() {
  current_test = "toolkit baseline measurement";
  for (bench::ToolkitWorkload workload :
       {bench::ToolkitWorkload::steady, bench::ToolkitWorkload::moving,
        bench::ToolkitWorkload::silent, bench::ToolkitWorkload::reset}) {
    bench::ToolkitCase test;
    test.sample_rate = 48000.0;
    test.channels = 2;
    test.block_frames = 100;
    test.audio_seconds = 0.25;
    test.repeats = 1;
    test.workload = workload;
    const auto result = bench::measure_toolkit_case(test);
    CHECK(std::isfinite(result.average_percent) && result.average_percent > 0.0);
    CHECK(result.worst_block_percent >= result.percentile_block_percent);
    CHECK(result.percentile_block_percent > 0.0);
    CHECK(result.blocks == 120);
    CHECK(result.checksum >= 0.0);
    if (workload == bench::ToolkitWorkload::silent) {
      CHECK(result.checksum == 0.0);
    }
    if (workload == bench::ToolkitWorkload::reset) {
      CHECK(result.resets == 3);
    }
  }
  {
    bench::ToolkitCase test;
    test.channels = 1;
    test.block_frames = 64;
    test.audio_seconds = 0.0;
    test.repeats = 1;
    bool rejected = false;
    try {
      (void)bench::measure_toolkit_case(test);
    } catch (const std::invalid_argument &) {
      rejected = true;
    }
    CHECK(rejected);
  }
  {
    bench::ToolkitCase test;
    test.sample_rate = 48000.0;
    test.channels = 1;
    test.block_frames = 7;
    test.audio_seconds = 100.0 / 48000.0;
    test.repeats = 1;
    test.mixed_blocks = true;
    const auto result = bench::measure_toolkit_case(test);
    CHECK(result.blocks == 4);
  }

  current_test = "convolution baseline variants";
  {
    bench::ConvolutionCase test;
    test.sample_rate = 48000.0;
    test.channels = 1;
    test.response_seconds = 0.02;
    test.block_frames = 64;
    test.audio_seconds = 0.1;
    test.repeats = 1;
    test.silent_input = true;
    test.mixed_blocks = true;
    const auto result = bench::measure_convolution(test);
    CHECK(std::isfinite(result.average_percent) && result.average_percent > 0.0);
    CHECK(result.worst_block_percent >= result.percentile_block_percent);
    CHECK(result.checksum == 0.0);
  }

  current_test = "convolution reset measurement";
  {
    bench::ConvolutionCase test;
    test.sample_rate = 48000.0;
    test.channels = 1;
    test.response_seconds = 0.05;
    const auto result = bench::measure_convolution_reset(test, 4);
    CHECK(result.reset_count == 4);
    CHECK(result.capacity_frames == 2400);
    CHECK(std::isfinite(result.seconds_per_reset) && result.seconds_per_reset > 0.0);
    CHECK(result.checksum >= 0.0);
    bool rejected = false;
    try {
      (void)bench::measure_convolution_reset(test, 0);
    } catch (const std::invalid_argument &) {
      rejected = true;
    }
    CHECK(rejected);
  }

  current_test = "construction memory measurement";
  {
    bench::ConvolutionCase test;
    test.sample_rate = 48000.0;
    test.channels = 2;
    test.response_seconds = 0.02;
    const auto result = bench::measure_construction_memory(test);
    if (result.measured) {
      CHECK(result.response_bytes > 0.0);
      CHECK(result.convolver_bytes > 0.0);
      CHECK(result.convolver_bytes > 8.0 * 960.0 * 2.0);
      CHECK(result.response_bytes > 8.0 * 960.0 * 2.0);
    }
  }
  CHECK(!bench::environment_report().empty());
}

void test_filters_measurement() {
  current_test = "filters measurement";
  for (bench::FilterMotion motion : {bench::FilterMotion::steady, bench::FilterMotion::moving}) {
    bench::FilterCase test;
    test.sample_rate = 48000.0;
    test.channels = 2;
    test.block_frames = 100;
    test.audio_seconds = 0.25;
    test.repeats = 1;
    test.motion = motion;
    const auto result = bench::measure_svf(test);
    CHECK(std::isfinite(result.average_percent) && result.average_percent > 0.0);
    CHECK(result.worst_block_percent >= result.percentile_block_percent);
    CHECK(result.percentile_block_percent > 0.0);
    CHECK(std::isfinite(result.checksum) && result.checksum != 0.0);
  }
  bench::FilterCase test;
  test.sample_rate = 48000.0;
  test.channels = 1;
  test.block_frames = 64;
  test.audio_seconds = 0.25;
  test.repeats = 1;
  const auto crossover = bench::measure_crossover(test);
  CHECK(crossover.average_percent > 0.0 && crossover.checksum != 0.0);
  const auto cost = bench::measure_filter_update_cost(48000.0, 100.0, 8000.0);
  CHECK(cost.svf_sample_nanoseconds > 0.0 && cost.crossover_sample_nanoseconds > 0.0);
  CHECK(cost.svf_update_and_sample_nanoseconds > 0.0);
  CHECK(cost.crossover_configure_and_sample_nanoseconds > 0.0);
  CHECK(std::isfinite(cost.checksum));
  test.motion = bench::FilterMotion::moving;
  bool rejected = false;
  try {
    (void)bench::measure_crossover(test);
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  CHECK(rejected);
  test.motion = bench::FilterMotion::steady;
  test.channels = 3;
  try {
    (void)bench::measure_svf(test);
    rejected = false;
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  CHECK(rejected);
  try {
    (void)bench::measure_filter_update_cost(48000.0, 100.0, 48000.0);
    rejected = false;
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  CHECK(rejected);
}

int main() {
  test_roundtrip();
  test_pcm_decoding_and_clipping();
  test_chunk_layout();
  test_malformed_rejection();
  test_truncation_and_mutation();
  test_limits_and_files();
  test_signals();
  test_spectrum();
  test_distortion();
  test_noncoherent_fit();
  test_chain();
  test_convolution_measurement();
  test_baseline_measurements();
  test_filters_measurement();
  if (failures != 0) {
    std::fprintf(stderr, "%d bench check(s) failed\n", failures);
    return 1;
  }
  std::puts("bench tests passed");
  return 0;
}
