#include <vespera/kit/module.hpp>

#ifndef EXTERNAL_MODULE_ID
#define EXTERNAL_MODULE_ID "org.example.test-amp"
#endif
#ifndef EXTERNAL_FIRST_UNIT
#define EXTERNAL_FIRST_UNIT "bool"
#endif
#ifndef EXTERNAL_SERVICES
#define EXTERNAL_SERVICES 0
#endif

namespace {

namespace kit = vespera::kit;

int created_instances = 0;
int attached_instances = 0;

constexpr kit::Parameter role([[maybe_unused]] uint32_t id, kit::Parameter parameter) {
#ifdef EXTERNAL_BYPASS_PARAMETER
  return id == EXTERNAL_BYPASS_PARAMETER ? parameter.bypass() : parameter;
#else
  return parameter;
#endif
}

// EXTERNAL_METADATA 1 describes parameters to the host: Gain shown as signed decibels, a labelled
// Mode and a stepped Voices. 2 exports the same metadata with a malformed element size.
#ifdef EXTERNAL_METADATA
constexpr const char *modes[] = {"Clean", "Crunch", "Lead"};
#endif

struct ExternalAmp {
#if EXTERNAL_FILES
  static constexpr auto files = kit::files(kit::file(1, "Response").types("wav").optional());
#endif
  static constexpr const char *id = EXTERNAL_MODULE_ID;
  static constexpr const char *name = "External Test Amp";
  static constexpr const char *version = "0.1.0";
  static constexpr uint32_t layouts = VS_LAYOUT_MONO | VS_LAYOUT_STEREO;
#ifdef EXTERNAL_METADATA
  static constexpr auto parameters = kit::parameters(
      role(1, kit::toggle(1, "Enabled").unit(EXTERNAL_FIRST_UNIT)),
      role(2, kit::knob(2, "Gain").shows(-12, 12).decibels()), kit::choice(3, "Mode", modes, 1),
      kit::knob(4, "Voices").range(1, 8).initial(2).steps(8).display_unit(" voices"));
#else
  static constexpr auto parameters = kit::parameters(
      role(1, kit::toggle(1, "Enabled").unit(EXTERNAL_FIRST_UNIT)), role(2, kit::knob(2, "Gain")));
#endif

  struct Processor {
    static constexpr uint32_t latency_frames = 0;
    static constexpr uint64_t tail_frames = 0;
    float gain;
#if EXTERNAL_SERVICES == 7
    kit::Derived<float, float> derived_gain;
    Processor(const kit::Setup &setup, kit::Host &host)
        : gain(setup.values[1]),
          derived_gain(host.derive<float>(gain, [](float value, kit::Job &) { return value; })) {}
#else
    explicit Processor(const kit::Setup &setup) : gain(setup.values[1]) {}
#endif
    void reset() noexcept {}
    void set_parameter(size_t index, float value) noexcept {
      if (index == 1) {
        gain = value;
#if EXTERNAL_SERVICES == 7
        derived_gain.request(value);
#endif
      }
    }
    kit::Frame process_frame(const kit::Frame &in) noexcept {
#if EXTERNAL_SERVICES == 7
      return {in[0] * *derived_gain.current(), in[1] * *derived_gain.current()};
#else
      return {in[0] * gain, in[1] * gain};
#endif
    }
  };
};

using Module = kit::Module<ExternalAmp>;

vs_result create(void **instance) {
  const vs_result result = Module::api.create(instance);
  created_instances += result == VS_OK;
  return result;
}

const vs_module_api api = [] {
  vs_module_api counted = Module::api;
  counted.create = create;
  return counted;
}();

}

// Lets tests confirm which operations create module instances.
extern "C" VS_MODULE_EXPORT int external_test_created_instances() { return created_instances; }
extern "C" VS_MODULE_EXPORT int external_test_attached_instances() { return attached_instances; }
extern "C" VS_MODULE_EXPORT const vs_module_api *vespera_module_entry(uint32_t version) {
  return version == VS_MODULE_ABI_VERSION ? &api : nullptr;
}
extern "C" VS_MODULE_EXPORT const vs_module_parameter_api *
vespera_module_parameter_entry(uint32_t version) {
  return version == VS_MODULE_PARAMETER_ABI_VERSION ? &Module::parameter_api : nullptr;
}
extern "C" VS_MODULE_EXPORT const vs_module_state_api *
vespera_module_state_entry(uint32_t version) {
  return version == VS_MODULE_STATE_ABI_VERSION ? &Module::state_api : nullptr;
}

#if EXTERNAL_FILES
extern "C" VS_MODULE_EXPORT const vs_module_files_api *
vespera_module_files_entry(uint32_t version) {
  return version == VS_MODULE_FILES_ABI_VERSION ? &Module::files_api : nullptr;
}
#endif
#ifdef EXTERNAL_METADATA
namespace {
const vs_module_metadata_api metadata = [] {
  vs_module_metadata_api described = Module::metadata_api;
#if EXTERNAL_METADATA == 2
  described.parameter_size = 4;
#endif
  return described;
}();
}
extern "C" VS_MODULE_EXPORT const vs_module_metadata_api *
vespera_module_metadata_entry(uint32_t version) {
  return version == VS_MODULE_METADATA_ABI_VERSION ? &metadata : nullptr;
}
#endif

#if EXTERNAL_SERVICES != 0
namespace {
vs_result attach(void *instance, const vs_host_services *services) {
  ++attached_instances;
  return Module::services_api.attach(instance, services);
}
const vs_module_services_api services = [] {
  auto api = Module::services_api;
  api.attach = attach;
#if EXTERNAL_SERVICES == 2
  api.struct_size = 4;
#elif EXTERNAL_SERVICES == 3
  api.abi_version = 2;
#elif EXTERNAL_SERVICES == 4
  api.attach = nullptr;
#elif EXTERNAL_SERVICES == 5
  api.status = nullptr;
#endif
  return api;
}();
}
extern "C" VS_MODULE_EXPORT const vs_module_services_api *
vespera_module_services_entry(uint32_t version) {
  return version == VS_MODULE_SERVICES_ABI_VERSION && EXTERNAL_SERVICES != 6 ? &services : nullptr;
}
#endif
