#include <vespera/kit/module.hpp>

namespace {

namespace kit = vespera::kit;

struct Gain {
  static constexpr const char *id = "dev.vespera.gain";
  static constexpr const char *name = "Gain";
  static constexpr const char *version = "0.1.0";
  static constexpr uint32_t layouts = VS_LAYOUT_MONO | VS_LAYOUT_STEREO;
  // Unity gain reproduces every input sample exactly, subnormals included.
  static constexpr bool flush_subnormals = false;
  static constexpr auto parameters = kit::parameters(
      kit::knob(1, "Gain").range(0, 4).initial(1).unit("linear").decimals(2).display_unit("x"));

  struct Processor {
    static constexpr uint32_t latency_frames = 0;
    static constexpr uint64_t tail_frames = 0;
    float gain;

    explicit Processor(const kit::Setup &setup) : gain(setup.values[0]) {}
    void reset() noexcept {}
    void set_parameter(size_t, float value) noexcept { gain = value; }
    kit::Frame process_frame(const kit::Frame &in) noexcept { return {in[0] * gain, in[1] * gain}; }
  };
};

}

VESPERA_KIT_MODULE(Gain)
