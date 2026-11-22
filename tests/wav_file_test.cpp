#include "wav_file.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

void check(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "FAILED: %s\n", message);
    std::exit(1);
  }
}

template <typename Call> bool throws(Call &&call) {
  try {
    call();
  } catch (const std::exception &) {
    return true;
  }
  return false;
}

void put(std::string &bytes, uint32_t value, int size) {
  for (int i = 0; i < size; ++i) {
    bytes.push_back(static_cast<char>(value >> (8 * i)));
  }
}

std::string wav(uint16_t tag, uint16_t channels, uint32_t rate, uint16_t bits,
                const std::string &data, uint32_t claimed, bool extensible = false,
                const std::string &extra_chunk = {}) {
  std::string format;
  put(format, extensible ? 0xfffeu : tag, 2);
  put(format, channels, 2);
  put(format, rate, 4);
  put(format, rate * channels * bits / 8u, 4);
  put(format, channels * bits / 8u, 2);
  put(format, bits, 2);
  if (extensible) {
    put(format, 22, 2);
    put(format, bits, 2);
    put(format, 0, 4);
    put(format, tag, 2);
    format.append(14, '\0');
  }
  std::string body = "WAVE" + extra_chunk + "fmt ";
  put(body, static_cast<uint32_t>(format.size()), 4);
  body += format + "data";
  put(body, claimed, 4);
  body += data;
  std::string file = "RIFF";
  put(file, static_cast<uint32_t>(body.size()), 4);
  return file + body;
}

struct Scratch {
  std::filesystem::path directory;
  Scratch() {
    directory =
        std::filesystem::temp_directory_path() / ("vespera-wav-test-" + std::to_string(getpid()));
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
  }
  ~Scratch() { std::filesystem::remove_all(directory); }
  std::filesystem::path file(const std::string &name, const std::string &bytes) const {
    const auto path = directory / name;
    std::ofstream(path, std::ios::binary) << bytes;
    return path;
  }
};

std::vector<std::vector<float>> read_all(vespera::WavReader &reader, uint64_t step) {
  const auto format = reader.format();
  std::vector<std::vector<float>> channels(format.channels, std::vector<float>(format.frames));
  uint64_t done = 0;
  for (;;) {
    std::array<float *, 2> destination{};
    std::vector<std::vector<float>> block(format.channels, std::vector<float>(step));
    for (uint32_t c = 0; c < format.channels; ++c) {
      destination[c] = block[c].data();
    }
    const uint64_t count = reader.read(destination.data(), step);
    for (uint32_t c = 0; c < format.channels; ++c) {
      for (uint64_t f = 0; f < count; ++f) {
        channels[c][done + f] = block[c][f];
      }
    }
    done += count;
    if (count < step) {
      break;
    }
  }
  check(done == format.frames, "the reader delivers every frame it announced");
  return channels;
}

void round_trip(const Scratch &scratch) {
  const auto path = scratch.directory / "round.wav";
  const uint64_t frames = 10000;
  std::vector<float> left(frames), right(frames);
  for (uint64_t f = 0; f < frames; ++f) {
    left[f] = static_cast<float>(f) * 1.0e-4f - 0.5f;
    right[f] = -left[f] * 3.0f;
  }
  left[7] = -0.0f;
  right[9] = std::numeric_limits<float>::denorm_min();
  {
    vespera::WavWriter writer(path, 44100, 2);
    const std::array<const float *, 2> first{left.data(), right.data()};
    writer.write(first.data(), 6000);
    std::vector<float> interleaved;
    for (uint64_t f = 6000; f < frames; ++f) {
      interleaved.push_back(left[f]);
      interleaved.push_back(right[f]);
    }
    writer.write_interleaved(interleaved.data(), frames - 6000);
    check(writer.frames() == frames, "the writer counts frames");
  }
  vespera::WavReader reader(path);
  check(reader.format().sample_rate == 44100 && reader.format().channels == 2 &&
            reader.format().frames == frames,
        "the written format reads back");
  const auto channels = read_all(reader, 777);
  for (uint64_t f = 0; f < frames; ++f) {
    check(std::bit_cast<uint32_t>(channels[0][f]) == std::bit_cast<uint32_t>(left[f]) &&
              std::bit_cast<uint32_t>(channels[1][f]) == std::bit_cast<uint32_t>(right[f]),
          "float samples survive bit for bit");
  }
}

void interrupted_writer(const Scratch &scratch) {
  const auto path = scratch.directory / "interrupted.wav";
  const auto copy = scratch.directory / "copy.wav";
  std::vector<float> samples(5000, 0.25f);
  vespera::WavWriter writer(path, 48000, 1);
  const float *channel = samples.data();
  writer.write(&channel, 3000);
  writer.sync();
  writer.write(&channel, 2000);
  std::filesystem::copy_file(path, copy);
  vespera::WavReader reader(copy);
  check(reader.format().frames >= 3000, "a file abandoned after sync() keeps the synced frames");
  read_all(reader, 4096);
}

void integer_formats(const Scratch &scratch) {
  std::string pcm16;
  put(pcm16, 0x8000, 2);
  put(pcm16, 0x4000, 2);
  put(pcm16, 0x7fff, 2);
  vespera::WavReader r16(scratch.file("pcm16.wav", wav(1, 1, 48000, 16, pcm16, 6)));
  auto s16 = read_all(r16, 2);
  check(s16[0][0] == -1.0f && s16[0][1] == 0.5f && s16[0][2] == 32767.0f / 32768.0f,
        "16-bit samples scale by 2^-15");

  std::string pcm24;
  put(pcm24, 0x800000, 3);
  put(pcm24, 0x400000, 3);
  put(pcm24, 0xffffff, 3);
  put(pcm24, 0x200000, 3);
  vespera::WavReader r24(scratch.file(
      "pcm24.wav", wav(1, 2, 96000, 24, pcm24, 12, true, std::string("LIST\x03\0\0\0abc\0", 12))));
  auto s24 = read_all(r24, 1);
  check(r24.format().frames == 2 && s24[0][0] == -1.0f && s24[1][0] == 0.5f &&
            s24[0][1] == -1.0f / 8388608.0f && s24[1][1] == 0.25f,
        "24-bit extensible stereo is sign-extended after an odd-sized chunk");

  std::string pcm32;
  put(pcm32, 0x80000000u, 4);
  put(pcm32, 0x20000000u, 4);
  vespera::WavReader r32(scratch.file("pcm32.wav", wav(1, 1, 8000, 32, pcm32, 8)));
  auto s32 = read_all(r32, 8);
  check(s32[0][0] == -1.0f && s32[0][1] == 0.25f, "32-bit integer samples scale by 2^-31");
}

void rejections(const Scratch &scratch) {
  const std::string two(8, '\0');
  const auto rejects = [&](const std::string &name, const std::string &bytes) {
    const auto path = scratch.file(name, bytes);
    return throws([&] { vespera::WavReader reader(path); });
  };
  check(throws([&] { vespera::WavReader reader(scratch.directory / "absent.wav"); }),
        "a missing file is rejected");
  check(rejects("empty.wav", ""), "an empty file is rejected");
  check(rejects("text.wav", "not a wave file at all"), "a non-RIFF file is rejected");
  check(rejects("surround.wav", wav(1, 6, 48000, 16, two, 8)), "more than two channels rejected");
  check(rejects("silent.wav", wav(1, 0, 48000, 16, two, 8)), "zero channels rejected");
  check(rejects("slow.wav", wav(1, 1, 4000, 16, two, 8)), "a rate below the minimum is rejected");
  check(rejects("fast.wav", wav(1, 1, 768000, 16, two, 8)), "a rate above the maximum is rejected");
  check(rejects("eight.wav", wav(1, 1, 48000, 8, two, 8)), "8-bit PCM is rejected");
  check(rejects("double.wav", wav(3, 1, 48000, 64, two, 8)), "binary64 float is rejected");
  check(rejects("adpcm.wav", wav(2, 1, 48000, 16, two, 8)), "compressed formats are rejected");
  auto no_data = wav(1, 1, 48000, 16, "", 0);
  no_data.resize(no_data.size() - 8);
  check(rejects("nodata.wav", no_data), "a file without a data chunk is rejected");
  check(rejects("header.wav", wav(1, 1, 48000, 16, two, 8).substr(0, 20)),
        "a truncated format chunk is rejected");

  vespera::WavReader truncated(
      scratch.file("claims.wav", wav(3, 2, 48000, 32, std::string(20, '\0'), 0xffffffffu)));
  check(truncated.format().frames == 2, "an overstated data size reads the whole frames present");

  check(throws([&] { vespera::WavWriter writer(scratch.directory / "bad.wav", 48000, 3); }) &&
            throws([&] { vespera::WavWriter writer(scratch.directory / "bad.wav", 100, 1); }) &&
            throws([&] {
              vespera::WavWriter writer(scratch.directory / "no/such/dir.wav", 48000, 1);
            }),
        "the writer rejects unsupported formats and unwritable paths");
  vespera::WavWriter writer(scratch.directory / "limit.wav", 48000, 2);
  check(writer.remaining_frames() == (0xffffffffull - 44) / 8, "the size limit is reported");
  const float *none = nullptr;
  check(throws([&] { writer.write(&none, writer.remaining_frames() + 1); }),
        "writing past the size limit is refused before any data is written");
  check(writer.frames() == 0, "a refused write leaves the file unchanged");
}

}

int main() {
  const Scratch scratch;
  round_trip(scratch);
  interrupted_writer(scratch);
  integer_formats(scratch);
  rejections(scratch);
  std::puts("wav_file: ok");
}
