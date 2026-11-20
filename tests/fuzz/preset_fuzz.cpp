#include <vespera/preset.hpp>

#include <cstdlib>
#include <stdexcept>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  vespera::Preset preset;
  try {
    preset = vespera::parse_preset(std::string_view(reinterpret_cast<const char *>(data), size));
  } catch (const std::invalid_argument &) {
    return 0;
  } catch (const std::length_error &) {
    return 0;
  }
  const auto serialized = vespera::serialize_preset(preset);
  if (vespera::serialize_preset(vespera::parse_preset(serialized)) != serialized) {
    std::abort();
  }
  return 0;
}
