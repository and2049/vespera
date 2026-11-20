#import <AppKit/AppKit.h>

NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(NSInteger, VesperaParameterKind) {
  VesperaParameterKindContinuous,
  VesperaParameterKindToggle,
  VesperaParameterKindStepped,
  VesperaParameterKindEnumerated,
};

/// How a module shows one parameter's values, from its parameter metadata extension
/// (`vs_module_metadata_api`), formatted and parsed by the code the layout renderer uses.
@interface VesperaParameterDisplay : NSObject
/// Describes every parameter `module` (a `const vs_module_api *`) declares, in descriptor order,
/// using `metadata` (a `const vs_module_metadata_api *`, or NULL when the module exports none).
/// Fails when the metadata is invalid; it is untrusted module data.
+ (nullable NSArray<VesperaParameterDisplay *> *)displaysForModule:(const void *)module
                                                          metadata:(nullable const void *)metadata
                                                             error:(NSError **)error;
@property(readonly) uint32_t identifier;
@property(readonly) VesperaParameterKind kind;
/// Values a toggle, stepped or enumerated parameter takes; 0 for continuous ones.
@property(readonly) NSInteger steps;
/// Whether the module's metadata describes this parameter.
@property(readonly) BOOL described;
- (NSString *)textForValue:(float)value;
/// Reads text as a user types it; returns NO when it is not a value.
- (BOOL)parseText:(NSString *)text value:(float *)value;
/// The value clamped to the parameter's range and moved to the nearest value it takes.
- (float)nearestValue:(float)value;
@end

typedef BOOL (^VesperaLayoutGetValue)(uint32_t identifier, float *value);
typedef BOOL (^VesperaLayoutSetValue)(uint32_t identifier, float value);
/// Returns NO when no host tempo is available.
typedef BOOL (^VesperaLayoutGetTempo)(double *bpm);

/// Host-side renderer for packages that declare a manifest `ui` layout. It is the renderer that
/// vespera/sdk/artwork_view_macos.h embeds in module binaries, compiled with host class names.
@interface VesperaLayoutRenderer : NSObject
+ (BOOL)configureFilesInView:(NSView *)view
                       slots:(NSArray<NSNumber *> *)slots
                        name:(NSString * (^)(uint32_t slot))name
                      action:(void (^)(uint32_t slot, NSView *anchor))action
                       error:(NSError **)error;
/// Parses and validates `layout` (a path relative to `resources`) and decodes every asset it
/// references, without creating a view.
+ (BOOL)validateResources:(NSString *)resources layout:(NSString *)layout error:(NSError **)error;
+ (BOOL)validateFileSlots:(NSArray<NSNumber *> *)slots
                resources:(NSString *)resources
                   layout:(NSString *)layout
                    error:(NSError **)error;
/// Widgets that specify no value text of their own show values as `parameters` describe.
+ (nullable NSView *)viewWithResources:(NSString *)resources
                                layout:(NSString *)layout
                            parameters:(nullable NSArray<VesperaParameterDisplay *> *)parameters
                              getValue:(VesperaLayoutGetValue)getValue
                              setValue:(VesperaLayoutSetValue)setValue
                                 tempo:(VesperaLayoutGetTempo)tempo
                                 error:(NSError **)error;
+ (void)refreshView:(NSView *)view;
@end

NS_ASSUME_NONNULL_END
