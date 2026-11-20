#include "file_config.hpp"

#ifndef FIXTURE_REJECTS_MISSING
VESPERA_KIT_MODULE(file_fixture::Config)
#else
using Module = vespera::kit::Module<file_fixture::Config>;
extern "C" VS_MODULE_EXPORT const vs_module_api *vespera_module_entry(uint32_t version) {
  return version == VS_MODULE_ABI_VERSION ? &Module::api : nullptr;
}
extern "C" VS_MODULE_EXPORT const vs_module_services_api *
vespera_module_services_entry(uint32_t version) {
  return version == VS_MODULE_SERVICES_ABI_VERSION ? &Module::services_api : nullptr;
}
extern "C" VS_MODULE_EXPORT const vs_module_state_api *
vespera_module_state_entry(uint32_t version) {
  return version == VS_MODULE_STATE_ABI_VERSION ? &Module::state_api : nullptr;
}
extern "C" VS_MODULE_EXPORT const vs_module_parameter_api *
vespera_module_parameter_entry(uint32_t version) {
  return version == VS_MODULE_PARAMETER_ABI_VERSION ? &Module::parameter_api : nullptr;
}
namespace {
vs_result reject_missing(void *instance, uint32_t slot, const vs_file_ref *file) {
  return file && file->state == VS_FILE_MISSING ? VS_ERROR_ARGUMENT
                                                : Module::files_api.set_file(instance, slot, file);
}
const vs_module_files_api files = [] {
  auto result = Module::files_api;
  result.set_file = reject_missing;
  return result;
}();
}
extern "C" VS_MODULE_EXPORT const vs_module_files_api *
vespera_module_files_entry(uint32_t version) {
  return version == VS_MODULE_FILES_ABI_VERSION ? &files : nullptr;
}
#endif
