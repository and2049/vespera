#pragma once
// Embeds the declarative layout renderer in a module's own UI binary. Define VESPERA_ARTWORK_VIEW
// (a package-unique class name) and VESPERA_ARTWORK_LAYOUT (the layout file name inside the
// resources directory) before including. Packages can instead declare the layout in their
// manifest `ui` object and ship no UI code; the host then renders it with the same renderer.
#define VS_UI_PREFIX VESPERA_ARTWORK_VIEW
#define VS_LAYOUT_VIEW VESPERA_ARTWORK_VIEW
#include <vespera/sdk/layout_view_macos.h>

namespace {
void *vs_artwork_create(const vs_module_ui_host *host, const char *directory) {
  if (!host || host->struct_size < VS_MODULE_UI_HOST_V1_SIZE || !host->get_value ||
      !host->set_value || !directory) {
    return nullptr;
  }
  NSString *error = nil;
  VESPERA_ARTWORK_VIEW *view =
      [[VESPERA_ARTWORK_VIEW alloc] initWithHost:host
                                       directory:[NSString stringWithUTF8String:directory]
                                          layout:VESPERA_ARTWORK_LAYOUT
                                  requireVersion:NO
                                           error:&error];
  if (!view) {
    NSLog(@"vespera module layout: %@", error);
    return nullptr;
  }
  return (__bridge_retained void *)view;
}
void vs_artwork_destroy(void *view) {
  NSView *native = (__bridge_transfer NSView *)view;
  [native removeFromSuperview];
}
void vs_artwork_refresh(void *view) { [(__bridge VESPERA_ARTWORK_VIEW *)view refresh]; }
const vs_module_ui_api vs_artwork_api{sizeof(vs_module_ui_api), VS_MODULE_UI_ABI_VERSION,
                                      "macos-nsview",           vs_artwork_create,
                                      vs_artwork_destroy,       vs_artwork_refresh};
}
extern "C" VS_MODULE_EXPORT const vs_module_ui_api *vespera_module_ui_entry(uint32_t version) {
  return version == VS_MODULE_UI_ABI_VERSION ? &vs_artwork_api : nullptr;
}
