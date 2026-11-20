import AppKit
import UniformTypeIdentifiers

@MainActor
private final class FileMenuItem: NSMenuItem {
  let perform: () -> Void
  init(_ title: String, perform: @escaping () -> Void) {
    self.perform = perform
    super.init(title: title, action: #selector(invoke), keyEquivalent: "")
    target = self
  }
  required init(coder: NSCoder) { fatalError("init(coder:) is unavailable") }
  @objc private func invoke() { perform() }
}

@MainActor
enum ModuleFileMenu {
  static func name(_ module: LoadedModule, slot: UInt32) -> String {
    if module.isImportingFile(slot) { return "Importing…" }
    guard let file = module.files.first(where: { $0.slot == slot }) else { return "" }
    return missing(module, file) ? "\(file.name) — Missing / Locate…" : file.name
  }

  static func missing(_ module: LoadedModule, _ file: ModuleFileReference) -> Bool {
    guard let path = try? FileLibrary.resolvedPath(file, resources: module.resourceDirectory) else {
      return true
    }
    let info = try? path.resourceValues(forKeys: [.isRegularFileKey, .fileSizeKey])
    return info?.isRegularFile != true || info?.fileSize.map(UInt64.init) != file.bytes
      || module.fileProblems[file.slot] != nil
  }

  static func show(_ module: LoadedModule, slot id: UInt32, anchor: NSView) {
    guard let slot = module.fileSlots.first(where: { $0.id == id }) else { return }
    let current = module.files.first { $0.slot == id }
    let menu = NSMenu(title: slot.name)
    if module.isImportingFile(id) {
      menu.addItem(FileMenuItem("Cancel Import") { [weak module] in module?.cancelFileImport(id) })
      menu.addItem(.separator())
    }
    if let directory = slot.bundled,
      let root = try? FileLibrary.contained(directory, in: module.resourceDirectory),
      let entries = try? FileLibrary.boundedEntries(in: root, limit: 256)
    {
      for file in entries.sorted(by: { $0.lastPathComponent < $1.lastPathComponent }).prefix(256)
      where slot.types.contains(file.pathExtension.lowercased()) {
        menu.addItem(
          FileMenuItem(file.lastPathComponent) { [weak module] in
            guard let module else { return }
            let resources = module.resourceDirectory
            module.importFile(
              id,
              operation: {
                try await FileLibrary.shared.bundledFile(file, resources: resources, slot: slot)
              }, reportError: present)
          })
      }
      if !menu.items.isEmpty { menu.addItem(.separator()) }
    }
    menu.addItem(
      FileMenuItem("Choose File…") { [weak module] in
        guard let module else { return }
        choose(module, slot: slot, locating: nil)
      })
    if let current {
      if missing(module, current) {
        menu.addItem(
          FileMenuItem("Locate…") { [weak module] in
            guard let module else { return }
            choose(module, slot: slot, locating: current)
          })
      }
      if let path = try? FileLibrary.resolvedPath(current, resources: module.resourceDirectory) {
        menu.addItem(
          FileMenuItem("Show in Finder") {
            NSWorkspace.shared.activateFileViewerSelecting([path])
          })
      }
    }
    if slot.optional {
      menu.addItem(
        FileMenuItem("Clear") { [weak module] in
          do { try module?.setFile(id, nil) } catch { present(error) }
        })
    }
    menu.popUp(positioning: nil, at: NSPoint(x: 0, y: anchor.bounds.height), in: anchor)
  }

  private static func choose(
    _ module: LoadedModule, slot: ModuleFileSlot, locating: ModuleFileReference?
  ) {
    let panel = NSOpenPanel()
    panel.canChooseFiles = true
    panel.canChooseDirectories = false
    panel.allowsMultipleSelection = false
    panel.allowedContentTypes = slot.types.compactMap { UTType(filenameExtension: $0) }
    panel.message = locating == nil ? slot.name : "Locate \(locating!.name)"
    panel.begin { response in
      guard response == .OK, let url = panel.url else { return }
      Task { @MainActor in
        module.importFile(
          slot.id,
          operation: {
            try await FileLibrary.shared.importFile(url, slot: slot)
          },
          confirm: { reference in
            if let locating, reference.sha256 != locating.sha256 {
              let alert = NSAlert()
              alert.messageText = "Replace \(locating.name)?"
              alert.informativeText =
                "The chosen file has different contents. The preset will reference the new file."
              alert.addButton(withTitle: "Replace")
              alert.addButton(withTitle: "Cancel")
              return alert.runModal() == .alertFirstButtonReturn
            }
            return true
          }, reportError: present)
      }
    }
  }

  private static func present(_ error: Error) {
    let alert = NSAlert(error: error)
    alert.runModal()
  }
}
