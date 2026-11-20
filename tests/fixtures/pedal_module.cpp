#include <vespera/kit/module.hpp>

#ifndef PEDAL_ID
#define PEDAL_ID "test.pedal"
#endif
#ifndef PEDAL_BYPASS_PARAMETER
#define PEDAL_BYPASS_PARAMETER 1
#endif

namespace {

namespace kit = vespera::kit;

constexpr kit::Parameter role(kit::Parameter parameter) {
  return parameter.id() == PEDAL_BYPASS_PARAMETER ? parameter.bypass() : parameter;
}

struct Pedal {
  static constexpr const char *id = PEDAL_ID;
  static constexpr const char *name = "Test Pedal";
  static constexpr const char *version = "0.1.0";
  static constexpr uint32_t layouts = VS_LAYOUT_MONO | VS_LAYOUT_STEREO;
  static constexpr auto parameters = kit::parameters(
      role(kit::toggle(1, "Enabled")), role(kit::knob(2, "Gain").range(0, 4).initial(1)),
      role(kit::knob(3, "Offset", 0)));

  struct Processor {
    static constexpr uint32_t latency_frames = 0;
    static constexpr uint64_t tail_frames = 0;
    std::array<float, 3> values{};
    explicit Processor(const kit::Setup &setup) {
      std::copy(setup.values.begin(), setup.values.end(), values.begin());
    }
    void reset() noexcept {}
    void set_parameter(size_t index, float value) noexcept { values[index] = value; }
    kit::Frame process_frame(const kit::Frame &in) noexcept {
      return {in[0] * values[1] + values[2], in[1] * values[1] + values[2]};
    }
  };
};

using Module = kit::Module<Pedal>;

}

extern "C" VS_MODULE_EXPORT const vs_module_api *vespera_module_entry(uint32_t version) {
  return version == VS_MODULE_ABI_VERSION ? &Module::api : nullptr;
}
extern "C" VS_MODULE_EXPORT const vs_module_parameter_api *
vespera_module_parameter_entry(uint32_t version) {
  return version == VS_MODULE_PARAMETER_ABI_VERSION ? &Module::parameter_api : nullptr;
}
