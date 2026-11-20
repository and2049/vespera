#include "graph_branch.hpp"

namespace vespera::graph_branch {

void merge(BranchPath &path, const float *const *main, const float *const *branch,
           float *const *target, uint32_t channels, uint32_t frames,
           const vs_parameter_event *events, uint32_t count) noexcept {
  uint32_t e = 0;
  for (uint32_t frame = 0; frame < frames;) {
    while (e < count && events[e].frame_offset <= frame) {
      path.apply(events[e].parameter_id, events[e].value);
      ++e;
    }
    uint32_t end = e < count ? events[e].frame_offset : frames;
    const bool ramping = path.ramp_position < path.ramp_length;
    if (ramping) {
      end = std::min(end, frame + (path.ramp_length - path.ramp_position));
    }
    const Gains settled = path.to;
    const bool only_main = settled == Gains{1.0f, 0.0f};
    const bool only_branch = settled == Gains{0.0f, 1.0f};
    uint32_t main_position = path.main_position;
    uint32_t branch_position = path.branch_position;
    for (uint32_t c = 0; c < channels; ++c) {
      main_position = path.main_position;
      branch_position = path.branch_position;
      const float *dry = main[c];
      const float *wet = branch[c];
      float *out = target[c];
      for (uint32_t k = frame; k < end; ++k) {
        const float m = delay(path.main_line, path.main_delay, c, main_position, dry[k]);
        const float w = delay(path.branch_line, path.branch_delay, c, branch_position, wet[k]);
        if (ramping) {
          const Gains g = path.gains_at(path.ramp_position + (k - frame) + 1);
          out[k] = g.main * m + g.branch * w;
        } else if (only_main) {
          out[k] = m;
        } else if (only_branch) {
          out[k] = w;
        } else {
          out[k] = settled.main * m + settled.branch * w;
        }
      }
    }
    path.main_position = main_position;
    path.branch_position = branch_position;
    if (ramping) {
      path.ramp_position += end - frame;
    }
    frame = end;
  }
}

}
