#import "LayoutRenderer.h"
#import <objc/runtime.h>
#define VS_LAYOUT_VIEW VesperaHostLayoutView
#define VS_LAYOUT_FILE_WIDGETS 1
#include <vespera/file_content.hpp>
#include <vespera/sdk/layout_view_macos.h>

@implementation VesperaParameterDisplay {
  vespera::ParameterDisplay parameter;
}

- (instancetype)initWithParameter:(vespera::ParameterDisplay)value {
  self = [super init];
  if (self) {
    parameter = std::move(value);
  }
  return self;
}

+ (NSArray<VesperaParameterDisplay *> *)displaysForModule:(const void *)module
                                                 metadata:(const void *)metadata
                                                    error:(NSError **)error {
  std::string reason;
  const auto parameters =
      vespera::describe_parameters(*static_cast<const vs_module_api *>(module),
                                   static_cast<const vs_module_metadata_api *>(metadata), &reason);
  if (!parameters) {
    if (error) {
      *error = [NSError errorWithDomain:@"dev.vespera.parameters"
                                   code:1
                               userInfo:@{NSLocalizedDescriptionKey : vs_layout_string(reason)}];
    }
    return nil;
  }
  NSMutableArray *result = [NSMutableArray arrayWithCapacity:parameters->size()];
  for (const vespera::ParameterDisplay &described : *parameters) {
    [result addObject:[[VesperaParameterDisplay alloc] initWithParameter:described]];
  }
  return result;
}

- (const vespera::ParameterDisplay &)parameter {
  return parameter;
}

- (uint32_t)identifier {
  return parameter.id;
}

- (VesperaParameterKind)kind {
  switch (parameter.kind) {
  case vespera::ParameterKind::toggle:
    return VesperaParameterKindToggle;
  case vespera::ParameterKind::stepped:
    return VesperaParameterKindStepped;
  case vespera::ParameterKind::enumerated:
    return VesperaParameterKindEnumerated;
  case vespera::ParameterKind::continuous:
    return VesperaParameterKindContinuous;
  }
  return VesperaParameterKindContinuous;
}

- (NSInteger)steps {
  return parameter.steps;
}

- (BOOL)described {
  return parameter.described;
}

- (NSString *)textForValue:(float)value {
  return vs_layout_string(parameter.text(value));
}

- (BOOL)parseText:(NSString *)text value:(float *)value {
  const std::optional<float> parsed = parameter.parse(vs_layout_utf8(text));
  if (parsed && value) {
    *value = *parsed;
  }
  return parsed.has_value();
}

- (float)nearestValue:(float)value {
  return parameter.nearest(value);
}
@end

@interface VesperaHostLayoutParameters : NSObject
@property(copy) VesperaLayoutGetValue getValue;
@property(copy) VesperaLayoutSetValue setValue;
@property(copy) VesperaLayoutGetTempo tempo;
@end

@implementation VesperaHostLayoutParameters
@end

namespace {
char parameters_key;

VesperaHostLayoutParameters *parameters(void *context) {
  return (__bridge VesperaHostLayoutParameters *)context;
}

vs_result get_value(void *context, uint32_t identifier, float *value) {
  return context && value && parameters(context).getValue(identifier, value) ? VS_OK
                                                                             : VS_ERROR_ARGUMENT;
}

vs_result set_value(void *context, uint32_t identifier, float value) {
  return context && parameters(context).setValue(identifier, value) ? VS_OK : VS_ERROR_ARGUMENT;
}

vs_result get_tempo(void *context, vs_tempo_context *tempo) {
  if (!context || !tempo || tempo->struct_size < sizeof(vs_tempo_context)) {
    return VS_ERROR_ARGUMENT;
  }
  double bpm = 120;
  tempo->flags = parameters(context).tempo(&bpm) ? VS_TEMPO_VALID : 0;
  tempo->bpm = bpm;
  tempo->beat_position = 0;
  return VS_OK;
}

NSError *layout_error(NSString *message) {
  return [NSError
      errorWithDomain:@"dev.vespera.layout"
                 code:1
             userInfo:@{NSLocalizedDescriptionKey : message ? message : @"invalid layout"}];
}
}

@implementation VesperaLayoutRenderer
+ (BOOL)validateFileSlots:(NSArray<NSNumber *> *)slots
                resources:(NSString *)resources
                   layout:(NSString *)name
                    error:(NSError **)error {
  NSString *reason = nil;
  NSDictionary *layout = nil;
  NSString *path = vs_layout_resolve(resources, name, vs_layout_max_layout_bytes, &reason);
  if (path) {
    try {
      const auto bytes =
          vespera::read_file_content(path.fileSystemRepresentation, vs_layout_max_layout_bytes);
      id parsed = [NSJSONSerialization JSONObjectWithData:[NSData dataWithBytes:bytes.data()
                                                                         length:bytes.size()]
                                                  options:0
                                                    error:nullptr];
      if ([parsed isKindOfClass:NSDictionary.class] &&
          [parsed[@"widgets"] isKindOfClass:NSArray.class] &&
          [parsed[@"widgets"] count] <= vs_layout_max_widgets) {
        layout = parsed;
      }
    } catch (const std::exception &failure) {
      reason = [NSString stringWithUTF8String:failure.what()];
    }
  }
  if (layout) {
    NSUInteger index = 0;
    for (NSDictionary *widget in layout[@"widgets"]) {
      if (![widget isKindOfClass:NSDictionary.class]) {
        reason = @"widget must be an object";
        break;
      }
      if ([widget[@"type"] isEqual:@"file"]) {
        if (!vs_layout_is_number(layout[@"version"])) {
          reason = @"file widget requires a numeric layout version";
          break;
        }
        if (!vs_layout_widget(widget, index, resources, [layout[@"version"] integerValue],
                              [NSMutableDictionary dictionary], &reason)) {
          break;
        }
        if (![slots containsObject:widget[@"slot"]]) {
          reason = @"file widget names an undeclared slot";
          break;
        }
      }
      ++index;
    }
  }
  if (!layout || reason) {
    if (error) {
      *error = layout_error(reason);
    }
    return NO;
  }
  return YES;
}
+ (BOOL)configureFilesInView:(NSView *)view
                       slots:(NSArray<NSNumber *> *)slots
                        name:(NSString * (^)(uint32_t))name
                      action:(void (^)(uint32_t, NSView *))action
                       error:(NSError **)error {
  for (NSView *child in view.subviews) {
    if (![child isKindOfClass:VSArtworkFile.class]) {
      continue;
    }
    VSArtworkFile *button = (VSArtworkFile *)child;
    if (![slots containsObject:@(button.slot)]) {
      if (error) {
        *error = layout_error(@"file widget names an undeclared slot");
      }
      return NO;
    }
    button.fileName = name;
    button.fileAction = action;
    [button refresh];
  }
  return YES;
}
+ (BOOL)validateResources:(NSString *)resources layout:(NSString *)layout error:(NSError **)error {
  NSString *reason = nil;
  if (vs_layout_load(resources, layout, YES, nil, &reason)) {
    return YES;
  }
  if (error) {
    *error = layout_error(reason);
  }
  return NO;
}

+ (NSView *)viewWithResources:(NSString *)resources
                       layout:(NSString *)layout
                   parameters:(NSArray<VesperaParameterDisplay *> *)parameters
                     getValue:(VesperaLayoutGetValue)getValue
                     setValue:(VesperaLayoutSetValue)setValue
                        tempo:(VesperaLayoutGetTempo)tempo
                        error:(NSError **)error {
  VesperaHostLayoutParameters *bridge = [[VesperaHostLayoutParameters alloc] init];
  bridge.getValue = getValue;
  bridge.setValue = setValue;
  bridge.tempo = tempo;
  vs_module_ui_host host{sizeof(vs_module_ui_host), (__bridge void *)bridge, get_value, set_value,
                         get_tempo};
  std::vector<vespera::ParameterDisplay> described;
  for (VesperaParameterDisplay *parameter in parameters) {
    described.push_back([parameter parameter]);
  }
  NSString *reason = nil;
  VesperaHostLayoutView *view = [[VesperaHostLayoutView alloc] initWithHost:&host
                                                                  directory:resources
                                                                     layout:layout
                                                             requireVersion:YES
                                                                 parameters:&described
                                                                      error:&reason];
  if (!view) {
    if (error) {
      *error = layout_error(reason);
    }
    return nil;
  }
  objc_setAssociatedObject(view, &parameters_key, bridge, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  return view;
}

+ (void)refreshView:(NSView *)view {
  if ([view isKindOfClass:VesperaHostLayoutView.class]) {
    [(VesperaHostLayoutView *)view refresh];
  }
}
@end
