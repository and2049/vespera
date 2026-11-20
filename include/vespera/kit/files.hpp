#ifndef VESPERA_KIT_FILES_HPP
#define VESPERA_KIT_FILES_HPP

#include <vespera/files.hpp>

#include <array>
#include <cstdint>

namespace vespera::kit {

class FileSlot {
public:
  constexpr FileSlot(uint32_t id, const char *name)
      : value_{id, name, "wav", nullptr, 8 << 20, 0} {}
  constexpr FileSlot types(const char *value) const {
    auto copy = *this;
    copy.value_.types = value;
    return copy;
  }
  constexpr FileSlot bundled(const char *value) const {
    auto copy = *this;
    copy.value_.bundled = value;
    return copy;
  }
  constexpr FileSlot limit(uint64_t value) const {
    auto copy = *this;
    copy.value_.max_bytes = value;
    return copy;
  }
  constexpr FileSlot optional() const {
    auto copy = *this;
    copy.value_.flags |= VS_FILE_SLOT_OPTIONAL;
    return copy;
  }
  constexpr vs_file_slot descriptor() const { return value_; }

private:
  vs_file_slot value_;
};

constexpr FileSlot file(uint32_t id, const char *name) { return FileSlot(id, name); }

template <size_t N> struct FileSlots {
  std::array<vs_file_slot, N> descriptors;
};

template <class... Slots> constexpr auto files(Slots... slots) {
  static_assert(sizeof...(Slots) <= max_file_slots);
  return FileSlots<sizeof...(Slots)>{{slots.descriptor()...}};
}

using File = FileReference;

}

#endif
