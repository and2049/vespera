#include "wav_file.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <stdexcept>
#include <string>

namespace vespera {
namespace {

constexpr uint64_t header_bytes = 44;
constexpr uint64_t max_data_bytes = 0xffffffffull - header_bytes;
constexpr uint64_t chunk_frames = 4096;
constexpr uint16_t format_pcm = 1;
constexpr uint16_t format_float = 3;
constexpr uint16_t format_extensible = 0xfffe;

uint16_t le16(const unsigned char *bytes) noexcept {
  return static_cast<uint16_t>(bytes[0] | (bytes[1] << 8));
}

uint32_t le32(const unsigned char *bytes) noexcept {
  return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
         (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

void put16(unsigned char *bytes, uint16_t value) noexcept {
  bytes[0] = static_cast<unsigned char>(value);
  bytes[1] = static_cast<unsigned char>(value >> 8);
}

void put32(unsigned char *bytes, uint32_t value) noexcept {
  put16(bytes, static_cast<uint16_t>(value));
  put16(bytes + 2, static_cast<uint16_t>(value >> 16));
}

[[noreturn]] void fail(const std::filesystem::path &path, const char *reason) {
  throw std::runtime_error("WAV file " + path.filename().string() + ": " + reason);
}

}

WavReader::WavReader(const std::filesystem::path &path) : file_(std::fopen(path.c_str(), "rb")) {
  if (!file_) {
    fail(path, "cannot be opened");
  }
  try {
    std::error_code error;
    const uint64_t file_size = std::filesystem::file_size(path, error);
    std::array<unsigned char, 40> bytes{};
    if (error || std::fread(bytes.data(), 1, 12, file_) != 12 ||
        std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
        std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
      fail(path, "is not a RIFF WAVE file");
    }
    bool described = false;
    uint64_t offset = 12;
    for (;;) {
      if (std::fread(bytes.data(), 1, 8, file_) != 8) {
        fail(path, described ? "has no audio data" : "has no format description");
      }
      const uint64_t size = le32(bytes.data() + 4);
      offset += 8;
      if (std::memcmp(bytes.data(), "fmt ", 4) == 0) {
        if (size < 16 || std::fread(bytes.data(), 1, std::min<uint64_t>(size, 40), file_) !=
                             std::min<uint64_t>(size, 40)) {
          fail(path, "has a truncated format description");
        }
        uint16_t tag = le16(bytes.data());
        const uint16_t bits = le16(bytes.data() + 14);
        if (tag == format_extensible) {
          if (size < 40) {
            fail(path, "has a truncated format description");
          }
          tag = le16(bytes.data() + 24);
        }
        format_.channels = le16(bytes.data() + 2);
        format_.sample_rate = le32(bytes.data() + 4);
        if (tag == format_pcm && bits == 16) {
          encoding_ = Encoding::pcm16;
        } else if (tag == format_pcm && bits == 24) {
          encoding_ = Encoding::pcm24;
        } else if (tag == format_pcm && bits == 32) {
          encoding_ = Encoding::pcm32;
        } else if (tag == format_float && bits == 32) {
          encoding_ = Encoding::float32;
        } else {
          fail(path, "uses an unsupported sample format");
        }
        bytes_per_sample_ = bits / 8u;
        if (format_.channels == 0 || format_.channels > wav_max_channels) {
          fail(path, "must be mono or stereo");
        }
        if (format_.sample_rate < wav_min_sample_rate ||
            format_.sample_rate > wav_max_sample_rate) {
          fail(path, "has an unsupported sample rate");
        }
        if (le16(bytes.data() + 12) != format_.channels * bytes_per_sample_) {
          fail(path, "has an inconsistent format description");
        }
        described = true;
      } else if (std::memcmp(bytes.data(), "data", 4) == 0) {
        if (!described) {
          fail(path, "has audio data before its format description");
        }
        const uint64_t available = std::min(size, file_size - offset);
        format_.frames = available / (format_.channels * bytes_per_sample_);
        if (fseeko(file_, static_cast<off_t>(offset), SEEK_SET) != 0) {
          fail(path, "cannot be read");
        }
        break;
      }
      offset += size + (size & 1u);
      if (offset > file_size || fseeko(file_, static_cast<off_t>(offset), SEEK_SET) != 0) {
        fail(path, described ? "has no audio data" : "has no format description");
      }
    }
    buffer_.resize(chunk_frames * format_.channels * bytes_per_sample_);
  } catch (...) {
    std::fclose(file_);
    throw;
  }
}

WavReader::~WavReader() { std::fclose(file_); }

uint64_t WavReader::read(float *const *channels, uint64_t frames) {
  const uint64_t wanted = std::min(frames, format_.frames - position_);
  const size_t frame_bytes = format_.channels * bytes_per_sample_;
  uint64_t done = 0;
  while (done < wanted) {
    const auto count = static_cast<size_t>(std::min(chunk_frames, wanted - done));
    if (std::fread(buffer_.data(), frame_bytes, count, file_) != count) {
      throw std::runtime_error("WAV file could not be read");
    }
    const unsigned char *sample = buffer_.data();
    for (size_t f = 0; f < count; ++f) {
      for (uint32_t c = 0; c < format_.channels; ++c, sample += bytes_per_sample_) {
        float value = 0.0f;
        switch (encoding_) {
        case Encoding::pcm16:
          value = static_cast<float>(static_cast<int16_t>(le16(sample))) / 32768.0f;
          break;
        case Encoding::pcm24:
          value =
              static_cast<float>(static_cast<int32_t>((static_cast<uint32_t>(le16(sample)) << 8) |
                                                      (static_cast<uint32_t>(sample[2]) << 24)) >>
                                 8) /
              8388608.0f;
          break;
        case Encoding::pcm32:
          value = static_cast<float>(static_cast<double>(static_cast<int32_t>(le32(sample))) /
                                     2147483648.0);
          break;
        case Encoding::float32:
          value = std::bit_cast<float>(le32(sample));
          break;
        }
        channels[c][done + f] = value;
      }
    }
    done += count;
  }
  position_ += done;
  return done;
}

WavWriter::WavWriter(const std::filesystem::path &path, uint32_t sample_rate, uint32_t channels)
    : sample_rate_(sample_rate), channels_(channels) {
  if (channels == 0 || channels > wav_max_channels || sample_rate < wav_min_sample_rate ||
      sample_rate > wav_max_sample_rate) {
    throw std::invalid_argument("WAV format is not supported");
  }
  file_ = std::fopen(path.c_str(), "wb");
  if (!file_) {
    fail(path, "cannot be created");
  }
  buffer_.resize(chunk_frames * channels * sizeof(float));
  try {
    write_header();
  } catch (...) {
    std::fclose(file_);
    file_ = nullptr;
    throw;
  }
}

WavWriter::~WavWriter() {
  try {
    close();
  } catch (...) {
    if (file_) {
      std::fclose(file_);
    }
  }
}

uint64_t WavWriter::remaining_frames() const noexcept {
  return max_data_bytes / (channels_ * sizeof(float)) - frames_;
}

void WavWriter::write(const float *const *channels, uint64_t frames) {
  if (!file_) {
    throw std::logic_error("WAV file is closed");
  }
  if (frames > remaining_frames()) {
    throw std::length_error("WAV file is full");
  }
  for (uint64_t done = 0; done < frames;) {
    const auto count = static_cast<size_t>(std::min(chunk_frames, frames - done));
    unsigned char *sample = buffer_.data();
    for (size_t f = 0; f < count; ++f) {
      for (uint32_t c = 0; c < channels_; ++c, sample += sizeof(float)) {
        put32(sample, std::bit_cast<uint32_t>(channels[c][done + f]));
      }
    }
    if (std::fwrite(buffer_.data(), channels_ * sizeof(float), count, file_) != count) {
      throw std::runtime_error("WAV file could not be written");
    }
    done += count;
    frames_ += count;
  }
}

void WavWriter::write_interleaved(const float *samples, uint64_t frames) {
  if (!file_) {
    throw std::logic_error("WAV file is closed");
  }
  if (frames > remaining_frames()) {
    throw std::length_error("WAV file is full");
  }
  for (uint64_t done = 0; done < frames;) {
    const auto count = static_cast<size_t>(std::min(chunk_frames, frames - done));
    unsigned char *sample = buffer_.data();
    const float *source = samples + done * channels_;
    for (size_t i = 0; i < count * channels_; ++i, sample += sizeof(float)) {
      put32(sample, std::bit_cast<uint32_t>(source[i]));
    }
    if (std::fwrite(buffer_.data(), channels_ * sizeof(float), count, file_) != count) {
      throw std::runtime_error("WAV file could not be written");
    }
    done += count;
    frames_ += count;
  }
}

void WavWriter::write_header() {
  std::array<unsigned char, header_bytes> header{};
  const auto data_bytes = static_cast<uint32_t>(frames_ * channels_ * sizeof(float));
  const auto frame_bytes = static_cast<uint16_t>(channels_ * sizeof(float));
  std::memcpy(header.data(), "RIFF", 4);
  put32(header.data() + 4, data_bytes + static_cast<uint32_t>(header_bytes) - 8);
  std::memcpy(header.data() + 8, "WAVEfmt ", 8);
  put32(header.data() + 16, 16);
  put16(header.data() + 20, format_float);
  put16(header.data() + 22, static_cast<uint16_t>(channels_));
  put32(header.data() + 24, sample_rate_);
  put32(header.data() + 28, sample_rate_ * frame_bytes);
  put16(header.data() + 32, frame_bytes);
  put16(header.data() + 34, 32);
  std::memcpy(header.data() + 36, "data", 4);
  put32(header.data() + 40, data_bytes);
  if (fseeko(file_, 0, SEEK_SET) != 0 ||
      std::fwrite(header.data(), 1, header.size(), file_) != header.size() ||
      fseeko(file_, 0, SEEK_END) != 0 || std::fflush(file_) != 0) {
    throw std::runtime_error("WAV file could not be written");
  }
}

void WavWriter::sync() {
  if (!file_) {
    throw std::logic_error("WAV file is closed");
  }
  write_header();
}

void WavWriter::close() {
  if (!file_) {
    return;
  }
  write_header();
  std::FILE *file = file_;
  file_ = nullptr;
  if (std::fclose(file) != 0) {
    throw std::runtime_error("WAV file could not be written");
  }
}

}
