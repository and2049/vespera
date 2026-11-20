#pragma once
// Token-styled AppKit controls for module UIs. Define a package-unique VS_UI_PREFIX before
// including: loaded packages share one Objective-C runtime, so class names must not collide.
#import <AppKit/AppKit.h>
#include <algorithm>
#include <cmath>
#include <vespera/ui_tokens.h>

#define VS_UI_JOIN_INNER(a, b) a##b
#define VS_UI_JOIN(a, b) VS_UI_JOIN_INNER(a, b)
#define VSStyledSwitch VS_UI_JOIN(VS_UI_PREFIX, StyledSwitch)
#define VSStyledCheckbox VS_UI_JOIN(VS_UI_PREFIX, StyledCheckbox)
#define VSStyledDropdown VS_UI_JOIN(VS_UI_PREFIX, StyledDropdown)
#define VSStyledMenu VS_UI_JOIN(VS_UI_PREFIX, StyledMenu)

static inline NSColor *vs_ui_color(vs_ui_gray gray) {
  return [NSColor colorWithWhite:gray.white alpha:gray.alpha];
}

static inline NSColor *vs_ui_rgb_color(vs_ui_rgb rgb) {
  return [NSColor colorWithSRGBRed:rgb.red green:rgb.green blue:rgb.blue alpha:1];
}

static inline NSFont *vs_ui_font(double size) {
  return [NSFont systemFontOfSize:size weight:NSFontWeightMedium];
}

static inline NSFont *vs_ui_value_font(double size) {
  return [NSFont monospacedDigitSystemFontOfSize:size weight:NSFontWeightRegular];
}

static inline NSDictionary *vs_ui_text(double size, NSColor *color) {
  return @{NSFontAttributeName : vs_ui_font(size), NSForegroundColorAttributeName : color};
}

static inline void vs_ui_draw_line(NSString *text, NSRect rect, NSFont *font, NSColor *color,
                                   NSTextAlignment alignment) {
  if (!font || !color) {
    return;
  }
  NSMutableParagraphStyle *style = [[NSMutableParagraphStyle alloc] init];
  style.lineBreakMode = NSLineBreakByTruncatingTail;
  style.alignment = alignment;
  CGFloat height = std::ceil(font.ascender - font.descender + font.leading);
  NSRect line = NSMakeRect(rect.origin.x, rect.origin.y + (rect.size.height - height) / 2,
                           rect.size.width, height);
  [text drawWithRect:line
             options:NSStringDrawingUsesLineFragmentOrigin | NSStringDrawingTruncatesLastVisibleLine
          attributes:@{
            NSFontAttributeName : font,
            NSForegroundColorAttributeName : color,
            NSParagraphStyleAttributeName : style
          }];
}

// Includes the 2 pt line-fragment padding NSTextField labels apply on each side.
static inline CGFloat vs_ui_label_width(NSString *text, vs_ui_size size) {
  NSFont *font = vs_ui_font(vs_ui_metrics_for(size).label_font_size);
  return std::ceil([text sizeWithAttributes:@{NSFontAttributeName : font}].width) + 4;
}

static inline NSTextField *vs_ui_label(NSString *text, vs_ui_size size) {
  NSTextField *label = [NSTextField labelWithString:text];
  label.font = vs_ui_font(vs_ui_metrics_for(size).label_font_size);
  label.textColor = vs_ui_color(VS_UI_TEXT_LABEL);
  label.lineBreakMode = NSLineBreakByTruncatingTail;
  return label;
}

static inline void vs_ui_style_value_field(NSTextField *field, vs_ui_size size) {
  field.font = vs_ui_value_font(vs_ui_metrics_for(size).value_font_size);
  field.textColor = vs_ui_color(VS_UI_TEXT_PRIMARY);
  field.alignment = NSTextAlignmentCenter;
  field.bordered = NO;
  field.drawsBackground = NO;
  field.focusRingType = NSFocusRingTypeNone;
}

static inline void vs_ui_draw_focus(NSRect rect, CGFloat radius) {
  NSBezierPath *ring = [NSBezierPath
      bezierPathWithRoundedRect:NSInsetRect(rect, VS_UI_FOCUS_WIDTH / 2, VS_UI_FOCUS_WIDTH / 2)
                        xRadius:radius
                        yRadius:radius];
  ring.lineWidth = VS_UI_FOCUS_WIDTH;
  [vs_ui_color(VS_UI_FOCUS) setStroke];
  [ring stroke];
}

static inline void vs_ui_draw_check(NSRect box, NSColor *color) {
  CGFloat unit = box.size.width / 20;
  NSBezierPath *check = [NSBezierPath bezierPath];
  [check moveToPoint:NSMakePoint(NSMinX(box) + 5.5 * unit, NSMinY(box) + 10.5 * unit)];
  [check lineToPoint:NSMakePoint(NSMinX(box) + 8.5 * unit, NSMinY(box) + 13.5 * unit)];
  [check lineToPoint:NSMakePoint(NSMinX(box) + 14.5 * unit, NSMinY(box) + 6.5 * unit)];
  check.lineWidth = std::max<CGFloat>(1.5, 2 * unit);
  check.lineCapStyle = NSLineCapStyleRound;
  check.lineJoinStyle = NSLineJoinStyleRound;
  [color setStroke];
  [check stroke];
}

static inline BOOL vs_ui_has_focus(NSView *view) {
  return view.window.isKeyWindow && view.window.firstResponder == view;
}

@interface VSStyledSwitch : NSButton
@property BOOL phaseStyle;
@end

@implementation VSStyledSwitch
- (BOOL)isFlipped {
  return YES;
}
- (void)drawRect:(NSRect)dirtyRect {
  BOOL on = self.state == NSControlStateValueOn;
  vs_ui_metrics compact = vs_ui_metrics_for(VS_UI_SIZE_COMPACT);
  if (!self.enabled) {
    CGContextSetAlpha(NSGraphicsContext.currentContext.CGContext, VS_UI_OPACITY_DISABLED);
  }
  if (self.phaseStyle) {
    NSBezierPath *box =
        [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(self.bounds, 1, 1)
                                        xRadius:VS_UI_METRICS_STANDARD.corner_radius
                                        yRadius:VS_UI_METRICS_STANDARD.corner_radius];
    [vs_ui_color(on ? VS_UI_FILL_STRONG : VS_UI_SURFACE_INSET) setFill];
    [box fill];
    vs_ui_draw_line(self.title, self.bounds, vs_ui_font(compact.label_font_size + 1),
                    vs_ui_color(on ? VS_UI_TEXT_ON_FILL : VS_UI_TEXT_LABEL), NSTextAlignmentCenter);
  } else {
    NSColor *ink = vs_ui_color(on ? VS_UI_FILL_STRONG : VS_UI_TEXT_SECONDARY);
    CGFloat y = (self.bounds.size.height - 12) / 2;
    NSRect track = NSMakeRect(self.bounds.size.width - 23, y, 22, 12);
    NSBezierPath *capsule = [NSBezierPath bezierPathWithRoundedRect:track
                                                            xRadius:track.size.height / 2
                                                            yRadius:track.size.height / 2];
    [ink setStroke];
    [capsule stroke];
    if (on) {
      [ink setFill];
      [capsule fill];
    }
    [vs_ui_color(on ? VS_UI_TEXT_ON_FILL : VS_UI_TEXT_LABEL) setFill];
    [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(track.origin.x + (on ? 12 : 2), y + 2, 8, 8)]
        fill];
    vs_ui_draw_line(self.title, NSMakeRect(0, 0, NSMinX(track) - 4, self.bounds.size.height),
                    vs_ui_font(compact.label_font_size), ink, NSTextAlignmentLeft);
  }
  if (vs_ui_has_focus(self)) {
    vs_ui_draw_focus(self.bounds, VS_UI_METRICS_COMPACT.corner_radius);
  }
}
- (BOOL)becomeFirstResponder {
  self.needsDisplay = YES;
  return [super becomeFirstResponder];
}
- (BOOL)resignFirstResponder {
  self.needsDisplay = YES;
  return [super resignFirstResponder];
}
@end

@interface VSStyledCheckbox : NSButton
@property vs_ui_size uiSize;
@end

@implementation VSStyledCheckbox
- (BOOL)isFlipped {
  return YES;
}
- (void)drawRect:(NSRect)dirtyRect {
  vs_ui_metrics metrics = vs_ui_metrics_for(self.uiSize);
  BOOL on = self.state == NSControlStateValueOn;
  if (!self.enabled) {
    CGContextSetAlpha(NSGraphicsContext.currentContext.CGContext, VS_UI_OPACITY_DISABLED);
  }
  CGFloat side = metrics.checkbox_size;
  NSRect box = NSMakeRect(0, (self.bounds.size.height - side) / 2, side, side);
  NSBezierPath *shape = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(box, .5, .5)
                                                        xRadius:metrics.checkbox_radius
                                                        yRadius:metrics.checkbox_radius];
  if (on) {
    [vs_ui_color(VS_UI_FILL_STRONG) setFill];
    [shape fill];
    vs_ui_draw_check(box, vs_ui_color(VS_UI_TEXT_ON_FILL));
  } else {
    [vs_ui_color(VS_UI_SURFACE_INSET_HOVER) setFill];
    [shape fill];
    [vs_ui_color(VS_UI_FILL_OFF_BORDER) setStroke];
    [shape stroke];
  }
  if (vs_ui_has_focus(self)) {
    vs_ui_draw_focus(NSInsetRect(box, -2, -2), metrics.checkbox_radius + 2);
  }
  if (self.title.length > 0) {
    CGFloat x = side + metrics.label_gap;
    vs_ui_draw_line(
        self.title, NSMakeRect(x, 0, self.bounds.size.width - x, self.bounds.size.height),
        vs_ui_font(metrics.font_size), vs_ui_color(VS_UI_TEXT_LABEL), NSTextAlignmentLeft);
  }
}
- (BOOL)becomeFirstResponder {
  self.needsDisplay = YES;
  return [super becomeFirstResponder];
}
- (BOOL)resignFirstResponder {
  self.needsDisplay = YES;
  return [super resignFirstResponder];
}
@end

@class VSStyledDropdown;
@interface VSStyledMenu : NSView
@property(weak) VSStyledDropdown *owner;
@property NSRect menuRect;
@property NSInteger highlighted;
@property(strong) id outsideMonitor;
- (void)dismiss;
@end

@interface VSStyledDropdown : NSPopUpButton
@property(weak) VSStyledMenu *expandedMenu;
@property vs_ui_size uiSize;
@property BOOL hovered;
- (NSRect)titleRect;
@end

@implementation VSStyledDropdown
- (BOOL)isFlipped {
  return YES;
}
- (NSRect)titleRect {
  vs_ui_metrics metrics = vs_ui_metrics_for(self.uiSize);
  CGFloat x = metrics.horizontal_padding;
  CGFloat width = self.bounds.size.width - x - vs_ui_dropdown_accessory_width(self.uiSize);
  return NSMakeRect(x, 0, std::max<CGFloat>(0, width), self.bounds.size.height);
}
- (void)updateTrackingAreas {
  for (NSTrackingArea *area in self.trackingAreas) {
    [self removeTrackingArea:area];
  }
  [self addTrackingArea:[[NSTrackingArea alloc]
                            initWithRect:NSZeroRect
                                 options:NSTrackingMouseEnteredAndExited |
                                         NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
                                   owner:self
                                userInfo:nil]];
  [super updateTrackingAreas];
}
- (void)mouseEntered:(NSEvent *)event {
  self.hovered = YES;
  self.needsDisplay = YES;
}
- (void)mouseExited:(NSEvent *)event {
  self.hovered = NO;
  self.needsDisplay = YES;
}
- (BOOL)becomeFirstResponder {
  self.needsDisplay = YES;
  return [super becomeFirstResponder];
}
- (BOOL)resignFirstResponder {
  self.needsDisplay = YES;
  return [super resignFirstResponder];
}
- (void)drawRect:(NSRect)dirtyRect {
  vs_ui_metrics metrics = vs_ui_metrics_for(self.uiSize);
  if (!self.enabled) {
    CGContextSetAlpha(NSGraphicsContext.currentContext.CGContext, VS_UI_OPACITY_DISABLED);
  }
  BOOL active = self.enabled && (self.hovered || self.expandedMenu);
  NSBezierPath *box = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(self.bounds, .5, .5)
                                                      xRadius:metrics.corner_radius
                                                      yRadius:metrics.corner_radius];
  [vs_ui_color(active ? VS_UI_SURFACE_INSET_HOVER : VS_UI_SURFACE_INSET) setFill];
  [box fill];
  if (active) {
    [vs_ui_color(VS_UI_BORDER_HOVER) setStroke];
    box.lineWidth = VS_UI_BORDER_WIDTH;
    [box stroke];
  }
  vs_ui_draw_line(self.titleOfSelectedItem ? self.titleOfSelectedItem : @"", [self titleRect],
                  vs_ui_font(metrics.font_size), vs_ui_color(VS_UI_TEXT_PRIMARY),
                  NSTextAlignmentLeft);
  CGFloat x = self.bounds.size.width - metrics.horizontal_padding - 5,
          y = self.bounds.size.height / 2;
  CGFloat direction = self.expandedMenu ? -1 : 1;
  NSBezierPath *chevron = [NSBezierPath bezierPath];
  [chevron moveToPoint:NSMakePoint(x - 4, y - 2 * direction)];
  [chevron lineToPoint:NSMakePoint(x, y + 2 * direction)];
  [chevron lineToPoint:NSMakePoint(x + 4, y - 2 * direction)];
  [vs_ui_color(VS_UI_TEXT_SECONDARY) setStroke];
  chevron.lineWidth = 1.5;
  [chevron stroke];
  if (vs_ui_has_focus(self)) {
    vs_ui_draw_focus(self.bounds, metrics.corner_radius);
  }
}
- (void)mouseDown:(NSEvent *)event {
  [self performClick:self];
}
- (void)performClick:(id)sender {
  if (!self.enabled) {
    return;
  }
  if (self.expandedMenu) {
    [self.expandedMenu dismiss];
    return;
  }
  vs_ui_metrics metrics = vs_ui_metrics_for(self.uiSize);
  NSView *root = self.superview;
  VSStyledMenu *menu = [[VSStyledMenu alloc] initWithFrame:root.bounds];
  [menu setBoundsOrigin:root.bounds.origin];
  menu.owner = self;
  menu.highlighted = self.indexOfSelectedItem;
  CGFloat height =
      static_cast<CGFloat>(self.numberOfItems) * metrics.menu_row_height + 2 * VS_UI_MENU_INSET;
  CGFloat y = NSMaxY(self.frame) + VS_UI_MENU_OFFSET;
  if (y + height > NSMaxY(root.bounds)) {
    y = std::max(NSMinY(root.bounds) + VS_UI_MENU_OFFSET,
                 NSMinY(self.frame) - height - VS_UI_MENU_OFFSET);
  }
  menu.menuRect = NSMakeRect(NSMinX(self.frame), y, self.frame.size.width, height);
  self.expandedMenu = menu;
  self.needsDisplay = YES;
  [root addSubview:menu positioned:NSWindowAbove relativeTo:nil];
  [self.window makeFirstResponder:menu];
  __weak VSStyledMenu *weakMenu = menu;
  menu.outsideMonitor = [NSEvent
      addLocalMonitorForEventsMatchingMask:NSEventMaskLeftMouseDown
                                   handler:^NSEvent *(NSEvent *event) {
                                     VSStyledMenu *strongMenu = weakMenu;
                                     if (strongMenu &&
                                         (event.window != strongMenu.window ||
                                          !NSPointInRect([strongMenu
                                                             convertPoint:event.locationInWindow
                                                                 fromView:nil],
                                                         strongMenu.menuRect))) {
                                       [strongMenu dismiss];
                                     }
                                     return event;
                                   }];
}
- (BOOL)accessibilityPerformPress {
  [self performClick:self];
  return YES;
}
- (void)keyDown:(NSEvent *)event {
  if (event.keyCode == 49 || event.keyCode == 36 || event.keyCode == 125) {
    [self performClick:self];
  } else {
    [super keyDown:event];
  }
}
@end

@implementation VSStyledMenu
- (BOOL)isFlipped {
  return YES;
}
- (BOOL)acceptsFirstResponder {
  return YES;
}
- (CGFloat)rowHeight {
  return vs_ui_metrics_for(self.owner.uiSize).menu_row_height;
}
- (NSInteger)rowAtPoint:(NSPoint)point {
  CGFloat offset = point.y - self.menuRect.origin.y - VS_UI_MENU_INSET;
  return offset < 0 ? -1 : static_cast<NSInteger>(offset / [self rowHeight]);
}
- (void)updateTrackingAreas {
  for (NSTrackingArea *area in self.trackingAreas) {
    [self removeTrackingArea:area];
  }
  [self addTrackingArea:[[NSTrackingArea alloc]
                            initWithRect:self.menuRect
                                 options:NSTrackingMouseMoved | NSTrackingActiveInKeyWindow
                                   owner:self
                                userInfo:nil]];
  [super updateTrackingAreas];
}
- (void)mouseMoved:(NSEvent *)event {
  NSInteger index = [self rowAtPoint:[self convertPoint:event.locationInWindow fromView:nil]];
  if (index >= 0 && index < self.owner.numberOfItems && [self.owner itemAtIndex:index].enabled) {
    self.highlighted = index;
    self.needsDisplay = YES;
  }
}
- (void)dismiss {
  if (self.outsideMonitor) {
    [NSEvent removeMonitor:self.outsideMonitor];
  }
  self.outsideMonitor = nil;
  VSStyledDropdown *owner = self.owner;
  [self.window makeFirstResponder:owner];
  owner.expandedMenu = nil;
  owner.needsDisplay = YES;
  [self removeFromSuperview];
}
- (void)viewWillMoveToWindow:(NSWindow *)window {
  if (!window && self.outsideMonitor) {
    [NSEvent removeMonitor:self.outsideMonitor];
    self.outsideMonitor = nil;
  }
  [super viewWillMoveToWindow:window];
}
- (void)choose:(NSInteger)index {
  VSStyledDropdown *owner = self.owner;
  if (index >= 0 && index < owner.numberOfItems && [owner itemAtIndex:index].enabled) {
    [owner selectItemAtIndex:index];
    [owner sendAction:owner.action to:owner.target];
    owner.needsDisplay = YES;
    [self dismiss];
  }
}
- (void)mouseDown:(NSEvent *)event {
  NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
  if (!NSPointInRect(point, self.menuRect)) {
    [self dismiss];
    return;
  }
  [self choose:[self rowAtPoint:point]];
}
- (void)keyDown:(NSEvent *)event {
  switch (event.keyCode) {
  case 53:
  case 48:
    [self dismiss];
    break;
  case 36:
  case 49:
    [self choose:self.highlighted];
    break;
  case 125:
  case 126: {
    NSInteger next = self.highlighted + (event.keyCode == 125 ? 1 : -1);
    while (next >= 0 && next < self.owner.numberOfItems) {
      if ([self.owner itemAtIndex:next].enabled) {
        self.highlighted = next;
        break;
      }
      next += event.keyCode == 125 ? 1 : -1;
    }
    self.needsDisplay = YES;
    break;
  }
  default:
    [super keyDown:event];
  }
}
- (void)drawRect:(NSRect)dirtyRect {
  vs_ui_metrics metrics = vs_ui_metrics_for(self.owner.uiSize);
  CGFloat radius = metrics.corner_radius + 1;
  NSBezierPath *box = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(self.menuRect, .5, .5)
                                                      xRadius:radius
                                                      yRadius:radius];
  [vs_ui_color(VS_UI_SURFACE_INSET) setFill];
  [box fill];
  [vs_ui_color(VS_UI_BORDER) setStroke];
  box.lineWidth = VS_UI_BORDER_WIDTH;
  [box stroke];
  CGFloat rowHeight = [self rowHeight];
  NSFont *font = vs_ui_font(metrics.font_size);
  for (NSInteger i = 0; i < self.owner.numberOfItems; ++i) {
    NSRect row =
        NSMakeRect(self.menuRect.origin.x + VS_UI_MENU_INSET,
                   self.menuRect.origin.y + VS_UI_MENU_INSET + static_cast<CGFloat>(i) * rowHeight,
                   self.menuRect.size.width - 2 * VS_UI_MENU_INSET, rowHeight);
    NSMenuItem *item = [self.owner itemAtIndex:i];
    BOOL selected = i == self.owner.indexOfSelectedItem;
    if (i == self.highlighted && item.enabled) {
      [vs_ui_color(VS_UI_SURFACE_HIGHLIGHT) setFill];
      [[NSBezierPath bezierPathWithRoundedRect:row
                                       xRadius:metrics.corner_radius - 2
                                       yRadius:metrics.corner_radius - 2] fill];
    }
    NSColor *ink = vs_ui_color(selected ? VS_UI_TEXT_PRIMARY : VS_UI_TEXT_LABEL);
    if (!item.enabled) {
      ink = [ink colorWithAlphaComponent:VS_UI_OPACITY_DISABLED];
    }
    CGFloat accessory = vs_ui_dropdown_accessory_width(self.owner.uiSize);
    vs_ui_draw_line(
        item.title,
        NSMakeRect(NSMinX(row) + metrics.horizontal_padding - VS_UI_MENU_INSET, NSMinY(row),
                   row.size.width - metrics.horizontal_padding + VS_UI_MENU_INSET - accessory,
                   rowHeight),
        font, ink, NSTextAlignmentLeft);
    if (selected) {
      CGFloat side = metrics.checkbox_size;
      vs_ui_draw_check(NSMakeRect(NSMaxX(row) - accessory + (accessory - side) / 2,
                                  NSMidY(row) - side / 2, side, side),
                       vs_ui_color(VS_UI_FILL_STRONG));
    }
  }
}
@end
