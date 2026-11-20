#ifndef VESPERA_TESTS_STATE_CONTROL_H
#define VESPERA_TESTS_STATE_CONTROL_H

#include <vespera/module.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STATE_CONTROL_SYMBOL "vespera_state_control"
#define STATE_MODULE_ID "dev.vespera.test.state"
#define STATE_PARAM_LEVEL 1u
#define STATE_PARAM_OFFSET 2u
#define STATE_SCHEMA_CURRENT 2u
#define STATE_SCHEMA_LEGACY 1u
#define STATE_MAX_VERSION 64

enum state_descriptor {
  STATE_DESCRIPTOR_VALID,
  STATE_DESCRIPTOR_DECLINE,
  STATE_DESCRIPTOR_SMALL_STRUCT,
  STATE_DESCRIPTOR_WRONG_ABI,
  STATE_DESCRIPTOR_ZERO_SCHEMA,
  STATE_DESCRIPTOR_NULL_SAVE
};

enum state_size_mode {
  STATE_SIZE_NORMAL,
  STATE_SIZE_ERROR,
  STATE_SIZE_LIMIT,
  STATE_SIZE_OVER_LIMIT
};

enum state_save_mode { STATE_SAVE_NORMAL, STATE_SAVE_ERROR, STATE_SAVE_SHORT, STATE_SAVE_LONG };

enum state_parameter_descriptor {
  STATE_PARAMETERS_VALID,
  STATE_PARAMETERS_DECLINE,
  STATE_PARAMETERS_SMALL_STRUCT,
  STATE_PARAMETERS_WRONG_ABI,
  STATE_PARAMETERS_NULL_GET,
  STATE_PARAMETERS_EXTENDED_STRUCT
};

enum state_get_mode {
  STATE_GET_NORMAL,
  STATE_GET_ERROR,
  STATE_GET_NAN,
  STATE_GET_OUT_OF_RANGE,
  STATE_GET_UNWRITTEN
};

typedef struct state_control {
  int32_t descriptor;
  int32_t size_mode;
  int32_t save_mode;
  int32_t fail_load;
  int32_t fail_prepare;
  int32_t parameter_descriptor;
  int32_t get_mode;
  int32_t fail_set;
  char version[STATE_MAX_VERSION];

  int32_t live_instances;
  uint32_t creates;
  uint32_t loads;
  uint32_t loads_while_prepared;
  uint32_t last_load_schema;
  uint32_t gets;
  uint32_t sets;
  uint32_t sets_while_prepared;
} state_control;

typedef state_control *(*state_control_fn)(void);

#ifdef __cplusplus
}
#endif

#endif
