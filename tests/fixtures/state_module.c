#include "state_control.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define STATE_LIMIT_BYTES (1024u * 1024u)

typedef struct state_instance {
  int prepared;
  float level;
  float offset;
} state_instance;

static state_control control;

VS_MODULE_EXPORT state_control *vespera_state_control(void) { return &control; }

static vs_result state_create(void **out) {
  state_instance *instance = (state_instance *)calloc(1, sizeof(state_instance));
  if (instance == NULL) {
    return VS_ERROR_STATE;
  }
  instance->level = 1.0f;
  control.live_instances++;
  control.creates++;
  *out = instance;
  return VS_OK;
}

static void state_destroy(void *opaque) {
  control.live_instances--;
  free(opaque);
}

static vs_result state_prepare(void *opaque, const vs_prepare_info *info,
                               vs_module_timing *timing) {
  (void)info;
  (void)timing;
  if (control.fail_prepare) {
    return VS_ERROR_UNSUPPORTED;
  }
  ((state_instance *)opaque)->prepared = 1;
  return VS_OK;
}

static void state_release(void *opaque) { ((state_instance *)opaque)->prepared = 0; }

static void state_reset(void *opaque) { (void)opaque; }

static vs_result state_process(void *opaque, const vs_process_block *block) {
  state_instance *instance = (state_instance *)opaque;
  uint32_t next = 0;
  for (uint32_t i = 0; i < block->frames; ++i) {
    while (next < block->event_count && block->events[next].frame_offset == i) {
      const vs_parameter_event *event = &block->events[next++];
      if (event->parameter_id == STATE_PARAM_LEVEL) {
        instance->level = event->value;
      } else {
        instance->offset = event->value;
      }
    }
    for (uint32_t c = 0; c < block->channels; ++c) {
      block->outputs[c][i] = block->inputs[c][i] * instance->level + instance->offset;
    }
  }
  return VS_OK;
}

static void put_float(uint8_t *out, float value) {
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  for (uint32_t i = 0; i < 4; ++i) {
    out[i] = (uint8_t)(bits >> (8 * i));
  }
}

static float get_float(const uint8_t *in) {
  uint32_t bits = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    bits |= (uint32_t)in[i] << (8 * i);
  }
  float value;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

static uint64_t current_size(void) {
  switch (control.size_mode) {
  case STATE_SIZE_LIMIT:
    return STATE_LIMIT_BYTES;
  case STATE_SIZE_OVER_LIMIT:
    return STATE_LIMIT_BYTES + 1u;
  default:
    return 8u;
  }
}

static vs_result state_size(void *opaque, uint64_t *bytes) {
  (void)opaque;
  if (control.size_mode == STATE_SIZE_ERROR) {
    return VS_ERROR_STATE;
  }
  *bytes = current_size();
  return VS_OK;
}

static vs_result state_save(void *opaque, uint8_t *buffer, uint64_t capacity, uint64_t *written) {
  const state_instance *instance = (const state_instance *)opaque;
  const uint64_t size = current_size();
  if (control.save_mode == STATE_SAVE_ERROR) {
    return VS_ERROR_STATE;
  }
  if (capacity < size) {
    return VS_ERROR_ARGUMENT;
  }
  if (size == 8u) {
    put_float(buffer, instance->level);
    put_float(buffer + 4, instance->offset);
  } else {
    for (uint64_t i = 0; i < size; ++i) {
      buffer[i] = (uint8_t)(i & 0xffu);
    }
  }
  *written = size;
  if (control.save_mode == STATE_SAVE_SHORT) {
    *written = size - 1u;
  } else if (control.save_mode == STATE_SAVE_LONG) {
    *written = capacity + 1u;
  }
  return VS_OK;
}

static int in_range(float value, float minimum, float maximum) {
  return isfinite(value) && value >= minimum && value <= maximum;
}

static vs_result state_load(void *opaque, uint32_t schema_version, const uint8_t *buffer,
                            uint64_t bytes) {
  state_instance *instance = (state_instance *)opaque;
  control.loads++;
  control.last_load_schema = schema_version;
  if (instance->prepared) {
    control.loads_while_prepared++;
    return VS_ERROR_STATE;
  }
  if (control.fail_load) {
    return VS_ERROR_STATE;
  }
  float level;
  float offset = 0.0f;
  if (schema_version == STATE_SCHEMA_LEGACY) {
    if (bytes != 4u) {
      return VS_ERROR_ARGUMENT;
    }
    level = get_float(buffer);
  } else if (schema_version == STATE_SCHEMA_CURRENT) {
    if (bytes != 8u) {
      return VS_ERROR_ARGUMENT;
    }
    level = get_float(buffer);
    offset = get_float(buffer + 4);
  } else {
    return VS_ERROR_UNSUPPORTED;
  }
  if (!in_range(level, -8.0f, 8.0f) || !in_range(offset, -1.0f, 1.0f)) {
    return VS_ERROR_ARGUMENT;
  }
  instance->level = level;
  instance->offset = offset;
  return VS_OK;
}

static const vs_parameter_descriptor parameters[] = {
    {STATE_PARAM_LEVEL, "Level", "", -8.0f, 8.0f, 1.0f},
    {STATE_PARAM_OFFSET, "Offset", "", -1.0f, 1.0f, 0.0f},
};

static vs_module_api api;
static vs_module_state_api state_api;

VS_MODULE_EXPORT const vs_module_api *vespera_module_entry(uint32_t host_abi_version) {
  if (host_abi_version != VS_MODULE_ABI_VERSION) {
    return NULL;
  }
  api = (vs_module_api){
      .struct_size = sizeof(vs_module_api),
      .abi_version = VS_MODULE_ABI_VERSION,
      .id = STATE_MODULE_ID,
      .name = "State",
      .version = control.version[0] != '\0' ? control.version : "1.0.0",
      .supported_layouts = VS_LAYOUT_MONO | VS_LAYOUT_STEREO,
      .parameters = parameters,
      .parameter_count = 2,
      .create = state_create,
      .destroy = state_destroy,
      .prepare = state_prepare,
      .release = state_release,
      .reset = state_reset,
      .process = state_process,
  };
  return &api;
}

static float *parameter_slot(state_instance *instance, uint32_t parameter_id) {
  if (parameter_id == STATE_PARAM_LEVEL) {
    return &instance->level;
  }
  return parameter_id == STATE_PARAM_OFFSET ? &instance->offset : NULL;
}

static vs_result state_get_value(void *opaque, uint32_t parameter_id, float *value) {
  float *slot = parameter_slot((state_instance *)opaque, parameter_id);
  control.gets++;
  if (slot == NULL || value == NULL) {
    return VS_ERROR_ARGUMENT;
  }
  switch (control.get_mode) {
  case STATE_GET_ERROR:
    return VS_ERROR_STATE;
  case STATE_GET_UNWRITTEN:
    return VS_OK;
  case STATE_GET_NAN:
    *value = NAN;
    return VS_OK;
  case STATE_GET_OUT_OF_RANGE:
    *value = 9.0f;
    return VS_OK;
  default:
    *value = *slot;
    return VS_OK;
  }
}

static vs_result state_set_value(void *opaque, uint32_t parameter_id, float value) {
  state_instance *instance = (state_instance *)opaque;
  float *slot = parameter_slot(instance, parameter_id);
  control.sets++;
  if (instance->prepared) {
    control.sets_while_prepared++;
  }
  if (slot == NULL) {
    return VS_ERROR_ARGUMENT;
  }
  if (control.fail_set) {
    return VS_ERROR_STATE;
  }
  *slot = value;
  return VS_OK;
}

/* Trailing storage lets the extended variant model a newer module whose descriptor has fields
   appended after the version 1 layout. */
static struct {
  vs_module_parameter_api api;
  uint64_t appended;
} parameter_storage;

VS_MODULE_EXPORT const vs_module_parameter_api *
vespera_module_parameter_entry(uint32_t host_parameter_abi_version) {
  if (host_parameter_abi_version != VS_MODULE_PARAMETER_ABI_VERSION) {
    return NULL;
  }
  vs_module_parameter_api *api = &parameter_storage.api;
  *api = (vs_module_parameter_api){
      .struct_size = sizeof(vs_module_parameter_api),
      .abi_version = VS_MODULE_PARAMETER_ABI_VERSION,
      .get_value = state_get_value,
      .set_value = state_set_value,
  };
  parameter_storage.appended = UINT64_MAX;
  switch (control.parameter_descriptor) {
  case STATE_PARAMETERS_DECLINE:
    return NULL;
  case STATE_PARAMETERS_SMALL_STRUCT:
    api->struct_size = sizeof(vs_module_parameter_api) - sizeof(void *);
    break;
  case STATE_PARAMETERS_WRONG_ABI:
    api->abi_version = VS_MODULE_PARAMETER_ABI_VERSION + 1u;
    break;
  case STATE_PARAMETERS_NULL_GET:
    api->get_value = NULL;
    break;
  case STATE_PARAMETERS_EXTENDED_STRUCT:
    api->struct_size = sizeof(parameter_storage);
    break;
  default:
    break;
  }
  return api;
}

VS_MODULE_EXPORT const vs_module_state_api *
vespera_module_state_entry(uint32_t host_state_abi_version) {
  if (host_state_abi_version != VS_MODULE_STATE_ABI_VERSION) {
    return NULL;
  }
  state_api = (vs_module_state_api){
      .struct_size = sizeof(vs_module_state_api),
      .abi_version = VS_MODULE_STATE_ABI_VERSION,
      .schema_version = STATE_SCHEMA_CURRENT,
      .size = state_size,
      .save = state_save,
      .load = state_load,
  };
  switch (control.descriptor) {
  case STATE_DESCRIPTOR_DECLINE:
    return NULL;
  case STATE_DESCRIPTOR_SMALL_STRUCT:
    state_api.struct_size = sizeof(vs_module_state_api) - sizeof(void *);
    break;
  case STATE_DESCRIPTOR_WRONG_ABI:
    state_api.abi_version = VS_MODULE_STATE_ABI_VERSION + 1u;
    break;
  case STATE_DESCRIPTOR_ZERO_SCHEMA:
    state_api.schema_version = 0;
    break;
  case STATE_DESCRIPTOR_NULL_SAVE:
    state_api.save = NULL;
    break;
  default:
    break;
  }
  return &state_api;
}
