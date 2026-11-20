#ifndef VESPERA_PRESET_HPP
#define VESPERA_PRESET_HPP

#include <vespera/runtime.hpp>

#include <filesystem>
#include <string>
#include <string_view>

namespace vespera {

inline constexpr std::size_t max_preset_file_bytes = 40 * 1024 * 1024;

std::string serialize_preset(const Preset &preset);
Preset parse_preset(std::string_view text);
Preset read_preset(const std::filesystem::path &path);
void write_preset(const std::filesystem::path &path, const Preset &preset);

}

#endif
