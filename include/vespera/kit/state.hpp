#ifndef VESPERA_KIT_STATE_HPP
#define VESPERA_KIT_STATE_HPP

#include <vespera/binary32.hpp>
#include <vespera/kit/parameters.hpp>
#include <vespera/module.h>

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace vespera::kit {

/*
 * Saved state is one little-endian IEEE 754 binary32 per parameter in table order, followed by
 * the bytes of the config's optional Extra. Schema 1 with no Extra is the layout every
 * parameter-only vespera module has used, so its bytes never change.
 *
 * A config may declare:
 *   static constexpr uint32_t state_schema;  // default 1
 *   using Extra = T;  // non-parameter state, copyable, with
 *                     //   uint64_t size() const noexcept;
 *                     //   void save(std::span<uint8_t> bytes) const noexcept;  // size() bytes
 *                     //   bool load(std::span<const uint8_t> bytes);  // false rejects them
 *   static vs_result migrate(uint32_t schema, std::span<const uint8_t> bytes,
 *                            std::vector<uint8_t> &current);
 * migrate rewrites state saved under an older schema as current-schema bytes, which are then
 * validated like any other; VS_ERROR_UNSUPPORTED declines the schema.
 */

// The binary32 codec itself lives in <vespera/binary32.hpp>, shared with the AU SDK; the kit
// re-exports its former names.
using vespera::binary32_bytes;
using vespera::load_binary32;
using vespera::store_binary32;

struct NoExtra {};

namespace detail {

template <class Config> struct ExtraOf {
  using type = NoExtra;
};
template <class Config>
  requires requires { typename Config::Extra; }
struct ExtraOf<Config> {
  using type = typename Config::Extra;
};

template <class Config> constexpr uint32_t state_schema() {
  if constexpr (requires { Config::state_schema; }) {
    return Config::state_schema;
  } else {
    return 1;
  }
}

}

template <class Config> class StateCodec {
public:
  using Extra = typename detail::ExtraOf<Config>::type;
  static constexpr auto &table = Config::parameters;
  static constexpr size_t count = table.size();
  static constexpr uint32_t schema = detail::state_schema<Config>();
  static constexpr uint64_t parameter_bytes = count * binary32_bytes;
  static constexpr bool has_extra = !std::same_as<Extra, NoExtra>;
  static constexpr bool migrates =
      requires(std::span<const uint8_t> bytes, std::vector<uint8_t> &current) {
        { Config::migrate(uint32_t{}, bytes, current) } -> std::same_as<vs_result>;
      };
  using Values = std::array<float, count>;

  static_assert(schema != 0, "state_schema must be positive");
  static_assert(
      !has_extra ||
          requires(const Extra &extra, Extra &target, std::span<uint8_t> out,
                   std::span<const uint8_t> in) {
            { extra.size() } noexcept -> std::convertible_to<uint64_t>;
            { extra.save(out) } noexcept;
            { target.load(in) } -> std::same_as<bool>;
          },
      "Extra must provide size() noexcept, save(span) noexcept and bool load(span)");

  static bool supports(uint32_t saved_schema) noexcept {
    return saved_schema == schema || migrates;
  }

  // Rejects byte counts that cannot hold current-schema state; migrated state is checked after
  // migration.
  static bool plausible(uint32_t saved_schema, uint64_t bytes) noexcept {
    if (saved_schema != schema) {
      return true;
    }
    return has_extra ? bytes >= parameter_bytes : bytes == parameter_bytes;
  }

  static uint64_t size(const Extra &extra) noexcept {
    if constexpr (has_extra) {
      return parameter_bytes + extra.size();
    } else {
      return parameter_bytes;
    }
  }

  static void save(const Values &values, const Extra &extra, uint8_t *out) noexcept {
    for (size_t i = 0; i < count; ++i) {
      store_binary32(values[i], out + i * binary32_bytes);
    }
    if constexpr (has_extra) {
      extra.save(std::span<uint8_t>(out + parameter_bytes, extra.size()));
    }
  }

  // Leaves values and extra untouched unless every byte is accepted.
  static vs_result load(uint32_t saved_schema, std::span<const uint8_t> bytes, Values &values,
                        Extra &extra) {
    if (saved_schema == schema) {
      return decode(bytes, values, extra);
    }
    if constexpr (migrates) {
      std::vector<uint8_t> current;
      const vs_result result = Config::migrate(saved_schema, bytes, current);
      return result == VS_OK ? decode(current, values, extra) : result;
    } else {
      return VS_ERROR_UNSUPPORTED;
    }
  }

private:
  static vs_result decode(std::span<const uint8_t> bytes, Values &values, Extra &extra) {
    if (has_extra ? bytes.size() < parameter_bytes : bytes.size() != parameter_bytes) {
      return VS_ERROR_ARGUMENT;
    }
    Values decoded{};
    for (size_t i = 0; i < count; ++i) {
      decoded[i] = load_binary32(bytes.data() + i * binary32_bytes);
      if (!table.accepts(i, decoded[i])) {
        return VS_ERROR_ARGUMENT;
      }
    }
    if constexpr (has_extra) {
      Extra candidate = extra;
      if (!candidate.load(bytes.subspan(parameter_bytes))) {
        return VS_ERROR_ARGUMENT;
      }
      extra = std::move(candidate);
    }
    values = decoded;
    return VS_OK;
  }
};

}

#endif
