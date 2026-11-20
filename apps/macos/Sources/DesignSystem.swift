import AppKit
import ModuleSDK
import SwiftUI

/// SwiftUI adapter for the canonical tokens in `include/vespera/ui_tokens.h`.
enum UISize: Sendable {
  case standard
  case compact

  var metrics: vs_ui_metrics {
    vs_ui_metrics_for(self == .compact ? VS_UI_SIZE_COMPACT : VS_UI_SIZE_STANDARD)
  }
  var dropdownAccessoryWidth: CGFloat {
    vs_ui_dropdown_accessory_width(self == .compact ? VS_UI_SIZE_COMPACT : VS_UI_SIZE_STANDARD)
  }
}

extension Color {
  init(_ gray: vs_ui_gray) { self.init(white: gray.white, opacity: gray.alpha) }
  init(_ rgb: vs_ui_rgb) { self.init(.sRGB, red: rgb.red, green: rgb.green, blue: rgb.blue) }
}

extension NSColor {
  convenience init(_ gray: vs_ui_gray) { self.init(white: gray.white, alpha: gray.alpha) }
}

enum Theme {
  static let window = Color(VS_UI_SURFACE_WINDOW)
  static let panel = Color(VS_UI_SURFACE_PANEL)
  static let header = Color(VS_UI_SURFACE_HEADER)
  static let inset = Color(VS_UI_SURFACE_INSET)
  static let insetHover = Color(VS_UI_SURFACE_INSET_HOVER)
  static let highlight = Color(VS_UI_SURFACE_HIGHLIGHT)
  static let control = Color(VS_UI_SURFACE_CONTROL)
  static let placeholder = Color(VS_UI_SURFACE_PLACEHOLDER)
  static let track = Color(VS_UI_SURFACE_TRACK)
  static let raised = Color(VS_UI_SURFACE_RAISED)
  static let hoverWash = Color(VS_UI_HOVER_WASH)
  static let separator = Color(VS_UI_SEPARATOR)
  static let dialTrack = Color(VS_UI_DIAL_TRACK)
  static let dialFace = Color(VS_UI_DIAL_FACE)
  static let meterTrack = Color(VS_UI_METER_TRACK)
  static let meterLevel = Color(VS_UI_METER_LEVEL)
  static let borderSubtle = Color(VS_UI_BORDER_SUBTLE)
  static let muted = Color(VS_UI_TEXT_MUTED)
  static let border = Color(VS_UI_BORDER)
  static let borderHover = Color(VS_UI_BORDER_HOVER)
  static let divider = Color(VS_UI_DIVIDER)
  static let focus = Color(VS_UI_FOCUS)
  static let scrim = Color(VS_UI_SCRIM)
  static let primary = Color(VS_UI_TEXT_PRIMARY)
  static let label = Color(VS_UI_TEXT_LABEL)
  static let secondary = Color(VS_UI_TEXT_SECONDARY)
  static let onFill = Color(VS_UI_TEXT_ON_FILL)
  static let fill = Color(VS_UI_FILL_STRONG)
  static let offBorder = Color(VS_UI_FILL_OFF_BORDER)
  static let warning = Color(VS_UI_STATUS_WARNING)
  static let clip = Color(VS_UI_STATUS_CLIP)

  static let disabledOpacity = VS_UI_OPACITY_DISABLED
  static let pressedOpacity = VS_UI_OPACITY_PRESSED
  static let borderWidth = CGFloat(VS_UI_BORDER_WIDTH)
  static let focusWidth = CGFloat(VS_UI_FOCUS_WIDTH)
  static let menuInset = CGFloat(VS_UI_MENU_INSET)
  static let menuMaxHeight = CGFloat(VS_UI_MENU_MAX_HEIGHT)
  static let menuOffset = CGFloat(VS_UI_MENU_OFFSET)
  static let panelRadius = CGFloat(VS_UI_PANEL_RADIUS)
  static let captionSize = VS_UI_CAPTION_FONT_SIZE

  static func font(_ size: Double, weight: Font.Weight = .medium) -> Font {
    .system(size: size, weight: weight)
  }
  static func valueFont(_ size: Double) -> Font {
    .system(size: size, weight: .regular).monospacedDigit()
  }

  /// Chevrons inside settings dropdowns and their menu rows: two points below label text.
  static func chevronFont(_ metrics: vs_ui_metrics) -> Font {
    .system(size: metrics.label_font_size - 2, weight: .semibold)
  }

  /// A disclosure chevron one point below label text, such as the tempo panel's toggle.
  static func disclosureChevronFont(_ metrics: vs_ui_metrics) -> Font {
    .system(size: metrics.label_font_size - 1, weight: .semibold)
  }

  /// The chevron separating a pedal board's chain chips.
  static func chainChevronFont() -> Font { .system(size: 9) }

  /// SF symbol icons sized from the current UI metrics' icon size.
  static func iconFont(_ metrics: vs_ui_metrics, grow: Double = 0, weight: Font.Weight = .light)
    -> Font
  {
    .system(size: metrics.icon_size + grow, weight: weight)
  }
}

private struct UISizeKey: EnvironmentKey {
  static let defaultValue = UISize.standard
}

extension EnvironmentValues {
  var uiSize: UISize {
    get { self[UISizeKey.self] }
    set { self[UISizeKey.self] = newValue }
  }
}

extension View {
  /// Selects standard or compact metrics for every design-system control below this view.
  func uiSize(_ size: UISize) -> some View { environment(\.uiSize, size) }

  /// Inline status or error text beneath host controls.
  func statusMessage() -> some View {
    font(Theme.font(UISize.compact.metrics.font_size)).foregroundStyle(Theme.secondary)
  }

  func focusRing(_ visible: Bool, radius: CGFloat) -> some View {
    overlay {
      RoundedRectangle(cornerRadius: radius)
        .strokeBorder(Theme.focus.opacity(visible ? 1 : 0), lineWidth: Theme.focusWidth)
    }
  }
}

/// Check-mark geometry shared with `vs_ui_draw_check` in `module_controls_macos.h`.
struct CheckMark: Shape {
  func path(in rect: CGRect) -> Path {
    let unit = rect.width / 20
    var path = Path()
    path.move(to: CGPoint(x: rect.minX + 5.5 * unit, y: rect.minY + 10.5 * unit))
    path.addLine(to: CGPoint(x: rect.minX + 8.5 * unit, y: rect.minY + 13.5 * unit))
    path.addLine(to: CGPoint(x: rect.minX + 14.5 * unit, y: rect.minY + 6.5 * unit))
    return path.strokedPath(
      StrokeStyle(lineWidth: max(1.5, 2 * unit), lineCap: .round, lineJoin: .round))
  }
}
