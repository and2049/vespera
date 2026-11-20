import AppKit
import SwiftUI
import UniformTypeIdentifiers

/// The preset header: previous/next, Delete, Save and Save As above the preset dropdown. The
/// more button swaps the buttons for Import and Reset.
struct PresetControls: View {
  static let defaultOption = "default"
  static let menuTitle = "Preset"
  @ObservedObject var presets: PresetController
  @Environment(\.presetControlsPresentation) private var presentation
  @State private var expanded: String?
  @State private var showsActions = false

  private var options: [SettingsOption] {
    [SettingsOption(id: Self.defaultOption, title: PresetStore.defaultName)]
      + presets.entries.map { entry in
        SettingsOption(
          id: entry.id.uuidString,
          title: presets.selection == .named(entry.id) ? presets.title : entry.name)
      }
  }

  private var selectionID: Binding<String> {
    Binding(
      get: {
        switch presets.selection {
        case .defaultPreset: Self.defaultOption
        case .named(let id): id.uuidString
        }
      },
      set: { id in
        presets.select(UUID(uuidString: id).map { .named($0) } ?? .defaultPreset)
      })
  }

  var body: some View {
    VStack(spacing: 22) {
      HStack(spacing: 13) {
        if showsActions {
          Spacer(minLength: 8)
          action("IMPORT", enabled: true, help: "Import a preset file") { presets.importPreset() }
          Rectangle().fill(Theme.separator).frame(width: 1, height: 18)
          action(
            "RESET", enabled: !presets.isDefault,
            help: "Load the saved Default into this preset; Save keeps it"
          ) { presets.reset() }
          iconButton("xmark", label: "Close preset actions") { showsActions = false }
        } else {
          iconButton(
            "arrow.left", label: "Previous preset", enabled: presets.order.count > 1
          ) { presets.step(-1) }
          Rectangle().fill(Theme.separator).frame(width: 1, height: 18)
          iconButton("arrow.right", label: "Next preset", enabled: presets.order.count > 1) {
            presets.step(1)
          }
          Spacer(minLength: 8)
          action("DELETE", enabled: !presets.isDefault, help: "Delete this preset") {
            presets.delete()
          }
          action("SAVE", enabled: presets.canSave, help: "Save changes to this preset") {
            presets.save()
          }
          action("SAVE AS…", enabled: true, help: "Save the current settings as a new preset") {
            presets.saveAs()
          }
          iconButton("ellipsis", label: "More preset actions", rotated: true) {
            showsActions = true
          }
        }
      }
      .frame(height: 18)

      SettingsDropdown(
        title: Self.menuTitle, options: options, selection: selectionID, expanded: $expanded,
        placeholder: PresetStore.defaultName, surface: .raised
      )
      .uiSize(.standard)
      .padding(.vertical, 3)
      .frame(height: 42)
    }
    .onAppear {
      showsActions = presentation.actions
      if presentation.menu { expanded = Self.menuTitle }
    }
    .overlay(alignment: .bottomLeading) {
      if let error = presets.error {
        Text(error).statusMessage().foregroundStyle(Theme.warning)
          .lineLimit(1).truncationMode(.tail)
          .help(error)
          .offset(y: 16)
          .accessibilityLabel("Preset error: \(error)")
      } else if let notice = presets.notice {
        Text(notice).statusMessage()
          .lineLimit(1).truncationMode(.tail)
          .help(notice)
          .offset(y: 16)
          .accessibilityLabel("Preset notice: \(notice)")
      }
    }
  }

  private func action(
    _ title: String, enabled: Bool, help: String, perform: @escaping () -> Void
  ) -> some View {
    Button(title, action: perform)
      .buttonStyle(.plain)
      .foregroundStyle(enabled ? Theme.primary : Theme.muted)
      .disabled(!enabled)
      .help(help)
      .accessibilityLabel(title.capitalized)
  }

  private func iconButton(
    _ symbol: String, label: String, enabled: Bool = true, rotated: Bool = false,
    perform: @escaping () -> Void
  ) -> some View {
    Button(action: perform) {
      Image(systemName: symbol)
        .rotationEffect(.degrees(rotated ? 90 : 0))
        .frame(minWidth: 14, minHeight: 18)
        .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
    .foregroundStyle(enabled ? Theme.primary : Theme.muted)
    .disabled(!enabled)
    .help(label)
    .accessibilityLabel(label)
  }
}

/// The presentation preset controls start with: the action row, the open menu, or neither.
struct PresetControlsPresentation {
  var actions = false
  var menu = false
}

private struct PresetControlsPresentationKey: EnvironmentKey {
  static let defaultValue = PresetControlsPresentation()
}

extension EnvironmentValues {
  var presetControlsPresentation: PresetControlsPresentation {
    get { self[PresetControlsPresentationKey.self] }
    set { self[PresetControlsPresentationKey.self] = newValue }
  }
}

extension PresetController.Prompts {
  /// Modal AppKit alerts and the open panel.
  @MainActor static let standard = PresetController.Prompts(
    unsavedChanges: { name in
      let alert = NSAlert()
      alert.messageText = "Save changes to “\(name)”?"
      alert.informativeText = "Your changes to this preset are lost if you discard them."
      alert.addButton(withTitle: "Save")
      alert.addButton(withTitle: "Cancel")
      alert.addButton(withTitle: "Discard")
      switch alert.runModal() {
      case .alertFirstButtonReturn: return .save
      case .alertThirdButtonReturn: return .discard
      default: return .cancel
      }
    },
    name: { suggestion in
      let alert = NSAlert()
      alert.messageText = "Save Preset As"
      alert.informativeText = "Name the new preset."
      let field = NSTextField(frame: NSRect(x: 0, y: 0, width: 260, height: 24))
      field.stringValue = suggestion
      alert.accessoryView = field
      alert.addButton(withTitle: "Save")
      alert.addButton(withTitle: "Cancel")
      alert.window.initialFirstResponder = field
      return alert.runModal() == .alertFirstButtonReturn ? field.stringValue : nil
    },
    confirmDelete: { name in
      let alert = NSAlert()
      alert.messageText = "Delete “\(name)”?"
      alert.informativeText = "The preset file is removed. Default is loaded instead."
      alert.addButton(withTitle: "Delete")
      alert.addButton(withTitle: "Cancel")
      alert.buttons.first?.hasDestructiveAction = true
      return alert.runModal() == .alertFirstButtonReturn
    },
    importFile: {
      let panel = NSOpenPanel()
      panel.title = "Import Preset"
      panel.canChooseDirectories = false
      panel.allowsMultipleSelection = false
      panel.allowedContentTypes =
        [UTType(filenameExtension: RigPreset.fileExtension), .json].compactMap { $0 }
      return panel.runModal() == .OK ? panel.url : nil
    })
}
