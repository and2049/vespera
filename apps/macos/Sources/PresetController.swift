import Foundation

/// The preset selection and its saved baseline. Default is reserved: it is saved automatically
/// while selected and can be neither deleted nor saved explicitly. A named preset changes only
/// through Save; while its current settings differ from the saved ones it is dirty.
@MainActor
final class PresetController: ObservableObject {
  enum Decision { case save, discard, cancel }

  /// Questions put to the user, replaceable for tests.
  struct Prompts {
    /// Whether to save, discard or keep the unsaved changes to the named preset.
    var unsavedChanges: (String) -> Decision
    /// A name for a new preset, starting from a suggestion; nil cancels.
    var name: (String) -> String?
    var confirmDelete: (String) -> Bool
    /// A preset file to import; nil cancels.
    var importFile: () -> URL?
  }

  /// What the selection's saved document holds.
  enum Baseline: Equatable {
    case saved(RigSnapshot)
    /// The saved document could not be loaded; nothing is saved automatically over it.
    case unavailable
  }

  static let autosaveDelay: TimeInterval = 0.3

  let store: PresetStore
  private let audio: AudioController
  private let prompts: Prompts
  @Published private(set) var entries: [PresetStore.Entry] = []
  @Published private(set) var selection = PresetStore.Selection.defaultPreset
  @Published private(set) var dirty = false
  @Published var error: String?
  /// Information about the loaded preset that needs no action, such as module versions that
  /// differ from the ones that saved it. Replaced by each load; cleared by saving.
  @Published private(set) var notice: String?
  private(set) var baseline = Baseline.unavailable
  private var pendingEvaluation: DispatchWorkItem?

  init(audio: AudioController, store: PresetStore, prompts: Prompts) {
    self.audio = audio
    self.store = store
    self.prompts = prompts
    audio.onRigEdit = { [weak self] in self?.scheduleEvaluation() }
  }

  var selectedName: String {
    switch selection {
    case .defaultPreset: PresetStore.defaultName
    case .named(let id): entries.first { $0.id == id }?.name ?? PresetStore.defaultName
    }
  }

  /// The dropdown title: a named preset with unsaved changes is marked with an asterisk.
  var title: String { dirty ? "* \(selectedName)" : selectedName }
  var isDefault: Bool { selection == .defaultPreset }
  var canSave: Bool { !isDefault && dirty }

  /// Default first, then named presets by name: the order of the dropdown and of stepping.
  var order: [PresetStore.Selection] { [.defaultPreset] + entries.map { .named($0.id) } }

  // MARK: Startup

  /// Restores the last selected preset, falling back to Default when it cannot be loaded.
  func restore() {
    let listed = store.entries()
    entries = listed.entries
    var messages = listed.failures
    let saved = store.loadSelection()
    if case .named(let id) = saved, !entries.contains(where: { $0.id == id }) {
      messages.append("The last selected preset is no longer in the library; Default was loaded.")
    }
    if case .named(let id) = saved, entries.contains(where: { $0.id == id }) {
      do {
        try apply(try store.loadDocument(id))
        try commit(saved)
        error = messages.isEmpty ? nil : messages.joined(separator: " ")
        return
      } catch {
        messages.append("Could not restore the last preset: \(error.localizedDescription)")
      }
    }
    do {
      try loadDefault()
    } catch {
      selection = .defaultPreset
      baseline = .unavailable
      messages.append("Could not load Default: \(error.localizedDescription)")
    }
    error = messages.isEmpty ? nil : messages.joined(separator: " ")
  }

  // MARK: Selection

  func select(_ target: PresetStore.Selection) {
    guard target != selection, order.contains(target) else { return }
    guard resolveUnsavedChanges() else { return }
    do {
      switch target {
      case .defaultPreset: try loadDefault()
      case .named(let id):
        try apply(try store.loadDocument(id))
        try commit(target)
      }
      error = nil
    } catch {
      self.error = "Could not load \(name(of: target)): \(error.localizedDescription)"
    }
  }

  func step(_ direction: Int) {
    let order = order
    guard order.count > 1, let index = order.firstIndex(of: selection) else { return }
    select(order[((index + direction) % order.count + order.count) % order.count])
  }

  // MARK: Saving

  @discardableResult
  func save() -> Bool {
    guard case .named(let id) = selection, dirty else { return false }
    do {
      let current = try audio.captureRig()
      try store.save(RigPreset(name: selectedName, rig: current), as: id)
      baseline = .saved(current)
      dirty = false
      notice = nil
      error = nil
      return true
    } catch {
      self.error = "Could not save \(selectedName): \(error.localizedDescription)"
      return false
    }
  }

  func saveAs() {
    guard let proposed = prompts.name(isDefault ? "" : selectedName) else { return }
    do {
      let name = try PresetStore.validName(proposed)
      guard !entries.contains(where: { $0.name.caseInsensitiveCompare(name) == .orderedSame })
      else { throw AmpError.message("A preset named \(name) already exists.") }
      flush()
      let current = try audio.captureRig()
      let id = UUID()
      try store.save(RigPreset(name: name, rig: current), as: id)
      entries = PresetStore.sorted(entries + [PresetStore.Entry(id: id, name: name)])
      selection = .named(id)
      baseline = .saved(current)
      dirty = false
      notice = nil
      error = nil
      persistSelection()
    } catch {
      self.error = error.localizedDescription
    }
  }

  // MARK: Delete, reset and import

  /// Deletes the selected named preset and loads Default.
  func delete() {
    guard case .named(let id) = selection, prompts.confirmDelete(selectedName) else { return }
    pendingEvaluation?.cancel()
    do { try store.delete(id) } catch {
      self.error = error.localizedDescription
      return
    }
    entries.removeAll { $0.id == id }
    do {
      try loadDefault()
      error = nil
    } catch {
      selection = .defaultPreset
      baseline = .unavailable
      dirty = false
      persistSelection()
      self.error = "Could not load Default: \(error.localizedDescription)"
    }
  }

  /// Loads the saved Default into the selected named preset, which then needs Save to keep it.
  func reset() {
    guard case .named = selection else { return }
    guard resolveUnsavedChanges() else { return }
    do {
      guard let saved = try store.loadDefaultDocument() else {
        throw AmpError.message("Default has not been saved yet")
      }
      try apply(saved)
      evaluate()
      error = nil
    } catch {
      self.error = "Could not reset to Default: \(error.localizedDescription)"
    }
  }

  /// Copies a validated preset file into the library, without linking to the original, and
  /// loads it. A name already in use gets a number rather than replacing anything.
  func importPreset() {
    guard let url = prompts.importFile() else { return }
    let document: PresetStore.Document
    do {
      document = try PresetStore.document(url)
      _ = try audio.stageRig(document.preset.rig)
    } catch {
      self.error = "Could not import \(url.lastPathComponent): \(error.localizedDescription)"
      return
    }
    guard resolveUnsavedChanges() else { return }
    let imported = document.preset
    let base =
      (try? PresetStore.validName(imported.name))
      ?? ((try? PresetStore.validName(url.deletingPathExtension().lastPathComponent)) ?? "Imported")
    let name = PresetStore.uniqueName(base, among: entries)
    let id = UUID()
    do { try store.save(RigPreset(name: name, rig: imported.rig), as: id) } catch {
      self.error = "Could not import \(url.lastPathComponent): \(error.localizedDescription)"
      return
    }
    entries = PresetStore.sorted(entries + [PresetStore.Entry(id: id, name: name)])
    do {
      try apply(document)
      try commit(.named(id))
      error = nil
    } catch {
      self.error = "Imported \(name), but could not load it: \(error.localizedDescription)"
    }
  }

  // MARK: Change tracking

  func scheduleEvaluation() {
    pendingEvaluation?.cancel()
    let work = DispatchWorkItem { [weak self] in
      MainActor.assumeIsolated { self?.evaluate() }
    }
    pendingEvaluation = work
    DispatchQueue.main.asyncAfter(deadline: .now() + Self.autosaveDelay, execute: work)
  }

  /// Runs any pending change evaluation now, saving Default when it is selected.
  func flush() {
    pendingEvaluation?.cancel()
    pendingEvaluation = nil
    evaluate()
  }

  /// Compares the current rig with the saved baseline: saves it when Default is selected,
  /// otherwise updates the dirty state.
  private func evaluate() {
    pendingEvaluation = nil
    guard case .saved(let saved) = baseline else {
      dirty = false
      return
    }
    let current: RigSnapshot
    do { current = try audio.captureRig() } catch {
      self.error = "Could not read the current settings: \(error.localizedDescription)"
      return
    }
    switch selection {
    case .defaultPreset:
      dirty = false
      guard current != saved else { return }
      do {
        try store.saveDefault(current)
        baseline = .saved(current)
        notice = nil
      } catch {
        self.error = "Could not save Default: \(error.localizedDescription)"
      }
    case .named:
      dirty = current != saved
    }
  }

  /// Whether the app may quit: saves Default, and asks about unsaved changes to a named preset.
  func prepareToTerminate() -> Bool { resolveUnsavedChanges() }

  // MARK: Transitions

  /// Saves Default if selected, then asks about unsaved changes to a named preset. False when
  /// the user cancels or saving fails, leaving everything as it was.
  private func resolveUnsavedChanges() -> Bool {
    flush()
    guard case .named = selection, dirty else { return true }
    switch prompts.unsavedChanges(selectedName) {
    case .save: return save()
    case .discard: return true
    case .cancel: return false
    }
  }

  /// Loads the saved Default, creating it from the current settings the first time.
  private func loadDefault() throws {
    if let saved = try store.loadDefaultDocument() {
      try apply(saved)
    } else {
      try store.saveDefault(try audio.captureRig())
    }
    try commit(.defaultPreset)
  }

  /// Makes `next` the selection with the settings just loaded as its clean baseline.
  private func commit(_ next: PresetStore.Selection) throws {
    let current = try audio.captureRig()
    pendingEvaluation?.cancel()
    pendingEvaluation = nil
    selection = next
    baseline = .saved(current)
    dirty = false
    persistSelection()
  }

  /// Loads a preset file and shows its notices. When moving modules to Special FX changes what
  /// it processes, its original bytes are preserved first; failing to preserve them cancels the
  /// load, since an autosave could otherwise replace the only copy.
  private func apply(_ document: PresetStore.Document) throws {
    let notices = try audio.applyRig(document.preset.rig) { [store] staged in
      guard staged.relocationLossy else { return }
      do { try store.preserve(document.data) } catch {
        throw AmpError.message(
          "Could not keep a copy of the original preset before moving modules to Special FX: "
            + error.localizedDescription)
      }
    }
    notice =
      notices.isEmpty
      ? nil : "Preset notices: \(notices.joined(separator: "; "))."
  }

  private func persistSelection() {
    do { try store.saveSelection(selection) } catch {
      self.error = error.localizedDescription
    }
  }

  private func name(of selection: PresetStore.Selection) -> String {
    switch selection {
    case .defaultPreset: PresetStore.defaultName
    case .named(let id): entries.first { $0.id == id }?.name ?? "the preset"
    }
  }
}
