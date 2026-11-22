#ifndef VESPERA_WAV_FILE_HPP
#define VESPERA_WAV_FILE_HPP

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace vespera {

inline constexpr uint32_t wav_min_sample_rate = 8000;
inline constexpr uint32_t wav_max_sample_rate = 384000;
inline constexpr uint32_t wav_max_channels = 2;

struct WavFormat {
  uint32_t sample_rate = 0;
  uint32_t channels = 0;
  uint64_t frames = 0;
};

// Streams a mono or stereo WAV file as binary32 samples without loading it whole. Accepts
// integer PCM (16, 24 or 32 bit) and binary32 float, plain or WAVE_FORMAT_EXTENSIBLE. Integer
// samples scale by 2^-(bits-1), so full-scale negative reads as exactly -1. A data chunk that
// claims more bytes than the file holds, as an interrupted recording leaves, is read up to the
// last whole frame present.
//
// The constructor and read() throw std::runtime_error for unreadable, malformed or unsupported
// files.
class WavReader {
public:
  explicit WavReader(const std::filesystem::path &path);
  ~WavReader();
  WavReader(const WavReader &) = delete;
  WavReader &operator=(const WavReader &) = delete;

  [[nodiscard]] const WavFormat &format() const noexcept { return format_; }

  // Fills channels[c][0, n) for every channel of the file and returns n, which is less than
  // `frames` only at the end of the data.
  uint64_t read(float *const *channels, uint64_t frames);

private:
  enum class Encoding { pcm16, pcm24, pcm32, float32 };

  std::FILE *file_ = nullptr;
  WavFormat format_;
  Encoding encoding_ = Encoding::pcm16;
  uint32_t bytes_per_sample_ = 0;
  uint64_t position_ = 0;
  std::vector<unsigned char> buffer_;
};

// Writes a binary32 float WAV file. The header always describes a complete file up to the last
// sync() or close(), so a writer that dies between them leaves a file WavReader still reads to
// the end of what reached the disk.
//
// A WAV file holds less than 4 GiB of samples; write() throws std::length_error rather than
// exceed that, and std::runtime_error when the disk refuses the data. The destructor closes
// without throwing.
class WavWriter {
public:
  WavWriter(const std::filesystem::path &path, uint32_t sample_rate, uint32_t channels);
  ~WavWriter();
  WavWriter(const WavWriter &) = delete;
  WavWriter &operator=(const WavWriter &) = delete;

  // Frames that still fit under the format's size limit.
  [[nodiscard]] uint64_t remaining_frames() const noexcept;
  [[nodiscard]] uint64_t frames() const noexcept { return frames_; }

  void write(const float *const *channels, uint64_t frames);
  // `samples` holds frames * channels values, frame by frame.
  void write_interleaved(const float *samples, uint64_t frames);

  // Rewrites the header for the frames written so far and flushes them to the file.
  void sync();
  void close();

private:
  void write_header();

  std::FILE *file_ = nullptr;
  uint32_t sample_rate_ = 0;
  uint32_t channels_ = 0;
  uint64_t frames_ = 0;
  std::vector<unsigned char> buffer_;
};

}

#endif
