#ifndef VESPERA_DSP_BENCH_WAV_HPP
#define VESPERA_DSP_BENCH_WAV_HPP

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace vespera::dsp::bench {

enum class SampleFormat { pcm16, pcm24, pcm32, float32 };

class WavError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

inline constexpr std::uint32_t wav_minimum_sample_rate = 8000;
inline constexpr std::uint32_t wav_maximum_sample_rate = 768000;

struct WavLimits {
  std::size_t max_file_bytes = std::size_t{256} << 20;
  std::size_t max_frames = std::size_t{64} << 20;
};

struct WavAudio {
  std::uint32_t sample_rate = 48000;
  std::uint16_t channels = 1;
  SampleFormat format = SampleFormat::float32;
  std::vector<float> samples;

  std::size_t frames() const { return channels == 0 ? 0 : samples.size() / channels; }

  std::vector<float> channel(std::size_t index) const {
    if (index >= channels) {
      throw std::out_of_range("WAV channel index out of range");
    }
    std::vector<float> result(frames());
    for (std::size_t frame = 0; frame < result.size(); ++frame) {
      result[frame] = samples[frame * channels + index];
    }
    return result;
  }
};

inline std::size_t bytes_per_sample(SampleFormat format) {
  switch (format) {
  case SampleFormat::pcm16:
    return 2;
  case SampleFormat::pcm24:
    return 3;
  case SampleFormat::pcm32:
  case SampleFormat::float32:
    return 4;
  }
  throw std::invalid_argument("unknown sample format");
}

inline std::string_view format_name(SampleFormat format) {
  switch (format) {
  case SampleFormat::pcm16:
    return "pcm16";
  case SampleFormat::pcm24:
    return "pcm24";
  case SampleFormat::pcm32:
    return "pcm32";
  case SampleFormat::float32:
    return "float32";
  }
  return "unknown";
}

inline std::optional<SampleFormat> parse_format_name(std::string_view name) {
  for (SampleFormat format :
       {SampleFormat::pcm16, SampleFormat::pcm24, SampleFormat::pcm32, SampleFormat::float32}) {
    if (format_name(format) == name) {
      return format;
    }
  }
  return std::nullopt;
}

namespace detail {

inline constexpr std::uint16_t wave_format_pcm = 1;
inline constexpr std::uint16_t wave_format_float = 3;
inline constexpr std::uint16_t wave_format_extensible = 0xFFFE;

inline constexpr std::array<std::uint8_t, 14> ksdataformat_guid_tail = {
    0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

inline std::uint16_t load_u16(const std::uint8_t *p) {
  return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

inline std::uint32_t load_u32(const std::uint8_t *p) {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

inline bool has_id(const std::uint8_t *p, std::string_view id) {
  return std::memcmp(p, id.data(), 4) == 0;
}

inline void store_u16(std::vector<std::uint8_t> &out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xFF));
  out.push_back(static_cast<std::uint8_t>(value >> 8));
}

inline void store_u32(std::vector<std::uint8_t> &out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
  }
}

inline void store_id(std::vector<std::uint8_t> &out, std::string_view id) {
  out.insert(out.end(), id.begin(), id.end());
}

struct FmtChunk {
  SampleFormat format;
  std::uint16_t channels;
  std::uint32_t sample_rate;
  std::uint16_t block_align;
};

inline SampleFormat resolve_format(std::uint16_t tag, std::uint16_t bits) {
  if (tag == wave_format_pcm) {
    switch (bits) {
    case 16:
      return SampleFormat::pcm16;
    case 24:
      return SampleFormat::pcm24;
    case 32:
      return SampleFormat::pcm32;
    default:
      throw WavError("unsupported PCM bit depth " + std::to_string(bits));
    }
  }
  if (tag == wave_format_float) {
    if (bits != 32) {
      throw WavError("unsupported IEEE float bit depth " + std::to_string(bits));
    }
    return SampleFormat::float32;
  }
  throw WavError("unsupported WAV format tag " + std::to_string(tag));
}

inline FmtChunk parse_fmt(std::span<const std::uint8_t> body) {
  if (body.size() < 16) {
    throw WavError("fmt chunk is shorter than 16 bytes");
  }
  const std::uint8_t *p = body.data();
  std::uint16_t tag = load_u16(p);
  const std::uint16_t channels = load_u16(p + 2);
  const std::uint32_t sample_rate = load_u32(p + 4);
  const std::uint32_t byte_rate = load_u32(p + 8);
  const std::uint16_t block_align = load_u16(p + 12);
  const std::uint16_t bits = load_u16(p + 14);

  if (body.size() > 16) {
    if (body.size() < 18) {
      throw WavError("fmt chunk has a partial extension size field");
    }
    const std::uint16_t extension_size = load_u16(p + 16);
    if (extension_size > body.size() - 18) {
      throw WavError("fmt extension exceeds fmt chunk");
    }
    if (tag == wave_format_extensible) {
      if (extension_size < 22) {
        throw WavError("WAVE_FORMAT_EXTENSIBLE extension is shorter than 22 bytes");
      }
      if (load_u16(p + 18) != bits) {
        throw WavError("WAVE_FORMAT_EXTENSIBLE valid bits must equal container bits");
      }
      const std::uint8_t *guid = p + 24;
      if (guid[1] != 0 || std::memcmp(guid + 2, ksdataformat_guid_tail.data(),
                                      ksdataformat_guid_tail.size()) != 0) {
        throw WavError("unsupported WAVE_FORMAT_EXTENSIBLE subformat");
      }
      tag = guid[0];
    }
  }
  if (tag == wave_format_extensible) {
    throw WavError("WAVE_FORMAT_EXTENSIBLE fmt chunk is missing its extension");
  }

  const SampleFormat format = resolve_format(tag, bits);
  if (channels < 1 || channels > 2) {
    throw WavError("unsupported channel count " + std::to_string(channels));
  }
  if (sample_rate < wav_minimum_sample_rate || sample_rate > wav_maximum_sample_rate) {
    throw WavError("unsupported sample rate " + std::to_string(sample_rate));
  }
  if (block_align != channels * bytes_per_sample(format)) {
    throw WavError("fmt block alignment does not match channels and bit depth");
  }
  if (byte_rate != static_cast<std::uint64_t>(sample_rate) * block_align) {
    throw WavError("fmt byte rate does not match sample rate and block alignment");
  }
  return {format, channels, sample_rate, block_align};
}

inline float decode_sample(const std::uint8_t *p, SampleFormat format) {
  switch (format) {
  case SampleFormat::pcm16:
    return static_cast<float>(static_cast<std::int16_t>(load_u16(p))) / 32768.0f;
  case SampleFormat::pcm24: {
    const std::uint32_t raw = static_cast<std::uint32_t>(p[0]) |
                              (static_cast<std::uint32_t>(p[1]) << 8) |
                              (static_cast<std::uint32_t>(p[2]) << 16);
    const std::int32_t value = static_cast<std::int32_t>(raw ^ 0x800000u) - 0x800000;
    return static_cast<float>(value) / 8388608.0f;
  }
  case SampleFormat::pcm32:
    return static_cast<float>(static_cast<double>(static_cast<std::int32_t>(load_u32(p))) /
                              2147483648.0);
  case SampleFormat::float32: {
    const float value = std::bit_cast<float>(load_u32(p));
    if (!std::isfinite(value)) {
      throw WavError("WAV contains a non-finite float sample");
    }
    return value;
  }
  }
  throw WavError("unknown sample format");
}

inline std::int64_t quantize(float sample, int bits) {
  const double scale = std::ldexp(1.0, bits - 1);
  const double scaled =
      std::nearbyint(static_cast<double>(std::clamp(sample, -1.0f, 1.0f)) * scale);
  return static_cast<std::int64_t>(std::clamp(scaled, -scale, scale - 1.0));
}

inline void encode_sample(std::vector<std::uint8_t> &out, float sample, SampleFormat format) {
  switch (format) {
  case SampleFormat::pcm16:
    store_u16(out, static_cast<std::uint16_t>(quantize(sample, 16) & 0xFFFF));
    return;
  case SampleFormat::pcm24: {
    const auto value = static_cast<std::uint32_t>(quantize(sample, 24) & 0xFFFFFF);
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    return;
  }
  case SampleFormat::pcm32:
    store_u32(out, static_cast<std::uint32_t>(quantize(sample, 32) & 0xFFFFFFFF));
    return;
  case SampleFormat::float32:
    store_u32(out, std::bit_cast<std::uint32_t>(sample));
    return;
  }
}

}

inline WavAudio parse_wav(std::span<const std::uint8_t> bytes, const WavLimits &limits = {}) {
  if (bytes.size() > limits.max_file_bytes) {
    throw WavError("WAV exceeds the size limit");
  }
  if (bytes.size() < 12) {
    throw WavError("WAV is truncated before the RIFF header ends");
  }
  if (!detail::has_id(bytes.data(), "RIFF") || !detail::has_id(bytes.data() + 8, "WAVE")) {
    throw WavError("not a RIFF/WAVE file");
  }
  const std::uint64_t riff_end = std::uint64_t{detail::load_u32(bytes.data() + 4)} + 8;
  if (riff_end < 12) {
    throw WavError("RIFF size is smaller than the WAVE header");
  }
  if (riff_end > bytes.size()) {
    throw WavError("WAV is truncated before the declared RIFF size");
  }
  if (riff_end < bytes.size()) {
    throw WavError("WAV has data after the declared RIFF size");
  }

  std::optional<detail::FmtChunk> fmt;
  std::optional<std::span<const std::uint8_t>> data;
  std::size_t position = 12;
  const std::size_t end = bytes.size();
  while (position < end) {
    if (end - position < 8) {
      throw WavError("WAV is truncated inside a chunk header");
    }
    const std::uint8_t *header = bytes.data() + position;
    const std::size_t size = detail::load_u32(header + 4);
    position += 8;
    if (size > end - position) {
      throw WavError("WAV chunk extends past the end of the RIFF data");
    }
    const auto body = bytes.subspan(position, size);
    const std::size_t padding = size & 1u;
    if (padding > end - position - size) {
      throw WavError("WAV chunk is missing its pad byte");
    }
    position += size + padding;

    if (detail::has_id(header, "fmt ")) {
      if (fmt) {
        throw WavError("WAV has more than one fmt chunk");
      }
      fmt = detail::parse_fmt(body);
    } else if (detail::has_id(header, "data")) {
      if (!fmt) {
        throw WavError("WAV data chunk precedes the fmt chunk");
      }
      if (data) {
        throw WavError("WAV has more than one data chunk");
      }
      data = body;
    }
  }
  if (!fmt) {
    throw WavError("WAV has no fmt chunk");
  }
  if (!data) {
    throw WavError("WAV has no data chunk");
  }
  if (data->size() % fmt->block_align != 0) {
    throw WavError("WAV data size is not a whole number of frames");
  }
  const std::size_t frames = data->size() / fmt->block_align;
  if (frames > limits.max_frames) {
    throw WavError("WAV exceeds the frame limit");
  }

  WavAudio audio;
  audio.sample_rate = fmt->sample_rate;
  audio.channels = fmt->channels;
  audio.format = fmt->format;
  const std::size_t width = bytes_per_sample(fmt->format);
  const std::size_t count = frames * fmt->channels;
  audio.samples.resize(count);
  for (std::size_t i = 0; i < count; ++i) {
    audio.samples[i] = detail::decode_sample(data->data() + i * width, fmt->format);
  }
  return audio;
}

inline std::vector<std::uint8_t> encode_wav(const WavAudio &audio) {
  if (audio.channels < 1 || audio.channels > 2) {
    throw WavError("WAV writer supports one or two channels");
  }
  if (audio.sample_rate < wav_minimum_sample_rate || audio.sample_rate > wav_maximum_sample_rate) {
    throw WavError("WAV writer sample rate is out of range");
  }
  if (audio.samples.size() % audio.channels != 0) {
    throw WavError("WAV writer samples are not a whole number of frames");
  }
  if (!std::all_of(audio.samples.begin(), audio.samples.end(),
                   [](float sample) { return std::isfinite(sample); })) {
    throw WavError("WAV writer received a non-finite sample");
  }
  const bool is_float = audio.format == SampleFormat::float32;
  const std::size_t width = bytes_per_sample(audio.format);
  const std::size_t block_align = width * audio.channels;
  const std::size_t fmt_size = is_float ? 18 : 16;
  const std::size_t fact_size = is_float ? 12 : 0;
  const std::size_t header_size = 12 + 8 + fmt_size + fact_size + 8;
  const std::uint64_t max_data = std::numeric_limits<std::uint32_t>::max() - (header_size - 8) - 1;
  if (audio.samples.size() > max_data / width) {
    throw WavError("WAV writer data exceeds the RIFF size limit");
  }
  const std::size_t data_size = audio.samples.size() * width;
  const std::size_t padding = data_size & 1u;

  std::vector<std::uint8_t> out;
  out.reserve(header_size + data_size + padding);
  detail::store_id(out, "RIFF");
  detail::store_u32(out, static_cast<std::uint32_t>(header_size - 8 + data_size + padding));
  detail::store_id(out, "WAVE");
  detail::store_id(out, "fmt ");
  detail::store_u32(out, static_cast<std::uint32_t>(fmt_size));
  detail::store_u16(out, is_float ? detail::wave_format_float : detail::wave_format_pcm);
  detail::store_u16(out, audio.channels);
  detail::store_u32(out, audio.sample_rate);
  detail::store_u32(out, static_cast<std::uint32_t>(audio.sample_rate * block_align));
  detail::store_u16(out, static_cast<std::uint16_t>(block_align));
  detail::store_u16(out, static_cast<std::uint16_t>(width * 8));
  if (is_float) {
    detail::store_u16(out, 0);
    detail::store_id(out, "fact");
    detail::store_u32(out, 4);
    detail::store_u32(out, static_cast<std::uint32_t>(audio.frames()));
  }
  detail::store_id(out, "data");
  detail::store_u32(out, static_cast<std::uint32_t>(data_size));
  for (float sample : audio.samples) {
    detail::encode_sample(out, sample, audio.format);
  }
  if (padding != 0) {
    out.push_back(0);
  }
  return out;
}

inline WavAudio read_wav_file(const std::filesystem::path &path, const WavLimits &limits = {}) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw WavError("cannot open " + path.string());
  }
  std::vector<std::uint8_t> bytes;
  std::array<char, 1 << 16> buffer{};
  while (stream) {
    stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = static_cast<std::size_t>(stream.gcount());
    if (count > limits.max_file_bytes - bytes.size()) {
      throw WavError("WAV exceeds the size limit");
    }
    bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(count));
  }
  if (stream.bad()) {
    throw WavError("cannot read " + path.string());
  }
  return parse_wav(bytes, limits);
}

inline void write_wav_file(const std::filesystem::path &path, const WavAudio &audio) {
  const std::vector<std::uint8_t> bytes = encode_wav(audio);
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    throw WavError("cannot create " + path.string());
  }
  stream.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  stream.close();
  if (!stream) {
    throw WavError("cannot write " + path.string());
  }
}

}

#endif
