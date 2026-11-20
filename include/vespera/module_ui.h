#ifndef VESPERA_MODULE_UI_H
#define VESPERA_MODULE_UI_H

#include <vespera/module.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VS_MODULE_UI_ABI_VERSION 1u
#define VS_MODULE_UI_ENTRY_NAME "vespera_module_ui_entry"

/* All UI calls and host callbacks run on the main thread. The host serializes parameter access
 * with processing. Modules copy the host table; its context remains valid until destroy returns.
 * On macOS create returns an owned NSView, released only by destroy. The host sets its frame.
 * refresh synchronizes visuals after parameter edits or state restoration. */
typedef struct vs_module_ui_host {
  uint32_t struct_size;
  void *context;
  vs_result (*get_value)(void *context, uint32_t id, float *value);
  vs_result (*set_value)(void *context, uint32_t id, float value);
  /* Main-thread snapshot, including when the control-only instance is not prepared. */
  vs_result (*get_tempo)(void *context, vs_tempo_context *tempo);
} vs_module_ui_host;

#define VS_MODULE_UI_HOST_V1_SIZE offsetof(vs_module_ui_host, get_tempo)

static inline vs_module_ui_host vs_module_ui_copy_host(const vs_module_ui_host *source) {
  vs_module_ui_host result;
  result.struct_size = sizeof(result);
  result.context = source->context;
  result.get_value = source->get_value;
  result.set_value = source->set_value;
  result.get_tempo = source->struct_size >= sizeof(result) ? source->get_tempo : 0;
  return result;
}

typedef struct vs_module_ui_api {
  uint32_t struct_size;
  uint32_t abi_version;
  const char *platform;
  void *(*create)(const vs_module_ui_host *host, const char *resource_directory);
  void (*destroy)(void *view);
  void (*refresh)(void *view);
} vs_module_ui_api;

typedef const vs_module_ui_api *(*vs_module_ui_entry_fn)(uint32_t version);

#ifdef __cplusplus
}
#endif
#endif
