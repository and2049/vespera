#ifndef VESPERA_PARAMETER_DISPLAY_HPP
#define VESPERA_PARAMETER_DISPLAY_HPP

// Text for parameter values: reading a module's parameter metadata (vs_module_metadata_api) as
// untrusted input, and formatting and parsing values with it or with a layout `format` object.
// Every host view and the layout renderer use these functions, so a value reads the same wherever
// it is shown.

#include <vespera/module.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#if defined(__APPLE__)
#include <xlocale.h>
#endif

namespace vespera {

enum class ParameterKind : uint32_t {
  continuous = VS_PARAMETER_CONTINUOUS,
  toggle = VS_PARAMETER_TOGGLE,
  stepped = VS_PARAMETER_STEPPED,
  enumerated = VS_PARAMETER_ENUMERATED
};

enum class OffEnd : uint32_t {
  none = VS_PARAMETER_OFF_NONE,
  minimum = VS_PARAMETER_OFF_MINIMUM,
  maximum = VS_PARAMETER_OFF_MAXIMUM
};

// How a value is shown as a number: a module's described display mapping, or the equivalent
// layout `format` object.
struct DisplayMapping {
  // Shown at the low and high end of the value range; empty shows the value range itself.
  std::optional<std::array<double, 2>> range;
  double skew = 1.0;
  int decimals = 0;
  std::string unit;
  // Shows magnitudes of 1000 and above in thousands with this unit.
  std::optional<std::string> kilo_unit;
  bool show_sign = false;
  OffEnd off = OffEnd::none;
  double off_tolerance = 0.0;
};

// Where a value lies in its range and the number shown for it.
struct ShownNumber {
  double low = 0.0;
  double high = 0.0;
  double skew = 1.0;
  // The value's position within [minimum, maximum], clamped to [0, 1].
  double position = 0.0;
  double shown = 0.0;
};

namespace display_detail {

inline std::string fixed(double value, int decimals) {
  char buffer[64];
#if defined(__APPLE__)
  int length = snprintf_l(buffer, sizeof buffer, nullptr, "%.*f", decimals, value);
#else
  int length = std::snprintf(buffer, sizeof buffer, "%.*f", decimals, value);
#endif
  if (length < 0) {
    return {};
  }
  if (static_cast<size_t>(length) < sizeof buffer) {
    return std::string(buffer, static_cast<size_t>(length));
  }
  std::string text(static_cast<size_t>(length) + 1, '\0');
#if defined(__APPLE__)
  snprintf_l(text.data(), text.size(), nullptr, "%.*f", decimals, value);
#else
  std::snprintf(text.data(), text.size(), "%.*f", decimals, value);
#endif
  text.resize(static_cast<size_t>(length));
  return text;
}

// Whether a formatted number has no non-zero digit, so it reads as zero.
inline bool reads_as_zero(std::string_view number) {
  return std::none_of(number.begin(), number.end(), [](char c) { return c >= '1' && c <= '9'; });
}

inline bool contains(std::string_view text, std::string_view part) {
  return text.find(part) != std::string_view::npos;
}

inline std::string lowercase(std::string_view text) {
  std::string result(text);
  for (char &c : result) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return result;
}

inline std::string_view trimmed(std::string_view text) {
  const auto space = [](char c) { return c == ' ' || (c >= '\t' && c <= '\r'); };
  while (!text.empty() && space(text.front())) {
    text.remove_prefix(1);
  }
  while (!text.empty() && space(text.back())) {
    text.remove_suffix(1);
  }
  return text;
}

// One UTF-8 sequence at `index`: its byte length, decoded code point and whether it is well
// formed.
struct Utf8Sequence {
  size_t length = 0;
  uint32_t code_point = 0;
  bool valid = false;
};

inline Utf8Sequence decode_utf8(std::string_view text, size_t index) noexcept {
  const auto lead = static_cast<unsigned char>(text[index]);
  if (lead < 0x80) {
    return {1, lead, true};
  }
  size_t length = 0;
  uint32_t minimum = 0;
  if ((lead & 0xe0) == 0xc0) {
    length = 2;
    minimum = 0x80;
  } else if ((lead & 0xf0) == 0xe0) {
    length = 3;
    minimum = 0x800;
  } else if ((lead & 0xf8) == 0xf0) {
    length = 4;
    minimum = 0x10000;
  }
  Utf8Sequence result{length, 0, false};
  if (length == 0 || index + length > text.size()) {
    return result;
  }
  result.code_point = lead & (0x7fu >> length);
  for (size_t k = 1; k < length; ++k) {
    const auto next = static_cast<unsigned char>(text[index + k]);
    if ((next & 0xc0) != 0x80) {
      return result;
    }
    result.code_point = (result.code_point << 6) | (next & 0x3fu);
  }
  result.valid = result.code_point >= minimum && result.code_point <= 0x10ffff &&
                 (result.code_point < 0xd800 || result.code_point > 0xdfff);
  return result;
}

// Rejects overlong encodings, surrogates and code points above U+10FFFF.
inline bool valid_utf8(std::string_view text) noexcept {
  size_t i = 0;
  while (i < text.size()) {
    const Utf8Sequence sequence = decode_utf8(text, i);
    if (!sequence.valid) {
      return false;
    }
    i += sequence.length;
  }
  return true;
}

// A NUL-terminated UTF-8 string shorter than limit bytes; null reads as empty when allowed.
inline std::optional<std::string> text(const char *value, size_t limit, bool allow_null,
                                       bool allow_empty) {
  if (value == nullptr) {
    return allow_null ? std::optional<std::string>(std::string()) : std::nullopt;
  }
  const size_t length = strnlen(value, limit);
  if (length >= limit || (!allow_empty && length == 0) ||
      !valid_utf8(std::string_view(value, length))) {
    return std::nullopt;
  }
  return std::string(value, length);
}

}

inline ShownNumber shown_number(const DisplayMapping &mapping, float minimum, float maximum,
                                double value) {
  ShownNumber result{mapping.range ? (*mapping.range)[0] : minimum,
                     mapping.range ? (*mapping.range)[1] : maximum, mapping.skew, 0, 0};
  result.position =
      maximum > minimum
          ? std::clamp((value - minimum) / (static_cast<double>(maximum) - minimum), 0.0, 1.0)
          : 0;
  const double curved =
      result.skew == 1 ? result.position : std::pow(result.position, 1 / result.skew);
  result.shown = result.low + curved * (result.high - result.low);
  return result;
}

// The text shown for a value of a parameter ranging over [minimum, maximum]. A shown number that
// rounds to zero carries no sign.
inline std::string format_number(const DisplayMapping &mapping, float minimum, float maximum,
                                 double value) {
  const ShownNumber display = shown_number(mapping, minimum, maximum, value);
  const double tolerance = mapping.off_tolerance;
  if ((mapping.off == OffEnd::minimum &&
       (display.position <= 0 || std::abs(display.shown - display.low) < tolerance)) ||
      (mapping.off == OffEnd::maximum &&
       (display.position >= 1 || std::abs(display.high - display.shown) < tolerance))) {
    return "OFF";
  }
  std::string unit = mapping.unit;
  const double shown = display.shown, magnitude = std::abs(shown);
  std::string number;
  if (mapping.kilo_unit && magnitude >= 1000) {
    const int decimals = magnitude < 9995 ? 2 : magnitude < 99950 ? 1 : 0;
    number = display_detail::fixed(shown / 1000, decimals);
    unit = *mapping.kilo_unit;
  } else if (mapping.decimals == 0) {
    number = display_detail::fixed(std::round(shown), 0);
  } else {
    number = display_detail::fixed(shown, mapping.decimals);
  }
  if (!number.empty() && number.front() == '-' && display_detail::reads_as_zero(number)) {
    number.erase(0, 1);
  } else if (mapping.show_sign && shown > 0 && !display_detail::reads_as_zero(number)) {
    number.insert(0, 1, '+');
  }
  return number + unit;
}

// Reads a decimal number at the start of text after leading white space, as typed into a field:
// an optional sign, digits with an optional point, and an optional exponent.
inline std::optional<double> scan_number(std::string_view text) {
  size_t i = 0;
  const auto digit = [&](size_t at) {
    return at < text.size() && text[at] >= '0' && text[at] <= '9';
  };
  while (i < text.size() && (text[i] == ' ' || (text[i] >= '\t' && text[i] <= '\r'))) {
    ++i;
  }
  const size_t start = i;
  if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
    ++i;
  }
  size_t digits = 0;
  for (; digit(i); ++i) {
    ++digits;
  }
  if (i < text.size() && text[i] == '.') {
    for (++i; digit(i); ++i) {
      ++digits;
    }
  }
  if (digits == 0) {
    return std::nullopt;
  }
  if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
    size_t exponent = i + 1;
    if (exponent < text.size() && (text[exponent] == '+' || text[exponent] == '-')) {
      ++exponent;
    }
    if (digit(exponent)) {
      for (i = exponent; digit(i); ++i) {
      }
    }
  }
  const std::string token(text.substr(start, i - start));
#if defined(__APPLE__)
  return strtod_l(token.c_str(), nullptr, nullptr);
#else
  return std::strtod(token.c_str(), nullptr);
#endif
}

// Reads typed text in the units `mapping` shows, given text already lowercased: `off` selects the
// off end, a `k` or a bare number within the kilo range of an all-kilo range reads thousands, and
// numbers are clamped to the range but never select the off end. scan reads the number from the
// text with commas replaced by points.
template <class Scan>
std::optional<float> parse_number(const DisplayMapping &mapping, float minimum, float maximum,
                                  std::string_view lowered, Scan &&scan) {
  if (mapping.off != OffEnd::none && display_detail::contains(lowered, "off")) {
    return mapping.off == OffEnd::minimum ? minimum : maximum;
  }
  std::string normalized(lowered);
  std::replace(normalized.begin(), normalized.end(), ',', '.');
  const std::optional<double> scanned = scan(std::string_view(normalized));
  if (!scanned || !std::isfinite(*scanned)) {
    return std::nullopt;
  }
  if (minimum == maximum) {
    return std::isfinite(minimum) ? std::optional<float>(minimum) : std::nullopt;
  }
  double number = *scanned;
  const ShownNumber display = shown_number(mapping, minimum, maximum, minimum);
  const double low = std::min(display.low, display.high);
  const double high = std::max(display.low, display.high);
  if (mapping.kilo_unit &&
      (display_detail::contains(lowered, "k") || (low >= 1000 && number <= high / 1000))) {
    number *= 1000;
  }
  if (display.low == display.high) {
    return std::nullopt;
  }
  double position = std::clamp((number - display.low) / (display.high - display.low), 0.0, 1.0);
  if (display.skew != 1) {
    position = std::pow(position, display.skew);
  }
  float result = static_cast<float>(minimum + position * (static_cast<double>(maximum) - minimum));
  if (mapping.off == OffEnd::minimum && result <= minimum) {
    result = std::nextafter(minimum, maximum);
  } else if (mapping.off == OffEnd::maximum && result >= maximum) {
    result = std::nextafter(maximum, minimum);
  }
  return std::isfinite(result) ? std::optional<float>(result) : std::nullopt;
}

inline std::optional<float> parse_number(const DisplayMapping &mapping, float minimum,
                                         float maximum, std::string_view text) {
  return parse_number(mapping, minimum, maximum, display_detail::lowercase(text), scan_number);
}

// Everything a host knows about showing one declared parameter.
struct ParameterDisplay {
  uint32_t id = 0;
  float minimum = 0.0f;
  float maximum = 1.0f;
  float default_value = 0.0f;
  // The descriptor's unit.
  std::string unit;
  ParameterKind kind = ParameterKind::continuous;
  // Values of a toggle, stepped or enumerated parameter; 0 for continuous ones.
  uint32_t steps = 0;
  std::vector<std::string> labels;
  // The module's described display mapping.
  std::optional<DisplayMapping> mapping;
  // Whether the module's metadata describes this parameter.
  bool described = false;

  // Whether the module describes how values are shown, with a mapping or labels. Layout widgets
  // without a format of their own then show values this way.
  bool shows_values() const noexcept {
    return mapping.has_value() || kind == ParameterKind::enumerated;
  }

  double position(double value) const noexcept {
    return maximum > minimum
               ? std::clamp((value - minimum) / (static_cast<double>(maximum) - minimum), 0.0, 1.0)
               : 0.0;
  }

  // The step a value is nearest to, counted from the minimum; 0 for continuous parameters.
  uint32_t index(double value) const noexcept {
    if (steps < 2) {
      return 0;
    }
    const double scaled = position(value) * static_cast<double>(steps - 1);
    return static_cast<uint32_t>(std::lround(scaled));
  }

  // value clamped to the range and, for toggles and stepped and enumerated parameters, moved to
  // the nearest value they take.
  float nearest(float value) const noexcept {
    if (!(maximum > minimum)) {
      return minimum;
    }
    value = std::clamp(value, minimum, maximum);
    if (steps < 2) {
      return value;
    }
    const uint32_t step = index(value);
    if (step == 0) {
      return minimum;
    }
    if (step == steps - 1) {
      return maximum;
    }
    return static_cast<float>(minimum + (static_cast<double>(maximum) - minimum) * step /
                                            static_cast<double>(steps - 1));
  }

  // The mapping used when the module describes none: whole percent of the range for a parameter
  // without a unit, otherwise the value with its unit.
  DisplayMapping generic_mapping() const {
    DisplayMapping result;
    if (kind == ParameterKind::stepped) {
      const double spacing = (static_cast<double>(maximum) - minimum) / (steps - 1);
      const bool whole = spacing == std::floor(spacing) && minimum == std::floor(minimum);
      result.decimals = whole ? 0 : 2;
      result.unit = unit.empty() ? "" : " " + unit;
    } else if (unit.empty()) {
      result.range = std::array<double, 2>{0, 100};
      result.unit = "%";
    } else {
      result.decimals = 2;
      result.unit = " " + unit;
    }
    return result;
  }

  std::string text(double value) const {
    if (!std::isfinite(value)) {
      return {};
    }
    switch (kind) {
    case ParameterKind::toggle:
      return position(value) >= 0.5 ? "On" : "Off";
    case ParameterKind::enumerated:
      return labels.empty() ? std::string()
                            : labels[std::min<size_t>(index(value), labels.size() - 1)];
    case ParameterKind::stepped:
      value = nearest(static_cast<float>(value));
      break;
    case ParameterKind::continuous:
      break;
    }
    return format_number(mapping ? *mapping : generic_mapping(), minimum, maximum, value);
  }

  // Reads text as typed by a user: a label, On or Off, or a number in the shown units.
  std::optional<float> parse(std::string_view typed) const {
    const std::string lowered = display_detail::lowercase(display_detail::trimmed(typed));
    if (kind == ParameterKind::enumerated) {
      for (size_t i = 0; i < labels.size(); ++i) {
        if (display_detail::lowercase(labels[i]) == lowered) {
          return nearest(static_cast<float>(minimum + (static_cast<double>(maximum) - minimum) *
                                                          static_cast<double>(i) /
                                                          static_cast<double>(steps - 1)));
        }
      }
      return std::nullopt;
    }
    if (kind == ParameterKind::toggle) {
      if (lowered == "on" || lowered == "1") {
        return maximum;
      }
      if (lowered == "off" || lowered == "0") {
        return minimum;
      }
      return std::nullopt;
    }
    const std::optional<float> value = parse_number(mapping ? *mapping : generic_mapping(), minimum,
                                                    maximum, lowered, scan_number);
    if (!value) {
      return std::nullopt;
    }
    return kind == ParameterKind::stepped ? nearest(*value) : *value;
  }
};

namespace display_detail {

constexpr uint32_t max_labels = 256;
constexpr uint32_t max_steps = 65536;
constexpr double max_shown = 1e9;
constexpr size_t max_unit_bytes = 65;
constexpr size_t max_label_bytes = 256;

inline std::string describe(uint32_t id) { return "parameter " + std::to_string(id); }

// Why one described parameter is invalid, or an empty string when it is valid.
inline std::string check(const vs_parameter_metadata &entry, ParameterDisplay &parameter,
                         const vs_module_api &api) {
  const std::string where = describe(entry.parameter_id);
  const uint32_t known_flags = VS_PARAMETER_DISPLAY_MAPPED | VS_PARAMETER_DISPLAY_SIGNED;
  if ((entry.flags & ~known_flags) != 0) {
    return where + " declares unknown display flags";
  }
  const bool mapped = (entry.flags & VS_PARAMETER_DISPLAY_MAPPED) != 0;
  if ((entry.flags & VS_PARAMETER_DISPLAY_SIGNED) != 0 && !mapped) {
    return where + " is signed without a display mapping";
  }
  const bool ranged = parameter.maximum > parameter.minimum;
  switch (entry.kind) {
  case VS_PARAMETER_CONTINUOUS:
    if (entry.steps != 0 || entry.labels != nullptr) {
      return where + " is continuous but declares steps or labels";
    }
    break;
  case VS_PARAMETER_TOGGLE:
    if (entry.steps != 2 || entry.labels != nullptr || mapped || !ranged) {
      return where + " is a toggle, which needs 2 steps, no labels, no display mapping and a "
                     "range with its minimum below its maximum";
    }
    break;
  case VS_PARAMETER_STEPPED:
  case VS_PARAMETER_ENUMERATED: {
    const bool enumerated = entry.kind == VS_PARAMETER_ENUMERATED;
    const uint32_t limit = enumerated ? max_labels : max_steps;
    if (entry.steps < 2 || entry.steps > limit || !ranged) {
      return where + " needs 2 to " + std::to_string(limit) +
             " steps over a range with its minimum below its maximum";
    }
    if (enumerated && (mapped || entry.labels == nullptr)) {
      return where + " is enumerated, which needs labels and no display mapping";
    }
    if (!enumerated && entry.labels != nullptr) {
      return where + " is stepped but declares labels";
    }
    const double scaled = (static_cast<double>(parameter.default_value) - parameter.minimum) /
                          (static_cast<double>(parameter.maximum) - parameter.minimum) *
                          (entry.steps - 1);
    if (std::abs(scaled - std::round(scaled)) > 1e-4) {
      return where + "'s default is not one of its " + std::to_string(entry.steps) + " steps";
    }
    if (enumerated) {
      for (uint32_t i = 0; i < entry.steps; ++i) {
        const std::optional<std::string> label =
            text(entry.labels[i], max_label_bytes, false, false);
        if (!label) {
          return where + " label " + std::to_string(i) +
                 " is not a non-empty UTF-8 string shorter than 256 bytes";
        }
        parameter.labels.push_back(*label);
      }
    }
    break;
  }
  default:
    return where + " declares unknown kind " + std::to_string(entry.kind);
  }
  if ((api.capabilities & VS_MODULE_BYPASS_PARAMETER) != 0 &&
      api.bypass_parameter == entry.parameter_id && entry.kind != VS_PARAMETER_TOGGLE) {
    return where + " is the bypass parameter but is not described as a toggle";
  }
  parameter.kind = static_cast<ParameterKind>(entry.kind);
  parameter.steps = entry.kind == VS_PARAMETER_CONTINUOUS ? 0 : entry.steps;
  parameter.described = true;
  if (!mapped) {
    return {};
  }
  const auto finite_shown = [](double value) {
    return std::isfinite(value) && std::abs(value) <= max_shown;
  };
  if (!finite_shown(entry.shown_minimum) || !finite_shown(entry.shown_maximum) ||
      entry.shown_minimum == entry.shown_maximum) {
    return where + " needs two different finite shown numbers within ±1e9";
  }
  if (!std::isfinite(entry.skew) || !(entry.skew > 0) || entry.skew > 1000) {
    return where + " needs a display skew above 0 and at most 1000";
  }
  if (entry.decimals > 6) {
    return where + " shows more than 6 decimals";
  }
  const std::optional<std::string> unit = text(entry.unit, max_unit_bytes, true, true);
  const std::optional<std::string> kilo = text(entry.kilo_unit, max_unit_bytes, true, true);
  if (!unit || !kilo) {
    return where + " has a display unit that is not a UTF-8 string of at most 64 bytes";
  }
  if (entry.off > VS_PARAMETER_OFF_MAXIMUM) {
    return where + " declares an unknown off end";
  }
  if (!std::isfinite(entry.off_tolerance) || entry.off_tolerance < 0 ||
      (entry.off_tolerance != 0 && entry.off == VS_PARAMETER_OFF_NONE)) {
    return where + " needs a finite, non-negative off tolerance, and an off end for a non-zero one";
  }
  DisplayMapping mapping;
  mapping.range = std::array<double, 2>{entry.shown_minimum, entry.shown_maximum};
  mapping.skew = entry.skew;
  mapping.decimals = static_cast<int>(entry.decimals);
  mapping.unit = *unit;
  if (!kilo->empty()) {
    mapping.kilo_unit = *kilo;
  }
  mapping.show_sign = (entry.flags & VS_PARAMETER_DISPLAY_SIGNED) != 0;
  mapping.off = static_cast<OffEnd>(entry.off);
  mapping.off_tolerance = entry.off_tolerance;
  parameter.mapping = std::move(mapping);
  return {};
}

}

// Describes every parameter a module declares, in descriptor order, from its metadata extension
// (null when the module exports none). Metadata is untrusted: when it is invalid, returns nothing
// and sets *error to the reason. The descriptors themselves must already have been validated.
inline std::optional<std::vector<ParameterDisplay>>
describe_parameters(const vs_module_api &api, const vs_module_metadata_api *metadata,
                    std::string *error) {
  const auto fail = [&](std::string reason) {
    if (error) {
      *error = "parameter metadata: " + std::move(reason);
    }
    return std::nullopt;
  };
  std::vector<ParameterDisplay> parameters;
  if (api.parameter_count > 0 && api.parameters == nullptr) {
    return fail("the module has a null parameter table");
  }
  parameters.reserve(api.parameter_count);
  for (uint32_t i = 0; i < api.parameter_count; ++i) {
    const vs_parameter_descriptor &d = api.parameters[i];
    ParameterDisplay parameter;
    parameter.id = d.id;
    parameter.minimum = d.minimum;
    parameter.maximum = d.maximum;
    parameter.default_value = d.default_value;
    parameter.unit = display_detail::text(d.unit, 256, true, true).value_or("");
    parameters.push_back(std::move(parameter));
  }
  if (metadata == nullptr) {
    return parameters;
  }
  if (metadata->struct_size < sizeof(vs_module_metadata_api)) {
    return fail("descriptor struct_size " + std::to_string(metadata->struct_size) +
                " is smaller than " + std::to_string(sizeof(vs_module_metadata_api)));
  }
  if (metadata->abi_version != VS_MODULE_METADATA_ABI_VERSION) {
    return fail("ABI version " + std::to_string(metadata->abi_version) +
                " does not match host metadata ABI version " +
                std::to_string(VS_MODULE_METADATA_ABI_VERSION));
  }
  if (metadata->parameter_count > api.parameter_count) {
    return fail("describes " + std::to_string(metadata->parameter_count) +
                " parameters, but the module declares " + std::to_string(api.parameter_count));
  }
  if (metadata->parameter_count > 0 && (metadata->parameters == nullptr ||
                                        metadata->parameter_size < sizeof(vs_parameter_metadata))) {
    return fail("the parameter table is null or its elements are smaller than " +
                std::to_string(sizeof(vs_parameter_metadata)) + " bytes");
  }
  const auto *bytes = reinterpret_cast<const unsigned char *>(metadata->parameters);
  for (uint32_t i = 0; i < metadata->parameter_count; ++i) {
    vs_parameter_metadata entry;
    std::memcpy(&entry, bytes + static_cast<size_t>(i) * metadata->parameter_size, sizeof entry);
    const auto parameter =
        std::find_if(parameters.begin(), parameters.end(),
                     [&](const ParameterDisplay &p) { return p.id == entry.parameter_id; });
    if (parameter == parameters.end()) {
      return fail(display_detail::describe(entry.parameter_id) + " is described but not declared");
    }
    if (parameter->described) {
      return fail(display_detail::describe(entry.parameter_id) + " is described twice");
    }
    const std::string reason = display_detail::check(entry, *parameter, api);
    if (!reason.empty()) {
      return fail(reason);
    }
  }
  return parameters;
}

}

#endif
