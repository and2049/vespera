#pragma once
// AppKit renderer for declarative module layouts (layout schema versions 1 and 2). The host
// compiles it to draw packages that declare a manifest `ui` layout;
// vespera/sdk/artwork_view_macos.h embeds the same renderer in a module's own UI binary. Define
// VS_LAYOUT_VIEW as an Objective-C class name unique across every loaded package before including:
// loaded packages share one runtime.
#import <AppKit/AppKit.h>
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <vector>
#include <vespera/module_ui.h>
#include <vespera/parameter_display.hpp>
#ifndef VS_UI_PREFIX
#define VS_UI_PREFIX VS_LAYOUT_VIEW
#endif
#include <vespera/module_controls_macos.h>
#define VSArtworkControl VS_UI_JOIN(VS_LAYOUT_VIEW, Control)
#define VSArtworkField VS_UI_JOIN(VS_LAYOUT_VIEW, Field)
#define VSArtworkFile VS_UI_JOIN(VS_LAYOUT_VIEW, File)
#ifndef VS_LAYOUT_FILE_WIDGETS
#define VS_LAYOUT_FILE_WIDGETS 0
#endif

#define VS_LAYOUT_VERSION 2

namespace {
constexpr NSUInteger vs_layout_max_widgets = 512, vs_layout_max_images = 64,
                     vs_layout_max_list = 256, vs_layout_max_assets = 256;
constexpr double vs_layout_max_coordinate = 100000, vs_layout_max_image_side = 65536,
                 vs_layout_max_image_pixels = 64.0 * 1024 * 1024;
constexpr unsigned long long vs_layout_max_layout_bytes = 4ull << 20,
                             vs_layout_max_asset_bytes = 64ull << 20;

BOOL vs_layout_fail(NSString **error, NSString *message) {
  if (error) {
    *error = message;
  }
  return NO;
}

BOOL vs_layout_is_number(id value) {
  return [value isKindOfClass:NSNumber.class] && std::isfinite([value doubleValue]);
}

BOOL vs_layout_numbers(id value, NSUInteger count) {
  if (![value isKindOfClass:NSArray.class] || [value count] != count) {
    return NO;
  }
  for (id item in value) {
    if (!vs_layout_is_number(item) || std::abs([item doubleValue]) > vs_layout_max_coordinate) {
      return NO;
    }
  }
  return YES;
}

BOOL vs_layout_rect(id value) {
  return vs_layout_numbers(value, 4) && [value[2] doubleValue] > 0 && [value[3] doubleValue] > 0;
}

// Resolves a resource name inside directory, following symbolic links, so that no name can reach
// a file outside the package's resources.
NSString *vs_layout_resolve(NSString *directory, id name, unsigned long long limit,
                            NSString **error) {
  if (![name isKindOfClass:NSString.class] || ![name length] || [name hasPrefix:@"/"] ||
      [[name pathComponents] containsObject:@".."]) {
    vs_layout_fail(error, [NSString stringWithFormat:@"asset %@ must be a relative path inside "
                                                     @"the package resources",
                                                     name]);
    return nil;
  }
  char root[PATH_MAX], path[PATH_MAX];
  if (!realpath(directory.fileSystemRepresentation, root)) {
    vs_layout_fail(error, [NSString stringWithFormat:@"missing resources directory %@", directory]);
    return nil;
  }
  NSString *candidate = [directory stringByAppendingPathComponent:name];
  if (!realpath(candidate.fileSystemRepresentation, path)) {
    vs_layout_fail(error, [NSString stringWithFormat:@"missing asset %@", name]);
    return nil;
  }
  NSString *rootPath = [NSString stringWithUTF8String:root];
  NSString *resolved = [NSString stringWithUTF8String:path];
  if (![resolved hasPrefix:[rootPath stringByAppendingString:@"/"]]) {
    vs_layout_fail(error,
                   [NSString stringWithFormat:@"asset %@ escapes the package resources", name]);
    return nil;
  }
  NSDictionary *attributes = [NSFileManager.defaultManager attributesOfItemAtPath:resolved
                                                                            error:nil];
  if (![attributes.fileType isEqualToString:NSFileTypeRegular]) {
    vs_layout_fail(error, [NSString stringWithFormat:@"asset %@ is not a regular file", name]);
    return nil;
  }
  if (attributes.fileSize > limit) {
    vs_layout_fail(error, [NSString stringWithFormat:@"asset %@ exceeds %llu bytes", name, limit]);
    return nil;
  }
  return resolved;
}

// With preferRaster, an SVG name resolves to a same-name PNG beside it when one exists, so an
// editable vector source can ship next to the raster export that is actually drawn.
NSImage *vs_layout_image(NSString *directory, NSString *name, BOOL preferRaster, NSString **error) {
  if (preferRaster && [name isKindOfClass:NSString.class] &&
      [name.pathExtension.lowercaseString isEqualToString:@"svg"]) {
    NSString *raster = [name.stringByDeletingPathExtension stringByAppendingPathExtension:@"png"];
    struct stat info;
    if (lstat([directory stringByAppendingPathComponent:raster].fileSystemRepresentation, &info) ==
        0) {
      name = raster;
    }
  }
  NSString *path = vs_layout_resolve(directory, name, vs_layout_max_asset_bytes, error);
  if (!path) {
    return nil;
  }
  NSImage *image = [[NSImage alloc] initWithContentsOfFile:path];
  NSSize size = image.size;
  BOOL valid = image.isValid && std::isfinite(size.width) && std::isfinite(size.height) &&
               size.width > 0 && size.height > 0 && size.width <= vs_layout_max_image_side &&
               size.height <= vs_layout_max_image_side;
  for (NSImageRep *rep in image.representations) {
    double wide = static_cast<double>(std::max<NSInteger>(rep.pixelsWide, 0)),
           high = static_cast<double>(std::max<NSInteger>(rep.pixelsHigh, 0));
    if (wide > vs_layout_max_image_side || high > vs_layout_max_image_side ||
        wide * high > vs_layout_max_image_pixels) {
      valid = NO;
    }
  }
  if (!valid) {
    vs_layout_fail(error, [NSString stringWithFormat:@"asset %@ is not a supported image within "
                                                     @"%.0f pixels per side",
                                                     name, vs_layout_max_image_side]);
    return nil;
  }
  return image;
}

NSSet<NSString *> *vs_layout_anchors() {
  return [NSSet setWithArray:@[ @"center", @"top", @"bottom" ]];
}

NSDictionary<NSString *, NSNumber *> *vs_layout_interpolations() {
  return @{
    @"default" : @(NSImageInterpolationDefault),
    @"none" : @(NSImageInterpolationNone),
    @"low" : @(NSImageInterpolationLow),
    @"medium" : @(NSImageInterpolationMedium),
    @"high" : @(NSImageInterpolationHigh)
  };
}

NSImageInterpolation vs_layout_interpolation(id name) {
  NSNumber *value = [name isKindOfClass:NSString.class] ? vs_layout_interpolations()[name] : nil;
  return value ? static_cast<NSImageInterpolation>(value.unsignedIntegerValue)
               : NSImageInterpolationDefault;
}

NSSet<NSString *> *vs_layout_fonts() {
  return [NSSet setWithArray:@[ @"system", @"monospaced", @"monospacedDigit" ]];
}

NSDictionary<NSString *, NSNumber *> *vs_layout_alignments() {
  return @{
    @"left" : @(NSTextAlignmentLeft),
    @"center" : @(NSTextAlignmentCenter),
    @"right" : @(NSTextAlignmentRight)
  };
}

NSSet<NSString *> *vs_layout_types() {
  return [NSSet setWithArray:@[
#if VS_LAYOUT_FILE_WIDGETS
    @"file",
#endif
    @"rotaryFilmStripKnob", @"imageSlider", @"imageButton", @"threeWayImageSwitch", @"textButton",
    @"textEditor", @"image", @"meter"
  ]];
}

// Widget fields introduced by layout version 2. Version 1 renderers ignore unknown fields, so a
// version 1 layout using one of these would draw or respond differently there.
NSArray<NSString *> *vs_layout_version2_fields() {
  return @[
    @"dragTravel", @"fineStep", @"handleTravel", @"handleAnchor", @"imageFollows", @"format",
    @"editing", @"dragAxis"
  ];
}

BOOL vs_layout_button(NSString *type) {
  return [type isEqualToString:@"imageButton"] || [type isEqualToString:@"threeWayImageSwitch"] ||
         [type isEqualToString:@"textButton"];
}

BOOL vs_layout_continuous(NSString *type) {
  return [type isEqualToString:@"rotaryFilmStripKnob"] || [type isEqualToString:@"imageSlider"] ||
         [type isEqualToString:@"textEditor"];
}

BOOL vs_layout_parameter(id value) {
  return vs_layout_is_number(value) && [value doubleValue] >= 0 &&
         [value doubleValue] <= UINT32_MAX &&
         [value doubleValue] == std::floor([value doubleValue]);
}

// Returns why a value format is invalid, or nil when it is valid.
NSString *vs_layout_format_error(id format) {
  if (![format isKindOfClass:NSDictionary.class]) {
    return @"format must be an object";
  }
  id range = format[@"range"];
  if (range && (!vs_layout_numbers(range, 2) || [range[0] doubleValue] == [range[1] doubleValue])) {
    return @"format range must be two different numbers";
  }
  id skew = format[@"skew"];
  if (skew &&
      (!vs_layout_is_number(skew) || !([skew doubleValue] > 0) || [skew doubleValue] > 1000)) {
    return @"format skew must be above 0 and at most 1000";
  }
  id decimals = format[@"decimals"];
  if (decimals &&
      (!vs_layout_is_number(decimals) || [decimals doubleValue] < 0 || [decimals doubleValue] > 6 ||
       [decimals doubleValue] != std::floor([decimals doubleValue]))) {
    return @"format decimals must be a whole number from 0 to 6";
  }
  for (NSString *key in @[ @"unit", @"kiloUnit" ]) {
    if (format[key] && (![format[key] isKindOfClass:NSString.class] || [format[key] length] > 64)) {
      return
          [NSString stringWithFormat:@"format %@ must be a string of at most 64 characters", key];
    }
  }
  if (format[@"signed"] && !vs_layout_is_number(format[@"signed"])) {
    return @"format signed must be a boolean";
  }
  id off = format[@"off"];
  if (off && ![off isEqual:@"minimum"] && ![off isEqual:@"maximum"]) {
    return @"format off must be \"minimum\" or \"maximum\"";
  }
  id tolerance = format[@"offTolerance"];
  if (tolerance && (!off || !vs_layout_is_number(tolerance) || [tolerance doubleValue] < 0)) {
    return @"format offTolerance must be a non-negative number and needs off";
  }
  return nil;
}

// Validates one "file" widget, the only widget type without a parameter.
NSDictionary *vs_layout_file_widget(id widget, NSString *where, NSInteger version,
                                    NSString **error) {
  auto fail = [&](NSString *message) {
    vs_layout_fail(error, [NSString stringWithFormat:@"%@: %@", where, message]);
    return (NSDictionary *)nil;
  };
  if (version < 2 || !vs_layout_parameter(widget[@"slot"]) ||
      [widget[@"slot"] unsignedLongLongValue] == 0 || widget[@"id"]) {
    return fail(@"file requires version 2 and a nonzero slot, not a parameter id");
  }
  if (widget[@"placeholder"] && ![widget[@"placeholder"] isKindOfClass:NSString.class]) {
    return fail(@"file placeholder must be a string");
  }
  NSSet *fields =
      [NSSet setWithArray:@[ @"type", @"slot", @"rect", @"name", @"help", @"placeholder" ]];
  for (NSString *key in widget) {
    if (![fields containsObject:key]) {
      return fail([NSString stringWithFormat:@"file does not support %@", key]);
    }
  }
  for (NSString *key in @[ @"name", @"help" ]) {
    if (widget[key] && ![widget[key] isKindOfClass:NSString.class]) {
      return fail(@"file name and help must be strings");
    }
  }
  return widget;
}

// Validates the drag geometry only an imageSlider may declare.
BOOL vs_layout_slider_drag(id widget, NSString *type, NSString *where, NSString **error) {
  auto fail = [&](NSString *message) {
    vs_layout_fail(error, [NSString stringWithFormat:@"%@: %@", where, message]);
    return NO;
  };
  id axis = widget[@"dragAxis"];
  if (axis) {
    if (![type isEqualToString:@"imageSlider"]) {
      return fail(@"dragAxis applies only to imageSlider");
    }
    if (![axis isEqual:@"vertical"] && ![axis isEqual:@"travel"]) {
      return fail(@"dragAxis must be \"vertical\" or \"travel\"");
    }
  }
  BOOL alongTravel = [axis isEqual:@"travel"];
  id travel = widget[@"handleTravel"];
  if (alongTravel && !travel) {
    return fail(@"dragAxis \"travel\" needs handleTravel");
  }
  if (travel) {
    if (![type isEqualToString:@"imageSlider"]) {
      return fail(@"handleTravel applies only to imageSlider");
    }
    if (!vs_layout_numbers(travel, 4)) {
      return fail(@"handleTravel must be [minimumX, minimumY, maximumX, maximumY]");
    }
    if (alongTravel && !(std::hypot([travel[2] doubleValue] - [travel[0] doubleValue],
                                    [travel[3] doubleValue] - [travel[1] doubleValue]) >= 1)) {
      return fail(@"handleTravel points must differ by at least 1 layout unit");
    }
    if (!alongTravel && !([travel[3] doubleValue] < [travel[1] doubleValue])) {
      return fail(@"handleTravel must have the maximum above the minimum unless dragAxis is "
                  @"\"travel\"");
    }
  }
  if (widget[@"handleAnchor"] && (!travel || !vs_layout_numbers(widget[@"handleAnchor"], 2))) {
    return fail(@"handleAnchor must be [x, y] and needs handleTravel");
  }
  return YES;
}

// Validates a textEditor's editing mode and reports whether it edits on click.
BOOL vs_layout_editor_editing(id widget, NSString *type, NSString *where, BOOL *field,
                              NSString **error) {
  auto fail = [&](NSString *message) {
    vs_layout_fail(error, [NSString stringWithFormat:@"%@: %@", where, message]);
    return NO;
  };
  id editing = widget[@"editing"];
  *field = [editing isEqual:@"click"];
  if (editing) {
    if (![type isEqualToString:@"textEditor"]) {
      return fail(@"editing applies only to textEditor");
    }
    if (!*field && ![editing isEqual:@"doubleClick"]) {
      return fail(@"editing must be \"doubleClick\" or \"click\"");
    }
    if (*field && [widget[@"choices"] isKindOfClass:NSArray.class] && [widget[@"choices"] count]) {
      return fail(@"editing \"click\" cannot be combined with choices");
    }
  }
  return YES;
}

// Validates a widget's image names, loading each distinct image once.
BOOL vs_layout_widget_images(id names, NSMutableDictionary *result, NSString *where,
                             NSString *directory, NSInteger version,
                             NSMutableDictionary<NSString *, NSImage *> *images, NSString **error) {
  auto fail = [&](NSString *message) {
    vs_layout_fail(error, [NSString stringWithFormat:@"%@: %@", where, message]);
    return NO;
  };
  if (![names isKindOfClass:NSArray.class] || [names count] > vs_layout_max_images) {
    return fail([NSString stringWithFormat:@"images must be an array of at most %lu names",
                                           static_cast<unsigned long>(vs_layout_max_images)]);
  }
  for (id name in names) {
    if (![name isKindOfClass:NSString.class]) {
      return fail(@"images must contain strings");
    }
    if ([name length] && !images[name]) {
      if (images.count >= vs_layout_max_assets) {
        return fail(@"too many distinct images");
      }
      NSString *reason = nil;
      NSImage *image = vs_layout_image(directory, name, version >= 2, &reason);
      if (!image) {
        return fail(reason);
      }
      images[name] = image;
    }
  }
  result[@"images"] = names;
  return YES;
}

// Validates a rotary film strip's frame count, defaulting it from the strip's aspect.
BOOL vs_layout_rotary_frames(NSMutableDictionary *result, NSArray *names,
                             NSMutableDictionary<NSString *, NSImage *> *images, NSString *where,
                             NSString **error) {
  auto fail = [&](NSString *message) {
    vs_layout_fail(error, [NSString stringWithFormat:@"%@: %@", where, message]);
    return NO;
  };
  NSSize strip = images[names[0]].size;
  if (!result[@"frames"]) {
    result[@"frames"] = @(std::max(1L, lround(strip.height / strip.width)));
  }
  NSInteger frames = [result[@"frames"] integerValue];
  if (frames < 1 || frames > 4096 ||
      [result[@"frames"] doubleValue] != static_cast<double>(frames)) {
    return fail(@"frames must be a whole number from 1 to 4096");
  }
  return YES;
}

// Checks one widget against the schema and returns a copy with defaults filled in.
NSDictionary *vs_layout_widget(id widget, NSUInteger index, NSString *directory, NSInteger version,
                               NSMutableDictionary<NSString *, NSImage *> *images,
                               NSString **error) {
  NSString *where = [NSString stringWithFormat:@"widget %lu", static_cast<unsigned long>(index)];
  if (![widget isKindOfClass:NSDictionary.class]) {
    vs_layout_fail(error, [where stringByAppendingString:@" must be an object"]);
    return nil;
  }
  if ([widget[@"name"] isKindOfClass:NSString.class]) {
    where = [where stringByAppendingFormat:@" (%@)", widget[@"name"]];
  }
  auto fail = [&](NSString *message) {
    vs_layout_fail(error, [NSString stringWithFormat:@"%@: %@", where, message]);
    return (NSDictionary *)nil;
  };
  NSString *type = widget[@"type"];
  if (![type isKindOfClass:NSString.class]) {
    return fail(@"type must be a string");
  }
  if (![vs_layout_types() containsObject:type]) {
    return fail([NSString stringWithFormat:@"unknown type \"%@\" for layout version %ld", type,
                                           static_cast<long>(version)]);
  }
  if (version < 2) {
    for (NSString *key in vs_layout_version2_fields()) {
      if (widget[key]) {
        return fail([NSString stringWithFormat:@"%@ requires layout version 2", key]);
      }
    }
  }
  if (!vs_layout_rect(widget[@"rect"])) {
    return fail(@"rect must be [x, y, width, height] with a positive size");
  }
  if ([type isEqualToString:@"file"]) {
    return vs_layout_file_widget(widget, where, version, error);
  }
  if (!vs_layout_parameter(widget[@"id"])) {
    return fail(@"id must be a parameter ID");
  }
  for (NSString *key in @[ @"name", @"label", @"unit", @"text", @"help", @"editing" ]) {
    if (widget[key] && ![widget[key] isKindOfClass:NSString.class]) {
      return fail([NSString stringWithFormat:@"%@ must be a string", key]);
    }
  }
  for (NSString *key in @[
         @"minimum", @"maximum", @"initial", @"constant", @"offValue", @"radio", @"frames",
         @"decimals", @"readonly", @"discrete", @"invert", @"tap", @"hostTempo", @"darkText",
         @"step", @"momentary", @"dragTravel", @"fineStep"
       ]) {
    if (widget[key] && !vs_layout_is_number(widget[key])) {
      return fail([NSString stringWithFormat:@"%@ must be a number or boolean", key]);
    }
  }
  NSMutableDictionary *result = [widget mutableCopy];
  if (!result[@"minimum"]) {
    result[@"minimum"] = @0;
  }
  if (!result[@"maximum"]) {
    result[@"maximum"] = @1;
  }
  if ([result[@"minimum"] doubleValue] > [result[@"maximum"] doubleValue]) {
    return fail(@"minimum exceeds maximum");
  }
  if (result[@"decimals"] &&
      ([result[@"decimals"] intValue] < 0 || [result[@"decimals"] intValue] > 6)) {
    return fail(@"decimals must be 0 to 6");
  }
  if (widget[@"interpolation"] && !vs_layout_interpolations()[widget[@"interpolation"]]) {
    return fail(@"interpolation must be \"default\", \"none\", \"low\", \"medium\" or \"high\"");
  }
  if (widget[@"condition"] && !vs_layout_numbers(widget[@"condition"], 3)) {
    return fail(@"condition must be [parameter, minimum, maximum]");
  }
  if (widget[@"curve"] &&
      (!vs_layout_numbers(widget[@"curve"], 3) || !([widget[@"curve"][2] doubleValue] > 0))) {
    return fail(@"curve must be [minimum, maximum, positive skew]");
  }
  if ([type isEqualToString:@"imageSlider"] &&
      (!vs_layout_numbers(widget[@"handle"], 2) || !([widget[@"handle"][0] doubleValue] > 0) ||
       !([widget[@"handle"][1] doubleValue] > 0))) {
    return fail(@"handle must be [width, height] with a positive size");
  }
  if (widget[@"step"] && [widget[@"step"] doubleValue] == 0) {
    return fail(@"step must not be zero");
  }
  if (version >= 2 && vs_layout_continuous(type) && widget[@"step"] &&
      !([widget[@"step"] doubleValue] > 0)) {
    return fail(@"step must be positive for rotaryFilmStripKnob, imageSlider and textEditor");
  }
  if (widget[@"fineStep"] && !([widget[@"fineStep"] doubleValue] > 0)) {
    return fail(@"fineStep must be positive");
  }
  if (widget[@"dragTravel"] && !([widget[@"dragTravel"] doubleValue] > 0 &&
                                 [widget[@"dragTravel"] doubleValue] <= vs_layout_max_coordinate)) {
    return fail(@"dragTravel must be a positive number of layout units");
  }
  if (!vs_layout_slider_drag(widget, type, where, error)) {
    return nil;
  }
  id follows = widget[@"imageFollows"];
  if (follows) {
    if (![follows isKindOfClass:NSArray.class] || [follows count] != 3 ||
        !vs_layout_parameter(follows[0]) || !vs_layout_is_number(follows[1]) ||
        !vs_layout_is_number(follows[2]) ||
        !([follows[1] doubleValue] < [follows[2] doubleValue])) {
      return fail(@"imageFollows must be [parameter, minimum, maximum] with minimum below maximum");
    }
    if ([type isEqualToString:@"rotaryFilmStripKnob"]) {
      return fail(@"imageFollows does not apply to rotaryFilmStripKnob");
    }
  }
  if (widget[@"format"]) {
    NSString *reason = vs_layout_format_error(widget[@"format"]);
    if (reason) {
      return fail(reason);
    }
    for (NSString *key in @[ @"unit", @"curve", @"decimals", @"offValue" ]) {
      if (widget[key]) {
        return fail([NSString stringWithFormat:@"format replaces %@; use only one", key]);
      }
    }
  }
  BOOL field = NO;
  if (widget[@"editing"] && !vs_layout_editor_editing(widget, type, where, &field, error)) {
    return nil;
  }
  id sets = widget[@"sets"];
  if (sets) {
    if (![sets isKindOfClass:NSArray.class] || [sets count] > 16) {
      return fail(@"sets must be an array of at most 16 [parameter, value] pairs");
    }
    for (id pair in sets) {
      if (!vs_layout_numbers(pair, 2) || [pair[0] doubleValue] < 0 ||
          [pair[0] doubleValue] != std::floor([pair[0] doubleValue])) {
        return fail(@"sets must contain [parameter, value] pairs");
      }
    }
  }
  id style = widget[@"textStyle"];
  if (style) {
    id color = [style isKindOfClass:NSDictionary.class] ? style[@"color"] : nil;
    BOOL valid = [style isKindOfClass:NSDictionary.class] &&
                 (!style[@"font"] || [vs_layout_fonts() containsObject:style[@"font"]]) &&
                 (!style[@"align"] || vs_layout_alignments()[style[@"align"]] != nil) &&
                 (!style[@"size"] ||
                  (vs_layout_is_number(style[@"size"]) && [style[@"size"] doubleValue] >= 1 &&
                   [style[@"size"] doubleValue] <= 200)) &&
                 (!color || vs_layout_numbers(color, 2) || vs_layout_numbers(color, 4));
    for (id component in [color isKindOfClass:NSArray.class] ? color : @[]) {
      valid = valid && [component doubleValue] >= 0 && [component doubleValue] <= 1;
    }
    if (!valid) {
      return fail(@"textStyle needs font (system, monospaced or monospacedDigit), size 1 to 200, "
                  @"align (left, center or right) and color [white, alpha] or [red, green, blue, "
                  @"alpha] in 0 to 1");
    }
    if (style[@"fit"] && (version < 2 || !field || !vs_layout_is_number(style[@"fit"]))) {
      return fail(@"textStyle fit must be a boolean and needs layout version 2 and editing "
                  @"\"click\"");
    }
  }
  for (NSString *key in @[ @"values", @"choices" ]) {
    id list = widget[key];
    if (!list) {
      continue;
    }
    if (![list isKindOfClass:NSArray.class] || [list count] > vs_layout_max_list) {
      return fail([NSString stringWithFormat:@"%@ must be an array of at most %lu entries", key,
                                             static_cast<unsigned long>(vs_layout_max_list)]);
    }
    for (id item in list) {
      if ([key isEqualToString:@"values"] ? !vs_layout_is_number(item)
                                          : ![item isKindOfClass:NSString.class]) {
        return fail([NSString stringWithFormat:@"%@ has an entry of the wrong type", key]);
      }
    }
  }
  id names = widget[@"images"] ? widget[@"images"] : @[];
  if (!vs_layout_widget_images(names, result, where, directory, version, images, error)) {
    return nil;
  }
  BOOL needsImage = [type isEqualToString:@"rotaryFilmStripKnob"] ||
                    [type isEqualToString:@"imageSlider"] || [type isEqualToString:@"image"];
  if (needsImage && (![names count] || ![names[0] length])) {
    return fail(@"needs an image");
  }
  if ([type isEqualToString:@"rotaryFilmStripKnob"] &&
      !vs_layout_rotary_frames(result, names, images, where, error)) {
    return nil;
  }
  if (version >= 2 && [type isEqualToString:@"image"]) {
    result[@"readonly"] = @YES;
  }
  return result;
}

// Reads and validates a layout. On success returns the layout with each widget's defaults filled
// in and stores its decoded artwork, keyed by name, in images.
NSDictionary *vs_layout_load(NSString *directory, NSString *name, BOOL requireVersion,
                             NSDictionary<NSString *, NSImage *> **loaded, NSString **error) {
  NSString *reason = nil;
  NSString *path = vs_layout_resolve(directory, name, vs_layout_max_layout_bytes, &reason);
  if (!path) {
    vs_layout_fail(error, reason);
    return nil;
  }
  auto fail = [&](NSString *message) {
    vs_layout_fail(error, [NSString stringWithFormat:@"%@: %@", name, message]);
    return (NSDictionary *)nil;
  };
  NSData *data = [NSData dataWithContentsOfFile:path];
  NSError *parse = nil;
  id layout = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:&parse] : nil;
  if (![layout isKindOfClass:NSDictionary.class]) {
    return fail(parse ? parse.localizedDescription : @"must contain a JSON object");
  }
  id declared = layout[@"version"];
  NSInteger version = 1;
  if (declared || requireVersion) {
    if (!vs_layout_is_number(declared) || [declared doubleValue] < 1 ||
        [declared doubleValue] != std::floor([declared doubleValue])) {
      return fail(@"version must be a positive whole number");
    }
    if ([declared doubleValue] > VS_LAYOUT_VERSION) {
      return fail([NSString stringWithFormat:@"version %@ is not supported; this renderer reads "
                                             @"layout versions 1 to %d",
                                             declared, VS_LAYOUT_VERSION]);
    }
    version = [declared integerValue];
  }
  if (!vs_layout_rect(layout[@"crop"])) {
    return fail(@"crop must be [x, y, width, height] with a positive size");
  }
  for (NSString *key in @[ @"backgroundRect", @"labelsRect" ]) {
    if (layout[key] && !vs_layout_rect(layout[key])) {
      return fail([NSString stringWithFormat:@"%@ must be [x, y, width, height] with a positive "
                                             @"size",
                                             key]);
    }
  }
  id insets = layout[@"visualInsets"];
  if (layout[@"anchor"] && ![vs_layout_anchors() containsObject:layout[@"anchor"]]) {
    return fail(@"anchor must be \"center\", \"top\" or \"bottom\"");
  }
  if (layout[@"interpolation"] && !vs_layout_interpolations()[layout[@"interpolation"]]) {
    return fail(@"interpolation must be \"default\", \"none\", \"low\", \"medium\" or \"high\"");
  }
  if (layout[@"help"] && ![layout[@"help"] isKindOfClass:NSString.class]) {
    return fail(@"help must be a string");
  }
  id ratio = layout[@"pixelRatio"];
  if (ratio) {
    if (version < 2) {
      return fail(@"pixelRatio requires layout version 2");
    }
    if (!vs_layout_is_number(ratio) || !([ratio doubleValue] > 0) || [ratio doubleValue] > 64) {
      return fail(@"pixelRatio must be above 0 and at most 64");
    }
  }
  if (insets) {
    if (!vs_layout_numbers(insets, 4)) {
      return fail(@"visualInsets must be [top, left, bottom, right]");
    }
    for (id inset in insets) {
      if ([inset doubleValue] < 0) {
        return fail(@"visualInsets must not be negative");
      }
    }
  }
  NSMutableDictionary<NSString *, NSImage *> *images = [NSMutableDictionary dictionary];
  for (NSString *key in @[ @"background", @"labels" ]) {
    id asset = layout[key];
    if ([key isEqualToString:@"labels"] && (!asset || [asset isEqual:@""])) {
      continue;
    }
    if (![asset isKindOfClass:NSString.class]) {
      return fail([NSString stringWithFormat:@"%@ must name an image", key]);
    }
    NSImage *image = vs_layout_image(directory, asset, version >= 2, &reason);
    if (!image) {
      return fail(reason);
    }
    images[asset] = image;
  }
  id widgets = layout[@"widgets"];
  if (![widgets isKindOfClass:NSArray.class] || [widgets count] > vs_layout_max_widgets) {
    return fail([NSString stringWithFormat:@"widgets must be an array of at most %lu entries",
                                           static_cast<unsigned long>(vs_layout_max_widgets)]);
  }
  NSMutableArray *normalized = [NSMutableArray array];
  for (NSUInteger index = 0; index < [widgets count]; ++index) {
    NSDictionary *widget =
        vs_layout_widget(widgets[index], index, directory, version, images, &reason);
    if (!widget) {
      return fail(reason);
    }
    if (!widget[@"interpolation"] && layout[@"interpolation"]) {
      NSMutableDictionary *inherited = [widget mutableCopy];
      inherited[@"interpolation"] = layout[@"interpolation"];
      widget = inherited;
    }
    [normalized addObject:widget];
  }
  NSMutableDictionary *result = [layout mutableCopy];
  result[@"widgets"] = normalized;
  result[@"version"] = @(version);
  if (loaded) {
    *loaded = images;
  }
  return result;
}

NSFont *vs_layout_font(NSString *name, CGFloat size) {
  return [name isEqualToString:@"monospaced"]
             ? [NSFont monospacedSystemFontOfSize:size weight:NSFontWeightRegular]
         : [name isEqualToString:@"monospacedDigit"]
             ? [NSFont monospacedDigitSystemFontOfSize:size weight:NSFontWeightRegular]
             : [NSFont systemFontOfSize:size];
}

NSColor *vs_layout_color(NSArray *color) {
  return color.count == 2
             ? [NSColor colorWithWhite:[color[0] doubleValue] alpha:[color[1] doubleValue]]
         : color.count == 4 ? [NSColor colorWithSRGBRed:[color[0] doubleValue]
                                                  green:[color[1] doubleValue]
                                                   blue:[color[2] doubleValue]
                                                  alpha:[color[3] doubleValue]]
                            : NSColor.blackColor;
}

std::string vs_layout_utf8(NSString *text) {
  const char *bytes = text.UTF8String;
  return bytes ? std::string(bytes) : std::string();
}

NSString *vs_layout_string(const std::string &text) {
  NSString *result = [NSString stringWithUTF8String:text.c_str()];
  return result ? result : @"";
}

// The display mapping a validated `format` object describes.
vespera::DisplayMapping vs_layout_mapping(NSDictionary *format) {
  vespera::DisplayMapping mapping;
  NSArray *range = format[@"range"];
  if (range) {
    mapping.range = std::array<double, 2>{[range[0] doubleValue], [range[1] doubleValue]};
  }
  mapping.skew = format[@"skew"] ? [format[@"skew"] doubleValue] : 1;
  mapping.decimals = [format[@"decimals"] intValue];
  mapping.unit = vs_layout_utf8(format[@"unit"]);
  if (format[@"kiloUnit"]) {
    mapping.kilo_unit = vs_layout_utf8(format[@"kiloUnit"]);
  }
  mapping.show_sign = [format[@"signed"] boolValue];
  mapping.off = [format[@"off"] isEqual:@"minimum"]   ? vespera::OffEnd::minimum
                : [format[@"off"] isEqual:@"maximum"] ? vespera::OffEnd::maximum
                                                      : vespera::OffEnd::none;
  mapping.off_tolerance = [format[@"offTolerance"] doubleValue];
  return mapping;
}

NSString *vs_layout_format(NSDictionary *format, float minimum, float maximum, double value) {
  return vs_layout_string(
      vespera::format_number(vs_layout_mapping(format), minimum, maximum, value));
}

// Reads typed text in the units `mapping` shows; returns NO when it is not a value. Numbers are
// read as NSScanner reads them, which also accepts digits of other scripts.
BOOL vs_layout_read(const vespera::DisplayMapping &mapping, float minimum, float maximum,
                    NSString *text, float *value) {
  std::optional<float> result = vespera::parse_number(
      mapping, minimum, maximum, vs_layout_utf8(text.lowercaseString),
      [](std::string_view normalized) -> std::optional<double> {
        NSString *typed = [[NSString alloc] initWithBytes:normalized.data()
                                                   length:normalized.size()
                                                 encoding:NSUTF8StringEncoding];
        double number = 0;
        if (!typed || ![[NSScanner scannerWithString:typed] scanDouble:&number]) {
          return std::nullopt;
        }
        return number;
      });
  if (!result) {
    return NO;
  }
  *value = *result;
  return YES;
}

BOOL vs_layout_parse(NSDictionary *format, float minimum, float maximum, NSString *text,
                     float *value) {
  return vs_layout_read(vs_layout_mapping(format), minimum, maximum, text, value);
}

// The parameter description that supplies a widget's value text when the layout gives it none:
// version 2 widgets without format, choices or any version 1 text field, bound to a parameter
// whose module describes how its values are shown.
const vespera::ParameterDisplay *
vs_layout_described(NSDictionary *spec, NSInteger version,
                    const std::vector<vespera::ParameterDisplay> *parameters) {
  if (!parameters || version < 2) {
    return nullptr;
  }
  for (NSString *key in @[
         @"format", @"choices", @"values", @"text", @"hostTempo", @"constant", @"unit", @"curve",
         @"decimals", @"offValue"
       ]) {
    if (spec[key]) {
      return nullptr;
    }
  }
  const uint32_t identifier = [spec[@"id"] unsignedIntValue];
  for (const vespera::ParameterDisplay &parameter : *parameters) {
    if (parameter.id == identifier) {
      return parameter.shows_values() ? &parameter : nullptr;
    }
  }
  return nullptr;
}

enum class vs_layout_behaviour { continuous, toggle, cycle, radio, tap };
}

@interface VSArtworkControl : NSControl <NSTextFieldDelegate> {
  vs_module_ui_host host;
  NSDictionary *spec;
  NSArray<NSImage *> *images;
  NSPoint start;
  float startValue;
  NSTimeInterval lastTap;
  NSTextField *editor;
  BOOL pressed;
  NSInteger version;
  CGFloat pixelRatio;
  vs_layout_behaviour behaviour;
  // How the module shows this widget's parameter, used when the layout specifies no text.
  std::optional<vespera::ParameterDisplay> described;
@public
  // Layout-unit rectangle the control occupies: rect, extended by a travelling slider handle.
  NSRect frameRect;
}
- (instancetype)initWithSpec:(NSDictionary *)definition
                        host:(vs_module_ui_host)callbacks
                      images:(NSDictionary *)assets
                     version:(NSInteger)layoutVersion
                  pixelRatio:(CGFloat)ratio
                   described:(const vespera::ParameterDisplay *)parameter;
- (void)refresh;
- (NSTextField *)field;
- (BOOL)fits;
- (CGFloat)textSize;
- (CGFloat)fitSize:(CGFloat)size width:(CGFloat)width;
- (void)setFieldFontSize:(CGFloat)size;
@end

@interface VSArtworkField : NSTextField
@end

@implementation VSArtworkField
- (void)mouseDown:(NSEvent *)event {
  [super mouseDown:event];
  [self.currentEditor performSelector:@selector(selectAll:) withObject:nil afterDelay:0];
}
- (void)cancelOperation:(id)sender {
  [self abortEditing];
  [self.window makeFirstResponder:nil];
  if ([self.superview isKindOfClass:VSArtworkControl.class]) {
    [(VSArtworkControl *)self.superview refresh];
  }
}
@end

@implementation VSArtworkControl {
  VSArtworkField *field;
}
- (instancetype)initWithSpec:(NSDictionary *)definition
                        host:(vs_module_ui_host)callbacks
                      images:(NSDictionary *)assets
                     version:(NSInteger)layoutVersion
                  pixelRatio:(CGFloat)ratio
                   described:(const vespera::ParameterDisplay *)parameter {
  self = [super initWithFrame:NSZeroRect];
  if (self) {
    host = callbacks;
    if (parameter) {
      described = *parameter;
    }
    spec = definition;
    version = layoutVersion;
    pixelRatio = ratio;
    NSMutableArray *loaded = [NSMutableArray array];
    for (NSString *name in spec[@"images"]) {
      [loaded
          addObject:assets[name] ? assets[name] : [[NSImage alloc] initWithSize:NSMakeSize(1, 1)]];
    }
    images = loaded;
    NSArray *r = spec[@"rect"];
    frameRect =
        NSMakeRect([r[0] doubleValue], [r[1] doubleValue], [r[2] doubleValue], [r[3] doubleValue]);
    if (spec[@"handleTravel"]) {
      frameRect = NSUnionRect(frameRect, NSUnionRect([self sprite:0], [self sprite:1]));
    }
    NSString *type = spec[@"type"];
    behaviour = [spec[@"tap"] boolValue] ? vs_layout_behaviour::tap
                : spec[@"radio"]         ? vs_layout_behaviour::radio
                : vs_layout_button(type) ? ([self states] == 2 ? vs_layout_behaviour::toggle
                                                               : vs_layout_behaviour::cycle)
                                         : vs_layout_behaviour::continuous;
    self.toolTip = spec[@"help"] ? spec[@"help"] : spec[@"label"];
    self.accessibilityLabel = spec[@"label"];
    self.accessibilityHelp = spec[@"help"];
    self.accessibilityRole = behaviour == vs_layout_behaviour::toggle ? NSAccessibilityCheckBoxRole
                             : behaviour == vs_layout_behaviour::radio
                                 ? NSAccessibilityRadioButtonRole
                             : behaviour == vs_layout_behaviour::tap ? NSAccessibilityButtonRole
                                                                     : NSAccessibilitySliderRole;
    self.accessibilityElement = spec[@"label"] || ![spec[@"readonly"] boolValue];
    if ([spec[@"editing"] isEqual:@"click"]) {
      [self makeField];
    }
  }
  return self;
}
// A text field that always shows the value; one click starts typing.
- (void)makeField {
  field = [[VSArtworkField alloc] initWithFrame:NSZeroRect];
  vs_ui_style_value_field(field, VS_UI_SIZE_COMPACT);
  field.bezeled = NO;
  field.editable = ![spec[@"readonly"] boolValue];
  field.selectable = field.editable;
  NSDictionary *style = spec[@"textStyle"];
  field.textColor = style                           ? vs_layout_color(style[@"color"])
                    : [spec[@"darkText"] boolValue] ? NSColor.blackColor
                                                    : vs_ui_color(VS_UI_TEXT_PRIMARY);
  if (style[@"align"]) {
    field.alignment =
        static_cast<NSTextAlignment>([vs_layout_alignments()[style[@"align"]] integerValue]);
  }
  field.usesSingleLineMode = YES;
  field.cell.scrollable = YES;
  field.cell.lineBreakMode = NSLineBreakByClipping;
  field.delegate = self;
  field.target = self;
  field.action = @selector(commit:);
  field.toolTip = spec[@"help"];
  field.accessibilityLabel = spec[@"label"];
  [self addSubview:field];
  self.toolTip = nil;
  self.accessibilityElement = NO;
}
- (NSTextField *)field {
  return field;
}
- (BOOL)fits {
  return [spec[@"textStyle"][@"fit"] boolValue];
}
// The field's text size in layout units, or in points (negative) when it follows the host font.
- (CGFloat)textSize {
  NSDictionary *style = spec[@"textStyle"];
  return style ? (style[@"size"] ? [style[@"size"] doubleValue] : 10) : -1;
}
- (void)setFrameSize:(NSSize)size {
  [super setFrameSize:size];
  field.frame = self.bounds;
}
// The largest size up to `size` at which every value this control can show fits `width` points.
- (CGFloat)fitSize:(CGFloat)size width:(CGFloat)width {
  NSCell *probe = [field.cell copy];
  float minimum = [spec[@"minimum"] floatValue], maximum = [spec[@"maximum"] floatValue];
  NSString *face = spec[@"textStyle"][@"font"];
  auto widest = [&](CGFloat points) {
    NSFont *font = vs_layout_font(face, points);
    if (!font) {
      return CGFLOAT_MAX;
    }
    probe.font = font;
    CGFloat result = 0;
    for (float position : {0.0f, 0.001f, 0.25f, 0.5f, 0.75f, 0.999f, 1.0f}) {
      probe.stringValue = [self text:static_cast<float>(minimum + position * (maximum - minimum))];
      result = std::max(result, [probe cellSizeForBounds:NSMakeRect(0, 0, 1e4, 1e4)].width);
    }
    return result;
  };
  size = std::max<CGFloat>(1, size);
  CGFloat needed = widest(size);
  if (needed > width) {
    size = std::max<CGFloat>(1, std::floor(size * width / needed * 10) / 10);
    // Cell padding does not scale with the font, so step down until the text fits.
    for (int attempt = 0; attempt < 2000 && size > 1 && widest(size) > width; ++attempt) {
      size = std::max<CGFloat>(1, size - 0.1);
    }
  }
  return size;
}
- (void)setFieldFontSize:(CGFloat)size {
  NSFont *font =
      vs_layout_font(spec[@"textStyle"] ? spec[@"textStyle"][@"font"] : @"monospacedDigit",
                     std::max<CGFloat>(1, size));
  if (font) {
    field.font = font;
  }
}
- (BOOL)isFlipped {
  return YES;
}
- (BOOL)acceptsFirstResponder {
  return ![spec[@"readonly"] boolValue];
}
// Maps a layout-unit rectangle into this control's coordinates.
- (NSRect)local:(NSRect)rect {
  CGFloat scale = frameRect.size.width > 0 ? self.bounds.size.width / frameRect.size.width : 0;
  return NSMakeRect((rect.origin.x - frameRect.origin.x) * scale,
                    (rect.origin.y - frameRect.origin.y) * scale, rect.size.width * scale,
                    rect.size.height * scale);
}
- (NSView *)hitTest:(NSPoint)point {
  if ([spec[@"readonly"] boolValue]) {
    return nil;
  }
  if (spec[@"handleTravel"]) {
    NSArray *r = spec[@"rect"];
    NSPoint local = [self convertPoint:point fromView:self.superview];
    return NSPointInRect(local, [self local:NSMakeRect([r[0] doubleValue], [r[1] doubleValue],
                                                       [r[2] doubleValue], [r[3] doubleValue])])
               ? self
               : nil;
  }
  return [super hitTest:point];
}
// The handle rectangle in layout units at a normalized position.
- (NSRect)sprite:(float)position {
  NSArray *travel = spec[@"handleTravel"], *handle = spec[@"handle"],
          *anchor = spec[@"handleAnchor"];
  CGFloat width = [handle[0] doubleValue], height = [handle[1] doubleValue];
  CGFloat x = [travel[0] doubleValue] +
              position * ([travel[2] doubleValue] - [travel[0] doubleValue]),
          y = [travel[1] doubleValue] +
              position * ([travel[3] doubleValue] - [travel[1] doubleValue]);
  return NSMakeRect(x - (anchor ? [anchor[0] doubleValue] : width / 2),
                    y - (anchor ? [anchor[1] doubleValue] : height / 2), width, height);
}
- (float)read:(uint32_t)identifier fallback:(float)fallback {
  float value = fallback;
  if (host.get_value(host.context, identifier, &value) != VS_OK || !std::isfinite(value)) {
    value = fallback;
  }
  return value;
}
- (float)value {
  if (spec[@"constant"]) {
    return [spec[@"constant"] floatValue];
  }
  float value = [self read:[spec[@"id"] unsignedIntValue] fallback:[spec[@"initial"] floatValue]];
  NSArray *values = spec[@"values"];
  if (values.count) {
    return [values[static_cast<NSUInteger>(
        std::clamp<long>(lround(value), 0, static_cast<long>(values.count) - 1))] floatValue];
  }
  return value;
}
- (float)minimum {
  return [spec[@"minimum"] floatValue];
}
- (float)maximum {
  return [spec[@"maximum"] floatValue];
}
- (float)position {
  float minimum = [self minimum], maximum = [self maximum];
  return maximum > minimum ? std::clamp(([self value] - minimum) / (maximum - minimum), 0.0f, 1.0f)
                           : 0;
}
- (BOOL)on {
  return [self position] >= 0.5f;
}
- (float)clickStep {
  return spec[@"step"] ? [spec[@"step"] floatValue] : 1;
}
// Distinct values a button steps through, wrapping at the range ends.
- (long)states {
  double range = [spec[@"maximum"] doubleValue] - [spec[@"minimum"] doubleValue];
  double step = std::abs([self clickStep]);
  return range > 0 ? lround(range / step) + 1 : 1;
}
- (float)keyStep {
  if (version >= 2 && spec[@"step"]) {
    return [spec[@"step"] floatValue];
  }
  return [spec[@"discrete"] boolValue] ? 1 : .01f;
}
- (float)fineStep {
  if (spec[@"fineStep"]) {
    return [spec[@"fineStep"] floatValue];
  }
  if ([spec[@"discrete"] boolValue]) {
    return [self keyStep];
  }
  return version >= 2 && spec[@"step"] ? [self keyStep] / 10 : .001f;
}
- (void)change:(float)value {
  if ([spec[@"readonly"] boolValue]) {
    return;
  }
  if (!std::isfinite(value)) {
    return;
  }
  value = std::clamp(value, [spec[@"minimum"] floatValue], [spec[@"maximum"] floatValue]);
  if ([spec[@"discrete"] boolValue]) {
    value = std::round(value);
  }
  if (host.set_value(host.context, [spec[@"id"] unsignedIntValue], value) == VS_OK) {
    for (NSArray *pair in spec[@"sets"]) {
      host.set_value(host.context, [pair[0] unsignedIntValue], [pair[1] floatValue]);
    }
    [self.superview setNeedsDisplay:YES];
    for (VSArtworkControl *control in self.superview.subviews) {
      [control refresh];
    }
    NSAccessibilityPostNotification(field ? (id)field : (id)self,
                                    NSAccessibilityValueChangedNotification);
  }
}
- (void)toggle {
  [self change:[self on] ? [self minimum] : [self maximum]];
}
- (void)cycle:(float)step {
  float minimum = [self minimum], maximum = [self maximum];
  float value = [self value] + step;
  [self change:value > maximum ? minimum : value < minimum ? maximum : value];
}
- (void)tap:(NSTimeInterval)now {
  if (lastTap > 0 && now - lastTap >= .25 && now - lastTap <= 1.5) {
    [self change:static_cast<float>((60 / (now - lastTap) - 40) / 200)];
  }
  lastTap = now;
}
- (void)press {
  switch (behaviour) {
  case vs_layout_behaviour::toggle:
    [self toggle];
    break;
  case vs_layout_behaviour::cycle:
    [self cycle:[self clickStep]];
    break;
  case vs_layout_behaviour::radio:
    [self change:[spec[@"radio"] floatValue]];
    break;
  case vs_layout_behaviour::tap:
    [self tap:NSProcessInfo.processInfo.systemUptime];
    break;
  case vs_layout_behaviour::continuous:
    break;
  }
}
// Moves an increment up or down: a toggle flips, a multi-position button moves one position
// without wrapping, and a continuous control moves by its step.
- (BOOL)stepBy:(int)direction fine:(BOOL)fine {
  if ([spec[@"readonly"] boolValue]) {
    return NO;
  }
  switch (behaviour) {
  case vs_layout_behaviour::toggle:
    [self toggle];
    return YES;
  case vs_layout_behaviour::cycle:
    [self change:[self value] + static_cast<float>(direction) * std::abs([self clickStep])];
    return YES;
  case vs_layout_behaviour::continuous:
    [self change:[self value] +
                 static_cast<float>(direction) * (fine ? [self fineStep] : [self keyStep])];
    return YES;
  default:
    return NO;
  }
}
- (id)accessibilityValue {
  switch (behaviour) {
  case vs_layout_behaviour::toggle:
    return @([self on] ? 1 : 0);
  case vs_layout_behaviour::radio:
    return @([self value] == [spec[@"radio"] floatValue] ? 1 : 0);
  case vs_layout_behaviour::tap:
    return nil;
  default:
    return @([self value]);
  }
}
- (NSString *)accessibilityValueDescription {
  NSArray *choices = spec[@"choices"];
  if (behaviour == vs_layout_behaviour::toggle) {
    return [self on] ? @"On" : @"Off";
  }
  if (behaviour == vs_layout_behaviour::radio || behaviour == vs_layout_behaviour::tap) {
    return nil;
  }
  if (choices.count || spec[@"format"] || described ||
      [spec[@"type"] isEqualToString:@"textEditor"]) {
    return [self displayValue];
  }
  if (behaviour == vs_layout_behaviour::cycle) {
    return nil;
  }
  return [NSString stringWithFormat:@"%ld%%", lround([self position] * 100)];
}
- (void)setAccessibilityValue:(id)value {
  if (![value respondsToSelector:@selector(floatValue)]) {
    return;
  }
  float number = [value floatValue];
  if (behaviour == vs_layout_behaviour::toggle) {
    [self change:number >= 0.5f ? [self maximum] : [self minimum]];
  } else if (behaviour == vs_layout_behaviour::radio) {
    if (number >= 0.5f) {
      [self press];
    }
  } else if (behaviour != vs_layout_behaviour::tap) {
    [self change:number];
  }
}
- (id)accessibilityMinValue {
  return behaviour == vs_layout_behaviour::toggle || behaviour == vs_layout_behaviour::radio
             ? @0
             : spec[@"minimum"];
}
- (id)accessibilityMaxValue {
  return behaviour == vs_layout_behaviour::toggle || behaviour == vs_layout_behaviour::radio
             ? @1
             : spec[@"maximum"];
}
- (BOOL)accessibilityPerformIncrement {
  return [self stepBy:1 fine:NO];
}
- (BOOL)accessibilityPerformDecrement {
  return [self stepBy:-1 fine:NO];
}
- (BOOL)accessibilityPerformPress {
  if ([spec[@"readonly"] boolValue] || behaviour == vs_layout_behaviour::continuous) {
    return NO;
  }
  [self press];
  return YES;
}
- (void)refresh {
  NSArray *condition = spec[@"condition"];
  if (condition.count == 3) {
    float value = 0;
    host.get_value(host.context, [condition[0] unsignedIntValue], &value);
    self.hidden = value < [condition[1] floatValue] || value > [condition[2] floatValue];
  }
  if (field && !field.currentEditor) {
    field.stringValue = [self displayValue];
  }
  self.needsDisplay = YES;
}
- (NSString *)displayValue {
  return [self text:[self value]];
}
- (NSString *)text:(float)number {
  if (spec[@"text"]) {
    return spec[@"text"];
  }
  if ([spec[@"hostTempo"] boolValue]) {
    vs_tempo_context tempo{sizeof(tempo), 0, 120, 0};
    if (host.get_tempo && host.get_tempo(host.context, &tempo) == VS_OK &&
        (tempo.flags & VS_TEMPO_VALID)) {
      return [NSString stringWithFormat:@"%.0f BPM", tempo.bpm];
    }
    return @"120 BPM";
  }
  double value = number;
  if (spec[@"offValue"] && value == [spec[@"offValue"] doubleValue]) {
    return @"OFF";
  }
  NSArray *choices = spec[@"choices"];
  if (choices.count) {
    return choices[static_cast<NSUInteger>(
        std::clamp<long>(lround(value), 0, static_cast<long>(choices.count) - 1))];
  }
  if (spec[@"format"]) {
    return vs_layout_format(spec[@"format"], [self minimum], [self maximum], value);
  }
  if (described) {
    return vs_layout_string(described->text(value));
  }
  NSArray *curve = spec[@"curve"];
  if (curve.count == 3) {
    value =
        [curve[0] doubleValue] + std::pow(std::clamp(value, 0.0, 1.0), 1 / [curve[2] doubleValue]) *
                                     ([curve[1] doubleValue] - [curve[0] doubleValue]);
  }
  NSString *unit = spec[@"unit"] ? spec[@"unit"] : @"%";
  if ([unit isEqualToString:@" Hz"] && value >= 1000) {
    return [NSString stringWithFormat:@"%.1f kHz", value / 1000];
  }
  if ([unit isEqualToString:@"%"] && !curve) {
    value *= 100;
  }
  return [NSString stringWithFormat:@"%.*f%@", [spec[@"decimals"] intValue], value, unit];
}
// Reads typed text with the inverse of the displayed mapping.
- (BOOL)parse:(NSString *)typed value:(float *)result {
  if (spec[@"format"]) {
    return vs_layout_parse(spec[@"format"], [self minimum], [self maximum], typed, result);
  }
  if (described) {
    std::optional<float> value;
    if (described->mapping) {
      float read = 0;
      if (vs_layout_read(*described->mapping, described->minimum, described->maximum, typed,
                         &read)) {
        value =
            described->kind == vespera::ParameterKind::stepped ? described->nearest(read) : read;
      }
    } else {
      value = described->parse(vs_layout_utf8(typed));
    }
    if (value) {
      *result = *value;
    }
    return value.has_value();
  }
  double value = 0;
  NSScanner *scanner = [NSScanner scannerWithString:typed];
  NSString *text =
      [typed stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
  if (spec[@"offValue"] && [text caseInsensitiveCompare:@"OFF"] == NSOrderedSame) {
    *result = [spec[@"offValue"] floatValue];
    return YES;
  }
  if (![scanner scanDouble:&value] || !std::isfinite(value)) {
    return NO;
  }
  NSString *suffix = [[typed substringFromIndex:scanner.scanLocation]
      stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
  if ([suffix isEqualToString:@"kHz"] && [spec[@"unit"] isEqualToString:@" Hz"]) {
    value *= 1000;
  }
  NSArray *curve = spec[@"curve"];
  if (curve.count == 3) {
    double minimum = [curve[0] doubleValue], maximum = [curve[1] doubleValue];
    value = std::pow(std::clamp((value - minimum) / (maximum - minimum), 0.0, 1.0),
                     [curve[2] doubleValue]);
  } else if (!spec[@"unit"] || [spec[@"unit"] isEqualToString:@"%"]) {
    value /= 100;
  }
  *result = static_cast<float>(value);
  return YES;
}
- (NSImage *)stateImage:(float)position {
  NSArray *follows = spec[@"imageFollows"];
  if (follows) {
    float low = [follows[1] floatValue], high = [follows[2] floatValue];
    position = std::clamp(
        ([self read:[follows[0] unsignedIntValue] fallback:low] - low) / (high - low), 0.0f, 1.0f);
  }
  return images.count ? images[static_cast<NSUInteger>(
                            lround(position * static_cast<float>(images.count - 1)))]
                      : nil;
}
- (void)drawRect:(NSRect)dirtyRect {
  [super drawRect:dirtyRect];
  if (self.bounds.size.width < 1 || self.bounds.size.height < 1) {
    return;
  }
  NSGraphicsContext.currentContext.imageInterpolation =
      vs_layout_interpolation(spec[@"interpolation"]);
  NSString *type = spec[@"type"];
  float minimum = [spec[@"minimum"] floatValue], maximum = [spec[@"maximum"] floatValue];
  float normalized = maximum > minimum
                         ? std::clamp(([self value] - minimum) / (maximum - minimum), 0.0f, 1.0f)
                         : 0;
  if ([spec[@"invert"] boolValue]) {
    normalized = 1 - normalized;
  }
  if ([type isEqualToString:@"meter"]) {
    [vs_ui_color(VS_UI_TEXT_PRIMARY) set];
    NSFrameRectWithWidth(self.bounds, 1);
    NSRect fill = NSInsetRect(self.bounds, 1, 1);
    fill.size.width *= normalized;
    NSRectFill(fill);
    return;
  }
  NSImage *image = images.count ? images[0] : nil;
  NSRect destination = self.bounds, source = NSZeroRect;
  if ([type isEqualToString:@"rotaryFilmStripKnob"]) {
    NSInteger count = [spec[@"frames"] integerValue];
    CGFloat height = image.size.height / static_cast<CGFloat>(count);
    NSInteger frame = lround(normalized * static_cast<float>(count - 1));
    source = NSMakeRect(0, image.size.height - static_cast<CGFloat>(frame + 1) * height,
                        image.size.width, height);
  } else if ([type isEqualToString:@"imageSlider"]) {
    if (spec[@"imageFollows"]) {
      image = [self stateImage:0];
    }
    if (spec[@"handleTravel"]) {
      destination = [self local:[self sprite:normalized]];
    } else {
      NSArray *handle = spec[@"handle"];
      NSArray *rect = spec[@"rect"];
      CGFloat height = [handle[1] doubleValue] / [rect[3] doubleValue] * self.bounds.size.height;
      destination.origin.y = (1 - normalized) * (self.bounds.size.height - height);
      destination.size.height = height;
    }
  } else if ([spec[@"momentary"] boolValue]) {
    image = images.count > 1 && pressed ? images[1] : image;
  } else if (images.count > 1) {
    image = [self stateImage:normalized];
  }
  [image drawInRect:destination
            fromRect:source
           operation:NSCompositingOperationSourceOver
            fraction:1
      respectFlipped:YES
               hints:nil];
  NSDictionary *style = spec[@"textStyle"];
  if (!field && [type isEqualToString:@"textEditor"] && style) {
    NSArray *rect = spec[@"rect"];
    NSMutableParagraphStyle *paragraph = [[NSMutableParagraphStyle alloc] init];
    paragraph.alignment = static_cast<NSTextAlignment>(
        [vs_layout_alignments()[style[@"align"] ? style[@"align"] : @"center"] integerValue]);
    CGFloat size = style[@"size"] ? [style[@"size"] doubleValue] : 10;
    [NSGraphicsContext saveGraphicsState];
    NSAffineTransform *transform = [NSAffineTransform transform];
    [transform scaleBy:self.bounds.size.width / [rect[2] doubleValue]];
    [transform concat];
    NSFont *face = vs_layout_font(style[@"font"], size);
    NSColor *ink = vs_layout_color(style[@"color"]);
    // AppKit can return no font while the context is scaled to a fraction of a point.
    if (face && ink) {
      [[self displayValue] drawInRect:NSMakeRect(0, 0, [rect[2] doubleValue], [rect[3] doubleValue])
                       withAttributes:@{
                         NSFontAttributeName : face,
                         NSForegroundColorAttributeName : ink,
                         NSParagraphStyleAttributeName : paragraph
                       }];
    }
    [NSGraphicsContext restoreGraphicsState];
  } else if (!field && [type isEqualToString:@"textEditor"]) {
    vs_ui_draw_line([self displayValue], self.bounds,
                    vs_ui_value_font(std::min(14.0, self.bounds.size.height * .7)),
                    [spec[@"darkText"] boolValue] ? NSColor.blackColor
                                                  : vs_ui_color(VS_UI_TEXT_PRIMARY),
                    NSTextAlignmentCenter);
  }
  if (vs_ui_has_focus(self)) {
    vs_ui_draw_focus(self.bounds, 3);
  }
}
- (void)mouseDown:(NSEvent *)event {
  if ([spec[@"readonly"] boolValue]) {
    return;
  }
  [self.window makeFirstResponder:self];
  if ([spec[@"momentary"] boolValue]) {
    pressed = YES;
    self.needsDisplay = YES;
  }
  if (behaviour == vs_layout_behaviour::tap) {
    [self tap:event.timestamp];
    return;
  }
  if (behaviour == vs_layout_behaviour::radio) {
    [self press];
    return;
  }
  NSString *type = spec[@"type"];
  if ([type isEqualToString:@"textEditor"] && event.clickCount == 2 && ![spec[@"choices"] count]) {
    editor = [[NSTextField alloc] initWithFrame:self.bounds];
    editor.stringValue = [self displayValue];
    editor.font = vs_ui_value_font(std::min(14.0, self.bounds.size.height * .7));
    editor.textColor = vs_ui_color(VS_UI_TEXT_PRIMARY);
    editor.backgroundColor = vs_ui_color(VS_UI_SURFACE_CONTROL);
    editor.bordered = NO;
    editor.alignment = NSTextAlignmentCenter;
    editor.delegate = self;
    [self addSubview:editor];
    [self.window makeFirstResponder:editor];
    [editor selectText:nil];
    return;
  }
  if (behaviour != vs_layout_behaviour::continuous) {
    [self press];
  } else if (event.clickCount == 2) {
    [self change:[spec[@"initial"] floatValue]];
  }
  start = event.locationInWindow;
  startValue = [self value];
}
- (void)mouseUp:(NSEvent *)event {
  if (pressed) {
    pressed = NO;
    self.needsDisplay = YES;
  }
}
- (void)mouseDragged:(NSEvent *)event {
  NSString *type = spec[@"type"];
  if (![type isEqualToString:@"rotaryFilmStripKnob"] && ![type isEqualToString:@"imageSlider"] &&
      ![type isEqualToString:@"textEditor"]) {
    return;
  }
  CGFloat scale = self.bounds.size.width / frameRect.size.width;
  if (!(scale > 0)) {
    return;
  }
  NSArray *travel = spec[@"handleTravel"];
  NSPoint location = event.locationInWindow;
  CGFloat moved = location.y - start.y, length = 0;
  if ([spec[@"dragAxis"] isEqual:@"travel"]) {
    CGFloat x = [travel[2] doubleValue] - [travel[0] doubleValue],
            y = [travel[3] doubleValue] - [travel[1] doubleValue];
    length = std::hypot(x, y);
    // Window y rises upwards while layout y descends.
    moved = ((location.x - start.x) * x - (location.y - start.y) * y) / length;
  }
  CGFloat distance = spec[@"dragTravel"] ? [spec[@"dragTravel"] doubleValue]
                     : length > 0        ? length
                     : travel            ? [travel[1] doubleValue] - [travel[3] doubleValue]
                                         : 180 * pixelRatio;
  float range = [spec[@"maximum"] floatValue] - [spec[@"minimum"] floatValue];
  float fine = event.modifierFlags & NSEventModifierFlagShift ? .1f : 1;
  if ([spec[@"invert"] boolValue]) {
    fine = -fine;
  }
  [self change:startValue + static_cast<float>(moved / scale / distance) * range * fine];
}
- (void)keyDown:(NSEvent *)event {
  BOOL fine = (event.modifierFlags & NSEventModifierFlagShift) != 0;
  unsigned short key = event.keyCode;
  if ([spec[@"readonly"] boolValue]) {
    [super keyDown:event];
  } else if (behaviour != vs_layout_behaviour::continuous &&
             (key == 49 || key == 36 || key == 76)) {
    [self press];
  } else if (key == 126 || key == 124) {
    [self stepBy:1 fine:fine];
  } else if (key == 125 || key == 123) {
    [self stepBy:-1 fine:fine];
  } else if (behaviour != vs_layout_behaviour::continuous) {
    [super keyDown:event];
  } else if (key == 116 || key == 121) {
    [self change:[self value] + (key == 116 ? 10 : -10) * [self keyStep]];
  } else if (key == 115 || key == 119) {
    [self change:key == 115 ? [self minimum] : [self maximum]];
  } else if (key == 51 || key == 117) {
    [self change:[spec[@"initial"] floatValue]];
  } else {
    [super keyDown:event];
  }
}
- (void)commit:(id)sender {
  float value = 0;
  if ([self parse:field.stringValue value:&value]) {
    [self change:value];
  }
  field.stringValue = [self displayValue];
}
- (void)controlTextDidEndEditing:(NSNotification *)notification {
  if (field && notification.object == field) {
    [self refresh];
    return;
  }
  float value = 0;
  if ([self parse:editor.stringValue value:&value]) {
    [self change:value];
  }
  [editor removeFromSuperview];
  editor = nil;
  self.needsDisplay = YES;
}
- (BOOL)control:(NSControl *)control
               textView:(NSTextView *)textView
    doCommandBySelector:(SEL)selector {
  if (selector != @selector(cancelOperation:)) {
    return NO;
  }
  if (field && control == field) {
    [field abortEditing];
    [self.window makeFirstResponder:nil];
    [self refresh];
    return YES;
  }
  editor.delegate = nil;
  [editor removeFromSuperview];
  editor = nil;
  [self.window makeFirstResponder:self];
  return YES;
}
@end

#if VS_LAYOUT_FILE_WIDGETS
@interface VSArtworkFile : NSButton
@property(nonatomic) NSRect frameRect;
@property(nonatomic) uint32_t slot;
@property(copy) NSString *placeholder;
@property(copy) NSString * (^fileName)(uint32_t);
@property(copy) void (^fileAction)(uint32_t, NSView *);
- (void)refresh;
@end

@implementation VSArtworkFile
- (void)refresh {
  NSString *name = self.fileName ? self.fileName(self.slot) : nil;
  self.title = name.length ? name : self.placeholder.length ? self.placeholder : @"Choose File…";
  self.toolTip = self.title;
  self.enabled = self.fileAction != nil;
}
- (void)mouseDown:(NSEvent *)event {
  if (self.fileAction) {
    self.fileAction(self.slot, self);
  }
}
- (BOOL)accessibilityPerformPress {
  if (!self.fileAction) {
    return NO;
  }
  self.fileAction(self.slot, self);
  return YES;
}
@end
#endif

@interface VS_LAYOUT_VIEW : NSView {
  NSDictionary *layout;
  NSImage *background, *labels;
  NSRect crop, content;
}
- (instancetype)initWithHost:(const vs_module_ui_host *)host
                   directory:(NSString *)directory
                      layout:(NSString *)name
              requireVersion:(BOOL)requireVersion
                       error:(NSString **)error;
// parameters, when given, describe how the module shows its parameter values; widgets that specify
// no text of their own show values that way.
- (instancetype)initWithHost:(const vs_module_ui_host *)host
                   directory:(NSString *)directory
                      layout:(NSString *)name
              requireVersion:(BOOL)requireVersion
                  parameters:(const std::vector<vespera::ParameterDisplay> *)parameters
                       error:(NSString **)error;
- (void)refresh;
@end

@implementation VS_LAYOUT_VIEW
- (instancetype)initWithHost:(const vs_module_ui_host *)host
                   directory:(NSString *)directory
                      layout:(NSString *)name
              requireVersion:(BOOL)requireVersion
                       error:(NSString **)error {
  return [self initWithHost:host
                  directory:directory
                     layout:name
             requireVersion:requireVersion
                 parameters:nullptr
                      error:error];
}
- (instancetype)initWithHost:(const vs_module_ui_host *)host
                   directory:(NSString *)directory
                      layout:(NSString *)name
              requireVersion:(BOOL)requireVersion
                  parameters:(const std::vector<vespera::ParameterDisplay> *)parameters
                       error:(NSString **)error {
  self = [super initWithFrame:NSZeroRect];
  if (self) {
    NSDictionary<NSString *, NSImage *> *images = nil;
    layout = vs_layout_load(directory, name, requireVersion, &images, error);
    if (!layout) {
      return nil;
    }
    NSArray *r = layout[@"crop"];
    crop =
        NSMakeRect([r[0] doubleValue], [r[1] doubleValue], [r[2] doubleValue], [r[3] doubleValue]);
    content = crop;
    NSArray *insets = layout[@"visualInsets"];
    if (insets.count == 4) {
      crop.origin.x -= [insets[1] doubleValue];
      crop.origin.y -= [insets[0] doubleValue];
      crop.size.width += [insets[1] doubleValue] + [insets[3] doubleValue];
      crop.size.height += [insets[0] doubleValue] + [insets[2] doubleValue];
    }
    self.frame = NSMakeRect(0, 0, crop.size.width, crop.size.height);
    self.toolTip = layout[@"help"];
    background = images[layout[@"background"]];
    labels = images[layout[@"labels"]];
    NSInteger version = [layout[@"version"] integerValue];
    CGFloat ratio = layout[@"pixelRatio"] ? [layout[@"pixelRatio"] doubleValue] : 1;
    for (NSDictionary *spec in layout[@"widgets"]) {
#if VS_LAYOUT_FILE_WIDGETS
      if ([spec[@"type"] isEqualToString:@"file"]) {
        VSArtworkFile *button = [[VSArtworkFile alloc] initWithFrame:NSZeroRect];
        NSArray *rect = spec[@"rect"];
        button.frameRect = NSMakeRect([rect[0] doubleValue], [rect[1] doubleValue],
                                      [rect[2] doubleValue], [rect[3] doubleValue]);
        button.slot = [spec[@"slot"] unsignedIntValue];
        button.placeholder = spec[@"placeholder"];
        button.bezelStyle = NSBezelStyleRounded;
        [self addSubview:button];
        continue;
      }
#endif
      VSArtworkControl *control =
          [[VSArtworkControl alloc] initWithSpec:spec
                                            host:vs_module_ui_copy_host(host)
                                          images:images
                                         version:version
                                      pixelRatio:ratio
                                       described:vs_layout_described(spec, version, parameters)];
      [self addSubview:control];
    }
    [self refresh];
  }
  return self;
}
- (BOOL)isFlipped {
  return YES;
}
- (CGFloat)scale {
  return std::min(self.bounds.size.width / crop.size.width,
                  self.bounds.size.height / crop.size.height);
}
- (NSPoint)origin {
  CGFloat height = crop.size.height * [self scale];
  NSString *anchor = layout[@"anchor"];
  return NSMakePoint((self.bounds.size.width - crop.size.width * [self scale]) / 2,
                     [anchor isEqualToString:@"top"]      ? 0
                     : [anchor isEqualToString:@"bottom"] ? self.bounds.size.height - height
                                                          : (self.bounds.size.height - height) / 2);
}
- (NSRect)placeRect:(NSRect)r {
  CGFloat scale = [self scale];
  NSPoint origin = [self origin];
  return NSMakeRect(origin.x + (r.origin.x - crop.origin.x) * scale,
                    origin.y + (r.origin.y - crop.origin.y) * scale, r.size.width * scale,
                    r.size.height * scale);
}
- (NSRect)place:(NSArray *)r {
  return [self placeRect:NSMakeRect([r[0] doubleValue], [r[1] doubleValue], [r[2] doubleValue],
                                    [r[3] doubleValue])];
}
- (void)layout {
  [super layout];
  CGFloat scale = [self scale], shared = CGFLOAT_MAX;
  NSMutableArray<VSArtworkControl *> *fields = [NSMutableArray array];
  for (NSView *view in self.subviews) {
#if VS_LAYOUT_FILE_WIDGETS
    if ([view isKindOfClass:VSArtworkFile.class]) {
      VSArtworkFile *button = (VSArtworkFile *)view;
      button.frame = [self placeRect:button.frameRect];
      button.font =
          [NSFont systemFontOfSize:std::max(1.0, std::min(14.0, button.bounds.size.height * .55))];
      continue;
    }
#endif
    if (![view isKindOfClass:VSArtworkControl.class]) {
      continue;
    }
    VSArtworkControl *control = (VSArtworkControl *)view;
    control.frame = [self placeRect:control->frameRect];
    if ([control field]) {
      [fields addObject:control];
    }
  }
  // Every fitted readout shares one size, the largest at which all of them fit, so they read as
  // one row of text.
  for (VSArtworkControl *control in fields) {
    if ([control fits]) {
      CGFloat size = [control textSize] * scale;
      shared = std::min(
          {shared, size, [control fitSize:size width:control->frameRect.size.width * scale]});
    }
  }
  for (VSArtworkControl *control in fields) {
    CGFloat size = [control textSize];
    [control setFieldFontSize:[control fits] ? shared
                              : size < 0     ? std::min(14.0, control.bounds.size.height * .7)
                                             : size * scale];
  }
}
- (void)drawRect:(NSRect)dirtyRect {
  [super drawRect:dirtyRect];
  NSGraphicsContext.currentContext.imageInterpolation =
      vs_layout_interpolation(layout[@"interpolation"]);
  NSArray *labelsRect = layout[@"labelsRect"] ? layout[@"labelsRect"] : @[ @0, @0, @1024, @750 ];
  NSRect rect = [self place:labelsRect];
  NSRect art = [self place:@[ @0, @0, @1024, @1 ]];
  art.size.height = background.size.height / background.size.width * art.size.width;
  if (layout[@"backgroundRect"]) {
    art = [self place:layout[@"backgroundRect"]];
  }
  [background drawInRect:art
                fromRect:NSZeroRect
               operation:NSCompositingOperationSourceOver
                fraction:1
          respectFlipped:YES
                   hints:nil];
  [NSGraphicsContext saveGraphicsState];
  NSRectClip([self place:@[
    @(content.origin.x), @(content.origin.y), @(content.size.width), @(content.size.height)
  ]]);
  [labels drawInRect:rect
            fromRect:NSZeroRect
           operation:NSCompositingOperationSourceOver
            fraction:1
      respectFlipped:YES
               hints:nil];
  [NSGraphicsContext restoreGraphicsState];
}
- (void)refresh {
  for (VSArtworkControl *control in self.subviews) {
    [control refresh];
  }
  self.needsDisplay = YES;
}
@end
