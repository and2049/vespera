#import <AppKit/AppKit.h>
#include <vespera/module_ui.h>

@interface ExternalTestPanel : NSView {
@public
  vs_module_ui_host host;
}
@end

@implementation ExternalTestPanel
- (void)drawRect:(NSRect)rect {
  [NSColor.cyanColor setFill];
  NSRectFill(rect);
}
- (void)mouseDown:(NSEvent *)event {
  host.set_value(host.context, 2, 0.75f);
}
@end

namespace {
void *create(const vs_module_ui_host *host, const char *) {
  ExternalTestPanel *view = [[ExternalTestPanel alloc] initWithFrame:NSMakeRect(0, 0, 200, 100)];
  view->host = *host;
  return (__bridge_retained void *)view;
}
void destroy(void *handle) {
  NSView *view = (__bridge_transfer NSView *)handle;
  [view removeFromSuperview];
}
void refresh(void *handle) { [(__bridge NSView *)handle setNeedsDisplay:YES]; }
const vs_module_ui_api api{
    sizeof(vs_module_ui_api), VS_MODULE_UI_ABI_VERSION, "macos-nsview", create, destroy, refresh};
}
extern "C" VS_MODULE_EXPORT const vs_module_ui_api *vespera_module_ui_entry(uint32_t version) {
  return version == VS_MODULE_UI_ABI_VERSION ? &api : nullptr;
}
