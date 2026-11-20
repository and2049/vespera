#ifndef VESPERA_KIT_MEDIA_HPP
#define VESPERA_KIT_MEDIA_HPP

// Reading a module's managed files: raw bytes with their content hash verified, and RIFF WAVE
// audio decoded to planar floats. Both are safe inside a kit build function: they observe the
// run's cancellation and progress callbacks.
// The kit's WAVE reader is deliberately independent of dsp/bench/wav.hpp, whose checks and
// tradeoffs serve offline bench inputs rather than untrusted module files.

#include <vespera/file_content.hpp>
#include <vespera/kit/files.hpp>
#include <vespera/module.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace vespera::kit {

// The run of a build function.
class Job {
public:
  explicit Job(const vs_work_run *run = nullptr) noexcept : run_(run) {}
  // True once a newer request or release makes this build pointless; return or throw promptly.
  bool cancelled() const noexcept { return run_ && run_->cancelled(run_) != 0; }
  void progress(float fraction) const noexcept {
    if (run_) {
      run_->progress(run_, fraction);
    }
  }

private:
  const vs_work_run *run_;
};

inline std::vector<uint8_t> read_file(const File &file, Job &job) {
  if (file.state != VS_FILE_PRESENT) {
    throw std::runtime_error("file is missing");
  }
  auto bytes = read_file_content(file.path, std::max(uint64_t{1}, file.bytes),
                                 [&job] { return job.cancelled(); });
  if (bytes.size() != file.bytes ||
      content_sha256(bytes, [&job] { return job.cancelled(); }) != file.sha256) {
    throw std::runtime_error("file content changed; Locate or choose it again");
  }
  return bytes;
}

struct AudioData {
  uint32_t sample_rate = 0;
  std::vector<std::vector<float>> channels;
};

inline AudioData read_audio(const File &file, Job &job) {
  const auto bytes = read_file(file, job);
  const auto u16 = [&](size_t at) {
    return uint32_t{bytes.at(at)} | (uint32_t{bytes.at(at + 1)} << 8);
  };
  const auto u32 = [&](size_t at) { return u16(at) | (u16(at + 2) << 16); };
  const auto tag = [&](size_t at, const char *text) {
    return at <= bytes.size() && bytes.size() - at >= 4 &&
           std::memcmp(bytes.data() + at, text, 4) == 0;
  };
  if (bytes.size() < 12 || !tag(0, "RIFF") || !tag(8, "WAVE") ||
      uint64_t{u32(4)} + 8 != bytes.size()) {
    throw std::runtime_error("Expected a complete RIFF WAVE file");
  }
  uint32_t encoding = 0, channels = 0, rate = 0, bits = 0, alignment = 0;
  size_t start = 0, size = 0;
  bool format_seen = false, data_seen = false;
  for (size_t at = 12; at < bytes.size();) {
    if (bytes.size() - at < 8) {
      throw std::runtime_error("Truncated WAVE chunk");
    }
    const size_t length = u32(at + 4);
    if (length > bytes.size() - at - 8) {
      throw std::runtime_error("Truncated WAVE data");
    }
    if (tag(at, "fmt ")) {
      if (format_seen || length < 16) {
        throw std::runtime_error("Invalid WAVE format");
      }
      format_seen = true;
      encoding = u16(at + 8);
      channels = u16(at + 10);
      rate = u32(at + 12);
      alignment = u16(at + 20);
      bits = u16(at + 22);
    } else if (tag(at, "data")) {
      if (data_seen) {
        throw std::runtime_error("Duplicate WAVE data");
      }
      data_seen = true;
      start = at + 8;
      size = length;
    }
    const size_t padded = length + (length & 1);
    if (padded > bytes.size() - at - 8) {
      throw std::runtime_error("Missing WAVE chunk padding");
    }
    at += 8 + padded;
  }
  if (!format_seen || !data_seen || channels < 1 || channels > 2 || rate < 8000 || rate > 192000 ||
      !((encoding == 1 && (bits == 16 || bits == 24)) || (encoding == 3 && bits == 32)) ||
      alignment != channels * (bits / 8) || !size || size % alignment ||
      size / alignment > (size_t{1} << 19)) {
    throw std::runtime_error(
        "Unsupported WAVE format or length (PCM16/24 or float32, mono/stereo)");
  }
  AudioData result{rate,
                   std::vector<std::vector<float>>(channels, std::vector<float>(size / alignment))};
  for (size_t frame = 0; frame < size / alignment; ++frame) {
    if ((frame % 256) == 0 && job.cancelled()) {
      throw std::runtime_error("Audio decode cancelled");
    }
    for (size_t channel = 0; channel < channels; ++channel) {
      const size_t at = start + frame * alignment + channel * (bits / 8);
      float value = 0;
      if (encoding == 3) {
        value = std::bit_cast<float>(u32(at));
      } else {
        uint32_t word = u16(at);
        if (bits == 24) {
          word |= uint32_t{bytes[at + 2]} << 16;
        }
        const int32_t signed_value =
            static_cast<int32_t>(word) -
            ((word & (uint32_t{1} << (bits - 1))) ? static_cast<int32_t>(uint32_t{1} << bits) : 0);
        value = static_cast<float>(signed_value) / static_cast<float>(uint32_t{1} << (bits - 1));
      }
      if (!std::isfinite(value)) {
        throw std::runtime_error("WAVE contains non-finite samples");
      }
      result.channels[channel][frame] = value;
    }
  }
  return result;
}

}

#endif
