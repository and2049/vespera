#pragma once

/* Canonical vespera UI design tokens, shared by the SwiftUI host (through the
   ModuleSDK module map) and AppKit module controls (module_controls_macos.h).
   Plain C so standalone module packages need only VESPERA_SDK_ROOT/include. */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vs_ui_gray {
  double white;
  double alpha;
} vs_ui_gray;

typedef struct vs_ui_rgb {
  double red;
  double green;
  double blue;
} vs_ui_rgb;

typedef enum vs_ui_size { VS_UI_SIZE_STANDARD = 0, VS_UI_SIZE_COMPACT = 1 } vs_ui_size;

typedef struct vs_ui_metrics {
  double control_height;
  double font_size;
  double label_font_size;
  double value_font_size;
  double title_font_size;
  double corner_radius;
  double horizontal_padding;
  double label_gap;
  double field_gap;
  double menu_row_height;
  double checkbox_size;
  double checkbox_radius;
  double icon_size;
  double panel_padding;
  double stack_spacing;
} vs_ui_metrics;

static const vs_ui_gray VS_UI_SURFACE_WINDOW = {0.09, 1};
static const vs_ui_gray VS_UI_SURFACE_PANEL = {0.17, 1};
static const vs_ui_gray VS_UI_SURFACE_HEADER = {0.12, 1};
static const vs_ui_gray VS_UI_SURFACE_INSET = {0.065, 1};
static const vs_ui_gray VS_UI_SURFACE_INSET_HOVER = {0.09, 1};
static const vs_ui_gray VS_UI_SURFACE_HIGHLIGHT = {0.15, 1};
static const vs_ui_gray VS_UI_SURFACE_CONTROL = {0.11, 1};
static const vs_ui_gray VS_UI_SURFACE_PLACEHOLDER = {0.13, 1};
static const vs_ui_gray VS_UI_SURFACE_TRACK = {0.24, 1};
static const vs_ui_gray VS_UI_SURFACE_RAISED = {0.2, 1};
static const vs_ui_gray VS_UI_HOVER_WASH = {1, 0.025};
static const vs_ui_gray VS_UI_SEPARATOR = {0.19, 1};

/* Rotary dial and level meter roles shared by host and module controls. */
static const vs_ui_gray VS_UI_DIAL_TRACK = {0.19, 1};
static const vs_ui_gray VS_UI_DIAL_FACE = {0.21, 1};
static const vs_ui_gray VS_UI_DIAL_RIM = {0.4, 1};
static const vs_ui_gray VS_UI_METER_TRACK = {0.18, 1};
static const vs_ui_gray VS_UI_METER_LEVEL = {1, 0.85};

static const vs_ui_gray VS_UI_BORDER = {0.25, 1};
static const vs_ui_gray VS_UI_BORDER_HOVER = {1, 0.16};
static const vs_ui_gray VS_UI_BORDER_SUBTLE = {1, 0.12};
static const vs_ui_gray VS_UI_DIVIDER = {1, 0.08};
static const vs_ui_gray VS_UI_FOCUS = {1, 0.5};
static const vs_ui_gray VS_UI_SCRIM = {0, 0.55};

static const vs_ui_gray VS_UI_TEXT_PRIMARY = {0.88, 1};
static const vs_ui_gray VS_UI_TEXT_LABEL = {0.75, 1};
static const vs_ui_gray VS_UI_TEXT_SECONDARY = {0.55, 1};
static const vs_ui_gray VS_UI_TEXT_MUTED = {0.35, 1};
static const vs_ui_gray VS_UI_TEXT_ON_FILL = {0.12, 1};

static const vs_ui_gray VS_UI_FILL_STRONG = {0.92, 1};
static const vs_ui_gray VS_UI_FILL_OFF_BORDER = {0.4, 1};

static const vs_ui_rgb VS_UI_STATUS_WARNING = {1, 0.62, 0.04};
static const vs_ui_rgb VS_UI_STATUS_CLIP = {1, 0.23, 0.19};

static const double VS_UI_OPACITY_DISABLED = 0.4;
static const double VS_UI_OPACITY_PRESSED = 0.7;
static const double VS_UI_BORDER_WIDTH = 1;
static const double VS_UI_FOCUS_WIDTH = 1.5;
static const double VS_UI_MENU_INSET = 4;
static const double VS_UI_MENU_MAX_HEIGHT = 212;
static const double VS_UI_MENU_OFFSET = 4;
static const double VS_UI_PANEL_RADIUS = 12;
/* Uppercase captions above host strip controls. */
static const double VS_UI_CAPTION_FONT_SIZE = 11;

static const vs_ui_metrics VS_UI_METRICS_STANDARD = {
    .control_height = 36,
    .font_size = 14,
    .label_font_size = 13,
    .value_font_size = 13,
    .title_font_size = 18,
    .corner_radius = 8,
    .horizontal_padding = 12,
    .label_gap = 8,
    .field_gap = 20,
    .menu_row_height = 34,
    .checkbox_size = 20,
    .checkbox_radius = 4,
    .icon_size = 18,
    .panel_padding = 20,
    .stack_spacing = 16,
};
static const vs_ui_metrics VS_UI_METRICS_COMPACT = {
    .control_height = 26,
    .font_size = 12,
    .label_font_size = 10,
    .value_font_size = 10,
    .title_font_size = 13,
    .corner_radius = 6,
    .horizontal_padding = 8,
    .label_gap = 6,
    .field_gap = 12,
    .menu_row_height = 26,
    .checkbox_size = 16,
    .checkbox_radius = 3,
    .icon_size = 14,
    .panel_padding = 16,
    .stack_spacing = 12,
};

static inline vs_ui_metrics vs_ui_metrics_for(vs_ui_size size) {
  return size == VS_UI_SIZE_COMPACT ? VS_UI_METRICS_COMPACT : VS_UI_METRICS_STANDARD;
}

/* Width reserved at a dropdown's trailing edge for its chevron, so titles
   truncate before reaching it. */
static inline double vs_ui_dropdown_accessory_width(vs_ui_size size) {
  return vs_ui_metrics_for(size).horizontal_padding + 12;
}

typedef struct vs_ui_labelled_field {
  double label_x;
  double label_width;
  double field_x;
  double field_width;
} vs_ui_labelled_field;

/* Places label/field pairs left to right between start and end. Each field
   follows its label by label_gap; spare width is shared equally between
   pairs, never below min_pair_gap. Returns 0 without writing when the row
   cannot fit, so callers fail visibly instead of overlapping controls. */
static inline int vs_ui_layout_labelled_row(double start, double end, double label_gap,
                                            double min_pair_gap, const double *label_widths,
                                            const double *field_widths, size_t count,
                                            vs_ui_labelled_field *out) {
  if (count == 0 || end < start) {
    return 0;
  }
  double content = 0;
  for (size_t i = 0; i < count; ++i) {
    if (label_widths[i] < 0 || field_widths[i] < 0) {
      return 0;
    }
    content += label_widths[i] + label_gap + field_widths[i];
  }
  double gaps = (double)(count - 1);
  double spare = (end - start) - content;
  if (spare < min_pair_gap * gaps) {
    return 0;
  }
  double pair_gap = count > 1 ? spare / gaps : 0;
  double x = start;
  for (size_t i = 0; i < count; ++i) {
    out[i].label_x = x;
    out[i].label_width = label_widths[i];
    out[i].field_x = x + label_widths[i] + label_gap;
    out[i].field_width = field_widths[i];
    x = out[i].field_x + field_widths[i] + pair_gap;
  }
  return 1;
}

#ifdef __cplusplus
}
#endif
