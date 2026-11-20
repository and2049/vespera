import Foundation
import Testing

@testable import VesperaUI

private let moved = SpecialFXRelocation.Module(
  id: "org.example.moved", name: "Moved", placement: .beforePreFX)

private func snapshot(_ id: String, _ value: Float = 0.5) -> ModuleSnapshot {
  ModuleSnapshot(
    id: id, version: "1", abiVersion: 2, state: nil,
    parameters: [ParameterSnapshot(id: 1, value: 1), ParameterSnapshot(id: 2, value: value)])
}

private func instance(
  _ id: String, _ module: String, ordinal: Int = 1,
  lifecycle: PedalInstanceSnapshot.Lifecycle = .chained, bypassed: Bool = false,
  value: Float = 0.5
) -> PedalInstanceSnapshot {
  PedalInstanceSnapshot(
    id: id, ordinal: ordinal, lifecycle: lifecycle, hostBypassed: bypassed,
    module: snapshot(module, value))
}

private func rig(
  pre: [PedalInstanceSnapshot] = [], preDrafts: [PedalInstanceSnapshot] = [],
  post: [PedalInstanceSnapshot] = [], special: SpecialFXSection = .init(selected: nil, modules: []),
  preEditors: EditorLayoutSnapshot? = nil
) -> RigSnapshot {
  RigSnapshot(
    globals: PresetGlobals(GlobalControlSettings()), amp: AmpSection(selected: nil, amps: []),
    cabinet: CabinetSection(independent: nil, cabinets: []), specialFX: special,
    preFX: BoardSnapshot(
      chain: pre, drafts: preDrafts,
      editors: preEditors ?? EditorLayoutSnapshot(kind: .pedals, slots: (pre + preDrafts).map(\.id))
    ),
    postFX: BoardSnapshot(
      chain: post, drafts: [], editors: EditorLayoutSnapshot(kind: .pedals, slots: [])))
}

@Test func relocationLeavesRigsWithoutMovedModulesUntouched() {
  let legacy = rig(
    pre: [instance("pre-fx.0", "org.example.pedal")],
    special: .init(selected: "org.example.other", modules: [snapshot("org.example.other")]))
  let result = SpecialFXRelocation.relocate(legacy, special: [moved])
  #expect(result.rig == legacy && result.report == .init())
  #expect(result.rig.specialFX.active == nil)
}

@Test func legacySchemaTwoWithoutActiveKeepsItsSelectionAndFileReference() throws {
  let id = "org.example.other"
  let hash = String(repeating: "a", count: 64)
  let reference = ModuleFileReference(
    slot: 1, scope: .library, path: hash, name: "response.wav", bytes: 4, sha256: hash)
  var module = snapshot(id)
  module.files = [reference]
  var legacy = RigPreset(name: "Legacy", rig: rig(special: .init(selected: id, modules: [module])))
  legacy.schemaVersion = 2
  let data = try JSONEncoder().encode(legacy)
  let json = try #require(try JSONSerialization.jsonObject(with: data) as? [String: Any])
  let savedRig = try #require(json["rig"] as? [String: Any])
  let special = try #require(savedRig["specialFX"] as? [String: Any])
  try #require(!special.keys.contains("active"))

  let decoded = try RigPreset.decode(data)
  #expect(decoded.rig.specialFX.selected == id)
  #expect(decoded.rig.specialFX.activeModules == [id])
  #expect(decoded.rig.specialFX.modules.first?.files == [reference])

  var captured = decoded.rig
  captured.specialFX.active = [id]
  let roundTrip = try RigPreset.decode(try RigPreset(name: "Captured", rig: captured).encoded())
  #expect(roundTrip.schemaVersion == 2 && roundTrip.rig == captured)
}

@Test func firstInChainMovesWithoutLoss() {
  let saved = rig(
    pre: [
      instance("pre-fx.0", "org.example.moved", value: 0.8),
      instance("pre-fx.1", "org.example.pedal"),
    ],
    special: .init(selected: "org.example.other", modules: [snapshot("org.example.other")]))
  let (result, report) = SpecialFXRelocation.relocate(saved, special: [moved])
  #expect(report == .init(notices: ["Moved moved from Pre FX to Special FX"], lossy: false))
  #expect(result.preFX.chain.map(\.id) == ["pre-fx.1"])
  #expect(result.preFX.editors.slots == [nil, "pre-fx.1"])
  #expect(result.specialFX.modules.last == snapshot("org.example.moved", 0.8))
  // The legacy selection keeps processing alongside the moved module.
  #expect(result.specialFX.active == ["org.example.moved", "org.example.other"])
  #expect(result.specialFX.selected == "org.example.other")
}

@Test func relocationReportsPositionChangesAndInstancesItCannotKeep() {
  let middle = rig(
    pre: [
      instance("pre-fx.0", "org.example.pedal"), instance("pre-fx.1", "org.example.moved"),
      instance("pre-fx.2", "org.example.pedal", ordinal: 2),
    ])
  let positioned = SpecialFXRelocation.relocate(middle, special: [moved]).report
  #expect(positioned.lossy)
  #expect(
    positioned.notices.last
      == "Moved ran at position 2 of 3 in the Pre FX chain and now runs before Pre FX")

  let late = SpecialFXRelocation.Module(id: moved.id, name: "Moved", placement: .afterPostFX)
  let last = rig(post: [
    instance("post-fx.0", "org.example.pedal"), instance("post-fx.1", moved.id),
  ])
  #expect(SpecialFXRelocation.relocate(last, special: [late]).report.lossy == false)
  let amp = SpecialFXRelocation.Module(id: moved.id, name: "Moved", placement: .beforeAmp)
  #expect(SpecialFXRelocation.relocate(last, special: [amp]).report.lossy)

  let twice = rig(
    pre: [instance("pre-fx.0", moved.id, value: 0.1)],
    preDrafts: [instance("pre-fx.4", moved.id, ordinal: 3, lifecycle: .editedDraft)],
    post: [instance("post-fx.0", moved.id, ordinal: 2, value: 0.2)])
  let (kept, report) = SpecialFXRelocation.relocate(twice, special: [moved])
  #expect(report.lossy)
  #expect(
    report.notices == [
      "Moved moved from Pre FX to Special FX",
      "Moved 2 in Post FX was not kept; Special FX holds one Moved",
      "Moved 3 in Pre FX was not kept; Special FX holds one Moved",
    ])
  #expect(kept.specialFX.modules == [snapshot(moved.id, 0.1)])
  #expect(kept.preFX.chain.isEmpty && kept.preFX.drafts.isEmpty && kept.postFX.chain.isEmpty)
  #expect(kept.preFX.editors.slots == [nil, nil])
}

@Test func draftsAndBypassedInstancesMoveWithoutProcessing() {
  let drafts = rig(
    preDrafts: [
      instance("pre-fx.3", moved.id, lifecycle: .untouchedDraft),
      instance("pre-fx.4", moved.id, ordinal: 2, lifecycle: .editedDraft, value: 0.3),
    ])
  let (fromDraft, report) = SpecialFXRelocation.relocate(drafts, special: [moved])
  #expect(!report.lossy && report.notices == ["Moved moved from Pre FX to Special FX"])
  #expect(fromDraft.specialFX.modules == [snapshot(moved.id, 0.3)])
  #expect(fromDraft.specialFX.active == [] && fromDraft.preFX.drafts.isEmpty)

  let untouched = rig(preDrafts: [instance("pre-fx.3", moved.id, lifecycle: .untouchedDraft)])
  let (dropped, quiet) = SpecialFXRelocation.relocate(untouched, special: [moved])
  #expect(quiet == .init() && dropped.preFX.drafts.isEmpty && dropped.specialFX.modules.isEmpty)

  let bypassed = rig(pre: [instance("pre-fx.0", moved.id, bypassed: true)])
  let (off, _) = SpecialFXRelocation.relocate(bypassed, special: [moved])
  #expect(off.specialFX.active == [] && off.specialFX.modules.count == 1)
}

@Test func existingSpecialFXSettingsYieldOnlyToAChainedInstance() {
  let existing = SpecialFXSection(
    selected: moved.id, active: [], modules: [snapshot(moved.id, 0.9)])
  let chained = rig(pre: [instance("pre-fx.0", moved.id, value: 0.1)], special: existing)
  let (replaced, report) = SpecialFXRelocation.relocate(chained, special: [moved])
  #expect(replaced.specialFX.modules == [snapshot(moved.id, 0.1)])
  #expect(replaced.specialFX.active == [moved.id])
  #expect(report.lossy && report.notices.count == 2)

  let draft = rig(
    preDrafts: [instance("pre-fx.4", moved.id, lifecycle: .editedDraft, value: 0.1)],
    special: existing)
  let (kept, draftReport) = SpecialFXRelocation.relocate(draft, special: [moved])
  #expect(kept.specialFX.modules == [snapshot(moved.id, 0.9)])
  #expect(draftReport.lossy)
  #expect(draftReport.notices.last == "Moved in Pre FX was not kept; Special FX holds one Moved")

  let rack = rig(
    post: [], special: .init(selected: nil, modules: []),
    preEditors: EditorLayoutSnapshot(kind: .rack, slots: ["pre-fx.0"]))
  var racked = rack
  racked.preFX.chain = [instance("pre-fx.0", moved.id)]
  let (cleared, _) = SpecialFXRelocation.relocate(racked, special: [moved])
  #expect(cleared.preFX.editors == EditorLayoutSnapshot(kind: .pedals, slots: []))
}

// MARK: Loading presets with moved modules

@MainActor
private final class MovedFixture {
  let root: URL
  let packages: [TestModulePackage]
  let audio: AudioController
  let store: PresetStore
  let presets: PresetController

  init() throws {
    root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
    let modules = root.appendingPathComponent("modules")
    let special = try TestModulePackage(
      category: "special-fx", id: "org.example.moved", parent: modules)
    var manifest = special.manifest
    manifest.placement = "before-pre-fx"
    try JSONEncoder().encode(manifest).write(
      to: special.directory.appendingPathComponent("module.json"))
    packages = [
      special,
      try TestModulePackage(category: "special-fx", id: "org.example.other", parent: modules),
      try TestModulePackage(category: "pre-fx", id: "org.example.pedal", parent: modules),
    ]
    audio = AudioController(
      amps: AmpCatalog(directory: modules), preFX: PedalCatalog(position: .pre, directory: modules),
      postFX: PedalCatalog(position: .post, directory: modules),
      cabinets: CabinetCatalog(directory: modules), specialFX: SpecialFXCatalog(directory: modules),
      defaults: MemoryDefaults())
    store = PresetStore(directory: root.appendingPathComponent("presets"))
    presets = PresetController(
      audio: audio, store: store,
      prompts: .init(
        unsavedChanges: { _ in .discard }, name: { _ in nil }, confirmDelete: { _ in false },
        importFile: { nil }))
  }

  deinit { try? FileManager.default.removeItem(at: root) }

  /// A saved preset whose Pre FX chain holds the moved module at `position` among `count`
  /// instances, the others being the installed pedal.
  func preset(_ name: String, position: Int, count: Int) throws -> UUID {
    var rig = try audio.captureRig()
    // Saved before the module was installed as Special FX: no Special FX settings of its own.
    rig.specialFX.modules.removeAll { $0.id == "org.example.moved" }
    let module = try #require(audio.specialFX.modules.first { $0.id == "org.example.moved" })
    var settings = try module.snapshot()
    settings.parameters[1].value = 0.42
    let pedal = try #require(audio.preFX.choices.first).instantiate().snapshot()
    rig.preFX.chain = (0..<count).map { index in
      PedalInstanceSnapshot(
        id: "pre-fx.m\(index)", ordinal: index + 1, lifecycle: .chained, hostBypassed: false,
        module: index == position ? settings : pedal)
    }
    rig.preFX.drafts = []
    rig.preFX.editors = EditorLayoutSnapshot(kind: .pedals, slots: [])
    let id = UUID()
    try store.save(RigPreset(name: name, rig: rig), as: id)
    return id
  }

  var superseded: [URL] {
    (try? FileManager.default.contentsOfDirectory(
      at: store.supersededDirectory, includingPropertiesForKeys: nil)) ?? []
  }
}

@MainActor
@Test func losslessRelocationLoadsWithANoticeAndNoBackup() throws {
  let fixture = try MovedFixture()
  defer { fixture.audio.stop() }
  let id = try fixture.preset("First", position: 0, count: 2)
  let before = try Data(contentsOf: fixture.store.url(for: id))
  fixture.presets.restore()
  fixture.presets.select(.named(id))
  #expect(fixture.presets.error == nil)
  #expect(fixture.presets.notice?.contains("moved from Pre FX to Special FX") == true)
  #expect(fixture.audio.specialFX.active == ["org.example.moved"])
  #expect(
    try fixture.audio.specialFX.modules.first { $0.id == "org.example.moved" }?.value(2) == 0.42)
  #expect(fixture.audio.preFX.chain.map(\.id) == ["pre-fx.m1"])
  #expect(fixture.superseded.isEmpty)
  #expect(try Data(contentsOf: fixture.store.url(for: id)) == before)
  #expect(!fixture.presets.dirty)
}

@MainActor
@Test func lossyRelocationPreservesTheOriginalFileOnce() throws {
  let fixture = try MovedFixture()
  defer { fixture.audio.stop() }
  let id = try fixture.preset("Middle", position: 1, count: 3)
  let original = try Data(contentsOf: fixture.store.url(for: id))
  fixture.presets.restore()
  fixture.presets.select(.named(id))
  #expect(fixture.presets.error == nil)
  #expect(fixture.presets.notice?.contains("now runs before Pre FX") == true)
  let copies = fixture.superseded
  #expect(copies.count == 1)
  #expect(try copies.first.map { try Data(contentsOf: $0) } == original)
  fixture.presets.select(.defaultPreset)
  fixture.presets.select(.named(id))
  #expect(fixture.superseded == copies)
}

@MainActor
@Test func duplicateRelocationKeepsOriginalDefaultThroughFirstAutosave() throws {
  let fixture = try MovedFixture()
  defer { fixture.audio.stop() }
  let movedID = "org.example.moved"
  let otherID = "org.example.other"
  let module = try #require(fixture.audio.specialFX.modules.first { $0.id == movedID })
  let other = try #require(fixture.audio.specialFX.modules.first { $0.id == otherID })
  var first = instance("pre-fx.0", movedID)
  try module.setValue(2, 0.25)
  first.module = try module.snapshot()
  var second = instance("pre-fx.1", movedID, ordinal: 2)
  try module.setValue(2, 0.75)
  second.module = try module.snapshot()
  let legacy = rig(
    pre: [first, second],
    special: .init(selected: otherID, modules: [try other.snapshot()]))
  try fixture.store.saveDefault(legacy)
  let original = try #require(try fixture.store.loadDefaultDocument())
  try #require(original.preset.schemaVersion == 1 && original.preset.rig.specialFX.active == nil)

  fixture.presets.restore()
  fixture.presets.flush()
  try #require(fixture.presets.error == nil, "\(fixture.presets.error ?? "")")
  #expect(fixture.presets.selection == .defaultPreset)
  #expect(
    fixture.presets.notice?.contains("\(module.name) moved from Pre FX to Special FX") == true)
  #expect(fixture.presets.notice?.contains("\(module.name) 2 in Pre FX was not kept") == true)
  #expect(fixture.audio.specialFX.shownModule?.id == otherID)
  #expect(fixture.audio.specialFX.active == [movedID, otherID])
  #expect(try module.value(2) == 0.25)
  #expect(try fixture.store.loadDefaultDocument()?.data == original.data)
  let backups = fixture.superseded
  try #require(backups.count == 1)
  let backup = try #require(backups.first)
  #expect(try Data(contentsOf: backup) == original.data)

  try other.setValue(2, 0.625)
  fixture.presets.flush()
  #expect(fixture.presets.error == nil && !fixture.presets.dirty)
  let saved = try #require(try fixture.store.loadDefaultDocument())
  #expect(saved.preset.schemaVersion == 3)
  #expect(saved.preset.rig.specialFX.selected == otherID)
  #expect(saved.preset.rig.specialFX.active == [movedID, otherID])
  #expect(saved.preset.rig.preFX.chain.isEmpty)
  #expect(saved.preset.rig.preFX.editors.slots.allSatisfy { $0 == nil })
  let kept = saved.preset.rig.specialFX.modules.filter { $0.id == movedID }
  #expect(kept.count == 1)
  #expect(kept.first?.parameters.first { $0.id == 2 }?.value == 0.25)
  #expect(saved.preset.rig == (try fixture.audio.captureRig()))
  #expect(fixture.superseded == backups)
  #expect(try Data(contentsOf: backup) == original.data)

  fixture.presets.restore()
  #expect(fixture.presets.error == nil && fixture.presets.notice == nil)
  #expect(try other.value(2) == 0.625)
  #expect(fixture.superseded == backups)
}

@MainActor
@Test func failingToPreserveTheOriginalCancelsTheLoad() throws {
  let fixture = try MovedFixture()
  defer { fixture.audio.stop() }
  let id = try fixture.preset("Middle", position: 1, count: 3)
  fixture.presets.restore()
  let before = try fixture.audio.captureRig()
  try FileManager.default.createDirectory(
    at: fixture.store.directory, withIntermediateDirectories: true)
  try Data("blocking file".utf8).write(to: fixture.store.supersededDirectory)
  fixture.presets.select(.named(id))
  #expect(fixture.presets.error?.contains("Could not keep a copy of the original preset") == true)
  #expect(fixture.presets.selection == .defaultPreset)
  #expect(try fixture.audio.captureRig() == before)
}

@MainActor
@Test func importingALegacyPresetRelocatesItsModules() throws {
  let fixture = try MovedFixture()
  defer { fixture.audio.stop() }
  let id = try fixture.preset("Legacy", position: 1, count: 2)
  let file = fixture.root.appendingPathComponent("legacy.\(RigPreset.fileExtension)")
  try FileManager.default.copyItem(at: fixture.store.url(for: id), to: file)
  try fixture.store.delete(id)
  let presets = PresetController(
    audio: fixture.audio, store: fixture.store,
    prompts: .init(
      unsavedChanges: { _ in .discard }, name: { _ in nil }, confirmDelete: { _ in false },
      importFile: { file }))
  presets.restore()
  presets.importPreset()
  #expect(presets.error == nil)
  #expect(fixture.audio.specialFX.active == ["org.example.moved"])
  #expect(fixture.audio.preFX.chain.map(\.module.id) == ["org.example.pedal"])
  #expect(presets.notice?.contains("moved from Pre FX to Special FX") == true)
  #expect(fixture.superseded.count == 1)
}
