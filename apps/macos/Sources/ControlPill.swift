import ModuleSDK
import SwiftUI

/// Chrome shared by the controls in the window's bottom corners: a rounded pill of plain
/// buttons, with an options panel opening above it.
extension View {
  func controlPill(_ metrics: vs_ui_metrics) -> some View {
    buttonStyle(.plain)
      .padding(.horizontal, metrics.horizontal_padding + 4)
      .frame(height: metrics.control_height + 6)
      .background(RoundedRectangle(cornerRadius: Theme.panelRadius - 2).fill(Theme.panel))
      .overlay(
        RoundedRectangle(cornerRadius: Theme.panelRadius - 2)
          .strokeBorder(Theme.divider, lineWidth: Theme.borderWidth)
      )
      .fixedSize(horizontal: true, vertical: false)
  }

  /// A corner pill's options panel: compact, fixed width, closed by Escape.
  func controlPanel(dismiss: @escaping () -> Void) -> some View {
    modifier(ControlPanelChrome(dismiss: dismiss))
  }
}

private struct ControlPanelChrome: ViewModifier {
  let dismiss: () -> Void
  @FocusState private var focused: Bool

  func body(content: Content) -> some View {
    content
      .uiSize(.compact)
      .frame(width: 248)
      .background {
        // A sibling focus anchor receives initial focus for Escape. Making the panel itself
        // focusable would report focus to every descendant button through `isFocused`.
        Color.clear.focusable().focusEffectDisabled().focused($focused)
          .accessibilityHidden(true)
      }
      .onAppear { focused = true }
      .onKeyPress(.escape) {
        dismiss()
        return .handled
      }
  }
}

/// Catches clicks outside an open corner panel and closes it.
struct ControlPanelDismissLayer: View {
  let dismiss: () -> Void

  var body: some View {
    Color.black.opacity(0.001)
      .contentShape(Rectangle())
      .onTapGesture(perform: dismiss)
      .accessibilityLabel("Dismiss control options")
  }
}

struct ControlPillDivider: View {
  let metrics: vs_ui_metrics

  var body: some View { Divider().frame(height: metrics.icon_size) }
}
