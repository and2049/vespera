#import <Foundation/Foundation.h>

#define VS_LAYOUT_VIEW VesperaModuleCheckLayoutView
#define VS_LAYOUT_FILE_WIDGETS 1
#include <vespera/sdk/layout_view_macos.h>

#include "checks.hpp"

#include <cmath>

namespace vespera::module_check {
namespace {

NSString *string(const std::string &text) { return [NSString stringWithUTF8String:text.c_str()]; }

std::string text(NSString *value) { return value ? std::string(value.UTF8String) : std::string(); }

float single(id value) { return static_cast<float>([value doubleValue]); }

// The parameter a layout reference names, when it is a whole number the module declares.
const Descriptor *referenced(const Binary &binary, id value) {
  const double parameter = [value doubleValue];
  if (parameter < 0 || parameter > UINT32_MAX || parameter != std::floor(parameter)) {
    return nullptr;
  }
  return binary.find(static_cast<uint32_t>(parameter));
}

// Compares an explicit layout format with the module's described display mapping. The shown number
// (range, skew) and the off end carry meaning, so disagreement there fails; decimals, units and
// signs are presentation a compact readout may abbreviate, so disagreement there warns.
// Fails when a layout mapping, applied to values from minimum to maximum, shows other numbers
// than the module's described mapping.
void compare_numbers(const std::string &name, const std::string &field,
                     const DisplayMapping &layout, float minimum, float maximum,
                     const ParameterDisplay &module, Findings &findings) {
  const DisplayMapping &described = *module.mapping;
  const double span = std::abs((*described.range)[1] - (*described.range)[0]);
  for (double position : {0.0, 0.001, 0.1, 0.25, 0.5, 0.75, 0.9, 0.999, 1.0}) {
    const double value = minimum + position * (static_cast<double>(maximum) - minimum);
    const double shown = shown_number(layout, minimum, maximum, value).shown;
    const double expected = shown_number(described, module.minimum, module.maximum, value).shown;
    if (!(std::abs(shown - expected) <= 1e-6 * span)) {
      findings.fail(name + ": " + field + " shows parameter " + std::to_string(module.id) + " at " +
                    number(value) + " as " + number(shown) + ", but the module's metadata shows " +
                    number(expected) + " (range or skew differ)");
      return;
    }
  }
}

void compare_format(const std::string &name, NSDictionary *widget, const ParameterDisplay &module,
                    Findings &findings) {
  const DisplayMapping layout = vs_layout_mapping(widget[@"format"]);
  const DisplayMapping &described = *module.mapping;
  const std::string parameter = "parameter " + std::to_string(module.id);
  compare_numbers(name, "format", layout, single(widget[@"minimum"]), single(widget[@"maximum"]),
                  module, findings);
  if (layout.off != described.off) {
    const auto end = [](OffEnd off) {
      return off == OffEnd::minimum   ? std::string("its minimum")
             : off == OffEnd::maximum ? std::string("its maximum")
                                      : std::string("neither end");
    };
    findings.fail(name + ": format shows OFF at " + end(layout.off) + " of " + parameter +
                  ", but the module's metadata at " + end(described.off));
  }
  std::vector<std::string> differences;
  if (layout.decimals != described.decimals) {
    differences.push_back("decimals");
  }
  if (layout.unit != described.unit) {
    differences.push_back("unit");
  }
  if (layout.kilo_unit != described.kilo_unit) {
    differences.push_back("kiloUnit");
  }
  if (layout.show_sign != described.show_sign) {
    differences.push_back("signed");
  }
  if (layout.off == described.off && layout.off_tolerance != described.off_tolerance) {
    differences.push_back("offTolerance");
  }
  if (!differences.empty()) {
    std::string list;
    for (const std::string &difference : differences) {
      list += (list.empty() ? "" : ", ") + difference;
    }
    findings.warn(name + ": format differs from the module's metadata for " + parameter + " in " +
                  list + "; omit format to show the module's own text");
  }
}

// Layout choices name the values of a parameter the module describes with labels of its own.
void compare_labels(const std::string &name, NSArray *choices, const ParameterDisplay &module,
                    Findings &findings) {
  if (![choices isKindOfClass:NSArray.class] || !choices.count) {
    return;
  }
  for (NSUInteger i = 0; i < choices.count && i < module.labels.size(); ++i) {
    if (text(choices[i]) != module.labels[i]) {
      findings.warn(name + ": choice " + std::to_string(i) + " reads \"" + text(choices[i]) +
                    "\", but the module's metadata names it \"" + module.labels[i] + "\"");
      return;
    }
  }
}

}

void check_layout(const Context &context, const LayoutTarget &target) {
  @autoreleasepool {
    const Binary &binary = context.binary;
    NSString *reason = nil;
    NSDictionary *layout =
        vs_layout_load(string(target.resources.string()), string(target.layout), YES, nil, &reason);
    if (!layout) {
      context.add("layout", Status::fail,
                  "the host renderer rejects " + target.layout + ": " + text(reason));
      return;
    }
    Findings findings;
    if (!binary.parameters) {
      findings.fail("layout controls need the parameter value extension, which the module does "
                    "not export");
    }
    NSArray *widgets = layout[@"widgets"];
    const NSInteger version = [layout[@"version"] integerValue];
    uint32_t controls = 0;
    uint32_t mappings = 0;
    uint32_t compared = 0;
    uint32_t defaults = 0;
    for (NSUInteger index = 0; index < widgets.count; ++index) {
      NSDictionary *widget = widgets[index];
      NSString *type = widget[@"type"];
      std::string name = "widget " + std::to_string(index);
      if ([widget[@"name"] isKindOfClass:NSString.class]) {
        name += " (" + text(widget[@"name"]) + ")";
      }
      const bool display = [widget[@"readonly"] boolValue] || [type isEqualToString:@"meter"] ||
                           (version >= 2 && [type isEqualToString:@"image"]);

      for (NSArray *pair in widget[@"sets"]) {
        const Descriptor *set = referenced(binary, pair[0]);
        const float value = single(pair[1]);
        if (!set) {
          findings.fail(name + ": sets names parameter " + number([pair[0] doubleValue]) +
                        ", which the module does not declare");
        } else if (!(value >= set->minimum && value <= set->maximum)) {
          findings.fail(name + ": sets parameter " + std::to_string(set->id) + " to " +
                        number(value) + ", outside " + range_text(set->minimum, set->maximum));
        }
      }
      for (NSString *key in @[ @"condition", @"imageFollows" ]) {
        NSArray *reference = widget[key];
        if ([reference isKindOfClass:NSArray.class] && !referenced(binary, reference[0])) {
          findings.fail(name + ": " + text(key) + " names parameter " +
                        number([reference[0] doubleValue]) + ", which the module does not declare");
        }
      }

      if ([widget[@"type"] isEqualToString:@"file"]) {
        const uint32_t slot = [widget[@"slot"] unsignedIntValue];
        if (std::none_of(binary.file_slots.begin(), binary.file_slots.end(),
                         [slot](const auto &file) { return file.id == slot; })) {
          findings.fail(name + ": file widget names an undeclared slot");
        }
        continue;
      }
      const uint32_t parameter = [widget[@"id"] unsignedIntValue];
      const Descriptor *d = binary.find(parameter);
      if (!d) {
        if (!display) {
          findings.fail(name + ": controls parameter " + std::to_string(parameter) +
                        ", which the module does not declare");
        } else if (!widget[@"constant"]) {
          findings.warn(name + ": displays parameter " + std::to_string(parameter) +
                        ", which the module does not declare");
        }
        continue;
      }
      const float minimum = single(widget[@"minimum"]);
      const float maximum = single(widget[@"maximum"]);
      if (!display) {
        ++controls;
        if (minimum < d->minimum || maximum > d->maximum) {
          findings.fail(name + ": range " + range_text(minimum, maximum) + " exceeds parameter " +
                        std::to_string(parameter) + "'s " + range_text(d->minimum, d->maximum));
        } else if (minimum != d->minimum || maximum != d->maximum) {
          findings.warn(name + ": range " + range_text(minimum, maximum) +
                        " covers part of parameter " + std::to_string(parameter) + "'s " +
                        range_text(d->minimum, d->maximum));
        }
        const float initial = widget[@"initial"] ? single(widget[@"initial"]) : 0.0f;
        if (!(initial >= d->minimum && initial <= d->maximum)) {
          findings.fail(name + ": reset value " + number(initial) + " is outside parameter " +
                        std::to_string(parameter) + "'s " + range_text(d->minimum, d->maximum));
        } else if (initial != d->default_value) {
          findings.warn(name + ": resets to " + number(initial) + ", but parameter " +
                        std::to_string(parameter) + " defaults to " + number(d->default_value));
        }
        for (NSString *key in @[ @"radio", @"offValue" ]) {
          if (!widget[key]) {
            continue;
          }
          const float value = single(widget[key]);
          if (!(value >= d->minimum && value <= d->maximum)) {
            findings.fail(name + ": " + text(key) + " " + number(value) + " is outside parameter " +
                          std::to_string(parameter) + "'s " + range_text(d->minimum, d->maximum));
          }
        }
      }
      for (NSString *key in @[ @"choices", @"values" ]) {
        NSArray *entries = widget[key];
        if ([entries isKindOfClass:NSArray.class] && entries.count &&
            (std::lround(d->minimum) < 0 ||
             std::lround(d->maximum) >= static_cast<long>(entries.count))) {
          findings.fail(name + ": " + text(key) + " has " + std::to_string(entries.count) +
                        " entries, but parameter " + std::to_string(parameter) + " ranges over " +
                        range_text(d->minimum, d->maximum));
        }
      }
      NSArray *curve = widget[@"curve"];
      if (curve && (d->minimum < 0.0f || d->maximum > 1.0f)) {
        findings.fail(name + ": curve maps values from 0 to 1, but parameter " +
                      std::to_string(parameter) + " ranges over " +
                      range_text(d->minimum, d->maximum) + ", so its readout is wrong above 1");
      }
      NSDictionary *format = widget[@"format"];
      const ParameterDisplay *described = binary.metadata ? binary.display(parameter) : nullptr;
      if (described && described->kind == ParameterKind::enumerated) {
        compare_labels(name, widget[@"choices"], *described, findings);
      }
      if (format && described &&
          (described->kind == ParameterKind::toggle ||
           described->kind == ParameterKind::enumerated)) {
        findings.fail(name + ": format shows parameter " + std::to_string(parameter) +
                      " as a number, but the module describes it as " +
                      (described->kind == ParameterKind::toggle ? "a toggle" : "named choices"));
      } else if (format && described && described->mapping) {
        compare_format(name, widget, *described, findings);
        ++compared;
      } else if (curve && described && described->mapping) {
        DisplayMapping legacy;
        legacy.range = std::array<double, 2>{[curve[0] doubleValue], [curve[1] doubleValue]};
        legacy.skew = [curve[2] doubleValue];
        // A curve reads the value itself as the position, clamped to [0, 1].
        compare_numbers(name, "curve", legacy, 0.0f, 1.0f, *described, findings);
        ++compared;
      } else if (format || curve) {
        NSArray *shown = format[@"range"];
        const bool skewed = format[@"skew"] && [format[@"skew"] doubleValue] != 1.0;
        const bool own_range =
            !shown || (single(shown[0]) == d->minimum && single(shown[1]) == d->maximum);
        if (skewed && own_range && !(d->minimum == 0.0f && d->maximum == 1.0f)) {
          findings.warn(name + ": format skews parameter " + std::to_string(parameter) +
                        " onto its own range " + range_text(d->minimum, d->maximum) +
                        ", so the readout differs from the value");
        }
        if (curve || skewed || !own_range) {
          ++mappings;
        }
      } else if (vs_layout_described(widget, version, &binary.displays)) {
        ++defaults;
      }
    }
    std::string passed = "the host renderer accepts " + target.layout + " (version " +
                         std::to_string(version) + ", " + std::to_string(widgets.count) +
                         " widgets); " + std::to_string(controls) +
                         " controls name declared parameters within their ranges";
    if (compared) {
      passed += "; " + std::to_string(compared) +
                " format(s) and curve(s) compared with the module's parameter metadata";
    }
    if (defaults) {
      passed += "; " + std::to_string(defaults) +
                " widget(s) show values as the module's parameter metadata describes";
    }
    if (mappings) {
      passed += "; " + std::to_string(mappings) +
                " readout mapping(s) (curve, or format range or skew) fit their parameter's "
                "range, but cannot be compared with the module's scaling because its parameter "
                "metadata describes no display mapping for them";
    }
    findings.report(context, "layout", passed);
  }
}

}
