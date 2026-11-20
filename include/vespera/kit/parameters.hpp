#ifndef VESPERA_KIT_PARAMETERS_HPP
#define VESPERA_KIT_PARAMETERS_HPP

#include <vespera/module.h>

#include <array>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace vespera::kit {

enum class Role : uint8_t { control, bypass };

// Maps a normalized position in [0, 1] to a plain value and back. A skew below 1 spends more of
// the travel near the minimum, as a frequency sweep usually wants.
struct Scale {
  double minimum = 0.0;
  double maximum = 1.0;
  double skew = 1.0;

  double plain(double normalized) const noexcept {
    const double shaped = skew == 1.0 ? normalized : std::pow(normalized, 1.0 / skew);
    return minimum + (maximum - minimum) * shaped;
  }

  double normalized(double plain) const noexcept {
    const double position = (plain - minimum) / (maximum - minimum);
    return skew == 1.0 ? position : std::pow(position, skew);
  }
};

// How a value is shown to the user. Declaring any part of it makes the module describe a display
// mapping: the shown number runs over the scale (maps() and skew()) unless shows() sets other ends.
struct Display {
  // Where the shown numbers at the stored minimum and maximum come from.
  enum class Ends : uint8_t { scale, percent_of_scale, own };

  bool declared = false;
  Ends ends = Ends::scale;
  // The shown numbers with Ends::own.
  double low = 0.0;
  double high = 0.0;
  uint32_t decimals = 0;
  const char *unit = "";
  const char *kilo_unit = "";
  bool show_sign = false;
  uint32_t off = VS_PARAMETER_OFF_NONE;
  double off_tolerance = 0.0;
};

struct Parameter {
  vs_parameter_descriptor descriptor{0, "", "", 0.0f, 1.0f, 0.0f};
  // Defaults to the descriptor range. maps() changes it without changing the descriptor, for
  // parameters stored normalized whose DSP and display value is a plain quantity.
  Scale scale{};
  double smoothing_seconds = 0.0;
  Role role = Role::control;
  // VS_PARAMETER_* kind, value count and value names, as described to the host.
  uint32_t kind = VS_PARAMETER_CONTINUOUS;
  uint32_t step_count = 0;
  const char *const *labels = nullptr;
  Display display{};

  constexpr Parameter shown() const {
    Parameter result = *this;
    result.display.declared = true;
    return result;
  }

  constexpr Parameter unit(const char *text) const {
    Parameter result = *this;
    result.descriptor.unit = text;
    return result;
  }

  constexpr Parameter range(float minimum, float maximum) const {
    Parameter result = *this;
    result.descriptor.minimum = minimum;
    result.descriptor.maximum = maximum;
    result.scale.minimum = minimum;
    result.scale.maximum = maximum;
    return result;
  }

  constexpr Parameter initial(float value) const {
    Parameter result = *this;
    result.descriptor.default_value = value;
    return result;
  }

  constexpr Parameter maps(double minimum, double maximum) const {
    Parameter result = *this;
    result.scale.minimum = minimum;
    result.scale.maximum = maximum;
    return result;
  }

  constexpr Parameter skew(double value) const {
    Parameter result = *this;
    result.scale.skew = value;
    return result;
  }

  constexpr Parameter smoothed(double seconds) const {
    Parameter result = *this;
    result.smoothing_seconds = seconds;
    return result;
  }

  // Also makes the parameter a toggle, which a bypass parameter is.
  constexpr Parameter bypass() const {
    Parameter result = *this;
    result.role = Role::bypass;
    result.kind = VS_PARAMETER_TOGGLE;
    result.step_count = 2;
    return result;
  }

  // Takes count evenly spaced values from the minimum to the maximum, both included.
  constexpr Parameter steps(uint32_t count) const {
    Parameter result = *this;
    result.kind = VS_PARAMETER_STEPPED;
    result.step_count = count;
    result.labels = nullptr;
    return result;
  }

  // The numbers shown at the stored minimum and maximum, instead of the scale's.
  constexpr Parameter shows(double low, double high) const {
    Parameter result = shown();
    result.display.ends = Display::Ends::own;
    result.display.low = low;
    result.display.high = high;
    return result;
  }

  constexpr Parameter decimals(uint32_t count) const {
    Parameter result = shown();
    result.display.decimals = count;
    return result;
  }

  // Text after the shown number, including any space: " dB".
  constexpr Parameter display_unit(const char *text) const {
    Parameter result = shown();
    result.display.unit = text;
    return result;
  }

  // Shows magnitudes of 1000 and above in thousands with this unit: " kHz".
  constexpr Parameter kilo_unit(const char *text) const {
    Parameter result = shown();
    result.display.kilo_unit = text;
    return result;
  }

  // A plus sign before shown numbers above zero.
  constexpr Parameter show_sign() const {
    Parameter result = shown();
    result.display.show_sign = true;
    return result;
  }

  // Shows OFF at the minimum, and for shown numbers less than tolerance away from it.
  constexpr Parameter off_at_minimum(double tolerance = 0.0) const {
    Parameter result = shown();
    result.display.off = VS_PARAMETER_OFF_MINIMUM;
    result.display.off_tolerance = tolerance;
    return result;
  }

  constexpr Parameter off_at_maximum(double tolerance = 0.0) const {
    Parameter result = shown();
    result.display.off = VS_PARAMETER_OFF_MAXIMUM;
    result.display.off_tolerance = tolerance;
    return result;
  }

  // Signed decibels: "+1.5 dB".
  constexpr Parameter decibels(uint32_t count = 1) const {
    return decimals(count).display_unit(" dB").show_sign();
  }

  // Whole hertz, switching to kilohertz from 1000: "800 Hz", "1.20 kHz".
  constexpr Parameter hertz() const { return decimals(0).display_unit(" Hz").kilo_unit(" kHz"); }

  // Whole percent of the scale: a 0–1 scale shows 0% to 100%.
  constexpr Parameter percent() const {
    Parameter result = decimals(0).display_unit("%");
    result.display.ends = Display::Ends::percent_of_scale;
    return result;
  }

  constexpr uint32_t id() const noexcept { return descriptor.id; }

  // The metadata the module exports for this parameter.
  constexpr vs_parameter_metadata metadata() const noexcept {
    vs_parameter_metadata result{
        descriptor.id,         kind, step_count, 0, labels, 0.0, 0.0, 0.0, nullptr, nullptr, 0,
        VS_PARAMETER_OFF_NONE, 0.0};
    if (!display.declared) {
      return result;
    }
    const bool own = display.ends == Display::Ends::own;
    const double factor = display.ends == Display::Ends::percent_of_scale ? 100.0 : 1.0;
    result.flags =
        VS_PARAMETER_DISPLAY_MAPPED | (display.show_sign ? VS_PARAMETER_DISPLAY_SIGNED : 0u);
    result.shown_minimum = own ? display.low : scale.minimum * factor;
    result.shown_maximum = own ? display.high : scale.maximum * factor;
    result.skew = scale.skew;
    result.unit = display.unit;
    result.kilo_unit = display.kilo_unit;
    result.decimals = display.decimals;
    result.off = display.off;
    result.off_tolerance = display.off_tolerance;
    return result;
  }
  double plain(double normalized) const noexcept { return scale.plain(normalized); }
  double normalized(double plain) const noexcept { return scale.normalized(plain); }
};

constexpr Parameter knob(uint32_t id, const char *name, float initial = 0.5f) {
  return Parameter{{id, name, "", 0.0f, 1.0f, initial}};
}

constexpr Parameter toggle(uint32_t id, const char *name, bool on = false) {
  Parameter result{{id, name, "bool", 0.0f, 1.0f, on ? 1.0f : 0.0f}};
  result.kind = VS_PARAMETER_TOGGLE;
  result.step_count = 2;
  return result;
}

// An integer-valued selector over options values starting at 0.
constexpr Parameter choice(uint32_t id, const char *name, uint32_t options, uint32_t initial = 0) {
  return Parameter{{id, name, "", 0.0f, static_cast<float>(options > 0 ? options - 1 : 0),
                    static_cast<float>(initial)}}
      .steps(options);
}

// An integer-valued selector whose values 0, 1, ... are named by labels, which must have static
// storage duration: `static constexpr const char *modes[] = {"Clean", "Lead"};`.
template <size_t N>
constexpr Parameter choice(uint32_t id, const char *name, const char *const (&labels)[N],
                           uint32_t initial = 0) {
  Parameter result = choice(id, name, static_cast<uint32_t>(N), initial);
  result.kind = VS_PARAMETER_ENUMERATED;
  result.labels = labels;
  return result;
}

template <size_t N> struct Table {
  std::array<Parameter, N> entries{};
  std::array<vs_parameter_descriptor, N> descriptors{};
  std::array<vs_parameter_metadata, N> metadata{};

  static constexpr size_t size() noexcept { return N; }

  constexpr const Parameter &operator[](size_t index) const noexcept { return entries[index]; }

  // N when id is not declared.
  constexpr size_t index(uint32_t id) const noexcept {
    for (size_t i = 0; i < N; ++i) {
      if (entries[i].descriptor.id == id) {
        return i;
      }
    }
    return N;
  }

  // N when the table declares no bypass parameter.
  constexpr size_t bypass_index() const noexcept {
    for (size_t i = 0; i < N; ++i) {
      if (entries[i].role == Role::bypass) {
        return i;
      }
    }
    return N;
  }

  constexpr bool has_bypass() const noexcept { return bypass_index() < N; }

  constexpr uint32_t bypass_parameter() const noexcept {
    return has_bypass() ? entries[bypass_index()].descriptor.id : 0;
  }

  constexpr uint32_t capabilities() const noexcept {
    return has_bypass() ? uint32_t{VS_MODULE_BYPASS_PARAMETER} : 0u;
  }

  bool accepts(size_t index, float value) const noexcept {
    return index < N && std::isfinite(value) && value >= entries[index].descriptor.minimum &&
           value <= entries[index].descriptor.maximum;
  }

  constexpr std::array<float, N> defaults() const noexcept {
    std::array<float, N> values{};
    for (size_t i = 0; i < N; ++i) {
      values[i] = entries[i].descriptor.default_value;
    }
    return values;
  }
};

namespace detail {

constexpr bool finite(double value) noexcept {
  return value == value && value <= std::numeric_limits<double>::max() &&
         value >= -std::numeric_limits<double>::max();
}

constexpr bool short_string(const char *text, size_t limit = 256) noexcept {
  if (text == nullptr) {
    return false;
  }
  for (size_t length = 0; length < limit; ++length) {
    if (text[length] == '\0') {
      return true;
    }
  }
  return false;
}

}

namespace detail {

// The rules hosts apply to parameter metadata, checked while compiling.
consteval void check_metadata(const Parameter &p) {
  const vs_parameter_descriptor &d = p.descriptor;
  if (p.kind == VS_PARAMETER_STEPPED || p.kind == VS_PARAMETER_ENUMERATED) {
    if (p.step_count < 2 || p.step_count > (p.kind == VS_PARAMETER_ENUMERATED ? 256u : 65536u)) {
      throw "steps need 2 to 65536 values, and choices with labels 2 to 256";
    }
    const double scaled = (static_cast<double>(d.default_value) - d.minimum) /
                          (static_cast<double>(d.maximum) - d.minimum) * (p.step_count - 1);
    const double nearest = static_cast<double>(static_cast<long long>(scaled + 0.5));
    if (scaled - nearest > 1e-4 || nearest - scaled > 1e-4) {
      throw "a stepped parameter's default must be one of its steps";
    }
    for (uint32_t i = 0; p.kind == VS_PARAMETER_ENUMERATED && i < p.step_count; ++i) {
      if (!short_string(p.labels[i]) || p.labels[i][0] == '\0') {
        throw "choice labels must be non-empty and shorter than 256 bytes";
      }
    }
  }
  const Display &shown = p.display;
  if (!shown.declared) {
    return;
  }
  if (p.kind == VS_PARAMETER_TOGGLE || p.kind == VS_PARAMETER_ENUMERATED) {
    throw "toggles and labelled choices are shown by name and take no display mapping";
  }
  const vs_parameter_metadata m = p.metadata();
  if (!finite(m.shown_minimum) || !finite(m.shown_maximum) || m.shown_minimum == m.shown_maximum ||
      m.shown_minimum > 1e9 || m.shown_minimum < -1e9 || m.shown_maximum > 1e9 ||
      m.shown_maximum < -1e9) {
    throw "shown numbers must be different and at most 1e9 in magnitude";
  }
  if (m.skew > 1000.0) {
    throw "a displayed parameter's skew must be at most 1000";
  }
  if (m.decimals > 6) {
    throw "display decimals must be at most 6";
  }
  if (!short_string(m.unit, 65) || !short_string(m.kilo_unit, 65)) {
    throw "display units must be at most 64 bytes";
  }
  if (!finite(m.off_tolerance) || m.off_tolerance < 0.0) {
    throw "an off tolerance must be finite and not negative";
  }
}

}

// Builds a parameter table. Every rule is checked during compilation; a violation fails with the
// message of the throw expression that rejected it.
template <class... Parameters>
  requires(std::same_as<Parameters, Parameter> && ...)
consteval Table<sizeof...(Parameters)> parameters(const Parameters &...entries) {
  Table<sizeof...(Parameters)> table{{entries...}, {entries.descriptor...}};
  size_t bypasses = 0;
  for (size_t i = 0; i < table.size(); ++i) {
    const Parameter &p = table.entries[i];
    const vs_parameter_descriptor &d = p.descriptor;
    if (d.id == 0) {
      throw "parameter ids must be non-zero";
    }
    for (size_t j = 0; j < i; ++j) {
      if (table.entries[j].descriptor.id == d.id) {
        throw "parameter ids must be unique";
      }
    }
    if (!detail::short_string(d.name) || d.name[0] == '\0' || !detail::short_string(d.unit)) {
      throw "parameter names must be non-empty and names and units shorter than 256 bytes";
    }
    if (!detail::finite(d.minimum) || !detail::finite(d.maximum) || !(d.minimum < d.maximum)) {
      throw "parameter ranges must be finite with minimum below maximum";
    }
    if (!(d.default_value >= d.minimum && d.default_value <= d.maximum)) {
      throw "parameter defaults must lie within their range";
    }
    if (!detail::finite(p.scale.minimum) || !detail::finite(p.scale.maximum) ||
        p.scale.minimum == p.scale.maximum || !detail::finite(p.scale.skew) ||
        !(p.scale.skew > 0.0)) {
      throw "parameter scales must be finite and distinct with a positive skew";
    }
    if (!detail::finite(p.smoothing_seconds) || p.smoothing_seconds < 0.0 ||
        p.smoothing_seconds > 60.0) {
      throw "parameter smoothing must be within [0, 60] seconds";
    }
    if (p.role == Role::bypass) {
      if (++bypasses > 1) {
        throw "a table declares at most one bypass parameter";
      }
      if (d.minimum != 0.0f || d.maximum != 1.0f) {
        throw "the bypass parameter must range from 0 to 1";
      }
      if (p.kind != VS_PARAMETER_TOGGLE) {
        throw "the bypass parameter must be a toggle";
      }
    }
    detail::check_metadata(p);
    table.metadata[i] = p.metadata();
  }
  return table;
}

}

#endif
