#ifndef VESPERA_TEST_METADATA_CONTROL_H
#define VESPERA_TEST_METADATA_CONTROL_H

#include <vespera/module.h>

// The metadata fixture's descriptor, which tests rewrite before loading the module. Its
// parameters are 1 Enabled (bypass, 0-1), 2 Level (dB, -24-12), 3 Mode (0-2, default 1) and
// 4 Cutoff (0-1).
typedef struct metadata_control {
  vs_module_metadata_api api;
  vs_parameter_metadata entries[8];
  // Non-zero makes the entry point decline the host's metadata ABI.
  int decline;
  // Restores the valid descriptor.
  void (*restore)(void);
} metadata_control;

#define METADATA_CONTROL_SYMBOL "vespera_metadata_control"
typedef metadata_control *(*metadata_control_fn)(void);

#endif
