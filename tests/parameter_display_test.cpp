#include "fixtures/metadata_control.h"

#include <vespera/parameter_display.hpp>
#include <vespera/runtime.hpp>

#include <dlfcn.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using vespera::DisplayMapping;
using vespera::OffEnd;
using vespera::ParameterDisplay;
using vespera::ParameterKind;

int failures = 0;
const char *current_test = "";

void check(bool condition, const std::string &message) {
  if (!condition) {
    std::fprintf(stderr, "[%s] %s\n", current_test, message.c_str());
    ++failures;
  }
}

void expect_text(const DisplayMapping &mapping, float minimum, float maximum, double value,
                 const std::string &expected) {
  const std::string text = vespera::format_number(mapping, minimum, maximum, value);
  check(text == expected, "value " + std::to_string(value) + " shows \"" + text +
                              "\", expected \"" + expected + "\"");
}

DisplayMapping decibels() {
  DisplayMapping mapping;
  mapping.decimals = 1;
  mapping.unit = " dB";
  mapping.show_sign = true;
  return mapping;
}

DisplayMapping hertz(double low, double high, double skew, OffEnd off = OffEnd::none) {
  DisplayMapping mapping;
  mapping.range = std::array<double, 2>{low, high};
  mapping.skew = skew;
  mapping.unit = " Hz";
  mapping.kilo_unit = " kHz";
  mapping.off = off;
  return mapping;
}

void test_signs() {
  current_test = "signs";
  const DisplayMapping db = decibels();
  expect_text(db, -12, 12, 0, "0.0 dB");
  expect_text(db, -12, 12, -0.0, "0.0 dB");
  expect_text(db, -12, 12, -0.04, "0.0 dB");
  expect_text(db, -12, 12, 0.04, "0.0 dB");
  expect_text(db, -12, 12, 0.06, "+0.1 dB");
  expect_text(db, -12, 12, 1.5, "+1.5 dB");
  expect_text(db, -12, 12, -1.5, "-1.5 dB");
  expect_text(db, -12, 12, 30, "+12.0 dB");
  DisplayMapping whole = db;
  whole.decimals = 0;
  expect_text(whole, -12, 12, -0.4, "0 dB");
  expect_text(whole, -12, 12, 0.6, "+1 dB");
  expect_text(whole, -12, 12, -0.6, "-1 dB");
  DisplayMapping unsigned_mapping = db;
  unsigned_mapping.show_sign = false;
  expect_text(unsigned_mapping, -12, 12, 3, "3.0 dB");
}

void test_kilo_thresholds() {
  current_test = "kilo";
  const DisplayMapping linear = hertz(0, 100000, 1);
  expect_text(linear, 0, 100000, 999.4, "999 Hz");
  expect_text(linear, 0, 100000, 999.5, "1000 Hz");
  expect_text(linear, 0, 100000, 1000, "1.00 kHz");
  expect_text(linear, 0, 100000, 9994, "9.99 kHz");
  expect_text(linear, 0, 100000, 9995, "10.0 kHz");
  expect_text(linear, 0, 100000, 99949, "99.9 kHz");
  expect_text(linear, 0, 100000, 99950, "100 kHz");
  DisplayMapping negative = linear;
  negative.range = std::array<double, 2>{-5000, 5000};
  expect_text(negative, 0, 1, 0, "-5.00 kHz");
  DisplayMapping no_kilo = linear;
  no_kilo.kilo_unit.reset();
  expect_text(no_kilo, 0, 100000, 20000, "20000 Hz");
  DisplayMapping empty_kilo = linear;
  empty_kilo.kilo_unit = "";
  expect_text(empty_kilo, 0, 100000, 20000, "20.0");
}

void test_skew_extremes() {
  current_test = "skew";
  for (double skew : {0.001, 0.6, 1.0, 2.0, 1000.0}) {
    DisplayMapping mapping;
    mapping.range = std::array<double, 2>{20, 500};
    mapping.skew = skew;
    const auto low = vespera::shown_number(mapping, 0, 1, 0);
    const auto high = vespera::shown_number(mapping, 0, 1, 1);
    check(low.shown == 20 && high.shown == 500,
          "skew " + std::to_string(skew) + " must keep the ends exact");
    double previous = 20;
    for (int i = 1; i <= 100; ++i) {
      const double shown = vespera::shown_number(mapping, 0, 1, i / 100.0).shown;
      check(shown >= previous, "skew " + std::to_string(skew) + " must be monotonic");
      previous = shown;
    }
  }
  DisplayMapping mapping;
  mapping.range = std::array<double, 2>{20, 500};
  mapping.skew = 0.6;
  const double half = vespera::shown_number(mapping, 0, 1, 0.5).shown;
  check(half == 20 + std::pow(0.5, 1 / 0.6) * 480, "skew follows the layout formula");
  check(vespera::shown_number(mapping, 0, 1, -3).position == 0 &&
            vespera::shown_number(mapping, 0, 1, 7).position == 1,
        "positions clamp to the range");
  check(vespera::shown_number(mapping, 1, 1, 1).shown == 20, "an empty range shows the low end");
}

void test_off_ends() {
  current_test = "off";
  DisplayMapping high_pass = hertz(20, 500, 0.6, OffEnd::minimum);
  expect_text(high_pass, 0, 1, 0, "OFF");
  expect_text(high_pass, 0, 1, -1, "OFF");
  expect_text(high_pass, 0, 1, 1e-6, "20 Hz");
  high_pass.off_tolerance = 0.5;
  expect_text(high_pass, 0, 1, 1e-6, "OFF");
  DisplayMapping low_pass = hertz(1000, 20000, 0.6, OffEnd::maximum);
  expect_text(low_pass, 0, 1, 1, "OFF");
  expect_text(low_pass, 0, 1, 0.999, "20.0 kHz");
  expect_text(low_pass, 0, 1, 0, "1.00 kHz");

  float value = 0;
  auto parsed = vespera::parse_number(high_pass, 0, 1, "OFF");
  check(parsed && *parsed == 0, "typing off selects the off end");
  parsed = vespera::parse_number(high_pass, 0, 1, "20");
  check(parsed && *parsed == std::nextafter(0.0f, 1.0f),
        "a typed number stays one float step inside the off end");
  parsed = vespera::parse_number(low_pass, 0, 1, "25 kHz");
  check(parsed && *parsed == std::nextafter(1.0f, 0.0f), "clamped typing never selects off");
  parsed = vespera::parse_number(low_pass, 0, 1, "5");
  value = parsed.value_or(-1);
  check(std::abs(vespera::shown_number(low_pass, 0, 1, value).shown - 5000) < 0.01,
        "a bare number reads kilohertz when the whole range is at least 1000");
  parsed = vespera::parse_number(high_pass, 0, 1, "0,1k");
  value = parsed.value_or(-1);
  check(std::abs(vespera::shown_number(high_pass, 0, 1, value).shown - 100) < 1e-3,
        "a comma decimal with a k suffix reads thousands");
}

void test_round_trips() {
  current_test = "round trips";
  std::vector<std::pair<DisplayMapping, std::array<float, 2>>> mappings = {
      {decibels(), {-24, 12}},
      {hertz(20, 500, 0.6, OffEnd::minimum), {0, 1}},
      {hertz(1000, 20000, 0.6, OffEnd::maximum), {0, 1}},
      {hertz(800, 12000, 0.5), {0, 1}},
  };
  DisplayMapping inverted;
  inverted.range = std::array<double, 2>{100, -100};
  inverted.decimals = 2;
  inverted.unit = "%";
  mappings.push_back({inverted, {0, 1}});
  DisplayMapping skewed;
  skewed.range = std::array<double, 2>{1, 20};
  skewed.skew = 0.25;
  skewed.decimals = 2;
  mappings.push_back({skewed, {0, 1}});
  for (const auto &[mapping, range] : mappings) {
    for (int i = 0; i <= 1000; ++i) {
      const double value = range[0] + static_cast<double>(range[1] - range[0]) * i / 1000.0;
      const std::string text = vespera::format_number(mapping, range[0], range[1], value);
      const auto parsed = vespera::parse_number(mapping, range[0], range[1], text);
      check(parsed.has_value(), "\"" + text + "\" must parse");
      if (parsed) {
        check(*parsed >= range[0] && *parsed <= range[1], "\"" + text + "\" parses within range");
        const std::string again = vespera::format_number(mapping, range[0], range[1], *parsed);
        // Shown numbers from 999.5 up to 1000 round to "1000 Hz" below the kilo switch, so the
        // value typed as 1.00 kHz may land just below 1000.
        const bool kilo_edge = text == "1.00 kHz" && again == "1000 Hz";
        check(again == text || kilo_edge, "\"" + text + "\" reads back as \"" + again + "\"");
      }
    }
  }
}

void test_scanning() {
  current_test = "scanning";
  const auto scanned = [](const char *text) { return vespera::scan_number(text); };
  check(scanned("5").value_or(0) == 5 && scanned(" \t5").value_or(0) == 5, "plain numbers");
  check(scanned(".5").value_or(0) == 0.5 && scanned("5.").value_or(0) == 5 &&
            scanned("-.5").value_or(0) == -0.5 && scanned("+5").value_or(0) == 5,
        "points and signs");
  check(scanned("1e3").value_or(0) == 1000 && scanned("1e").value_or(0) == 1 &&
            scanned("1e+").value_or(0) == 1 && scanned("1E-2x").value_or(0) == 0.01,
        "exponents need digits");
  check(scanned("0x10").value_or(1) == 0 && scanned("5khz").value_or(0) == 5 &&
            scanned("1.2.3").value_or(0) == 1.2,
        "trailing text ends the number");
  for (const char *text : {"", "abc", "inf", "nan", "infinity", "- 5", "--5", ".", "-", "+.e1"}) {
    check(!scanned(text), std::string("\"") + text + "\" is not a number");
  }
  check(std::isinf(scanned("1e400").value_or(0)), "overflow reads as infinite");
  for (const char *text : {"1e400", "nan", "inf", ""}) {
    check(!vespera::parse_number(decibels(), -12, 12, text), "non-finite typing is rejected");
  }
}

ParameterDisplay parameter(ParameterKind kind, float minimum, float maximum, uint32_t steps = 0) {
  ParameterDisplay result;
  result.id = 7;
  result.kind = kind;
  result.minimum = minimum;
  result.maximum = maximum;
  result.steps = steps;
  return result;
}

void test_parameter_text() {
  current_test = "parameter text";
  ParameterDisplay toggle = parameter(ParameterKind::toggle, 0, 1, 2);
  check(toggle.text(0) == "Off" && toggle.text(0.49) == "Off" && toggle.text(0.5) == "On" &&
            toggle.text(1) == "On",
        "toggles show Off and On by the upper half of the range");
  check(toggle.parse("on") == 1.0f && toggle.parse(" OFF ") == 0.0f && !toggle.parse("maybe"),
        "toggles read On and Off");
  check(toggle.nearest(0.7f) == 1 && toggle.nearest(0.2f) == 0, "toggles snap to their ends");
  check(!toggle.shows_values(), "toggles leave layout text alone");

  ParameterDisplay modes = parameter(ParameterKind::enumerated, 0, 2, 3);
  modes.labels = {"Clean", "Crunch", "Lead"};
  check(modes.text(0) == "Clean" && modes.text(1.4) == "Crunch" && modes.text(1.6) == "Lead" &&
            modes.text(9) == "Lead",
        "enumerations name the nearest value");
  check(modes.parse("crunch") == 1.0f && modes.parse("LEAD") == 2.0f && !modes.parse("Fuzz"),
        "enumerations read their labels");
  check(modes.shows_values(), "labels supply layout text");
  ParameterDisplay offset = parameter(ParameterKind::enumerated, 10, 30, 3);
  offset.labels = {"A", "B", "C"};
  check(offset.text(20) == "B" && offset.parse("c") == 30.0f && offset.nearest(24) == 20,
        "enumerations index by position, not by value");

  ParameterDisplay voices = parameter(ParameterKind::stepped, 1, 8, 8);
  check(voices.text(3.4) == "3" && voices.nearest(3.6f) == 4 && voices.nearest(-2) == 1 &&
            voices.nearest(20) == 8,
        "stepped parameters snap to whole steps");
  check(voices.parse("5.2") == 5.0f, "typed stepped values snap");
  ParameterDisplay quarters = parameter(ParameterKind::stepped, 0, 1, 5);
  check(quarters.nearest(0.3f) == 0.25f && quarters.text(0.3) == "0.25",
        "fractional steps keep two decimals");

  ParameterDisplay knob = parameter(ParameterKind::continuous, 0, 1);
  check(knob.text(0.5) == "50%" && knob.parse("25").value_or(-1) == 0.25f,
        "continuous parameters without a unit show whole percent");
  ParameterDisplay gain = parameter(ParameterKind::continuous, 0, 4);
  gain.unit = "linear";
  check(gain.text(1) == "1.00 linear", "a described unit follows the value");
  check(!knob.shows_values(), "an undescribed display leaves layout text alone");

  ParameterDisplay level = parameter(ParameterKind::continuous, -24, 12);
  level.mapping = decibels();
  check(level.text(-0.01) == "0.0 dB" && level.text(6) == "+6.0 dB", "mappings format values");
  check(level.parse("+3 dB") == 3.0f && level.parse("-100") == -24.0f, "mappings parse values");

  for (const ParameterDisplay &any : {toggle, modes, voices, knob, level}) {
    check(any.text(std::numeric_limits<double>::quiet_NaN()).empty() &&
              any.text(std::numeric_limits<double>::infinity()).empty(),
          "non-finite values have no text");
    check(!any.parse("nan") && !any.parse(""), "non-numbers do not parse");
  }
}

void test_finite_range_boundaries() {
  current_test = "finite range boundaries";
  const float extreme = std::numeric_limits<float>::max();
  for (auto kind : {ParameterKind::continuous, ParameterKind::stepped, ParameterKind::enumerated}) {
    auto display = parameter(kind, -extreme, extreme, kind == ParameterKind::continuous ? 0 : 3);
    display.labels = {"Low", "Middle", "High"};
    for (float value : {-extreme, 0.0f, extreme}) {
      check(display.nearest(value) == value, "snapping preserves finite endpoints and midpoint");
      check(std::isfinite(display.position(value)), "position stays finite");
    }
    check(display.position(extreme) == 1 && display.position(0) == 0.5, "extreme span positions");
    if (kind == ParameterKind::enumerated) {
      check(display.parse("Low") == -extreme && display.parse("Middle") == 0 &&
                display.parse("High") == extreme && display.text(extreme) == "High",
            "extreme enumeration endpoints and midpoint");
    } else {
      DisplayMapping mapping;
      mapping.range = std::array<double, 2>{0, 100};
      display.mapping = mapping;
      check(display.text(extreme) == "100" && display.parse("0") == -extreme &&
                display.parse("50") == 0 && display.parse("100") == extreme,
            "extreme mapped endpoints and midpoint");
      display.mapping.reset();
      display.unit = "Hz";
      for (const char *text : {"-3.4028234663852886e38 Hz", "0 Hz", "3.4028234663852886e38 Hz"}) {
        const auto result = display.parse(text);
        check(result && std::isfinite(*result), "unit parsing returns finite values");
      }
      for (float value : {-extreme, 0.0f, extreme}) {
        check(display.parse(display.text(value)) == value, "extreme unit values round trip");
      }
    }
  }
  for (bool unit : {false, true}) {
    for (bool mapped : {false, true}) {
      auto display = parameter(ParameterKind::continuous, 440, 440);
      if (unit) {
        display.unit = "Hz";
      }
      if (mapped) {
        DisplayMapping mapping;
        mapping.range = std::array<double, 2>{0, 100};
        display.mapping = mapping;
      }
      for (const char *text : {"440 Hz", "0", "100", "-100"}) {
        check(display.parse(text) == 440, "constant ranges parse to their sole value");
      }
      check(!display.parse("nan") && !display.parse("1e400"),
            "constant ranges still reject non-finite input");
    }
  }
  DisplayMapping degenerate;
  degenerate.range = std::array<double, 2>{50, 50};
  check(!vespera::parse_number(degenerate, 0, 1, "50"), "degenerate mappings do not return NaN");
  DisplayMapping invalid;
  invalid.skew = std::numeric_limits<double>::quiet_NaN();
  check(!vespera::parse_number(invalid, 0, 1, "0.5"), "non-finite results are rejected");
}

struct Module {
  std::vector<vs_parameter_descriptor> descriptors = {
      {1, "Enabled", "", 0, 1, 1},
      {2, "Level", "dB", -24, 12, 0},
      {3, "Mode", "", 0, 2, 1},
      {4, "Cutoff", "", 0, 1, 0.5f},
  };
  vs_module_api api{};
  std::vector<vs_parameter_metadata> entries;
  vs_module_metadata_api metadata{};

  Module() {
    api.struct_size = sizeof api;
    api.abi_version = VS_MODULE_ABI_VERSION;
    api.parameters = descriptors.data();
    api.parameter_count = static_cast<uint32_t>(descriptors.size());
    api.capabilities = VS_MODULE_BYPASS_PARAMETER;
    api.bypass_parameter = 1;
    static const char *const labels[] = {"Clean", "Crunch", "Lead"};
    entries = {
        {1, VS_PARAMETER_TOGGLE, 2, 0, nullptr, 0, 0, 0, nullptr, nullptr, 0, 0, 0},
        {2, VS_PARAMETER_CONTINUOUS, 0, VS_PARAMETER_DISPLAY_MAPPED | VS_PARAMETER_DISPLAY_SIGNED,
         nullptr, -24, 12, 1, " dB", nullptr, 1, 0, 0},
        {3, VS_PARAMETER_ENUMERATED, 3, 0, labels, 0, 0, 0, nullptr, nullptr, 0, 0, 0},
        {4, VS_PARAMETER_CONTINUOUS, 0, VS_PARAMETER_DISPLAY_MAPPED, nullptr, 20, 20000, 0.5, " Hz",
         " kHz", 0, VS_PARAMETER_OFF_MINIMUM, 0},
    };
  }

  std::optional<std::vector<ParameterDisplay>> describe(std::string *error) {
    metadata = {sizeof metadata, VS_MODULE_METADATA_ABI_VERSION, entries.data(),
                static_cast<uint32_t>(entries.size()), sizeof(vs_parameter_metadata)};
    return vespera::describe_parameters(api, &metadata, error);
  }
};

void expect_rejected(const std::string &name, const std::function<void(Module &)> &change,
                     const std::string &reason) {
  Module module;
  change(module);
  std::string error;
  const auto described = module.describe(&error);
  check(!described, name + ": must be rejected");
  check(error.find(reason) != std::string::npos,
        name + ": error \"" + error + "\" should mention \"" + reason + "\"");
}

void test_validation() {
  current_test = "validation";
  Module valid;
  std::string error;
  const auto described = valid.describe(&error);
  check(described.has_value(), "a valid table is accepted: " + error);
  if (described) {
    check(described->size() == 4, "every declared parameter is described");
    check((*described)[0].kind == ParameterKind::toggle && (*described)[0].steps == 2,
          "toggle kind");
    check((*described)[1].text(1.5) == "+1.5 dB", "described mapping");
    check((*described)[2].text(2) == "Lead", "described labels");
    check((*described)[3].text(0) == "OFF" && (*described)[3].text(1) == "20.0 kHz",
          "described off end and kilo unit");
  }

  Module absent;
  const auto plain = vespera::describe_parameters(absent.api, nullptr, &error);
  check(plain && plain->size() == 4 && !(*plain)[0].described &&
            (*plain)[0].kind == ParameterKind::continuous && !(*plain)[1].mapping,
        "without metadata every parameter is continuous and undescribed");

  Module partial;
  partial.entries.resize(1);
  const auto some = partial.describe(&error);
  check(some && (*some)[0].described && !(*some)[1].described,
        "a module may describe some parameters");

  Module wide;
  struct Appended {
    vs_parameter_metadata entry;
    double future;
  };
  std::vector<Appended> appended;
  for (const auto &entry : wide.entries) {
    appended.push_back({entry, 42});
  }
  vs_module_metadata_api larger{sizeof(vs_module_metadata_api) + 8, VS_MODULE_METADATA_ABI_VERSION,
                                &appended[0].entry, 4, sizeof(Appended)};
  const auto grown = vespera::describe_parameters(wide.api, &larger, &error);
  check(grown && (*grown)[2].text(0) == "Clean",
        "larger descriptors and elements with appended fields are read: " + error);

  using M = Module;
  {
    Module m;
    m.metadata = {8, VS_MODULE_METADATA_ABI_VERSION, m.entries.data(), 4,
                  sizeof(vs_parameter_metadata)};
    check(!vespera::describe_parameters(m.api, &m.metadata, &error) &&
              error.find("struct_size") != std::string::npos,
          "short descriptor struct_size is rejected");
    m.metadata = {sizeof m.metadata, 2, m.entries.data(), 4, sizeof(vs_parameter_metadata)};
    check(!vespera::describe_parameters(m.api, &m.metadata, &error) &&
              error.find("ABI version") != std::string::npos,
          "another metadata ABI version is rejected");
    m.metadata = {sizeof m.metadata, VS_MODULE_METADATA_ABI_VERSION, nullptr, 4,
                  sizeof(vs_parameter_metadata)};
    check(!vespera::describe_parameters(m.api, &m.metadata, &error) &&
              error.find("null") != std::string::npos,
          "a null table is rejected");
    m.metadata = {sizeof m.metadata, VS_MODULE_METADATA_ABI_VERSION, m.entries.data(), 4,
                  sizeof(vs_parameter_metadata) - 8};
    check(!vespera::describe_parameters(m.api, &m.metadata, &error) &&
              error.find("smaller than") != std::string::npos,
          "a short element size is rejected");
    m.metadata = {sizeof m.metadata, VS_MODULE_METADATA_ABI_VERSION, m.entries.data(), 5,
                  sizeof(vs_parameter_metadata)};
    check(!vespera::describe_parameters(m.api, &m.metadata, &error) &&
              error.find("describes 5") != std::string::npos,
          "more entries than parameters are rejected");
  }
  expect_rejected(
      "undeclared", [](M &m) { m.entries[3].parameter_id = 9; }, "described but not declared");
  expect_rejected("twice", [](M &m) { m.entries[3].parameter_id = 2; }, "described twice");
  expect_rejected("unknown kind", [](M &m) { m.entries[3].kind = 4; }, "unknown kind");
  expect_rejected("continuous steps", [](M &m) { m.entries[3].steps = 3; }, "continuous");
  expect_rejected("toggle steps", [](M &m) { m.entries[0].steps = 3; }, "toggle");
  expect_rejected(
      "toggle mapping", [](M &m) { m.entries[0].flags = VS_PARAMETER_DISPLAY_MAPPED; }, "toggle");
  expect_rejected(
      "toggle labels", [](M &m) { m.entries[0].labels = m.entries[2].labels; }, "toggle");
  expect_rejected(
      "bypass not a toggle",
      [](M &m) {
        m.entries[0].kind = VS_PARAMETER_STEPPED;
        m.entries[0].steps = 2;
      },
      "bypass parameter");
  expect_rejected(
      "toggle without range",
      [](M &m) {
        m.descriptors[3] = {4, "Cutoff", "", 1, 1, 1};
        m.entries[3] = m.entries[0];
        m.entries[3].parameter_id = 4;
      },
      "toggle");
  expect_rejected(
      "one step",
      [](M &m) {
        m.entries[2].kind = VS_PARAMETER_STEPPED;
        m.entries[2].labels = nullptr;
        m.entries[2].steps = 1;
      },
      "steps");
  expect_rejected("too many labels", [](M &m) { m.entries[2].steps = 257; }, "steps");
  expect_rejected(
      "steps disagreeing with the default", [](M &m) { m.entries[2].steps = 2; },
      "default is not one of its");
  expect_rejected("null labels", [](M &m) { m.entries[2].labels = nullptr; }, "labels");
  static const char *const empty_label[] = {"Clean", "", "Lead"};
  expect_rejected("empty label", [](M &m) { m.entries[2].labels = empty_label; }, "label 1");
  static const char *const bad_label[] = {"Clean", "\xc0\xaf", "Lead"};
  expect_rejected("invalid label", [](M &m) { m.entries[2].labels = bad_label; }, "label 1");
  expect_rejected(
      "labels on stepped", [](M &m) { m.entries[2].kind = VS_PARAMETER_STEPPED; },
      "stepped but declares labels");
  expect_rejected(
      "mapping on enumeration", [](M &m) { m.entries[2].flags = VS_PARAMETER_DISPLAY_MAPPED; },
      "enumerated");
  expect_rejected(
      "default off step", [](M &m) { m.descriptors[2].default_value = 0.5f; },
      "default is not one of");
  expect_rejected("unknown flag", [](M &m) { m.entries[1].flags |= 4; }, "unknown display flags");
  expect_rejected(
      "signed without mapping", [](M &m) { m.entries[1].flags = VS_PARAMETER_DISPLAY_SIGNED; },
      "signed");
  expect_rejected(
      "equal shown ends", [](M &m) { m.entries[1].shown_maximum = -24; }, "shown numbers");
  expect_rejected(
      "non-finite shown end",
      [](M &m) { m.entries[1].shown_minimum = std::numeric_limits<double>::infinity(); },
      "shown numbers");
  expect_rejected(
      "huge shown end", [](M &m) { m.entries[1].shown_maximum = 1e10; }, "shown numbers");
  for (double skew : {0.0, -1.0, 1000.5, std::numeric_limits<double>::quiet_NaN()}) {
    expect_rejected(
        "skew " + std::to_string(skew), [skew](M &m) { m.entries[3].skew = skew; }, "skew");
  }
  expect_rejected("decimals", [](M &m) { m.entries[1].decimals = 7; }, "decimals");
  static const std::string long_unit(65, 'u');
  expect_rejected("long unit", [](M &m) { m.entries[1].unit = long_unit.c_str(); }, "display unit");
  expect_rejected("invalid unit", [](M &m) { m.entries[3].kilo_unit = "\xff"; }, "display unit");
  expect_rejected("unknown off end", [](M &m) { m.entries[3].off = 3; }, "off end");
  expect_rejected(
      "negative tolerance", [](M &m) { m.entries[3].off_tolerance = -1; }, "off tolerance");
  expect_rejected(
      "tolerance without off", [](M &m) { m.entries[1].off_tolerance = 0.5; }, "off tolerance");
  Module unmapped;
  unmapped.entries[1].flags = 0;
  unmapped.entries[1].skew = -5;
  unmapped.entries[1].unit = "\xff";
  check(unmapped.describe(&error).has_value(),
        "display fields without a mapping are ignored: " + error);
}

template <class Function> void expect_throws(Function &&function, const std::string &part) {
  try {
    function();
    check(false, "expected a failure mentioning \"" + part + "\"");
  } catch (const std::exception &error) {
    check(std::string(error.what()).find(part) != std::string::npos,
          std::string("unexpected failure: ") + error.what());
  }
}

const vespera::ModuleMetadata &only_module(const vespera::Graph &graph,
                                           std::vector<vespera::NodeMetadata> &storage) {
  storage = graph.nodes();
  if (storage.size() != 1) {
    throw std::runtime_error("expected one node");
  }
  return storage[0].module;
}

void test_runtime(const fs::path &fixture, const fs::path &gain, const fs::path &probe) {
  current_test = "runtime";
  void *library = dlopen(fixture.c_str(), RTLD_NOW | RTLD_LOCAL);
  check(library != nullptr, "the fixture opens");
  if (!library) {
    return;
  }
  const auto control_fn =
      reinterpret_cast<metadata_control_fn>(dlsym(library, METADATA_CONTROL_SYMBOL));
  metadata_control *control = control_fn();
  std::vector<vespera::NodeMetadata> storage;
  {
    vespera::Graph graph;
    graph.add_module(fixture);
    const vespera::ModuleMetadata &module = only_module(graph, storage);
    check(module.parameter_metadata, "the extension is reported");
    check(module.parameters[0].display.kind == ParameterKind::toggle, "the toggle is exposed");
    check(module.parameters[1].display.text(-3) == "-3.0 dB", "mappings reach host code");
    check(module.parameters[2].display.text(0) == "Clean", "labels reach host code");
    check(module.parameters[3].display.parse("off") == 0.0f, "host code parses with them");
  }
  control->entries[1].skew = 0;
  expect_throws(
      [&] {
        vespera::Graph graph;
        graph.add_module(fixture);
      },
      "parameter metadata: parameter 2 needs a display skew");
  expect_throws(
      [&] {
        vespera::ModuleCatalog catalog;
        catalog.add(fixture);
      },
      "parameter metadata");
  control->restore();
  control->entries[0].kind = VS_PARAMETER_CONTINUOUS;
  control->entries[0].steps = 0;
  expect_throws(
      [&] {
        vespera::Graph graph;
        graph.add_module(fixture);
      },
      "bypass parameter but is not described as a toggle");
  control->restore();
  control->decline = 1;
  {
    vespera::Graph graph;
    graph.add_module(fixture);
    const vespera::ModuleMetadata &module = only_module(graph, storage);
    check(!module.parameter_metadata && !module.parameters[2].display.described &&
              module.parameters[2].display.kind == ParameterKind::continuous,
          "a declined extension leaves the module loadable without metadata");
  }
  control->restore();
  {
    vespera::Graph graph;
    graph.add_module(probe);
    const vespera::ModuleMetadata &module = only_module(graph, storage);
    check(!module.parameter_metadata, "a module without the extension reports none");
    for (const auto &p : module.parameters) {
      check(!p.display.described && !p.display.mapping && p.display.id == p.id &&
                p.display.minimum == p.minimum && p.display.maximum == p.maximum,
            "undescribed parameters keep their descriptor range");
    }
  }
  {
    vespera::Graph graph;
    graph.add_module(gain);
    const vespera::ModuleMetadata &module = only_module(graph, storage);
    check(module.parameter_metadata && module.parameters[0].display.described,
          "kit modules export metadata");
  }
}

}

int main(int argc, char **argv) {
  if (argc != 4) {
    std::fprintf(stderr, "usage: %s <metadata-fixture> <gain-module> <probe-fixture>\n",
                 argc > 0 ? argv[0] : "parameter_display_test");
    return 2;
  }
  const std::function<void()> tests[] = {
      test_signs,
      test_kilo_thresholds,
      test_skew_extremes,
      test_off_ends,
      test_round_trips,
      test_scanning,
      test_parameter_text,
      test_finite_range_boundaries,
      test_validation,
      [&] { test_runtime(fs::canonical(argv[1]), fs::canonical(argv[2]), fs::canonical(argv[3])); },
  };
  for (const auto &test : tests) {
    try {
      test();
    } catch (const std::exception &error) {
      std::fprintf(stderr, "[%s] unexpected exception: %s\n", current_test, error.what());
      ++failures;
    }
  }
  if (failures != 0) {
    std::fprintf(stderr, "%d parameter display check(s) failed\n", failures);
    return 1;
  }
  std::puts("parameter display tests passed");
  return 0;
}
