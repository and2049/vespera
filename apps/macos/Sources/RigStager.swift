import Foundation

/// Resolves, validates and commits whole-rig changes against the installed modules. The
/// `AudioController` that owns it stays the face callers see; the hooks cover the state only
/// that owner keeps: submitting through the bridge, publishing committed globals, and stopping
/// audio that can no longer match the controls.
@MainActor
final class RigStager {
  /// Saved settings for modules that are not installed: kept, never applied or dropped.
  struct UnavailableSettings: Equatable {
    var amps: [AmpSnapshot] = []
    var cabinets: [ModuleSnapshot] = []
    var specialFX: [ModuleSnapshot] = []
  }

  /// A preset resolved against the installed modules and validated on separate module
  /// instances; nothing it describes is shown or processing yet.
  struct StagedRig {
    let settings: [(module: LoadedModule, settings: StagedModuleSettings)]
    let ampIndex: Int?
    let cabinetSelection: CabinetCatalog.Selection
    let specialFXShown: Int?
    let specialFXActive: Set<String>
    let preFX: PedalCatalog.StagedBoard
    let postFX: PedalCatalog.StagedBoard
    let globals: GlobalControlSettings
    let unavailable: UnavailableSettings
    /// Information for the user: modules moved to Special FX, missing files, and installed
    /// modules whose version differs from the one that saved their settings (which still load,
    /// since the module's state interface accepted their schema).
    let notices: [String]
    /// Moving modules to Special FX changed what the preset processes; its original file should
    /// be preserved before the load commits.
    let relocationLossy: Bool
    /// The single audio request replacing every module section and the global controls.
    let request: [String: Any]
  }

  private let amps: AmpCatalog
  private let cabinets: CabinetCatalog
  private let preFX: PedalCatalog
  private let postFX: PedalCatalog
  private let specialFX: SpecialFXCatalog
  /// Settings carried from the last rig that loaded, for modules it described that are no
  /// longer installed, so saving it again keeps them.
  private var unavailable = UnavailableSettings()
  private let submit: ([String: Any]) throws -> Void
  /// Publishes a committed rig's globals and clears anything the owner would send on its behalf.
  private let commit: (StagedRig) -> Void
  /// Stops running audio that may no longer match the controls; false when audio was not
  /// running, so the caller can say which it was.
  private let stopIfRunning: () -> Bool
  private let report: (Error) -> Void

  init(
    amps: AmpCatalog, cabinets: CabinetCatalog, preFX: PedalCatalog, postFX: PedalCatalog,
    specialFX: SpecialFXCatalog,
    submit: @escaping ([String: Any]) throws -> Void,
    commit: @escaping (StagedRig) -> Void,
    stopIfRunning: @escaping () -> Bool,
    report: @escaping (Error) -> Void
  ) {
    self.amps = amps
    self.cabinets = cabinets
    self.preFX = preFX
    self.postFX = postFX
    self.specialFX = specialFX
    self.submit = submit
    self.commit = commit
    self.stopIfRunning = stopIfRunning
    self.report = report
  }

  /// The cabinet section of a rig request; empty when no cabinet is linked.
  static func cabinetDescription(_ cabinet: LoadedModule?) throws -> [[String: Any]] {
    guard let cabinet else { return [] }
    var result = try cabinet.audioDescription()
    result["slot"] = "cabinet"
    return [result]
  }

  /// The amp-owned EQ section of a rig request; empty when the amp declares no EQ.
  static func equalizerDescription(_ amp: LoadedModule?) throws -> [[String: Any]] {
    guard let equalizer = amp?.equalizer else { return [] }
    var result = try equalizer.audioDescription()
    result["slot"] = "eq"
    return [result]
  }

  /// The module sections and global controls audio starts with: what is currently shown.
  func startConfiguration(globals: GlobalControlSettings) throws -> [String: Any] {
    [
      "amp": try amps.activeAmp?.audioDescription() ?? NSNull(),
      "cabinet": try Self.cabinetDescription(cabinets.activeCabinet),
      "eq": try Self.equalizerDescription(amps.activeAmp),
      "specialFX": try specialFX.audioDescriptions(), "preFX": try preFX.audioDescriptions(),
      "postFX": try postFX.audioDescriptions(), "globals": try AudioBridge.object(globals),
    ]
  }

  /// The preset request describing what is currently shown, used to restore the audio side.
  func currentRigRequest(globals: GlobalControlSettings) throws -> [String: Any] {
    var request = try startConfiguration(globals: globals)
    request["op"] = BridgeOp.preset.rawValue
    return request
  }

  func captureRig(globals: GlobalControlSettings) throws -> RigSnapshot {
    let independent: String? =
      switch cabinets.selection {
      case .linked: nil
      case .independent(let id): id
      }
    return RigSnapshot(
      globals: PresetGlobals(globals),
      amp: AmpSection(
        selected: amps.activeAmp?.id,
        amps: try amps.amps.map {
          AmpSnapshot(module: try $0.snapshot(), equalizer: try $0.equalizer?.snapshot())
        } + unavailable.amps),
      cabinet: CabinetSection(
        independent: independent,
        cabinets: try cabinets.cabinets.map { try $0.snapshot() } + unavailable.cabinets),
      specialFX: SpecialFXSection(
        selected: specialFX.shownModule?.id, active: specialFX.active.sorted(),
        modules: try specialFX.modules.map { try $0.snapshot() } + unavailable.specialFX),
      preFX: try preFX.snapshot(), postFX: try postFX.snapshot())
  }

  /// Resolves and validates `rig` without changing anything. Installed modules the preset does
  /// not mention take their default settings.
  func stageRig(_ rig: RigSnapshot, globals: GlobalControlSettings) throws -> StagedRig {
    try rig.validate()
    let (rig, relocation) = SpecialFXRelocation.relocate(
      rig,
      special: specialFX.modules.map { .init(id: $0.id, name: $0.name, placement: $0.placement) })
    var settings: [(module: LoadedModule, settings: StagedModuleSettings)] = []
    var unavailable = UnavailableSettings()
    var notices = relocation.notices

    let savedAmps = try index(rig.amp.amps, \.module.id, "amp")
    var ampSettings: [String: StagedModuleSettings] = [:]
    var equalizerSettings: [String: StagedModuleSettings] = [:]
    for amp in amps.amps {
      let saved = savedAmps[amp.id]
      ampSettings[amp.id] = try staged(
        amp, saved?.module, into: &settings, notices: &notices)
      if let equalizer = amp.equalizer {
        equalizerSettings[amp.id] = try staged(
          equalizer, saved?.equalizer, into: &settings, notices: &notices)
      } else if saved?.equalizer != nil {
        throw AmpError.message("The preset has EQ settings for \(amp.name), which has no EQ")
      }
    }
    unavailable.amps = rig.amp.amps.filter { saved in
      !amps.amps.contains { $0.id == saved.module.id }
    }
    let ampIndex = try selection(rig.amp.selected, in: amps.amps, kind: "an amp")

    let savedCabinets = try index(rig.cabinet.cabinets, \.id, "cabinet")
    var cabinetSettings: [String: StagedModuleSettings] = [:]
    for cabinet in cabinets.cabinets {
      cabinetSettings[cabinet.id] = try staged(
        cabinet, savedCabinets[cabinet.id], into: &settings, notices: &notices)
    }
    unavailable.cabinets = rig.cabinet.cabinets.filter { saved in
      !cabinets.cabinets.contains { $0.id == saved.id }
    }
    let cabinetSelection =
      rig.cabinet.independent.map(CabinetCatalog.Selection.independent) ?? .linked
    if let id = rig.cabinet.independent, !cabinets.cabinets.contains(where: { $0.id == id }) {
      throw AmpError.message("The preset uses cabinet \(id), which is not installed")
    }
    let amp = ampIndex.map { amps.amps[$0] }
    let cabinet = try cabinets.cabinet(for: amp, selection: cabinetSelection)

    let savedSpecialFX = try index(rig.specialFX.modules, \.id, "special FX")
    var specialFXSettings: [String: StagedModuleSettings] = [:]
    for module in specialFX.modules {
      specialFXSettings[module.id] = try staged(
        module, savedSpecialFX[module.id], into: &settings, notices: &notices)
    }
    unavailable.specialFX = rig.specialFX.modules.filter { saved in
      !specialFX.modules.contains { $0.id == saved.id }
    }
    let specialFXActive = Set(rig.specialFX.activeModules)
    for id in rig.specialFX.activeModules where !specialFX.modules.contains(where: { $0.id == id })
    {
      throw AmpError.message("The preset uses special FX module \(id), which is not installed")
    }
    let specialFXShown =
      specialFX.modules.firstIndex { $0.id == rig.specialFX.selected }
      ?? specialFX.modules.firstIndex { specialFXActive.contains($0.id) }
      ?? (specialFX.modules.isEmpty ? nil : 0)

    let preBoard = try preFX.stage(rig.preFX)
    let postBoard = try postFX.stage(rig.postFX)
    for (saved, board) in [(rig.preFX, preBoard), (rig.postFX, postBoard)] {
      for (snapshot, instance) in zip(saved.chain + saved.drafts, board.chain + board.drafts) {
        notices += versionNotices(instance.module, snapshot.module, existing: notices)
      }
    }
    let nextGlobals = rig.globals.applied(to: globals)

    let request: [String: Any] = [
      "op": BridgeOp.preset.rawValue,
      "amp": try amp.flatMap { amp in try ampSettings[amp.id].map { try amp.audioDescription($0) } }
        ?? NSNull(),
      "cabinet": try described(
        cabinet, cabinet.flatMap { cabinetSettings[$0.id] }, slot: "cabinet"),
      "eq": try described(amp?.equalizer, amp.flatMap { equalizerSettings[$0.id] }, slot: "eq"),
      "specialFX": try specialFX.audioDescriptions(
        active: specialFXActive, staged: specialFXSettings),
      "preFX": try preFX.descriptions(preBoard.chain),
      "postFX": try postFX.descriptions(postBoard.chain),
      "globals": try AudioBridge.object(nextGlobals),
    ]
    return StagedRig(
      settings: settings, ampIndex: ampIndex, cabinetSelection: cabinetSelection,
      specialFXShown: specialFXShown, specialFXActive: specialFXActive, preFX: preBoard,
      postFX: postBoard, globals: nextGlobals, unavailable: unavailable, notices: notices,
      relocationLossy: relocation.lossy, request: request)
  }

  /// Loads `rig` as one change: the audio side receives every section and the global controls
  /// in one request, and only once it accepts them do the catalogs, editors and controls change.
  /// On failure the previous rig stays shown and audible.
  /// `beforeCommit` runs after staging and before anything changes; throwing cancels the load.
  /// Returns the staged notices.
  ///
  /// If the controls cannot take the settings after the audio side accepted them, both are
  /// returned to the previous rig. If that also fails, `error` reports it, running audio stops
  /// so it cannot keep playing settings the controls do not show, and the thrown error says so.
  @discardableResult
  func applyRig(
    _ rig: RigSnapshot, globals: GlobalControlSettings,
    beforeCommit: ((StagedRig) throws -> Void)? = nil
  ) throws -> [String] {
    let staged = try stageRig(rig, globals: globals)
    try beforeCommit?(staged)
    let previous = try staged.settings.map { entry in
      let snapshot = try entry.module.snapshot()
      return (
        entry.module,
        StagedModuleSettings(
          state: snapshot.state, parameters: snapshot.parameters,
          files: snapshot.files)
      )
    }
    try submit(staged.request)
    var applied = 0
    do {
      for entry in staged.settings {
        try entry.module.apply(entry.settings)
        applied += 1
      }
    } catch {
      var failures: [String] = []
      for (module, settings) in previous.prefix(applied + 1) {
        do { try module.apply(settings) } catch {
          failures.append(
            "\(module.name) kept the preset's settings (\(error.localizedDescription))")
        }
      }
      var audioRestored = true
      do { try submit(try currentRigRequest(globals: globals)) } catch {
        audioRestored = false
        failures.append("the audio side kept the preset (\(error.localizedDescription))")
      }
      guard !failures.isEmpty else { throw error }
      var message =
        "Could not load the preset: \(error.localizedDescription). Restoring the previous settings also failed: \(failures.joined(separator: "; "))."
      if !audioRestored && stopIfRunning() {
        message += " Audio was stopped because it may not match the controls; start it again."
      } else {
        message += " Audio may not match the controls; restart audio or reload a preset."
      }
      report(AmpError.message(message))
      throw AmpError.message(message)
    }
    if let index = staged.ampIndex { amps.restore(selecting: index) }
    cabinets.restore(selection: staged.cabinetSelection, amp: staged.ampIndex.map { amps.amps[$0] })
    specialFX.restore(shown: staged.specialFXShown, active: staged.specialFXActive)
    preFX.commit(staged.preFX)
    postFX.commit(staged.postFX)
    unavailable = staged.unavailable
    commit(staged)
    return staged.notices
  }

  /// The preset's `items` by ID, rejecting a preset that lists one twice.
  private func index<T>(_ items: [T], _ id: (T) -> String, _ kind: String) throws -> [String: T] {
    var result: [String: T] = [:]
    for item in items {
      guard result.updateValue(item, forKey: id(item)) == nil else {
        throw AmpError.message("The preset lists \(kind) \(id(item)) twice")
      }
    }
    return result
  }

  /// Missing-file and saved-version notices for `module`, deduplicated against `existing`.
  private func versionNotices(
    _ module: LoadedModule, _ saved: ModuleSnapshot?, existing: [String]
  ) -> [String] {
    var result: [String] = []
    for file in saved?.files ?? [] where ModuleFileMenu.missing(module, file) {
      result.append("\(module.name): \(file.name) is missing; use Locate in its file picker")
    }
    guard let saved, saved.version != module.version else { return result }
    let notice = "\(module.name) \(module.version) loaded settings saved with \(saved.version)"
    if !existing.contains(notice) { result.append(notice) }
    return result
  }

  /// Validates `saved` settings for `module` on one of the module's own instances, recording
  /// them for `applyRig` and appending the notices they earn to `notices`.
  private func staged(
    _ module: LoadedModule, _ saved: ModuleSnapshot?,
    into settings: inout [(module: LoadedModule, settings: StagedModuleSettings)],
    notices: inout [String]
  ) throws -> StagedModuleSettings {
    notices += versionNotices(module, saved, existing: notices)
    let staged = try module.stage(saved ?? module.defaultSnapshot())
    settings.append((module, staged))
    return staged
  }

  /// Where `id` selects in `modules`, or nothing when a preset that selects none can.
  private func selection(_ id: String?, in modules: [LoadedModule], kind: String) throws -> Int? {
    guard let id else {
      if modules.isEmpty { return nil }
      throw AmpError.message("The preset does not select \(kind)")
    }
    guard let index = modules.firstIndex(where: { $0.id == id }) else {
      throw AmpError.message("The preset uses \(kind) \(id), which is not installed")
    }
    return index
  }

  /// `module`'s audio description carrying `staged` settings under `slot`.
  private func described(_ module: LoadedModule?, _ staged: StagedModuleSettings?, slot: String)
    throws -> [[String: Any]]
  {
    guard let module, let staged else { return [] }
    var description = try module.audioDescription(staged)
    description["slot"] = slot
    return [description]
  }
}
