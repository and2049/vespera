#ifndef VESPERA_FILES_HPP
#define VESPERA_FILES_HPP

#include <vespera/module.h>
#include <vespera/parameter_display.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace vespera {

inline constexpr uint32_t max_file_slots = 16;
inline constexpr uint64_t max_asset_bytes = 64 * 1024 * 1024;

struct FileSlotMetadata {
  uint32_t id = 0;
  std::string name;
  std::string types;
  std::string bundled;
  uint64_t max_bytes = 0;
  bool optional = false;
};

struct FileReference {
  uint32_t state = VS_FILE_NONE;
  std::string path;
  std::string name;
  uint64_t bytes = 0;
  std::array<uint8_t, 32> sha256{};

  vs_file_ref descriptor() const {
    vs_file_ref result{
        sizeof(vs_file_ref), state, state == VS_FILE_PRESENT ? path.c_str() : nullptr,
        name.c_str(),        bytes, {}};
    std::copy(sha256.begin(), sha256.end(), result.sha256);
    return result;
  }
  std::string key(double rate, uint32_t block, std::string_view parameters = {}) const {
    if (!std::isfinite(rate) || rate <= 0 || !block || parameters.size() > 512) {
      throw std::invalid_argument("invalid file build key");
    }
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (uint8_t byte : sha256) {
      result += digits[byte >> 4];
      result += digits[byte & 15];
    }
    return result + ":" + std::to_string(std::bit_cast<uint64_t>(rate)) + ":" +
           std::to_string(block) + ":" + std::string(parameters);
  }
};

inline bool safe_relative_file_path(std::string_view text) {
  if (text.empty() || text.size() > 4096 || text.find('\0') != text.npos ||
      text.find('\\') != text.npos || text.find(':') != text.npos || text.find("//") != text.npos ||
      text.back() == '/' || !display_detail::valid_utf8(text)) {
    return false;
  }
  const std::filesystem::path path(text);
  if (path.is_absolute() || path.has_root_name()) {
    return false;
  }
  for (const auto &part : path) {
    if (part == ".." || part == "." || part.empty()) {
      return false;
    }
  }
  return true;
}

inline bool valid_file_types(std::string_view text) {
  if (text.empty() || text.size() > 255) {
    return false;
  }
  bool letter = false;
  for (char c : text) {
    if (c == ',') {
      if (!letter) {
        return false;
      }
      letter = false;
    } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
      letter = true;
    } else {
      return false;
    }
  }
  return letter;
}

inline std::optional<std::vector<FileSlotMetadata>>
describe_file_slots(const vs_module_files_api *api, std::string *error = nullptr) {
  const auto fail = [&](const char *message) -> std::optional<std::vector<FileSlotMetadata>> {
    if (error) {
      *error = message;
    }
    return std::nullopt;
  };
  if (!api) {
    return std::vector<FileSlotMetadata>{};
  }
  if (api->struct_size < sizeof(vs_module_files_api) ||
      api->abi_version != VS_MODULE_FILES_ABI_VERSION || !api->set_file ||
      api->slot_count > max_file_slots || api->slot_size < sizeof(vs_file_slot) ||
      api->slot_size > 4096 || (api->slot_count && !api->slots)) {
    return fail("invalid files descriptor");
  }
  std::vector<FileSlotMetadata> result;
  std::set<uint32_t> ids;
  for (uint32_t i = 0; i < api->slot_count; ++i) {
    vs_file_slot slot{};
    std::memcpy(&slot, reinterpret_cast<const uint8_t *>(api->slots) + size_t{i} * api->slot_size,
                sizeof(slot));
    if (!slot.id || !ids.insert(slot.id).second || !slot.name || strnlen(slot.name, 256) == 0 ||
        strnlen(slot.name, 256) == 256 || !display_detail::valid_utf8(slot.name) || !slot.types ||
        strnlen(slot.types, 256) == 256 || !valid_file_types(slot.types) || !slot.max_bytes ||
        slot.max_bytes > max_asset_bytes || (slot.flags & ~uint32_t{VS_FILE_SLOT_OPTIONAL}) ||
        (slot.bundled &&
         (strnlen(slot.bundled, 4097) > 4096 || !safe_relative_file_path(slot.bundled)))) {
      return fail("invalid file slot");
    }
    result.push_back({slot.id, slot.name, slot.types, slot.bundled ? slot.bundled : "",
                      slot.max_bytes, bool(slot.flags & VS_FILE_SLOT_OPTIONAL)});
  }
  return result;
}

inline bool valid_file_reference(const vs_file_ref &file, uint64_t limit) {
  if (file.struct_size < sizeof(vs_file_ref) || file.state > VS_FILE_MISSING ||
      file.bytes > limit) {
    return false;
  }
  if (file.state == VS_FILE_NONE) {
    return !file.path && (!file.name || !*file.name) && file.bytes == 0 &&
           std::all_of(std::begin(file.sha256), std::end(file.sha256),
                       [](uint8_t byte) { return byte == 0; });
  }
  if (!file.name || strnlen(file.name, 256) == 0 || strnlen(file.name, 256) == 256 ||
      !display_detail::valid_utf8(file.name)) {
    return false;
  }
  if (file.state == VS_FILE_MISSING) {
    return !file.path;
  }
  return file.path && strnlen(file.path, 4097) <= 4096 && display_detail::valid_utf8(file.path) &&
         std::filesystem::path(file.path).is_absolute();
}

inline bool valid_file_reference(const FileReference &file, uint64_t limit) {
  return (file.state != VS_FILE_NONE || file.path.empty()) && file.path.size() <= 4096 &&
         file.name.size() < 256 && file.path.find('\0') == std::string::npos &&
         file.name.find('\0') == std::string::npos && display_detail::valid_utf8(file.path) &&
         valid_file_reference(file.descriptor(), limit);
}

}

#endif
