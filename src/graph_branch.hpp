#ifndef VESPERA_SRC_GRAPH_BRANCH_HPP
#define VESPERA_SRC_GRAPH_BRANCH_HPP

// The host-realised side of a branch merge: gains, alignment delay lines and the sample-accurate
// merge itself, shared by every graph that prepares branch paths.

#include <vespera/parameter_display.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

namespace vespera::graph_branch {

struct Gains {
  float main = 1.0f;
  float branch = 0.0f;

  bool operator==(const Gains &) const = default;
};

// The prepared merge of one branch node: host-realised gains and the alignment delay lines.
struct BranchPath {
  uint32_t mix_parameter = 0;
  // The mix parameter's metadata: the host mixes with its nearest value, the same canonical value a
  // module that quantizes the parameter stores and reports.
  ParameterDisplay mix_display;
  std::optional<uint32_t> bypass_parameter;
  float mix = 0.0f;
  bool on = true;
  Gains from;
  Gains to;
  uint32_t ramp_position = 0;
  uint32_t ramp_length = 1;
  uint32_t main_delay = 0;
  uint32_t branch_delay = 0;
  uint32_t main_position = 0;
  uint32_t branch_position = 0;
  std::vector<float> main_line;
  std::vector<float> branch_line;

  Gains gains_at(uint32_t position) const noexcept {
    if (position >= ramp_length) {
      return to;
    }
    const float t = static_cast<float>(position) / static_cast<float>(ramp_length);
    return {from.main + (to.main - from.main) * t, from.branch + (to.branch - from.branch) * t};
  }

  void retarget(bool snap) noexcept {
    const Gains target = on ? Gains{1.0f - mix, mix} : Gains{1.0f, 0.0f};
    from = snap ? target : gains_at(ramp_position);
    to = target;
    ramp_position = from == to ? ramp_length : 0;
  }

  void apply(uint32_t parameter_id, float value) noexcept {
    if (parameter_id == mix_parameter) {
      mix = mix_display.nearest(value);
    } else if (bypass_parameter && parameter_id == *bypass_parameter) {
      on = value >= 0.5f;
    } else {
      return;
    }
    retarget(false);
  }

  void clear() noexcept {
    std::fill(main_line.begin(), main_line.end(), 0.0f);
    std::fill(branch_line.begin(), branch_line.end(), 0.0f);
    main_position = 0;
    branch_position = 0;
    retarget(true);
  }
};

// One delayed sample through a ring line shared per channel.
inline float delay(std::vector<float> &line, uint32_t length, uint32_t channel, uint32_t &position,
                   float input) noexcept {
  if (length == 0) {
    return input;
  }
  float &slot = line[static_cast<size_t>(channel) * length + position];
  const float output = slot;
  slot = input;
  position = position + 1 == length ? 0 : position + 1;
  return output;
}

// Mixes the main signal and the branch output into target. Settled extremes copy one delayed
// signal, so a fully dry or fully wet merge is bit-exact.
void merge(BranchPath &path, const float *const *main, const float *const *branch,
           float *const *target, uint32_t channels, uint32_t frames,
           const vs_parameter_event *events, uint32_t count) noexcept;

}

#endif
