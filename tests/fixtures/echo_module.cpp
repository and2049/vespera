// Tail fixture: output is the input plus up to four echoes, each half the level of the one before,
// spaced by the Interval parameter in frames, plus a constant Hum offset. It declares no tail, as
// hosted plug-ins that misreport theirs do.
#include <vespera/kit/module.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

namespace kit = vespera::kit;

constexpr size_t max_interval = 32000;
constexpr size_t max_repeats = 4;
constexpr size_t line_frames = max_interval * max_repeats + 1;

struct Echo {
  static constexpr const char *id = "test.echo";
  static constexpr const char *name = "Test Echo";
  static constexpr const char *version = "0.1.0";
  static constexpr uint32_t layouts = VS_LAYOUT_MONO | VS_LAYOUT_STEREO;
  static constexpr bool flush_subnormals = false;
  static constexpr auto parameters =
      kit::parameters(kit::knob(1, "Interval").range(1, max_interval).initial(4000),
                      kit::knob(2, "Repeats").range(0, max_repeats).initial(3),
                      kit::knob(3, "Hum").range(0, 1).initial(0));

  struct Processor {
    static constexpr uint32_t latency_frames = 0;
    static constexpr uint64_t tail_frames = 0;
    size_t interval;
    size_t repeats;
    float hum;
    std::vector<kit::Frame> line;
    size_t position = 0;

    explicit Processor(const kit::Setup &setup)
        : interval(static_cast<size_t>(setup.values[0])),
          repeats(static_cast<size_t>(setup.values[1])), hum(setup.values[2]), line(line_frames) {}
    void reset() noexcept {
      std::fill(line.begin(), line.end(), kit::Frame{});
      position = 0;
    }
    void set_parameter(size_t index, float value) noexcept {
      if (index == 0) {
        interval = static_cast<size_t>(value);
      } else if (index == 1) {
        repeats = static_cast<size_t>(value);
      } else {
        hum = value;
      }
    }
    kit::Frame process_frame(const kit::Frame &in) noexcept {
      line[position] = in;
      kit::Frame out{in[0] + hum, in[1] + hum};
      float gain = 1.0f;
      for (size_t k = 1; k <= repeats; ++k) {
        gain *= 0.5f;
        const auto &echo = line[(position + line_frames - k * interval) % line_frames];
        out[0] += gain * echo[0];
        out[1] += gain * echo[1];
      }
      position = (position + 1) % line_frames;
      return out;
    }
  };
};

}

VESPERA_KIT_MODULE(Echo)
