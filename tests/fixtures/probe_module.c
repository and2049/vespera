#include "probe_control.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct probe_instance {
  int32_t ordinal;
  float level;
  float offset;
} probe_instance;

static probe_control control = {.fail_prepare_instance = -1, .fault_instance = -1};

VS_MODULE_EXPORT probe_control *vespera_probe_control(void) { return &control; }

static void record_op(int32_t op, int32_t instance) {
  if (control.op_count < PROBE_MAX_OPS) {
    control.ops[control.op_count].op = op;
    control.ops[control.op_count].instance = instance;
  }
  control.op_count++;
}

static vs_result probe_create(void **out) {
  if (control.fail_create || control.next_instance >= PROBE_MAX_INSTANCES) {
    return VS_ERROR_STATE;
  }
  probe_instance *instance = (probe_instance *)malloc(sizeof(probe_instance));
  if (instance == NULL) {
    return VS_ERROR_STATE;
  }
  instance->ordinal = control.next_instance++;
  instance->level = 1.0f;
  instance->offset = 0.0f;
  control.live_instances++;
  record_op(PROBE_OP_CREATE, instance->ordinal);
  *out = instance;
  return VS_OK;
}

static void probe_destroy(void *opaque) {
  probe_instance *instance = (probe_instance *)opaque;
  record_op(PROBE_OP_DESTROY, instance->ordinal);
  control.live_instances--;
  free(instance);
}

static vs_result probe_prepare(void *opaque, const vs_prepare_info *info,
                               vs_module_timing *timing) {
  probe_instance *instance = (probe_instance *)opaque;
  record_op(PROBE_OP_PREPARE, instance->ordinal);
  control.last_prepare[instance->ordinal] = *info;
  if (instance->ordinal == control.fail_prepare_instance) {
    return VS_ERROR_UNSUPPORTED;
  }
  timing->latency_frames = control.latency_frames[instance->ordinal];
  timing->tail_frames = control.tail_frames[instance->ordinal];
  return VS_OK;
}

static void probe_release(void *opaque) {
  record_op(PROBE_OP_RELEASE, ((probe_instance *)opaque)->ordinal);
}

static void probe_reset(void *opaque) {
  record_op(PROBE_OP_RESET, ((probe_instance *)opaque)->ordinal);
}

static int overlaps(const float *a, const float *b, uint32_t frames) {
  uintptr_t x = (uintptr_t)a;
  uintptr_t y = (uintptr_t)b;
  uintptr_t size = (uintptr_t)frames * sizeof(float);
  return x < y + size && y < x + size;
}

static void apply(probe_instance *instance, const vs_parameter_event *event) {
  if (event->parameter_id == PROBE_PARAM_LEVEL) {
    instance->level = event->value;
  } else if (event->parameter_id == PROBE_PARAM_OFFSET) {
    instance->offset = event->value;
  }
}

static vs_result probe_process(void *opaque, const vs_process_block *block) {
  probe_instance *instance = (probe_instance *)opaque;
  probe_block_record *record = &control.blocks[instance->ordinal];
  record->calls++;
  record->channels = block->channels;
  record->frames = block->frames;
  record->flags = block->flags;
  record->frame_position = block->frame_position;
  record->event_count = block->event_count;
  record->tempo = block->tempo != NULL;
  for (uint32_t c = 0; c < block->channels; ++c) {
    for (uint32_t d = 0; d < block->channels; ++d) {
      if (overlaps(block->inputs[c], block->outputs[d], block->frames)) {
        record->aliased = 1;
      }
    }
  }
  for (uint32_t e = 0; e < block->event_count; ++e) {
    if (control.event_count < PROBE_MAX_EVENTS) {
      probe_event_record *log = &control.events[control.event_count];
      log->instance = instance->ordinal;
      log->block = record->calls;
      log->event = block->events[e];
    }
    control.event_count++;
  }

  if (instance->ordinal == control.fault_instance && control.fault == PROBE_FAULT_ERROR) {
    return VS_ERROR_PROCESS;
  }

  uint32_t next = 0;
  for (uint32_t i = 0; i < block->frames; ++i) {
    while (next < block->event_count && block->events[next].frame_offset == i) {
      apply(instance, &block->events[next++]);
    }
    for (uint32_t c = 0; c < block->channels; ++c) {
      block->outputs[c][i] = block->inputs[c][i] * instance->level + instance->offset;
    }
  }

  if (instance->ordinal == control.fault_instance) {
    if (control.fault == PROBE_FAULT_NAN) {
      block->outputs[0][block->frames - 1] = NAN;
    } else if (control.fault == PROBE_FAULT_INFINITY) {
      block->outputs[block->channels - 1][0] = INFINITY;
    }
  }
  return VS_OK;
}

static const vs_parameter_descriptor valid_parameters[] = {
    {PROBE_PARAM_LEVEL, "Level", "", 0.0f, 2.0f, 1.0f},
    {PROBE_PARAM_OFFSET, "Offset", "", -1.0f, 1.0f, 0.0f},
};

static vs_parameter_descriptor variant_parameters[2];
/* Trailing storage lets the extended variant model a newer module whose descriptor has fields
   appended after the version 1 layout. */
static struct {
  vs_module_api api;
  uint64_t appended;
} variant_storage;
static vs_module_api *const variant = &variant_storage.api;

static const vs_module_api *make_variant(void) {
  variant_parameters[0] = valid_parameters[0];
  variant_parameters[1] = valid_parameters[1];
  *variant = (vs_module_api){
      .struct_size = sizeof(vs_module_api),
      .abi_version = VS_MODULE_ABI_VERSION,
      .id = "dev.vespera.test.probe",
      .name = "Probe",
      .version = "0.0.1",
      .supported_layouts = VS_LAYOUT_MONO | VS_LAYOUT_STEREO,
      .parameters = variant_parameters,
      .parameter_count = 2,
      .create = probe_create,
      .destroy = probe_destroy,
      .prepare = probe_prepare,
      .release = probe_release,
      .reset = probe_reset,
      .process = probe_process,
  };
  switch (control.descriptor) {
  case PROBE_DESCRIPTOR_NULL:
    return NULL;
  case PROBE_DESCRIPTOR_WRONG_ABI:
    variant->abi_version = VS_MODULE_ABI_VERSION + 1;
    break;
  case PROBE_DESCRIPTOR_SMALL_STRUCT:
    variant->struct_size = sizeof(vs_module_api) - sizeof(void *);
    break;
  case PROBE_DESCRIPTOR_NULL_PROCESS:
    variant->process = NULL;
    break;
  case PROBE_DESCRIPTOR_DUPLICATE_PARAMETER:
    variant_parameters[1].id = PROBE_PARAM_LEVEL;
    break;
  case PROBE_DESCRIPTOR_NONFINITE_BOUND:
    variant_parameters[1].maximum = INFINITY;
    break;
  case PROBE_DESCRIPTOR_DEFAULT_OUT_OF_RANGE:
    variant_parameters[0].default_value = 3.0f;
    break;
  case PROBE_DESCRIPTOR_INVERTED_BOUNDS:
    variant_parameters[0].minimum = 2.0f;
    variant_parameters[0].maximum = 0.0f;
    break;
  case PROBE_DESCRIPTOR_BAD_LAYOUTS:
    variant->supported_layouts = 4u;
    break;
  case PROBE_DESCRIPTOR_UNQUALIFIED_ID:
    variant->id = "probe";
    break;
  case PROBE_DESCRIPTOR_NULL_NAME:
    variant->name = NULL;
    break;
  case PROBE_DESCRIPTOR_NULL_PARAMETER_TABLE:
    variant->parameters = NULL;
    break;
  case PROBE_DESCRIPTOR_TOO_MANY_PARAMETERS:
    variant->parameter_count = 100000;
    break;
  case PROBE_DESCRIPTOR_GAIN_IMPOSTOR:
    variant->id = "dev.vespera.gain";
    variant->version = "0.1.0";
    break;
  case PROBE_DESCRIPTOR_MONO_ONLY:
    variant->supported_layouts = VS_LAYOUT_MONO;
    break;
  case PROBE_DESCRIPTOR_OVERLONG_NAME_UTF8:
    variant->name = "Probe \xc0\xaf";
    break;
  case PROBE_DESCRIPTOR_SURROGATE_VERSION_UTF8:
    variant->version = "0.0.1-\xed\xa0\x80";
    break;
  case PROBE_DESCRIPTOR_TRUNCATED_PARAMETER_UTF8:
    variant_parameters[0].name = "Level \xe2\x82";
    break;
  case PROBE_DESCRIPTOR_OUT_OF_RANGE_UNIT_UTF8:
    variant_parameters[1].unit = "\xf4\x90\x80\x80";
    break;
  case PROBE_DESCRIPTOR_UNICODE:
    variant->name = "Probe \xe2\x9c\x93";
    variant->version = "0.0.1-\xce\xb2";
    variant_parameters[0].name = "Lautst\xc3\xa4rke";
    variant_parameters[1].unit = "\xc2\xb5s \xf0\x9f\x8e\xb8";
    break;
  case PROBE_DESCRIPTOR_EXTENDED_STRUCT:
    variant->struct_size = sizeof(variant_storage);
    variant_storage.appended = UINT64_MAX;
    break;
  case PROBE_DESCRIPTOR_BYPASS_OFFSET:
    variant_parameters[1].minimum = 0.0f;
    variant->capabilities = VS_MODULE_BYPASS_PARAMETER;
    variant->bypass_parameter = PROBE_PARAM_OFFSET;
    break;
  case PROBE_DESCRIPTOR_TEMPO_AND_MIDI:
    variant->capabilities = VS_MODULE_HOST_TEMPO | VS_MODULE_MIDI_INPUT;
    break;
  case PROBE_DESCRIPTOR_BYPASS_UNDECLARED_PARAMETER:
    variant->capabilities = VS_MODULE_BYPASS_PARAMETER;
    variant->bypass_parameter = 1;
    break;
  case PROBE_DESCRIPTOR_BYPASS_WRONG_RANGE:
    variant->capabilities = VS_MODULE_BYPASS_PARAMETER;
    variant->bypass_parameter = PROBE_PARAM_LEVEL;
    break;
  case PROBE_DESCRIPTOR_BYPASS_WITHOUT_CAPABILITY:
    variant_parameters[1].minimum = 0.0f;
    variant->bypass_parameter = PROBE_PARAM_OFFSET;
    break;
  case PROBE_DESCRIPTOR_UNKNOWN_CAPABILITY:
    variant->capabilities = 8u;
    break;
  default:
    break;
  }
  return variant;
}

VS_MODULE_EXPORT const vs_module_api *vespera_module_entry(uint32_t host_abi_version) {
  if (control.descriptor == PROBE_DESCRIPTOR_ABI_1_ONLY) {
    if (host_abi_version != 1) {
      return NULL;
    }
    make_variant();
    variant->abi_version = 1;
    return variant;
  }
  if (host_abi_version != VS_MODULE_ABI_VERSION) {
    return NULL;
  }
  return make_variant();
}
