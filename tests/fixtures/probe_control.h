#ifndef VESPERA_TESTS_PROBE_CONTROL_H
#define VESPERA_TESTS_PROBE_CONTROL_H

#include <vespera/module.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROBE_CONTROL_SYMBOL "vespera_probe_control"
#define PROBE_PARAM_LEVEL 10u
#define PROBE_PARAM_OFFSET 20u
#define PROBE_MAX_INSTANCES 16
#define PROBE_MAX_OPS 512
#define PROBE_MAX_EVENTS 2048

enum probe_descriptor {
  PROBE_DESCRIPTOR_VALID,
  PROBE_DESCRIPTOR_NULL,
  PROBE_DESCRIPTOR_WRONG_ABI,
  PROBE_DESCRIPTOR_SMALL_STRUCT,
  PROBE_DESCRIPTOR_NULL_PROCESS,
  PROBE_DESCRIPTOR_DUPLICATE_PARAMETER,
  PROBE_DESCRIPTOR_NONFINITE_BOUND,
  PROBE_DESCRIPTOR_DEFAULT_OUT_OF_RANGE,
  PROBE_DESCRIPTOR_INVERTED_BOUNDS,
  PROBE_DESCRIPTOR_BAD_LAYOUTS,
  PROBE_DESCRIPTOR_UNQUALIFIED_ID,
  PROBE_DESCRIPTOR_NULL_NAME,
  PROBE_DESCRIPTOR_NULL_PARAMETER_TABLE,
  PROBE_DESCRIPTOR_TOO_MANY_PARAMETERS,
  PROBE_DESCRIPTOR_GAIN_IMPOSTOR,
  PROBE_DESCRIPTOR_MONO_ONLY,
  PROBE_DESCRIPTOR_OVERLONG_NAME_UTF8,
  PROBE_DESCRIPTOR_SURROGATE_VERSION_UTF8,
  PROBE_DESCRIPTOR_TRUNCATED_PARAMETER_UTF8,
  PROBE_DESCRIPTOR_OUT_OF_RANGE_UNIT_UTF8,
  PROBE_DESCRIPTOR_UNICODE,
  PROBE_DESCRIPTOR_EXTENDED_STRUCT,
  /* Valid capability declarations: bypass through PROBE_PARAM_OFFSET, then host tempo and MIDI. */
  PROBE_DESCRIPTOR_BYPASS_OFFSET,
  PROBE_DESCRIPTOR_TEMPO_AND_MIDI,
  /* Invalid capability declarations. */
  PROBE_DESCRIPTOR_BYPASS_UNDECLARED_PARAMETER,
  PROBE_DESCRIPTOR_BYPASS_WRONG_RANGE,
  PROBE_DESCRIPTOR_BYPASS_WITHOUT_CAPABILITY,
  PROBE_DESCRIPTOR_UNKNOWN_CAPABILITY,
  /* A module built for module ABI 1: the entry point declines the current ABI. */
  PROBE_DESCRIPTOR_ABI_1_ONLY,
  PROBE_DESCRIPTOR_COUNT
};

enum probe_op {
  PROBE_OP_CREATE,
  PROBE_OP_PREPARE,
  PROBE_OP_RELEASE,
  PROBE_OP_RESET,
  PROBE_OP_DESTROY
};

enum probe_fault { PROBE_FAULT_NONE, PROBE_FAULT_ERROR, PROBE_FAULT_NAN, PROBE_FAULT_INFINITY };

typedef struct probe_op_record {
  int32_t op;
  int32_t instance;
} probe_op_record;

typedef struct probe_event_record {
  int32_t instance;
  uint32_t block;
  vs_parameter_event event;
} probe_event_record;

typedef struct probe_block_record {
  uint32_t calls;
  uint32_t channels;
  uint32_t frames;
  uint32_t flags;
  uint64_t frame_position;
  uint32_t aliased;
  uint32_t event_count;
  uint32_t tempo;
} probe_block_record;

typedef struct probe_control {
  int32_t descriptor;
  int32_t fail_create;
  int32_t fail_prepare_instance;
  int32_t fault_instance;
  int32_t fault;
  uint32_t latency_frames[PROBE_MAX_INSTANCES];
  uint64_t tail_frames[PROBE_MAX_INSTANCES];

  int32_t next_instance;
  int32_t live_instances;
  vs_prepare_info last_prepare[PROBE_MAX_INSTANCES];
  probe_block_record blocks[PROBE_MAX_INSTANCES];
  uint32_t op_count;
  probe_op_record ops[PROBE_MAX_OPS];
  uint32_t event_count;
  probe_event_record events[PROBE_MAX_EVENTS];
} probe_control;

typedef probe_control *(*probe_control_fn)(void);

#ifdef __cplusplus
}
#endif

#endif
