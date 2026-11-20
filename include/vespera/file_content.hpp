#ifndef VESPERA_FILE_CONTENT_HPP
#define VESPERA_FILE_CONTENT_HPP

#include <vespera/files.hpp>

#include <array>
#include <bit>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include <fstream>
#endif

namespace vespera {

inline std::array<uint8_t, 32> content_sha256(const std::vector<uint8_t> &bytes,
                                              const std::function<bool()> &cancelled = {}) {
  constexpr std::array<uint32_t, 64> constants{
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
      0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
      0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
      0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
      0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
      0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
      0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
      0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
      0xc67178f2};
  std::array<uint32_t, 8> state{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  const size_t blocks = (bytes.size() + 9 + 63) / 64;
  const uint64_t bits = uint64_t{bytes.size()} * 8;
  for (size_t block = 0; block < blocks; ++block) {
    if ((block % 256) == 0 && cancelled && cancelled()) {
      throw std::runtime_error("hash cancelled");
    }
    std::array<uint32_t, 64> words{};
    for (size_t i = 0; i < 64; ++i) {
      const size_t offset = block * 64 + i;
      const auto value = offset < bytes.size()    ? bytes[offset]
                         : offset == bytes.size() ? uint8_t{0x80}
                         : offset >= blocks * 64 - 8
                             ? static_cast<uint8_t>(bits >> ((blocks * 64 - 1 - offset) * 8))
                             : uint8_t{0};
      words[i / 4] |= uint32_t{value} << (24 - (i % 4) * 8);
    }
    for (size_t i = 16; i < 64; ++i) {
      const uint32_t a = words[i - 15], b = words[i - 2];
      words[i] = words[i - 16] + (std::rotr(a, 7) ^ std::rotr(a, 18) ^ (a >> 3)) + words[i - 7] +
                 (std::rotr(b, 17) ^ std::rotr(b, 19) ^ (b >> 10));
    }
    auto [a, b, c, d, e, f, g, h] = state;
    for (size_t i = 0; i < 64; ++i) {
      const uint32_t t = h + (std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25)) +
                         ((e & f) ^ (~e & g)) + constants[i] + words[i];
      const uint32_t u =
          (std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      h = g;
      g = f;
      f = e;
      e = d + t;
      d = c;
      c = b;
      b = a;
      a = t + u;
    }
    const std::array<uint32_t, 8> work{a, b, c, d, e, f, g, h};
    for (size_t i = 0; i < state.size(); ++i) {
      state[i] += work[i];
    }
  }
  std::array<uint8_t, 32> hash{};
  for (size_t i = 0; i < hash.size(); ++i) {
    hash[i] = static_cast<uint8_t>(state[i / 4] >> (24 - (i % 4) * 8));
  }
  return hash;
}

// The value of one lowercase hex digit, or -1 when `c` is not one.
inline int hex_digit(char c) noexcept {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  return -1;
}

inline std::string file_hash_text(const std::array<uint8_t, 32> &hash) {
  constexpr char digits[] = "0123456789abcdef";
  std::string text;
  text.reserve(64);
  for (uint8_t byte : hash) {
    text += digits[byte >> 4];
    text += digits[byte & 15];
  }
  return text;
}

inline std::vector<uint8_t> read_file_content(const std::filesystem::path &path, uint64_t limit,
                                              const std::function<bool()> &cancelled = {}) {
  if (!limit || limit > max_asset_bytes) {
    throw std::invalid_argument("invalid file byte limit");
  }
  if (cancelled && cancelled()) {
    throw std::runtime_error("file load cancelled");
  }
  if (path.native().find(std::filesystem::path::value_type{}) !=
      std::filesystem::path::string_type::npos) {
    throw std::invalid_argument("file path contains NUL");
  }
  std::vector<uint8_t> result;
#if defined(__unix__) || defined(__APPLE__)
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
  if (fd < 0) {
    throw std::runtime_error("file is missing or unreadable");
  }
  struct Close {
    int fd;
    ~Close() { ::close(fd); }
  } close{fd};
  struct stat info{};
  if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
      static_cast<uint64_t>(info.st_size) > limit) {
    throw std::runtime_error("file is not regular or exceeds its byte limit");
  }
  result.resize(static_cast<size_t>(info.st_size));
  size_t used = 0;
  while (used < result.size()) {
    if (cancelled && cancelled()) {
      throw std::runtime_error("file load cancelled");
    }
    const ssize_t count =
        ::read(fd, result.data() + used, std::min<size_t>(65536, result.size() - used));
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0) {
      throw std::runtime_error("file read failed");
    }
    if (count == 0) {
      throw std::runtime_error("file changed while reading");
    }
    used += static_cast<size_t>(count);
  }
  for (;;) {
    if (cancelled && cancelled()) {
      throw std::runtime_error("file load cancelled");
    }
    uint8_t extra = 0;
    const auto count = ::read(fd, &extra, 1);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count != 0) {
      throw std::runtime_error("file changed while reading");
    }
    break;
  }
#else
  if (std::filesystem::is_symlink(path) || !std::filesystem::is_regular_file(path)) {
    throw std::runtime_error("file is not regular");
  }
  const auto size = std::filesystem::file_size(path);
  if (size > limit) {
    throw std::length_error("file exceeds its byte limit");
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("file is unreadable");
  }
  result.resize(static_cast<size_t>(size));
  for (size_t used = 0; used < result.size();) {
    if (cancelled && cancelled()) {
      throw std::runtime_error("file load cancelled");
    }
    const auto count = std::min<size_t>(65536, result.size() - used);
    input.read(reinterpret_cast<char *>(result.data() + used), static_cast<std::streamsize>(count));
    if (input.gcount() != static_cast<std::streamsize>(count)) {
      throw std::runtime_error("file changed while reading");
    }
    used += count;
  }
  if ((cancelled && cancelled()) || input.get() != EOF || input.bad()) {
    throw std::runtime_error("file changed while reading");
  }
#endif
  return result;
}

inline FileReference resolve_file_reference(FileReference file, uint64_t limit) {
  if (file.state == VS_FILE_NONE) {
    return file;
  }
  try {
    const auto bytes = read_file_content(file.path, limit);
    if (bytes.size() != file.bytes || content_sha256(bytes) != file.sha256) {
      throw std::runtime_error("file content changed");
    }
    file.state = VS_FILE_PRESENT;
  } catch (...) {
    file.state = VS_FILE_MISSING;
  }
  return file;
}

}

#endif
