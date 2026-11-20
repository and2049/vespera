import ModuleSDK
import NativeGallery
import SwiftUI

/// Reference sheet for the shared design system: SwiftUI host components beside the AppKit
/// module controls, both driven by `ui_tokens.h`. Launch with `VESPERA_COMPONENT_GALLERY=1`.
struct ComponentGallery: View {
  @State private var expanded: String?
  @State private var dropdown = ["standard": "short", "compact": "short"]
  @State private var checked = true
  @State private var segment = "Mappings"
  @State private var number = 64
  @State private var level = -18.0

  private static let swatches: [(String, Color)] = [
    ("window", Theme.window), ("panel", Theme.panel), ("header", Theme.header),
    ("inset", Theme.inset), ("inset hover", Theme.insetHover), ("highlight", Theme.highlight),
    ("control", Theme.control), ("placeholder", Theme.placeholder), ("track", Theme.track),
    ("border", Theme.border), ("primary", Theme.primary), ("label", Theme.label),
    ("secondary", Theme.secondary), ("fill", Theme.fill), ("warning", Theme.warning),
    ("clip", Theme.clip),
  ]

  var body: some View {
    ScrollView {
      VStack(alignment: .leading, spacing: UISize.standard.metrics.panel_padding) {
        LazyVGrid(columns: Array(repeating: GridItem(.fixed(64)), count: 8), spacing: 8) {
          ForEach(Self.swatches, id: \.0) { name, color in
            VStack(spacing: 4) {
              RoundedRectangle(cornerRadius: UISize.compact.metrics.corner_radius).fill(color)
                .overlay {
                  RoundedRectangle(cornerRadius: UISize.compact.metrics.corner_radius)
                    .strokeBorder(Theme.divider, lineWidth: Theme.borderWidth)
                }
                .frame(height: 32)
              SettingsLabel(name)
            }
          }
        }.uiSize(.compact)
        HStack(alignment: .top, spacing: UISize.standard.metrics.panel_padding) {
          column(.standard, title: "Standard")
          column(.compact, title: "Compact")
          SettingsPanel(title: "AppKit module controls") {
            NativeControls().frame(width: 580, height: 380)
          }.uiSize(.compact)
        }
      }
      .padding(UISize.standard.metrics.panel_padding)
    }
    .background(Theme.window)
    .settingsDropdownOverlay()
    .preferredColorScheme(.dark)
  }

  private func column(_ size: UISize, title: String) -> some View {
    let key = size == .compact ? "compact" : "standard"
    return SettingsPanel(title: title) {
      SettingsIconButton(systemName: "xmark", label: "Close") {}
    } content: {
      SettingsField(label: "Dropdown") {
        SettingsDropdown(
          title: "\(title) dropdown",
          options: [
            SettingsOption(id: "short", title: "Condenser 184"),
            SettingsOption(id: "long", title: "A very long device name that must truncate cleanly"),
          ],
          selection: Binding(get: { dropdown[key] ?? "short" }, set: { dropdown[key] = $0 }),
          expanded: $expanded)
      }
      SettingsField(label: "Disabled dropdown") {
        SettingsDropdown(
          title: "\(title) disabled", options: [], selection: .constant(""),
          expanded: $expanded, placeholder: "No MIDI sources available")
      }
      HStack(spacing: size.metrics.field_gap) {
        Toggle("Checkbox", isOn: $checked).toggleStyle(SettingsCheckboxStyle())
        Toggle("Disabled", isOn: .constant(true)).toggleStyle(SettingsCheckboxStyle())
          .disabled(true)
      }
      HStack(spacing: size.metrics.label_gap) {
        Button("Primary") {}.buttonStyle(SettingsButtonStyle(primary: true))
        Button("Secondary") {}.buttonStyle(SettingsButtonStyle())
        Button("Disabled") {}.buttonStyle(SettingsButtonStyle()).disabled(true)
      }
      SettingsSegmentedControl(
        title: "\(title) segments",
        options: ["Input", "Mappings"].map { SettingsOption(id: $0, title: $0) },
        selection: $segment
      ).fixedSize()
      HStack(spacing: size.metrics.label_gap) {
        SettingsNumberField(label: "Number", value: $number, range: 0...127).frame(width: 86)
        SettingsCell { Text("Read only") }
        SettingsEmptyCell().frame(width: 64)
      }
      SettingsSlider(label: "\(title) slider", value: $level, range: -60...0)
      SettingsDivider()
      Text("Status message").statusMessage()
    }
    .uiSize(size)
    .frame(width: 360)
  }
}

private struct NativeControls: NSViewRepresentable {
  func makeNSView(context: Context) -> NSView { VesperaNativeControlGallery() }
  func updateNSView(_ view: NSView, context: Context) {}
}

@MainActor
enum ComponentGalleryWindow {
  private static var window: NSWindow?

  static func openIfRequested() {
    guard ProcessInfo.processInfo.environment["VESPERA_COMPONENT_GALLERY"] == "1" else { return }
    let window = NSWindow(
      contentRect: NSRect(x: 0, y: 0, width: 1420, height: 900),
      styleMask: [.titled, .closable, .resizable], backing: .buffered, defer: false)
    window.title = "vespera component gallery"
    window.appearance = NSAppearance(named: .darkAqua)
    window.backgroundColor = NSColor(VS_UI_SURFACE_WINDOW)
    window.contentView = NSHostingView(rootView: ComponentGallery())
    window.isReleasedWhenClosed = false
    window.center()
    window.makeKeyAndOrderFront(nil)
    self.window = window
  }
}
