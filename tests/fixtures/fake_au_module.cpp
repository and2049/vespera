// An Audio Unit module hosting the in-process fake unit of tests/fake_audio_unit.hpp, which the
// loading test registers before preparing it.
#include "fake_audio_unit.hpp"

#include <vespera/sdk/audio_unit.hpp>

namespace {
struct Configuration {
  static constexpr uint32_t type = fake_au::type, subtype = fake_au::subtype,
                            manufacturer = fake_au::manufacturer;
  static constexpr const char *id = "test.fake-au";
  static constexpr const char *name = "Fake Audio Unit";
  static constexpr uint32_t bypass_parameter = 1;
  static constexpr auto parameters =
      std::to_array<vs_parameter_descriptor>({{1, "Enabled", "bool", 0, 1, 1}});
  static constexpr auto mapping = std::to_array<vespera::sdk::AudioUnitParameter>(
      {{fake_au::active_parameter, "Active", 0, 1}});
  static constexpr auto settings = std::array<vespera::sdk::AudioUnitSetting, 0>{};
};
using Module = vespera::sdk::AudioUnitModule<Configuration>;
}

extern "C" VS_MODULE_EXPORT const vs_module_api *vespera_module_entry(uint32_t version) {
  return version == VS_MODULE_ABI_VERSION ? &Module::api : nullptr;
}
extern "C" VS_MODULE_EXPORT const vs_module_parameter_api *
vespera_module_parameter_entry(uint32_t version) {
  return version == VS_MODULE_PARAMETER_ABI_VERSION ? &Module::parameter_api : nullptr;
}
