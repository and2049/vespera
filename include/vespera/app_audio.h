#ifndef VESPERA_APP_AUDIO_H
#define VESPERA_APP_AUDIO_H

#include <vespera/module.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Control-thread JSON bridge. Responses belong to the instance until its next request.
 * No bridge function is called on the audio callback thread. */
typedef struct vs_app_audio_api {
  uint32_t struct_size;
  uint32_t version;
  void *(*create)(void);
  void (*destroy)(void *instance);
  const char *(*request)(void *instance, const char *json);
} vs_app_audio_api;
typedef const vs_app_audio_api *(*vs_app_audio_entry_fn)(uint32_t version);
VS_MODULE_EXPORT const vs_app_audio_api *vespera_app_audio_entry(uint32_t version);

#ifdef __cplusplus
}
#endif
#endif
