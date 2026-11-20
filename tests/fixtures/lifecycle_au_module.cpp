// An Audio Unit module for a component that is not installed. Built with an unusable lifecycle
// lock directory, preparation must fail before any AudioToolbox call; with a usable one it reaches
// component lookup and reports the component as unsupported.
#include <vespera/sdk/audio_unit.hpp>

namespace {
struct Configuration {
  static constexpr uint32_t type = 0x61756678, subtype = 0x766c636b, manufacturer = 0x76737061;
  static constexpr const char *id = "test.lifecycle.au";
  static constexpr const char *name = "Lifecycle";
  static constexpr uint32_t bypass_parameter = 1;
  static constexpr const char *shared_engine = "test.lifecycle/1";
  static constexpr uint32_t shared_stage = 1;
  static constexpr auto parameters =
      std::to_array<vs_parameter_descriptor>({{1, "Enabled", "bool", 0, 1, 1}});
  static constexpr auto mapping =
      std::to_array<vespera::sdk::AudioUnitParameter>({{0, "Active", 0, 1}});
  static constexpr auto settings = std::array<vespera::sdk::AudioUnitSetting, 0>{};
};
using Module = vespera::sdk::AudioUnitModule<Configuration>;
}

extern "C" VS_MODULE_EXPORT const vs_module_api *vespera_module_entry(uint32_t version) {
  return version == VS_MODULE_ABI_VERSION ? &Module::api : nullptr;
}
extern "C" VS_MODULE_EXPORT const vs_module_shared_api *
vespera_module_shared_entry(uint32_t version) {
  return version == VS_MODULE_SHARED_ABI_VERSION ? Module::shared_api() : nullptr;
}
