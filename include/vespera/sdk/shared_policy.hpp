#ifndef VESPERA_SDK_SHARED_POLICY_HPP
#define VESPERA_SDK_SHARED_POLICY_HPP

// Pure compile-time validation of a Configuration's routing and native-branch declarations,
// shared by the Audio Unit wrapper's static assertions and by tests of invalid declarations.
// Defining it apart from the wrapper keeps the policy checkable without instantiating a module.

#include <vespera/module.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace vespera::sdk {

// Hosts accept at most this many target assignments from translating one parameter.
inline constexpr size_t shared_assignments_per_parameter = 4;

// Checks a Configuration's optional route_parameter and route_pins without instantiating the
// module, so invalid declarations can also be tested.
template <class Configuration> constexpr bool route_pins_valid() {
  constexpr bool has_parameter = requires { Configuration::route_parameter; };
  constexpr bool has_pins = requires { Configuration::route_pins; };
  if constexpr (!has_parameter && !has_pins) {
    return true;
  } else if constexpr (has_parameter != has_pins) {
    return false;
  } else {
    const auto &parameters = Configuration::parameters;
    const size_t n = parameters.size();
    auto index = [&](uint32_t id) {
      for (size_t i = 0; i < n; ++i) {
        if (parameters[i].id == id) {
          return i;
        }
      }
      return n;
    };
    auto gate = [](uint32_t id) {
      if constexpr (requires { Configuration::power_gates; }) {
        for (auto g : Configuration::power_gates) {
          if (g == id) {
            return true;
          }
        }
      }
      return false;
    };
    constexpr bool gated = requires { Configuration::power_gates; };
    const size_t selector = index(Configuration::route_parameter);
    if (selector == n || (gated && selector == 0) || gate(Configuration::route_parameter)) {
      return false;
    }
    if constexpr (requires { Configuration::bypass_parameter; }) {
      if (Configuration::bypass_parameter == Configuration::route_parameter) {
        return false;
      }
    }
    const auto &route = parameters[selector];
    if (route.minimum != 0 || !(route.maximum >= 1 && route.maximum <= 7) ||
        route.maximum != static_cast<float>(static_cast<uint32_t>(route.maximum))) {
      return false;
    }
    const auto last = static_cast<uint32_t>(route.maximum);
    if constexpr (requires { Configuration::metadata; }) {
      for (const auto &entry : Configuration::metadata) {
        if (entry.parameter_id == Configuration::route_parameter &&
            (entry.kind == VS_PARAMETER_CONTINUOUS || entry.steps != last + 1)) {
          return false;
        }
      }
    }
    const auto &pins = Configuration::route_pins;
    size_t distinct = 0;
    for (size_t k = 0; k < pins.size(); ++k) {
      const auto &pin = pins[k];
      const size_t j = index(pin.parameter);
      if (j == n || j == selector || pin.route > last || (gated && j == 0) || gate(pin.parameter)) {
        return false;
      }
      const auto &target = Configuration::mapping[j];
      if (!(pin.value >= std::min(target.minimum, target.maximum) &&
            pin.value <= std::max(target.minimum, target.maximum))) {
        return false;
      }
      bool repeated = false;
      for (size_t m = 0; m < k; ++m) {
        if (pins[m].parameter == pin.parameter) {
          if (pins[m].route == pin.route) {
            return false;
          }
          repeated = true;
        }
      }
      distinct += repeated ? 0 : 1;
    }
    return distinct + 1 <= shared_assignments_per_parameter;
  }
}

template <class Configuration> constexpr bool native_branch_valid() {
  if constexpr (
      !requires { Configuration::native_branch_mix_parameter; } &&
      !requires { Configuration::native_branch_first_after_stage; } &&
      !requires { Configuration::native_branch_route; }) {
    return true;
  } else if constexpr (requires {
                         Configuration::native_branch_mix_parameter;
                         Configuration::native_branch_first_after_stage;
                         Configuration::native_branch_route;
                         Configuration::route_parameter;
                         Configuration::route_pins;
                         Configuration::shared_engine;
                         Configuration::shared_stage;
                       }) {
    constexpr uint32_t mix = Configuration::native_branch_mix_parameter;
    constexpr uint32_t active = Configuration::native_branch_route;
    constexpr uint32_t fence = Configuration::native_branch_first_after_stage;
    if (!route_pins_valid<Configuration>() || Configuration::shared_stage >= fence ||
        mix == Configuration::route_parameter) {
      return false;
    }
    // The selector drives the AU as well as the host's topology. Same-route writes must
    // canonicalize to the identical value, not send a fractional selector to the AU.
    bool selector_discrete = false;
    if constexpr (requires { Configuration::metadata; }) {
      for (const auto &entry : Configuration::metadata) {
        if (entry.parameter_id == Configuration::route_parameter) {
          selector_discrete = entry.kind == VS_PARAMETER_TOGGLE ||
                              entry.kind == VS_PARAMETER_STEPPED ||
                              entry.kind == VS_PARAMETER_ENUMERATED;
        }
      }
    }
    if (!selector_discrete) {
      return false;
    }
    auto pinned = [](uint32_t id) {
      for (const auto &p : Configuration::route_pins) {
        if (p.route == active && p.parameter == id) {
          return true;
        }
      }
      return false;
    };
    if constexpr (requires { Configuration::bypass_parameter; }) {
      if (mix == Configuration::bypass_parameter || !pinned(Configuration::bypass_parameter)) {
        return false;
      }
    }
    for (const auto &p : Configuration::parameters) {
      if (p.id == mix) {
        return p.minimum == 0 && p.maximum == 1 && pinned(mix);
      }
    }
  }
  return false;
}

}

#endif
