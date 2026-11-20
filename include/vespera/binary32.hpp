#ifndef VESPERA_BINARY32_HPP
#define VESPERA_BINARY32_HPP

#include <bit>
#include <cstddef>
#include <cstdint>

namespace vespera {

inline constexpr uint64_t binary32_bytes = 4;

// One little-endian IEEE 754 binary32, the byte order shared by module parameter state in the
// kit and the AU SDK.
inline void store_binary32(float value, uint8_t *out) noexcept {
  const uint32_t bits = std::bit_cast<uint32_t>(value);
  for (size_t byte = 0; byte < binary32_bytes; ++byte) {
    out[byte] = static_cast<uint8_t>(bits >> (byte * 8));
  }
}

inline float load_binary32(const uint8_t *in) noexcept {
  uint32_t bits = 0;
  for (size_t byte = 0; byte < binary32_bytes; ++byte) {
    bits |= static_cast<uint32_t>(in[byte]) << (byte * 8);
  }
  return std::bit_cast<float>(bits);
}

}

#endif
