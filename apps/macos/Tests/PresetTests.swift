import Foundation
import Testing

@testable import VesperaUI

/// Deterministic pseudo-random numbers (SplitMix64), so every randomized rig is reproducible.
struct SeededGenerator: RandomNumberGenerator {
  private var state: UInt64
  init(seed: UInt64) { state = seed }
  mutating func next() -> UInt64 {
    state &+= 0x9E37_79B9_7F4A_7C15
    var z = state
    z = (z ^ (z >> 30)) &* 0xBF58_476D_1CE4_E5B9
    z = (z ^ (z >> 27)) &* 0x94D0_49BB_1331_11EB
    return z ^ (z >> 31)
  }
}

/// The audio bridge renders offline only for processes that opt in.
let offlineRenderingEnabled: Void = { setenv("VESPERA_ENABLE_RENDER_GRAPH", "1", 1) }()

/// Defaults kept in memory, so tests never persist settings anywhere.
final class MemoryDefaults: UserDefaults, @unchecked Sendable {
  private var values: [String: Any] = [:]
  override func object(forKey key: String) -> Any? { values[key] }
  override func data(forKey key: String) -> Data? { values[key] as? Data }
  override func set(_ value: Any?, forKey key: String) { values[key] = value }
  override func removeObject(forKey key: String) { values[key] = nil }
}

/// Saved state of the opaque-state fixture (schema 3): enabled, gain, bias and polarity. Bias
/// and polarity are not parameters; only state loading sets them.
func opaqueState(enabled: Float = 1, gain: Float, bias: Float, polarity: Float) -> Data {
  var data = Data()
  for value in [enabled, gain, bias, polarity] {
    withUnsafeBytes(of: value.bitPattern.littleEndian) { data.append(contentsOf: $0) }
  }
  return data
}

func hasOpaqueState(_ module: LoadedModule) -> Bool {
  (try? module.snapshot().state?.schemaVersion) == 3
}

/// Modules for every section: opaque-state and parameter-only amps with EQs and cabinet links,
/// two cabinets, two special FX, pre FX with and without a power parameter, and post FX with a
/// rack. Catalogs built from `modules` are independent of the user's installed modules.
@MainActor
final class RigFixture {
  let root: URL
  let modules: URL
  let packages: [TestModulePackage]

  init() throws {
    _ = offlineRenderingEnabled
    root = FileManager.default.temporaryDirectory
      .appendingPathComponent("redsun/preset-tests/\(UUID().uuidString)")
    modules = root.appendingPathComponent("modules")
    try FileManager.default.createDirectory(at: modules, withIntermediateDirectories: true)
    let parent = modules
    packages = [
      try TestModulePackage(
        category: "amp", id: "org.example.amp-a", equalizer: "org.example.amp-a.eq",
        parent: parent, opaqueState: true,
        cabinet: .init(supported: true, defaultModule: "org.example.cab-a")),
      try TestModulePackage(
        category: "amp", id: "org.example.amp-b", equalizer: "org.example.amp-b.eq",
        parent: parent, cabinet: .init(supported: true, defaultModule: "org.example.cab-b")),
      try TestModulePackage(
        category: "cab", id: "org.example.cab-a", parent: parent, opaqueState: true),
      try TestModulePackage(category: "cab", id: "org.example.cab-b", parent: parent),
      try TestModulePackage(
        category: "special-fx", id: "org.example.sfx-a", parent: parent, opaqueState: true),
      try TestModulePackage(category: "special-fx", id: "org.example.sfx-b", parent: parent),
      try TestModulePackage(
        category: "pre-fx", id: "org.example.pre-a", parent: parent, opaqueState: true),
      try TestModulePackage(
        category: "pre-fx", id: "org.example.pre-b", bypass: nil, parent: parent),
      try TestModulePackage(
        category: "post-fx", id: "org.example.post-a", parent: parent, opaqueState: true),
      try TestModulePackage(
        category: "post-fx", id: "org.example.post-rack", presentation: .rack, parent: parent,
        opaqueState: true),
    ]
  }

  deinit { try? FileManager.default.removeItem(at: root) }

  func audio() -> AudioController {
    AudioController(
      amps: AmpCatalog(directory: modules), preFX: PedalCatalog(position: .pre, directory: modules),
      postFX: PedalCatalog(position: .post, directory: modules),
      cabinets: CabinetCatalog(directory: modules), specialFX: SpecialFXCatalog(directory: modules),
      defaults: MemoryDefaults())
  }

  func store(_ name: String = "store") -> PresetStore {
    PresetStore(directory: root.appendingPathComponent(name))
  }
}

@MainActor
func fixedModules(_ audio: AudioController) -> [LoadedModule] {
  audio.amps.amps.flatMap { [$0] + ($0.equalizer.map { [$0] } ?? []) } + audio.cabinets.cabinets
    + audio.specialFX.modules
}

/// `spread` below 1 keeps parameters within that fraction of their range around the module's
/// defaults, so hosted plugins stay audible; 1 draws from the whole range.
@MainActor
func randomize(_ module: LoadedModule, _ rng: inout SeededGenerator, spread: Float = 1) throws {
  if spread < 1 {
    let defaults = Dictionary(
      uniqueKeysWithValues: try module.defaultSnapshot().parameters.map { ($0.id, $0.value) })
    for parameter in module.parameters where parameter.id != 1 {
      let range = parameter.maximum - parameter.minimum
      let value =
        (defaults[parameter.id] ?? parameter.minimum)
        + Float.random(in: -spread...spread, using: &rng) * range
      try module.setValue(parameter.id, min(parameter.maximum, max(parameter.minimum, value)))
    }
  } else if hasOpaqueState(module) {
    try module.restoreState(
      opaqueState(
        enabled: Bool.random(using: &rng) ? 1 : 0, gain: Float.random(in: 0...2, using: &rng),
        bias: Float.random(in: -0.5...0.5, using: &rng),
        polarity: Bool.random(using: &rng) ? 1 : -1))
  } else {
    for parameter in module.parameters {
      try module.setValue(
        parameter.id, Float.random(in: parameter.minimum...parameter.maximum, using: &rng))
    }
  }
}

/// Changes everything a preset covers, by seed: settings of every module (including opaque
/// state and modules that are not selected), selections, cabinet link, chain contents, order,
/// bypass, drafts, editor layout and global controls.
@MainActor
func randomizeRig(_ audio: AudioController, seed: UInt64, spread: Float = 1) throws {
  var rng = SeededGenerator(seed: seed)
  for module in fixedModules(audio) { try randomize(module, &rng, spread: spread) }
  audio.amps.select(Int.random(in: 0..<audio.amps.amps.count, using: &rng))
  if Bool.random(using: &rng) {
    if audio.cabinets.locked { audio.cabinets.toggleLock() }
    audio.cabinets.select(Int.random(in: 0..<audio.cabinets.cabinets.count, using: &rng))
  } else if !audio.cabinets.locked {
    audio.cabinets.toggleLock()
  }
  audio.specialFX.show(Int.random(in: 0..<audio.specialFX.modules.count, using: &rng))
  for module in audio.specialFX.modules {
    audio.specialFX.setActive(module.id, Bool.random(using: &rng))
  }
  for board in [audio.preFX, audio.postFX] {
    for _ in 0..<6 {
      let choice = board.choices.randomElement(using: &rng)!.id
      switch Int.random(in: 0..<7, using: &rng) {
      case 0: board.add(choice)
      case 1: if let id = board.chain.randomElement(using: &rng)?.id { board.duplicate(id) }
      case 2: if board.chain.count > 1, let id = board.chain.first?.id { board.remove(id) }
      case 3:
        if board.chain.count > 1, let id = board.chain.randomElement(using: &rng)?.id {
          board.move(id, to: Int.random(in: 0..<board.chain.count, using: &rng))
        }
      case 4:
        if let id = board.chain.randomElement(using: &rng)?.id {
          board.setBypassed(id, Bool.random(using: &rng))
        }
      case 5:
        board.addDraft(choice, in: Int.random(in: 0..<PedalEditorLayout.capacity, using: &rng))
        if let draft = board.drafts.last, Bool.random(using: &rng),
          let parameter = draft.module.parameters.first(where: { $0.id != 1 })
        {
          try draft.module.setValue(
            parameter.id, Float.random(in: parameter.minimum...parameter.maximum, using: &rng))
        }
      default:
        if let id = (board.chain + board.drafts).randomElement(using: &rng)?.id {
          board.chooseEditor(id, in: Int.random(in: 0..<PedalEditorLayout.capacity, using: &rng))
        }
      }
    }
    for instance in board.chain { try randomize(instance.module, &rng, spread: spread) }
  }
  var globals = audio.globals
  globals.inputGain = Double(Int.random(in: -120...48, using: &rng)) / 2
  globals.gateEnabled = Bool.random(using: &rng)
  globals.gateThreshold = Double(Int.random(in: -120...0, using: &rng))
  globals.transpose = Double(Int.random(in: -12...12, using: &rng))
  globals.doublerEnabled = Bool.random(using: &rng)
  globals.doublerTime = Double(Int.random(in: 100...4000, using: &rng)) / 100
  globals.outputGain = Double(Int.random(in: -120...48, using: &rng)) / 2
  audio.updateGlobals(globals)
}

@MainActor
func payload(_ audio: AudioController) throws -> Data {
  try JSONSerialization.data(withJSONObject: audio.startConfiguration(), options: .sortedKeys)
}

/// A deterministic stereo test signal: seeded noise with an impulse and a decaying tone.
func testSignal(frames: Int = 2048, seed: UInt64 = 7) -> [[Float]] {
  var rng = SeededGenerator(seed: seed)
  return (0..<2).map { channel in
    (0..<frames).map { frame in
      let tone = Float(sin(Double(frame) * 0.031 * Double(channel + 1))) * expf(-Float(frame) / 900)
      let noise = Float.random(in: -0.2...0.2, using: &rng)
      return (frame == 0 ? 0.8 : 0) + 0.4 * tone + noise
    }
  }
}

func save(_ rig: RigSnapshot, to url: URL) throws {
  try RigPreset(name: "Saved", rig: rig).encoded().write(to: url, options: .atomic)
}

@MainActor
@Test func presetRoundTripRestoresTheWholeRigAndLiveDescriptions() throws {
  let fixture = try RigFixture()
  let audio = fixture.audio()
  defer { audio.stop() }
  #expect(audio.amps.amps.count == 2 && audio.cabinets.cabinets.count == 2)

  // A rich rig: an opaque-state amp with EQ settings, retained settings on the amp that is not
  // selected, an independent cabinet, a selected special FX with another holding settings,
  // legacy and new chain identities, duplicates with independent opaque state, host bypass,
  // a hidden edited draft, a rack editor and changed global controls.
  let ampA = try #require(audio.amps.amps.firstIndex { $0.id == "org.example.amp-a" })
  audio.amps.select(ampA)
  try audio.amps.amps[ampA].restoreState(opaqueState(gain: 1.5, bias: 0.2, polarity: -1))
  try audio.amps.amps[ampA].equalizer?.setValue(2, 0.3)
  let ampB = audio.amps.amps[1 - ampA]
  try ampB.setValue(2, 0.9)
  try ampB.equalizer?.setValue(2, 0.1)
  audio.cabinets.toggleLock()
  audio.cabinets.select(
    try #require(audio.cabinets.cabinets.firstIndex { $0.id == "org.example.cab-b" }))
  try audio.cabinets.activeCabinet?.setValue(2, 0.4)
  let cabA = try #require(audio.cabinets.cabinets.first { $0.id == "org.example.cab-a" })
  try cabA.restoreState(opaqueState(gain: 0.5, bias: -0.1, polarity: -1))
  let sfxB = try #require(audio.specialFX.modules.firstIndex { $0.id == "org.example.sfx-b" })
  audio.specialFX.show(sfxB)
  audio.specialFX.setActive(audio.specialFX.modules[sfxB].id, true)
  try audio.specialFX.modules[sfxB].setValue(2, 0.6)
  try audio.specialFX.modules[1 - sfxB].restoreState(opaqueState(gain: 2, bias: 0.4, polarity: 1))
  let pre = audio.preFX
  #expect(pre.chain.map(\.id) == ["pre-fx.0", "pre-fx.1"])
  let opaquePre = try #require(pre.chain.first { $0.module.id == "org.example.pre-a" })
  try opaquePre.module.restoreState(opaqueState(gain: 1.25, bias: 0.05, polarity: -1))
  pre.duplicate(opaquePre.id)
  let copy = try #require(
    pre.chain.first { $0.module.id == "org.example.pre-a" && $0.id != opaquePre.id })
  try copy.module.restoreState(opaqueState(gain: 0.75, bias: -0.05, polarity: 1))
  pre.setBypassed("pre-fx.0", true)
  #expect(pre.chain.first { $0.id == "pre-fx.0" }?.hostBypassed == true)
  pre.chooseEditor(PedalCatalog.libraryPrefix + "org.example.pre-b", in: 2)
  let draft = try #require(pre.drafts.first)
  try draft.module.setValue(2, 0.33)
  pre.chooseEditor("", in: 2)
  #expect(pre.drafts.map(\.id) == [draft.id] && !pre.editors.visible.contains(draft.id))
  let post = audio.postFX
  post.showEditor(
    try #require(post.chain.first { $0.module.boardWidth == PedalEditorLayout.capacity }).id)
  var globals = audio.globals
  globals.inputGain = 3.5
  globals.gateEnabled = true
  globals.gateThreshold = -55
  globals.transpose = -2
  globals.doublerEnabled = true
  globals.doublerTime = 12.25
  globals.outputGain = -4
  audio.updateGlobals(globals)

  let saved = try audio.captureRig()
  let savedPayload = try payload(audio)
  let savedTargets = audio.midiTargets.map(\.id)
  let savedEditors = (pre.editors, post.editors)
  let input = testSignal()
  let savedAudio = try audio.renderOffline(input: input)
  let file = fixture.root.appendingPathComponent("rig.\(RigPreset.fileExtension)")
  try save(saved, to: file)

  // Excluded settings: changed after saving, they must survive the load.
  let stereo = audio.globals.stereoInput
  audio.updateTempo(TempoSettings(bpm: 97))
  let tempo = audio.tempo
  try randomizeRig(audio, seed: 42)
  #expect(try audio.captureRig() != saved)
  #expect(try audio.renderOffline(input: input) != savedAudio)

  let fixed = fixedModules(audio)
  var submitted: [[String: Any]] = []
  audio.submitPreset = { submitted.append($0) }
  try audio.applyRig(try PresetStore.read(file).rig)
  audio.submitPreset = nil
  #expect(try audio.captureRig() == saved)
  #expect(try payload(audio) == savedPayload)
  // The one request the audio side receives describes exactly what the controls now show.
  #expect(submitted.count == 1)
  var request = try #require(submitted.first)
  #expect(request.removeValue(forKey: "op") as? String == "preset")
  #expect(try JSONSerialization.data(withJSONObject: request, options: .sortedKeys) == savedPayload)

  // Parameters alone do not reproduce the sound: the opaque state must reach the audio side.
  var stripped = try audio.startConfiguration()
  stripped.removeValue(forKey: "globals")
  for key in ["cabinet", "eq", "specialFX", "preFX", "postFX"] {
    stripped[key] = (stripped[key] as? [[String: Any]])?.map {
      var description = $0
      description.removeValue(forKey: "state")
      return description
    }
  }
  if var amp = stripped["amp"] as? [String: Any] {
    amp.removeValue(forKey: "state")
    stripped["amp"] = amp
  }
  struct Rendered: Decodable { let output: [[Float]] }
  let parameterOnly = try JSONDecoder().decode(
    Rendered.self,
    from: AudioBridge().request([
      "op": "renderGraph", "configuration": stripped, "input": input, "bpm": audio.tempo.bpm,
    ])
  ).output
  #expect(parameterOnly != savedAudio)
  #expect(audio.midiTargets.map(\.id) == savedTargets)
  #expect(pre.editors == savedEditors.0 && post.editors == savedEditors.1)
  #expect(pre.drafts.map(\.id) == [draft.id] && pre.drafts[0].lifecycle == .editedDraft)
  #expect(try pre.drafts[0].module.value(2) == 0.33)
  #expect(pre.chain.first { $0.id == "pre-fx.0" }?.hostBypassed == true)
  #expect(zip(fixedModules(audio), fixed).allSatisfy { $0 === $1 })
  #expect(audio.amps.activeAmp?.id == "org.example.amp-a")
  #expect(audio.cabinets.selection == .independent("org.example.cab-b"))
  #expect(audio.specialFX.shownModule?.id == "org.example.sfx-b")
  #expect(audio.specialFX.active == ["org.example.sfx-b"])
  #expect(audio.globals.stereoInput == stereo && audio.tempo == tempo)
  #expect(try audio.renderOffline(input: input) == savedAudio)

  // A fresh controller and catalogs build the same live descriptions and the same sound.
  let fresh = fixture.audio()
  defer { fresh.stop() }
  try fresh.applyRig(try PresetStore.read(file).rig)
  #expect(try fresh.captureRig() == saved)
  #expect(try payload(fresh) == savedPayload)
  #expect(try fresh.renderOffline(input: input) == savedAudio)

  // Opaque state reaches the live description, not just the control copy.
  let description = try #require(
    (try audio.startConfiguration()["preFX"] as? [[String: Any]])?.first {
      $0["slot"] as? String == copy.id
    })
  let state = try #require(description["state"] as? [String: Any])
  #expect(state["schema"] as? UInt32 == 3)
  #expect(
    state["data"] as? String
      == opaqueState(gain: 0.75, bias: -0.05, polarity: 1).map { String(format: "%02x", $0) }
      .joined())
}

/// Randomized-state round trips (not memory fuzzing): each seed builds a rig, renders it from a
/// fresh graph, saves it, scrambles everything, reloads the file and renders again from a fresh
/// graph. These fixtures are deterministic, so audio must match exactly.
@MainActor
@Test func seededRandomRigsReloadToIdenticalDescriptionsAndAudio() throws {
  let fixture = try RigFixture()
  let audio = fixture.audio()
  defer { audio.stop() }
  let input = testSignal(frames: 1536, seed: 99)
  var changedAudio = 0
  for seed in UInt64(1)...8 {
    try randomizeRig(audio, seed: seed)
    let rig = try audio.captureRig()
    let description = try payload(audio)
    let rendered = try audio.renderOffline(input: input)
    #expect(try audio.renderOffline(input: input) == rendered)
    let file = fixture.root.appendingPathComponent("seed-\(seed).\(RigPreset.fileExtension)")
    try save(rig, to: file)
    try randomizeRig(audio, seed: seed &* 7919 &+ 13)
    if try audio.renderOffline(input: input) != rendered { changedAudio += 1 }
    try audio.applyRig(try PresetStore.read(file).rig)
    #expect(try audio.captureRig() == rig, "seed \(seed)")
    #expect(try payload(audio) == description, "seed \(seed)")
    #expect(try audio.renderOffline(input: input) == rendered, "seed \(seed)")
  }
  #expect(changedAudio >= 6)
}

@MainActor
@Test func rejectedPresetsLeaveTheRigAndLiveDescriptionsUntouched() throws {
  let fixture = try RigFixture()
  let audio = fixture.audio()
  defer { audio.stop() }
  try randomizeRig(audio, seed: 5)
  let valid = try audio.captureRig()
  try randomizeRig(audio, seed: 6)
  let before = try audio.captureRig()
  let beforePayload = try payload(audio)
  let input = testSignal(frames: 512)
  let beforeAudio = try audio.renderOffline(input: input)

  func unchanged(_ comment: Comment) throws {
    #expect(try audio.captureRig() == before, comment)
    #expect(try payload(audio) == beforePayload, comment)
  }
  func rejects(_ rig: RigSnapshot, _ comment: Comment) throws {
    #expect(throws: (any Error).self, comment) { try audio.applyRig(rig) }
    try unchanged(comment)
  }

  var rig = valid
  rig.preFX.chain[0].module.id = "org.example.missing"
  try rejects(rig, "missing chain module")
  rig = valid
  rig.preFX.drafts.append(
    PedalInstanceSnapshot(
      id: "pre-fx.draft", ordinal: 9, lifecycle: .editedDraft, hostBypassed: true,
      module: ModuleSnapshot(
        id: "org.example.missing", version: "1", abiVersion: 1, state: nil, parameters: [])))
  try rejects(rig, "missing draft module")
  rig = valid
  rig.amp.selected = "org.example.missing"
  try rejects(rig, "missing amp")
  rig = valid
  rig.cabinet.independent = "org.example.missing"
  try rejects(rig, "missing cabinet")
  rig = valid
  rig.specialFX.active = (rig.specialFX.active ?? []) + ["org.example.missing"]
  try rejects(rig, "missing active special FX")
  rig = valid
  rig.specialFX.active = ["org.example.sfx-a", "org.example.sfx-a"]
  try rejects(rig, "special FX active twice")
  rig = valid
  rig.postFX.chain.append(rig.postFX.chain[0])
  try rejects(rig, "duplicate instance identity")
  rig = valid
  rig.postFX.chain[0].id = "pre-fx.0"
  try rejects(rig, "identity from the other board")
  rig = valid
  rig.preFX.editors = EditorLayoutSnapshot(kind: .pedals, slots: ["pre-fx.unknown"])
  try rejects(rig, "editor for an unknown instance")
  rig = valid
  rig.preFX.chain[0].lifecycle = .editedDraft
  try rejects(rig, "draft inside the chain")
  rig = valid
  rig.globals.transpose = 0.5
  try rejects(rig, "invalid globals")
  let opaque = try #require(rig.amp.amps.firstIndex { $0.module.id == "org.example.amp-a" })
  rig = valid
  rig.amp.amps[opaque].module.state?.schemaVersion = 9
  try rejects(rig, "unsupported state schema")
  rig = valid
  rig.amp.amps[opaque].module.state?.data = opaqueState(gain: 1, bias: 0, polarity: 0.5)
  try rejects(rig, "state rejected by the module")
  rig = valid
  rig.amp.amps[opaque].module.state = nil
  try rejects(rig, "state missing for a module with state")
  rig = valid
  rig.amp.amps[opaque].module.abiVersion = 99
  try rejects(rig, "other module ABI")
  rig = valid
  rig.amp.amps[opaque].module.abiVersion = 0
  try rejects(rig, "module ABI 0")
  rig = valid
  rig.amp.amps[opaque].module.parameters[1].value = 7
  try rejects(rig, "parameter out of range")
  rig = valid
  rig.amp.amps.append(rig.amp.amps[opaque])
  try rejects(rig, "amp listed twice")
  rig = valid
  rig.amp.amps[opaque].module.state?.data = Data(count: 2 * 1024 * 1024)
  try rejects(rig, "oversized state")

  // The audio side rejecting the whole preset changes nothing either.
  audio.submitPreset = { _ in throw AmpError.message("Rejected by the audio side") }
  try rejects(valid, "audio rejection")
  audio.submitPreset = nil
  #expect(try audio.renderOffline(input: input) == beforeAudio)

  // Unreadable files are rejected before reaching the rig.
  let corrupt = fixture.root.appendingPathComponent("corrupt.\(RigPreset.fileExtension)")
  let encoded = try RigPreset(name: "Valid", rig: valid).encoded()
  var json = try #require(try JSONSerialization.jsonObject(with: encoded) as? [String: Any])
  for (data, comment) in [
    (encoded.prefix(encoded.count / 2), "truncated"), (Data("[]".utf8), "not a preset"),
    (Data(), "empty"),
  ] as [(Data, Comment)] {
    try data.write(to: corrupt)
    #expect(throws: (any Error).self, comment) { try PresetStore.read(corrupt) }
  }
  json["schemaVersion"] = RigPreset.schemaVersion + 1
  try JSONSerialization.data(withJSONObject: json).write(to: corrupt)
  #expect(throws: (any Error).self) { try PresetStore.read(corrupt) }
  json["schemaVersion"] = 1
  json["format"] = "dev.vespera.preset"
  try JSONSerialization.data(withJSONObject: json).write(to: corrupt)
  #expect(throws: (any Error).self) { try PresetStore.read(corrupt) }
  try unchanged("after corrupt files")

  // Unknown installed-module settings are carried, not dropped: they survive save and load.
  rig = valid
  rig.amp.amps.append(
    AmpSnapshot(
      module: ModuleSnapshot(
        id: "org.example.uninstalled", version: "3", abiVersion: 1,
        state: ModuleStateSnapshot(schemaVersion: 1, data: Data([1, 2, 3])), parameters: []),
      equalizer: nil))
  try audio.applyRig(rig)
  #expect(try audio.captureRig().amp.amps.contains { $0.module.id == "org.example.uninstalled" })
}

@MainActor
private final class PromptRecorder {
  var decisions: [PresetController.Decision] = []
  var names: [String?] = []
  var deletions: [Bool] = []
  var imports: [URL?] = []
  var asked: [String] = []

  var prompts: PresetController.Prompts {
    PresetController.Prompts(
      unsavedChanges: { [unowned self] name in
        asked.append(name)
        return decisions.removeFirst()
      },
      name: { [unowned self] _ in names.removeFirst() },
      confirmDelete: { [unowned self] _ in deletions.removeFirst() },
      importFile: { [unowned self] in imports.removeFirst() })
  }
}

@MainActor
@Test func presetLibraryTracksDirtyStateAutosavesDefaultAndAsksBeforeLosingChanges() throws {
  let fixture = try RigFixture()
  let audio = fixture.audio()
  defer { audio.stop() }
  let store = fixture.store()
  let prompts = PromptRecorder()
  let presets = PresetController(audio: audio, store: store, prompts: prompts.prompts)
  presets.restore()
  #expect(presets.selection == .defaultPreset && presets.error == nil)
  #expect(try store.loadDefault()?.rig == audio.captureRig())

  // Default saves itself; it is never dirty.
  let amp = try #require(audio.amps.activeAmp)
  try amp.setValue(2, 0.7)
  presets.flush()
  #expect(!presets.dirty && !presets.canSave && presets.title == "Default")
  #expect(try store.loadDefault()?.rig == audio.captureRig())
  let defaultRig = try audio.captureRig()

  // Save As selects a new, clean named preset.
  prompts.names = ["Crunch"]
  presets.saveAs()
  guard case .named(let crunch) = presets.selection else {
    Issue.record("Save As did not select the new preset")
    return
  }
  #expect(presets.title == "Crunch" && !presets.dirty)
  #expect(try store.load(crunch).rig == audio.captureRig())
  prompts.names = ["crunch"]
  presets.saveAs()
  #expect(presets.error != nil && presets.entries.count == 1)
  prompts.names = ["Default"]
  presets.saveAs()
  #expect(presets.error != nil && presets.entries.count == 1)

  // Editing a named preset marks it dirty without saving anything; undoing the edit clears it.
  let defaultFile = try Data(
    contentsOf: store.directory.appendingPathComponent("Default.\(RigPreset.fileExtension)"))
  try amp.setValue(2, 0.2)
  presets.flush()
  #expect(presets.dirty && presets.canSave && presets.title == "* Crunch")
  #expect(
    try Data(
      contentsOf: store.directory.appendingPathComponent("Default.\(RigPreset.fileExtension)"))
      == defaultFile)
  try amp.setValue(2, 0.7)
  presets.flush()
  #expect(!presets.dirty && presets.title == "Crunch")

  // Leaving a dirty preset asks first. Cancel keeps everything; Discard switches and loads.
  audio.preFX.add("org.example.pre-a")
  presets.flush()
  let edited = try audio.captureRig()
  #expect(presets.dirty)
  prompts.decisions = [.cancel]
  presets.select(.defaultPreset)
  #expect(prompts.asked == ["Crunch"] && presets.selection == .named(crunch) && presets.dirty)
  #expect(try audio.captureRig() == edited)
  prompts.decisions = [.discard]
  presets.select(.defaultPreset)
  #expect(presets.selection == .defaultPreset && !presets.dirty)
  #expect(try audio.captureRig() == defaultRig)
  presets.select(.named(crunch))
  #expect(try audio.captureRig() == store.load(crunch).rig && !presets.dirty)

  // Save from the prompt keeps the changes; a failed save cancels the switch.
  try amp.setValue(2, 0.45)
  presets.flush()
  prompts.decisions = [.save]
  presets.select(.defaultPreset)
  #expect(presets.selection == .defaultPreset)
  #expect(try store.load(crunch).module(amp.id)?.parameters[1].value == 0.45)
  presets.select(.named(crunch))
  try amp.setValue(2, 0.55)
  presets.flush()
  let named = store.url(for: crunch).deletingLastPathComponent()
  try FileManager.default.setAttributes([.posixPermissions: 0o555], ofItemAtPath: named.path)
  prompts.decisions = [.save]
  presets.select(.defaultPreset)
  try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: named.path)
  #expect(presets.selection == .named(crunch) && presets.dirty && presets.error != nil)
  #expect(try audio.amps.activeAmp?.value(2) == 0.55)
  #expect(presets.save() && !presets.dirty)

  // Previous and next follow the dropdown order and wrap.
  prompts.names = ["Alpha"]
  presets.saveAs()
  let alpha = presets.entries.first { $0.name == "Alpha" }!.id
  #expect(presets.order == [.defaultPreset, .named(alpha), .named(crunch)])
  presets.step(1)
  #expect(presets.selection == .named(crunch))
  presets.step(1)
  #expect(presets.selection == .defaultPreset)
  presets.step(-1)
  #expect(presets.selection == .named(crunch))

  // Reset loads the saved Default into the named preset, which then needs Save.
  presets.reset()
  #expect(presets.selection == .named(crunch) && presets.dirty)
  #expect(try audio.captureRig() == store.loadDefault()?.rig)
  #expect(try store.load(crunch).rig != store.loadDefault()?.rig)
  prompts.decisions = [.discard]
  presets.select(.named(alpha))
  #expect(!presets.dirty)

  // Quitting: a dirty named preset asks; Cancel keeps running, Discard leaves the file alone.
  try amp.setValue(2, 0.11)
  presets.flush()
  let alphaFile = try Data(contentsOf: store.url(for: alpha))
  prompts.decisions = [.cancel, .discard]
  #expect(!presets.prepareToTerminate())
  #expect(presets.prepareToTerminate())
  #expect(try Data(contentsOf: store.url(for: alpha)) == alphaFile)

  // Startup restores the last selected preset as saved, not the unsaved edits.
  let next = fixture.audio()
  defer { next.stop() }
  let restored = PresetController(audio: next, store: store, prompts: prompts.prompts)
  restored.restore()
  #expect(restored.selection == .named(alpha) && !restored.dirty)
  #expect(try next.captureRig() == store.load(alpha).rig)

  // Delete never applies to Default; deleting a named preset falls back to Default.
  prompts.decisions = [.discard]
  prompts.deletions = [false, true]
  presets.delete()
  #expect(presets.selection == .named(alpha))
  presets.delete()
  #expect(presets.selection == .defaultPreset && !presets.entries.contains { $0.id == alpha })
  #expect(!FileManager.default.fileExists(atPath: store.url(for: alpha).path))
  #expect(try audio.captureRig() == store.loadDefault()?.rig)
  presets.delete()
  presets.reset()
  #expect(
    presets.selection == .defaultPreset
      && FileManager.default.fileExists(
        atPath: store.directory.appendingPathComponent("Default.\(RigPreset.fileExtension)").path)
  )
}

@MainActor
@Test func importCopiesValidatedPresetsWithoutOverwritingOrLinking() throws {
  let fixture = try RigFixture()
  let audio = fixture.audio()
  defer { audio.stop() }
  let store = fixture.store()
  let prompts = PromptRecorder()
  let presets = PresetController(audio: audio, store: store, prompts: prompts.prompts)
  presets.restore()
  prompts.names = ["Lead"]
  presets.saveAs()
  try randomizeRig(audio, seed: 31)
  let source = fixture.root.appendingPathComponent("outside/Lead.\(RigPreset.fileExtension)")
  try FileManager.default.createDirectory(
    at: source.deletingLastPathComponent(), withIntermediateDirectories: true)
  let imported = try audio.captureRig()
  try RigPreset(name: "Lead", rig: imported).encoded().write(to: source)
  prompts.decisions = [.discard]
  let lead = try store.load(presets.entries[0].id)
  try audio.applyRig(lead.rig)
  presets.flush()

  prompts.imports = [source]
  presets.importPreset()
  #expect(presets.entries.map(\.name) == ["Lead", "Lead 2"] && presets.error == nil)
  guard case .named(let id) = presets.selection else {
    Issue.record("The imported preset is not selected")
    return
  }
  #expect(try audio.captureRig() == imported && !presets.dirty)
  #expect(try store.load(id).rig == imported)
  #expect(try store.load(presets.entries[0].id).rig == lead.rig)
  try FileManager.default.removeItem(at: source)
  #expect(try store.load(id).rig == imported)

  // Invalid or unresolvable files change nothing and copy nothing.
  let before = try audio.captureRig()
  var missing = imported
  missing.postFX.chain[0].module.id = "org.example.missing"
  try RigPreset(name: "Missing", rig: missing).encoded().write(to: source)
  let broken = fixture.root.appendingPathComponent("outside/broken.json")
  try Data("{\"format\":\"dev.vespera.rig-preset\"".utf8).write(to: broken)
  prompts.imports = [source, broken, nil]
  presets.importPreset()
  #expect(presets.error?.contains("org.example.missing") == true)
  presets.importPreset()
  #expect(presets.error != nil)
  presets.importPreset()
  #expect(presets.entries.count == 2 && presets.selection == .named(id))
  #expect(try audio.captureRig() == before)
}

@MainActor
@Test func startupFallsBackToDefaultWithoutOverwritingAnUnreadableDefault() throws {
  let fixture = try RigFixture()
  let store = fixture.store()
  let prompts = PromptRecorder()
  try FileManager.default.createDirectory(at: store.directory, withIntermediateDirectories: true)
  let defaultURL = store.directory.appendingPathComponent("Default.\(RigPreset.fileExtension)")
  try Data("not json".utf8).write(to: defaultURL)
  let missing = UUID()
  try store.saveSelection(.named(missing))
  let audio = fixture.audio()
  defer { audio.stop() }
  let presets = PresetController(audio: audio, store: store, prompts: prompts.prompts)
  presets.restore()
  #expect(presets.selection == .defaultPreset && presets.baseline == .unavailable)
  #expect(presets.error?.contains("Default") == true)
  #expect(presets.error?.contains("no longer in the library") == true)
  try audio.amps.activeAmp?.setValue(2, 0.1)
  presets.flush()
  #expect(try Data(contentsOf: defaultURL) == Data("not json".utf8))
}

extension RigPreset {
  func module(_ id: String) -> ModuleSnapshot? {
    rig.amp.amps.first { $0.module.id == id }?.module
  }
}

@MainActor
@Test func modulesFromOtherVersionsLoadWithANotice() throws {
  let fixture = try RigFixture()
  let audio = fixture.audio()
  defer { audio.stop() }
  let store = fixture.store()
  let prompts = PromptRecorder()
  let presets = PresetController(audio: audio, store: store, prompts: prompts.prompts)
  presets.restore()
  #expect(presets.notice == nil)
  try randomizeRig(audio, seed: 12)
  var rig = try audio.captureRig()
  let amp = try #require(rig.amp.amps.firstIndex { $0.module.id == "org.example.amp-a" })
  rig.amp.amps[amp].module.version = "0.0-test"
  let installed = rig.postFX.chain.map(\.module.version)
  try #require(!installed.isEmpty)
  for index in rig.postFX.chain.indices { rig.postFX.chain[index].module.version = "9.9-test" }
  let notices = try audio.applyRig(rig)
  #expect(notices.count >= 2)
  #expect(notices.contains { $0.contains("0.0-test") && $0.contains("Opaque State") })
  #expect(notices.contains { $0.contains("9.9-test") && $0.contains(installed[0]) })
  let loaded = try audio.captureRig()
  #expect(loaded.amp.amps.first { $0.module.id == "org.example.amp-a" }?.module.version == "2.1.0")
  #expect(loaded.postFX.chain.map(\.module.version) == installed)
  var expected = rig
  expected.amp.amps[amp].module.version = "2.1.0"
  for index in expected.postFX.chain.indices {
    expected.postFX.chain[index].module.version = installed[index]
  }
  #expect(loaded == expected)

  // Selecting such a preset shows the notice; loading one without differences clears it.
  let id = UUID()
  try store.save(RigPreset(name: "Older", rig: rig), as: id)
  presets.restore()
  presets.select(.named(id))
  #expect(presets.selection == .named(id) && presets.error == nil)
  #expect(presets.notice?.contains("0.0-test") == true)
  #expect(presets.notice?.contains("9.9-test") == true)
  prompts.decisions = [.discard]
  presets.select(.defaultPreset)
  #expect(presets.notice == nil)
}

/// Saved module settings keep their meaning from `VS_MODULE_SETTINGS_ABI_MINIMUM` on, so presets
/// written by modules built for ABI 1 load unchanged and are saved again with the current ABI.
@MainActor
@Test func presetsSavedWithAnEarlierModuleABIStillLoad() throws {
  let fixture = try RigFixture()
  let audio = fixture.audio()
  defer { audio.stop() }
  try randomizeRig(audio, seed: 21)
  let current = try audio.captureRig()
  let abi = try #require(current.modules.first?.abiVersion)
  #expect(abi > 1 && current.modules.allSatisfy { $0.abiVersion == abi })
  #expect(LoadedModule.restoresSavedABI(1) && LoadedModule.restoresSavedABI(abi))
  #expect(!LoadedModule.restoresSavedABI(0) && !LoadedModule.restoresSavedABI(abi + 1))
  let encoded = try #require(String(data: JSONEncoder().encode(current), encoding: .utf8))
  let marker = "\"abiVersion\":\(abi)"
  #expect(encoded.contains(marker))
  let saved = try JSONDecoder().decode(
    RigSnapshot.self,
    from: Data(encoded.replacingOccurrences(of: marker, with: "\"abiVersion\":1").utf8))
  #expect(saved.modules.allSatisfy { $0.abiVersion == 1 })
  try randomizeRig(audio, seed: 22)
  #expect(try audio.applyRig(saved).isEmpty)
  #expect(try audio.captureRig() == current)
}

@MainActor
@Test func failedRollbackIsReportedAndRecoveryFailuresAreCompound() throws {
  let fixture = try RigFixture()
  let audio = fixture.audio()
  defer { audio.stop() }
  let ampA = try #require(audio.amps.amps.first { $0.id == "org.example.amp-a" })
  let library = try #require(dlopen(ampA.binaryURL.path, RTLD_NOW | RTLD_NOLOAD))
  let control = try #require(dlsym(library, "opaque_state_prepare_control"))
  let failPrepare = unsafeBitCast(
    control, to: (@convention(c) () -> UnsafeMutablePointer<Int32>).self)()
  defer { failPrepare.pointee = 0 }
  var rig = try audio.captureRig()
  let index = try #require(rig.amp.amps.firstIndex { $0.module.id == ampA.id })
  rig.amp.amps[index].module.state?.data = opaqueState(gain: 1.5, bias: 0.3, polarity: -1)
  rig.amp.amps[index].module.parameters[1].value = 1.5
  let before = try audio.captureRig()
  let beforePayload = try payload(audio)

  // A prepared control copy that cannot prepare again fails the commit after the audio side
  // accepted the preset. With the audio side restored, the original error is reported alone.
  try ampA.prepare()
  failPrepare.pointee = 1
  var requests: [[String: Any]] = []
  audio.submitPreset = { requests.append($0) }
  #expect(throws: (any Error).self) { try audio.applyRig(rig) }
  #expect(requests.count == 2 && audio.error == nil)
  #expect(try audio.captureRig() == before && payload(audio) == beforePayload)
  let restored = try #require(requests.last)
  var expected = try audio.startConfiguration()
  expected["op"] = "preset"
  #expect(
    try JSONSerialization.data(withJSONObject: restored, options: .sortedKeys)
      == JSONSerialization.data(withJSONObject: expected, options: .sortedKeys))

  // When restoring the audio side fails too, the error says so and is shown.
  failPrepare.pointee = 0
  try ampA.prepare()
  failPrepare.pointee = 1
  var calls = 0
  audio.submitPreset = { _ in
    calls += 1
    if calls == 2 { throw AmpError.message("audio restore refused") }
  }
  var thrown: String?
  do { try audio.applyRig(rig) } catch { thrown = error.localizedDescription }
  #expect(calls == 2)
  #expect(thrown?.contains("Restoring the previous settings also failed") == true)
  #expect(thrown?.contains("audio restore refused") == true)
  #expect(audio.error == thrown)
  #expect(try audio.captureRig() == before)
}

@MainActor
@Test func presetFilesBoundModuleListsAndNames() throws {
  let fixture = try RigFixture()
  let audio = fixture.audio()
  defer { audio.stop() }
  let rig = try audio.captureRig()
  func padded(_ count: Int) -> RigSnapshot {
    var result = rig
    let extra = count - result.amp.amps.count
    result.amp.amps += (0..<extra).map {
      AmpSnapshot(
        module: ModuleSnapshot(
          id: "org.example.extra-\($0)", version: "1", abiVersion: 1, state: nil, parameters: []),
        equalizer: nil)
    }
    result.cabinet.cabinets = (0..<count).map {
      ModuleSnapshot(
        id: "org.example.cab-\($0)", version: "1", abiVersion: 1, state: nil, parameters: [])
    }
    result.specialFX.modules = result.cabinet.cabinets
    return result
  }
  #expect(throws: Never.self) { try padded(RigSnapshot.maximumInstances).validate() }
  var over = padded(RigSnapshot.maximumInstances)
  over.amp.amps.append(over.amp.amps[0])
  #expect(throws: (any Error).self) { try over.validate() }
  over = padded(RigSnapshot.maximumInstances)
  over.cabinet.cabinets.append(over.cabinet.cabinets[0])
  #expect(throws: (any Error).self) { try over.validate() }
  over = padded(RigSnapshot.maximumInstances)
  over.specialFX.modules.append(over.specialFX.modules[0])
  #expect(throws: (any Error).self) { try over.validate() }

  let store = fixture.store()
  let longest = String(repeating: "n", count: PresetStore.maximumNameLength)
  let names = [
    longest, longest + "n", "two\nlines", "default", " padded", "", "Kept",
  ]
  for name in names { try store.save(RigPreset(name: name, rig: rig), as: UUID()) }
  let listed = store.entries()
  #expect(Set(listed.entries.map(\.name)) == [longest, "Kept"])
  #expect(listed.failures.count == names.count - 2)

  try store.saveDefault(rig)
  #expect(try store.loadDefault()?.name == PresetStore.defaultName)
  let defaultURL = store.directory.appendingPathComponent("Default.\(RigPreset.fileExtension)")
  try RigPreset(name: "Other", rig: rig).encoded().write(to: defaultURL)
  #expect(throws: (any Error).self) { try store.loadDefault() }
}

@MainActor
@Test func savedEditorLayoutsLoadAsBoardSpansIncludingLegacyRacks() throws {
  let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
  try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
  defer { try? FileManager.default.removeItem(at: root) }
  let packages = [
    try TestModulePackage(
      category: "post-fx", id: "org.example.delay", boardWidth: 2, parent: root),
    try TestModulePackage(category: "post-fx", id: "org.example.pedal", parent: root),
    try TestModulePackage(
      category: "post-fx", id: "org.example.rack", presentation: .rack, parent: root),
  ]
  let board = PedalCatalog(position: .post, directory: root)
  #expect(board.error == nil && packages.count == 3)
  #expect(board.chain.map(\.id) == ["post-fx.0", "post-fx.1", "post-fx.2"])
  let saved = try board.snapshot()
  #expect(
    saved.editors == EditorLayoutSnapshot(kind: .pedals, slots: ["post-fx.0", "post-fx.1", nil]))

  func load(_ json: String) throws -> PedalEditorLayout {
    var snapshot = saved
    snapshot.editors = try JSONDecoder().decode(EditorLayoutSnapshot.self, from: Data(json.utf8))
    return try board.stage(snapshot).editors
  }
  // Layouts written before editors declared board widths.
  #expect(try load(#"{"kind": "rack", "slots": ["post-fx.2"]}"#) == .rack("post-fx.2"))
  #expect(
    try load(#"{"kind": "pedals", "slots": ["post-fx.1", null, "post-fx.0"]}"#).spans
      == [.editor("post-fx.1", width: 1), .free, .editor("post-fx.0", width: 2)])
  #expect(
    try load(#"{"kind": "pedals", "slots": [null, "post-fx.1"]}"#) == .pedals([nil, "post-fx.1"]))
  #expect(try load(#"{"kind": "pedals", "slots": []}"#) == PedalEditorLayout())
  // Saved spans wider than the board now are, hiding what no longer fits from the right.
  #expect(
    try load(#"{"kind": "pedals", "slots": ["post-fx.1", null, null, "post-fx.0"]}"#).spans
      == [.editor("post-fx.1", width: 1), .free, .editor("post-fx.0", width: 2)])
  #expect(
    try load(#"{"kind": "pedals", "slots": ["post-fx.0", "post-fx.2"]}"#).spans
      == [.editor("post-fx.0", width: 2), .free, .free])

  for invalid in [
    #"{"kind": "rack", "slots": []}"#, #"{"kind": "rack", "slots": [null]}"#,
    #"{"kind": "rack", "slots": ["post-fx.2", null]}"#,
    #"{"kind": "pedals", "slots": [null, null, null, null, null]}"#,
    #"{"kind": "pedals", "slots": ["post-fx.1", "post-fx.1"]}"#,
    #"{"kind": "pedals", "slots": ["post-fx.9"]}"#,
  ] {
    #expect(throws: AmpError.self, "\(invalid)") { try load(invalid) }
  }

  // Current layouts save one slot per span and load unchanged.
  board.chooseEditor("post-fx.2", in: 0)
  board.chooseEditor("post-fx.0", in: 2)
  board.chooseEditor("post-fx.1", in: 0)
  let layout = board.editors
  #expect(layout.spans == [.editor("post-fx.1", width: 1), .free, .editor("post-fx.0", width: 2)])
  let current = try board.snapshot()
  #expect(
    current.editors == EditorLayoutSnapshot(kind: .pedals, slots: ["post-fx.1", nil, "post-fx.0"]))
  let decoded = try JSONDecoder().decode(BoardSnapshot.self, from: JSONEncoder().encode(current))
  board.chooseEditor("post-fx.2", in: 0)
  let staged = try board.stage(decoded)
  #expect(staged.editors == layout)
  board.commit(staged)
  #expect(board.editors == layout)
}

@Test func presetSchemaFollowsWhatSpecialFXNeed() throws {
  func rig(_ special: SpecialFXSection, files: Bool = false) -> RigSnapshot {
    var module = ModuleSnapshot(
      id: "org.example.sfx-a", version: "1", abiVersion: 2, state: nil, parameters: [])
    if files {
      module.files = [
        ModuleFileReference(
          slot: 1, scope: .library, path: String(repeating: "a", count: 64), name: "ir.wav",
          bytes: 4,
          sha256: String(repeating: "a", count: 64))
      ]
    }
    var special = special
    if !special.modules.isEmpty { special.modules[0] = module }
    return RigSnapshot(
      globals: PresetGlobals(GlobalControlSettings()), amp: AmpSection(selected: nil, amps: []),
      cabinet: CabinetSection(independent: nil, cabinets: []), specialFX: special,
      preFX: BoardSnapshot(chain: [], drafts: [], editors: .init(kind: .pedals, slots: [])),
      postFX: BoardSnapshot(chain: [], drafts: [], editors: .init(kind: .pedals, slots: [])))
  }
  let a = "org.example.sfx-a"
  let b = "org.example.sfx-b"
  let module = ModuleSnapshot(id: a, version: "1", abiVersion: 2, state: nil, parameters: [])
  for (section, files, schema) in [
    (SpecialFXSection(selected: a, active: [a], modules: [module]), false, 1),
    (SpecialFXSection(selected: a, active: [a], modules: [module]), true, 2),
    (SpecialFXSection(selected: nil, active: [], modules: []), false, 1),
    (SpecialFXSection(selected: a, modules: [module]), false, 1),
    (SpecialFXSection(selected: a, active: [], modules: [module]), false, 3),
    (SpecialFXSection(selected: a, active: [b], modules: [module]), false, 3),
    (SpecialFXSection(selected: a, active: [a, b], modules: [module]), true, 3),
    (SpecialFXSection(selected: nil, active: [a], modules: [module]), false, 3),
  ] as [(SpecialFXSection, Bool, Int)] {
    let preset = RigPreset(name: "P", rig: rig(section, files: files))
    #expect(preset.schemaVersion == schema, "\(section)")
    let data = try preset.encoded()
    let decoded = try RigPreset.decode(data)
    #expect(decoded.schemaVersion == schema && decoded.rig == preset.rig)
    if schema == 3 {
      var json = try #require(try JSONSerialization.jsonObject(with: data) as? [String: Any])
      for older in [1, 2] {
        json["schemaVersion"] = older
        #expect("\(section) as schema \(older)") {
          try RigPreset.decode(try JSONSerialization.data(withJSONObject: json))
        } throws: {
          $0.localizedDescription == "Multiple special FX require preset schema 3"
            || (files && older == 1
              && $0.localizedDescription == "File references require preset schema 2")
        }
      }
    }
  }

  // A legacy file has no active list: its selected module alone processes, and decoding then
  // encoding it again reproduces the file exactly.
  let legacy = try RigPreset(
    name: "Legacy", rig: rig(SpecialFXSection(selected: a, modules: [module]))
  ).encoded()
  #expect(String(decoding: legacy, as: UTF8.self).contains("\"active\"") == false)
  let read = try RigPreset.decode(legacy)
  #expect(read.schemaVersion == 1 && read.rig.specialFX.activeModules == [a])
  #expect(try read.encoded() == legacy)

  var twice = rig(SpecialFXSection(selected: a, active: [a, a], modules: [module]))
  #expect(throws: AmpError.self) { try twice.validate() }
  twice.specialFX.active = (0...RigSnapshot.maximumInstances).map { "org.example.\($0)" }
  #expect(throws: AmpError.self) { try twice.validate() }
}

@MainActor
@Test func specialFXPresetsChooseShownAndActiveModules() throws {
  let first = try TestModulePackage(category: "special-fx", id: "org.example.sfx-a")
  let second = try TestModulePackage(category: "special-fx", id: "org.example.sfx-b")
  let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
  defer { try? FileManager.default.removeItem(at: root) }
  for package in [first, second] { try package.link(into: root) }
  let empty = root.appendingPathComponent("empty")
  let audio = AudioController(
    amps: AmpCatalog(directory: empty), preFX: PedalCatalog(directory: empty),
    postFX: PedalCatalog(position: .post, directory: empty),
    cabinets: CabinetCatalog(directory: empty), specialFX: SpecialFXCatalog(directory: root),
    defaults: MemoryDefaults())
  defer { audio.stop() }
  let base = try audio.captureRig()
  #expect(base.specialFX.active == [] && base.specialFX.selected == "org.example.sfx-a")
  #expect(RigPreset(name: "Fresh", rig: base).schemaVersion == 3)

  var requests: [[String: Any]] = []
  audio.submitPreset = { requests.append($0) }
  var rig = base
  rig.specialFX.selected = nil
  rig.specialFX.active = ["org.example.sfx-b"]
  try audio.applyRig(rig)
  #expect(audio.specialFX.shownModule?.id == "org.example.sfx-b")
  #expect(audio.specialFX.active == ["org.example.sfx-b"])
  let section = try #require(requests.last?["specialFX"] as? [[String: Any]])
  #expect(section.map { $0["slot"] as? String } == ["specialFX.org.example.sfx-b"])

  // A legacy preset saved before any special FX was installed selects nothing and loads.
  rig = base
  rig.specialFX = SpecialFXSection(selected: nil, modules: [])
  try audio.applyRig(rig)
  #expect(audio.specialFX.active.isEmpty && audio.specialFX.shownModule?.id == "org.example.sfx-a")

  // Selecting an uninstalled module only affects the shown editor, unless it is active.
  rig = base
  rig.specialFX.selected = "org.example.uninstalled"
  rig.specialFX.active = ["org.example.sfx-a", "org.example.sfx-b"]
  try audio.applyRig(rig)
  #expect(audio.specialFX.active == ["org.example.sfx-a", "org.example.sfx-b"])
  let before = try audio.captureRig()
  for active in [["org.example.uninstalled"], nil] as [[String]?] {
    rig = base
    rig.specialFX.selected = "org.example.uninstalled"
    rig.specialFX.active = active
    #expect(throws: AmpError.self) { try audio.applyRig(rig) }
    #expect(try audio.captureRig() == before)
  }

  // Unavailable settings survive a load and the next capture.
  rig = base
  rig.specialFX.modules.append(
    ModuleSnapshot(
      id: "org.example.uninstalled", version: "1", abiVersion: 2, state: nil, parameters: []))
  try audio.applyRig(rig)
  #expect(try audio.captureRig().specialFX.modules.contains { $0.id == "org.example.uninstalled" })
}
