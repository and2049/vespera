// Timing fixture: output is the input delayed by DELAY_LATENCY frames, which the module reports
// as its latency, plus half of it delayed by a further DELAY_ECHO frames. It reports DELAY_TAIL
// as its tail, which may be UINT64_MAX for an unbounded one.
#include <vespera/kit/module.hpp>

#include <array>
#include <cstdint>

#ifndef DELAY_ID
#define DELAY_ID "test.delay"
#endif
#ifndef DELAY_LATENCY
#define DELAY_LATENCY 100
#endif
#ifndef DELAY_ECHO
#define DELAY_ECHO 32
#endif
#ifndef DELAY_TAIL
#define DELAY_TAIL 64
#endif

namespace {

namespace kit = vespera::kit;

constexpr size_t line_frames = 256;
static_assert(DELAY_LATENCY + DELAY_ECHO < line_frames);

struct Delay {
  static constexpr const char *id = DELAY_ID;
  static constexpr const char *name = "Test Delay";
  static constexpr const char *version = "0.1.0";
  static constexpr uint32_t layouts = VS_LAYOUT_MONO | VS_LAYOUT_STEREO;
  static constexpr bool flush_subnormals = false;
  static constexpr auto parameters = kit::parameters(kit::knob(1, "Level").range(0, 4).initial(1));

  struct Processor {
    static constexpr uint32_t latency_frames = DELAY_LATENCY;
    static constexpr uint64_t tail_frames = DELAY_TAIL;
    float level;
    std::array<kit::Frame, line_frames> line{};
    size_t position = 0;

    explicit Processor(const kit::Setup &setup) : level(setup.values[0]) {}
    void reset() noexcept {
      line.fill({});
      position = 0;
    }
    void set_parameter(size_t, float value) noexcept { level = value; }
    kit::Frame process_frame(const kit::Frame &in) noexcept {
      line[position] = in;
      const auto &direct = line[(position + line_frames - DELAY_LATENCY) % line_frames];
      const auto &echo = line[(position + line_frames - DELAY_LATENCY - DELAY_ECHO) % line_frames];
      position = (position + 1) % line_frames;
      return {level * (direct[0] + 0.5f * echo[0]), level * (direct[1] + 0.5f * echo[1])};
    }
  };
};

}

VESPERA_KIT_MODULE(Delay)
