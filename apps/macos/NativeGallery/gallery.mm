#import "NativeGallery.h"
#define VS_UI_PREFIX VesperaGallery
#include <vespera/module_controls_macos.h>

@interface VesperaGalleryCanvas : NSView
@end

@implementation VesperaGalleryCanvas
- (BOOL)isFlipped {
  return YES;
}
- (void)drawRect:(NSRect)dirtyRect {
  [vs_ui_color(VS_UI_SURFACE_PANEL) setFill];
  NSRectFill(self.bounds);
}
@end

namespace {
void add(NSView *parent, NSView *child, NSRect frame) {
  child.frame = frame;
  [parent addSubview:child];
}

VSStyledDropdown *dropdown(vs_ui_size size, NSArray<NSString *> *items, BOOL enabled) {
  VSStyledDropdown *menu = [[VSStyledDropdown alloc] initWithFrame:NSZeroRect pullsDown:NO];
  menu.uiSize = size;
  [menu addItemsWithTitles:items];
  menu.autoenablesItems = NO;
  menu.enabled = enabled;
  return menu;
}

VSStyledCheckbox *checkbox(vs_ui_size size, NSString *title, BOOL on, BOOL enabled) {
  VSStyledCheckbox *box = [VSStyledCheckbox checkboxWithTitle:title target:nil action:nil];
  box.uiSize = size;
  box.state = on ? NSControlStateValueOn : NSControlStateValueOff;
  box.enabled = enabled;
  return box;
}

CGFloat column(NSView *canvas, vs_ui_size size, CGFloat x) {
  vs_ui_metrics metrics = vs_ui_metrics_for(size);
  CGFloat y = metrics.panel_padding;
  CGFloat width = 260;
  add(canvas, vs_ui_label(size == VS_UI_SIZE_COMPACT ? @"COMPACT" : @"STANDARD", size),
      NSMakeRect(x, y, width, metrics.label_font_size + 4));
  y += metrics.label_font_size + 4 + metrics.label_gap;
  NSArray *items = @[
    @"Condenser 184", @"A very long microphone description that truncates", @"Disabled choice"
  ];
  VSStyledDropdown *menu = dropdown(size, items, YES);
  [menu itemAtIndex:2].enabled = NO;
  add(canvas, menu, NSMakeRect(x, y, width, metrics.control_height));
  y += metrics.control_height + metrics.stack_spacing;
  VSStyledDropdown *truncated = dropdown(size, items, YES);
  [truncated selectItemAtIndex:1];
  add(canvas, truncated, NSMakeRect(x, y, width / 2, metrics.control_height));
  add(canvas, dropdown(size, items, NO),
      NSMakeRect(x + width / 2 + metrics.label_gap, y, width / 2 - metrics.label_gap,
                 metrics.control_height));
  y += metrics.control_height + metrics.stack_spacing;
  NSArray *boxes = @[
    checkbox(size, @"Enabled", YES, YES), checkbox(size, @"Off", NO, YES),
    checkbox(size, @"Disabled", YES, NO)
  ];
  for (NSUInteger i = 0; i < boxes.count; ++i) {
    add(canvas, boxes[i],
        NSMakeRect(x + static_cast<CGFloat>(i) * width / 3, y, width / 3, metrics.control_height));
  }
  y += metrics.control_height + metrics.stack_spacing;
  NSArray<NSString *> *labels = @[ @"KEY", @"SCALE" ];
  const double labelWidths[]{vs_ui_label_width(labels[0], size),
                             vs_ui_label_width(labels[1], size)};
  const double fieldWidths[]{56, 110};
  vs_ui_labelled_field row[2];
  if (vs_ui_layout_labelled_row(x, x + width, metrics.label_gap, metrics.field_gap, labelWidths,
                                fieldWidths, 2, row)) {
    for (int i = 0; i < 2; ++i) {
      add(canvas, vs_ui_label(labels[static_cast<NSUInteger>(i)], size),
          NSMakeRect(row[i].label_x, y + (metrics.control_height - metrics.label_font_size - 4) / 2,
                     row[i].label_width, metrics.label_font_size + 4));
      add(canvas, dropdown(size, i == 0 ? @[ @"C♯", @"D" ] : @[ @"Harmonic Minor", @"Major" ], YES),
          NSMakeRect(row[i].field_x, y, row[i].field_width, metrics.control_height));
    }
  }
  y += metrics.control_height + metrics.stack_spacing;
  NSTextField *value = [NSTextField textFieldWithString:@"440.0 Hz"];
  vs_ui_style_value_field(value, size);
  add(canvas, value, NSMakeRect(x, y, 90, metrics.control_height));
  return y + metrics.control_height + metrics.panel_padding;
}
} // namespace

NSView *VesperaNativeControlGallery(void) {
  VesperaGalleryCanvas *canvas = [[VesperaGalleryCanvas alloc] initWithFrame:NSZeroRect];
  CGFloat height =
      std::max(column(canvas, VS_UI_SIZE_STANDARD, 20), column(canvas, VS_UI_SIZE_COMPACT, 300));
  vs_ui_metrics compact = vs_ui_metrics_for(VS_UI_SIZE_COMPACT);
  CGFloat y = height;
  NSArray *switches = @[ @"ROOM SEND", @"Ø  PHASE" ];
  for (NSUInteger i = 0; i < switches.count; ++i) {
    for (int on = 0; on < 2; ++on) {
      VSStyledSwitch *toggle = [VSStyledSwitch checkboxWithTitle:switches[i] target:nil action:nil];
      toggle.phaseStyle = i == 1;
      toggle.state = on ? NSControlStateValueOn : NSControlStateValueOff;
      add(canvas, toggle,
          NSMakeRect(20 + static_cast<CGFloat>(i * 2 + static_cast<NSUInteger>(on)) * 135, y,
                     i == 1 ? 92 : 124, 28));
    }
  }
  canvas.frame = NSMakeRect(0, 0, 580, y + 28 + compact.panel_padding);
  return canvas;
}
