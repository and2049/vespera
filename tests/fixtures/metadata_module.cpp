// A pass-through module on the raw ABI whose parameter metadata tests can rewrite through
// vespera_metadata_control() before a host loads it, to check how hosts treat malformed metadata.
#include "metadata_control.h"

#include <cstring>
#include <new>

namespace {

const vs_parameter_descriptor parameters[] = {
    {1, "Enabled", "", 0.0f, 1.0f, 1.0f},
    {2, "Level", "dB", -24.0f, 12.0f, 0.0f},
    {3, "Mode", "", 0.0f, 2.0f, 1.0f},
    {4, "Cutoff", "", 0.0f, 1.0f, 0.5f},
};

const char *const modes[] = {"Clean", "Crunch", "Lead"};

metadata_control control;

void restore() {
  control.decline = 0;
  control.api = {sizeof(vs_module_metadata_api), VS_MODULE_METADATA_ABI_VERSION, control.entries, 4,
                 sizeof(vs_parameter_metadata)};
  control.entries[0] = {1, VS_PARAMETER_TOGGLE,   2, 0, nullptr, 0, 0, 0, nullptr, nullptr,
                        0, VS_PARAMETER_OFF_NONE, 0};
  control.entries[1] = {2,       VS_PARAMETER_CONTINUOUS,
                        0,       VS_PARAMETER_DISPLAY_MAPPED | VS_PARAMETER_DISPLAY_SIGNED,
                        nullptr, -24,
                        12,      1,
                        " dB",   nullptr,
                        1,       VS_PARAMETER_OFF_NONE,
                        0};
  control.entries[2] = {3, VS_PARAMETER_ENUMERATED, 3, 0, modes, 0, 0, 0, nullptr, nullptr,
                        0, VS_PARAMETER_OFF_NONE,   0};
  control.entries[3] = {4,       VS_PARAMETER_CONTINUOUS,
                        0,       VS_PARAMETER_DISPLAY_MAPPED,
                        nullptr, 20,
                        20000,   0.5,
                        " Hz",   " kHz",
                        0,       VS_PARAMETER_OFF_MINIMUM,
                        0};
}

struct Instance {
  float values[4] = {1.0f, 0.0f, 1.0f, 0.5f};
};

vs_result create(void **instance) {
  *instance = new (std::nothrow) Instance;
  return *instance ? VS_OK : VS_ERROR_STATE;
}
void destroy(void *instance) { delete static_cast<Instance *>(instance); }
vs_result prepare(void *, const vs_prepare_info *, vs_module_timing *timing) {
  timing->latency_frames = 0;
  timing->tail_frames = 0;
  return VS_OK;
}
void release(void *) {}
void reset(void *) {}
vs_result process(void *, const vs_process_block *block) {
  for (uint32_t c = 0; c < block->channels; ++c) {
    std::memcpy(block->outputs[c], block->inputs[c], block->frames * sizeof(float));
  }
  return VS_OK;
}

vs_result get_value(void *instance, uint32_t id, float *value) {
  if (id < 1 || id > 4) {
    return VS_ERROR_ARGUMENT;
  }
  *value = static_cast<Instance *>(instance)->values[id - 1];
  return VS_OK;
}
vs_result set_value(void *instance, uint32_t id, float value) {
  if (id < 1 || id > 4) {
    return VS_ERROR_ARGUMENT;
  }
  static_cast<Instance *>(instance)->values[id - 1] = value;
  return VS_OK;
}

const vs_module_api api = {sizeof(vs_module_api),
                           VS_MODULE_ABI_VERSION,
                           "test.metadata",
                           "Metadata",
                           "1.0.0",
                           VS_LAYOUT_MONO | VS_LAYOUT_STEREO,
                           parameters,
                           4,
                           create,
                           destroy,
                           prepare,
                           release,
                           reset,
                           process,
                           VS_MODULE_BYPASS_PARAMETER,
                           1};

const vs_module_parameter_api parameter_api = {
    sizeof(vs_module_parameter_api), VS_MODULE_PARAMETER_ABI_VERSION, get_value, set_value};

}

extern "C" VS_MODULE_EXPORT metadata_control *vespera_metadata_control(void) {
  if (control.api.struct_size == 0) {
    restore();
  }
  control.restore = restore;
  return &control;
}

extern "C" VS_MODULE_EXPORT const vs_module_api *vespera_module_entry(uint32_t version) {
  return version == VS_MODULE_ABI_VERSION ? &api : nullptr;
}

extern "C" VS_MODULE_EXPORT const vs_module_parameter_api *
vespera_module_parameter_entry(uint32_t version) {
  return version == VS_MODULE_PARAMETER_ABI_VERSION ? &parameter_api : nullptr;
}

extern "C" VS_MODULE_EXPORT const vs_module_metadata_api *
vespera_module_metadata_entry(uint32_t version) {
  if (control.api.struct_size == 0) {
    restore();
  }
  return version == VS_MODULE_METADATA_ABI_VERSION && !control.decline ? &control.api : nullptr;
}
