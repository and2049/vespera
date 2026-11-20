import AppKit
import SwiftUI
import Testing

@testable import VesperaUI

/// A catalog over three installed modules: `a` (with a power parameter), `b` (without one) and
/// `rack` (four board units wide, with a power parameter), discovered in that order.
@MainActor
private final class ChainFixture {
  let root: URL
  let packages: [TestModulePackage]
  let board: PedalCatalog
  var changes: [[[String: Any]]] = []

  init(position: PedalCatalog.Position = .post) throws {
    root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    packages = [
      try TestModulePackage(category: position.rawValue, id: "org.example.a", parent: root),
      try TestModulePackage(
        category: position.rawValue, id: "org.example.b", bypass: nil, parent: root),
      try TestModulePackage(
        category: position.rawValue, id: "org.example.rack", boardWidth: 4, parent: root),
    ]
    board = PedalCatalog(position: position, directory: root)
    board.onChange = { [unowned self] in changes.append($0) }
  }

  deinit { try? FileManager.default.removeItem(at: root) }

  func id(_ module: String, _ occurrence: Int = 0) -> String {
    board.chain.filter { $0.module.id == "org.example.\(module)" }[occurrence].id
  }

  /// The audio-relevant state: descriptions serialized with sorted keys.
  func audioState() throws -> Data {
    try JSONSerialization.data(withJSONObject: board.audioDescriptions(), options: .sortedKeys)
  }
}

@MainActor
@Test func choosingEditorsNeverChangesTheProcessingChain() throws {
  let fixture = try ChainFixture()
  let board = fixture.board
  #expect(board.error == nil)
  #expect(board.chain.map(\.module.id) == ["org.example.a", "org.example.b", "org.example.rack"])
  #expect(board.editors == .pedals([fixture.id("a"), fixture.id("b"), nil]))
  let modules = board.chain.map(\.module)
  try board.chain[0].module.setValue(2, 0.8)
  let before = try fixture.audioState()

  board.showEditor(fixture.id("rack"))
  #expect(board.editors == .rack(fixture.id("rack")))
  #expect(board.editors.spans == [.editor(fixture.id("rack"), width: 4)])
  #expect(board.hiddenInstances.map(\.id) == [fixture.id("a"), fixture.id("b")])
  board.chooseEditor(fixture.id("b"), in: 0)
  #expect(board.editors == .pedals([fixture.id("b"), nil, nil]))
  board.chooseEditor(fixture.id("a"), in: 2)
  board.chooseEditor(fixture.id("b"), in: 2)
  #expect(board.editors == .pedals([fixture.id("a"), nil, fixture.id("b")]))
  board.chooseEditor("", in: 0)
  board.chooseEditor("", in: 2)
  #expect(board.editors.visible.isEmpty)
  #expect(board.hiddenInstances.count == 3)

  #expect(fixture.changes.isEmpty)
  #expect(try fixture.audioState() == before)
  #expect(zip(board.chain.map(\.module), modules).allSatisfy { $0 === $1 })
  #expect(try board.chain[0].module.value(2) == 0.8)
  let slots = try board.audioDescriptions().map { $0["slot"] as? String }
  #expect(slots == board.chain.map(\.id))
}

@MainActor
@Test func duplicatesHaveIndependentIdentityAndState() throws {
  let fixture = try ChainFixture(position: .pre)
  let board = fixture.board
  let original = fixture.id("a")
  try board.chain[0].module.setValue(2, 0.9)
  try board.chain[0].module.setValue(1, 1)
  board.duplicate(original)
  #expect(fixture.changes.count == 1)
  #expect(board.chain[0].id == original)
  let copy = board.chain[1]
  #expect(copy.id != original && copy.module.id == "org.example.a")
  #expect(copy.module !== board.chain[0].module)
  #expect(copy.title == "External Test Amp 2")
  #expect(try copy.module.value(2) == 0.9 && copy.module.value(1) == 1)
  try copy.module.setValue(2, 0.2)
  #expect(try board.chain[0].module.value(2) == 0.9)
  #expect(board.editors.visible.contains(copy.id))

  board.add("org.example.a")
  let third = try #require(board.chain.last)
  #expect(third.title == "External Test Amp 3")
  #expect(try third.module.value(2) == 0.5)
  board.remove(copy.id)
  board.add("org.example.a")
  #expect(board.chain.last?.title == "External Test Amp 2")
  let ids = board.chain.map(\.id)
  #expect(Set(ids).count == ids.count)
  let slots = try #require(fixture.changes.last).map { $0["slot"] as? String }
  #expect(slots == ids)
}

@MainActor
@Test func reorderingAndBypassPreserveIdentitySettingsAndPosition() throws {
  let fixture = try ChainFixture()
  let board = fixture.board
  let a = fixture.id("a")
  let b = fixture.id("b")
  let rack = fixture.id("rack")
  let editors = board.editors
  try board.chain[0].module.setValue(2, 0.7)
  board.move(a, to: 2)
  #expect(board.chain.map(\.id) == [b, rack, a])
  #expect(try board.instance(a)?.module.value(2) == 0.7)
  board.move(rack, by: -1)
  #expect(board.chain.map(\.id) == [rack, b, a])
  #expect(board.editors == editors)

  board.setBypassed(b, true)
  #expect(board.instance(b)?.bypassed == true)
  #expect(board.chain.map(\.id) == [rack, b, a])
  let description = try #require(fixture.changes.last)
  #expect(description.map { $0["slot"] as? String } == [rack, b, a])
  #expect(description[1]["bypassed"] as? Bool == true)
  board.move(b, to: 0)
  #expect(board.instance(b)?.bypassed == true)
  #expect(try board.audioDescriptions()[0]["bypassed"] as? Bool == true)
  board.setBypassed(b, false)
  #expect(try board.audioDescriptions()[0]["bypassed"] == nil)

  let changes = fixture.changes.count
  try board.instance(a)?.module.setValue(1, 1)
  #expect(board.instance(a)?.bypassed == false)
  var parameters: [(String, UInt32, Float)] = []
  board.onParameterChange = { parameters.append(($0, $1, $2)) }
  board.setBypassed(a, true)
  #expect(board.instance(a)?.bypassed == true)
  #expect(parameters.count == 1 && parameters[0].0 == a && parameters[0].1 == 1)
  #expect(parameters[0].2 == 0)
  #expect(fixture.changes.count == changes)
  #expect(try board.audioDescriptions().map { $0["slot"] as? String } == [b, rack, a])
}

@MainActor
@Test func removingAShownEditorChoosesTheNearestHiddenSurvivor() throws {
  let fixture = try ChainFixture()
  let board = fixture.board
  board.add("org.example.a")
  let a = fixture.id("a")
  let b = fixture.id("b")
  let rack = fixture.id("rack")
  let a2 = fixture.id("a", 1)
  board.chooseEditor(a, in: 0)
  board.chooseEditor(b, in: 1)
  board.chooseEditor("", in: 2)
  board.remove(a)
  #expect(board.editors == .pedals([a2, b, nil]))
  board.remove(b)
  #expect(board.editors == .pedals([a2, nil, nil]))

  board.showEditor(rack)
  board.remove(rack)
  #expect(board.chain.map(\.id) == [a2])
  #expect(board.editors == .pedals([a2]))
  board.add("org.example.rack")
  let rack2 = fixture.id("rack")
  #expect(board.editors == .rack(rack2))
  board.chooseEditor(a2, in: 0)
  board.remove(a2)
  #expect(board.editors == .rack(rack2))
  board.remove(rack2)
  #expect(board.chain.isEmpty && board.editors.visible.isEmpty)
}

@MainActor
@Test func rejectedChainChangesRollBackModelAndEditors() throws {
  let fixture = try ChainFixture()
  let board = fixture.board
  try board.chain[0].module.setValue(1, 1)
  let ids = board.chain.map(\.id)
  let editors = board.editors
  let before = try fixture.audioState()
  board.onChange = { _ in throw AmpError.message("Rejected") }
  board.add("org.example.a")
  board.duplicate(ids[0])
  board.remove(ids[0])
  board.move(ids[0], to: 2)
  board.setBypassed(ids[1], true)
  #expect(board.chain.map(\.id) == ids)
  #expect(board.editors == editors)
  #expect(board.instance(ids[1])?.bypassed == false)
  #expect(board.error == "Rejected")
  #expect(try fixture.audioState() == before)

  board.onChange = nil
  board.onParameterChange = { _, _, _ in throw AmpError.message("Refused") }
  board.setBypassed(ids[0], true)
  #expect(board.instance(ids[0])?.bypassed == false)
  #expect(board.error == "Refused")
}

@MainActor
@Test func onlyADeclaredBypassParameterBypassesTheModule() throws {
  let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
  defer { try? FileManager.default.removeItem(at: root) }
  let package = try TestModulePackage(
    category: "post-fx", id: "org.example.level", bypass: nil, parent: root)
  let board = PedalCatalog(position: .post, directory: root)
  let instance = try #require(board.chain.first)
  #expect(instance.module.id == package.manifest.id && instance.module.bypassParameter == nil)
  #expect(instance.module.parameters.first?.name == "Enabled")
  var parameters = 0
  board.onParameterChange = { _, _, _ in parameters += 1 }
  board.setBypassed(instance.id, true)
  #expect(parameters == 0 && board.instance(instance.id)?.bypassed == true)
  #expect(try board.audioDescriptions()[0]["bypassed"] as? Bool == true)
  #expect(try board.chain[0].module.value(1) == 0)
}

@MainActor
@Test func aBypassParameterOtherThanParameterOneBypassesTheModule() throws {
  let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
  defer { try? FileManager.default.removeItem(at: root) }
  let package = try TestModulePackage(
    category: "post-fx", id: "org.example.second", bypass: 2, parent: root)
  let board = PedalCatalog(position: .post, directory: root)
  let instance = try #require(board.chain.first)
  #expect(instance.module.id == package.manifest.id && instance.module.bypassParameter == 2)
  var changes: [UInt32] = []
  board.onParameterChange = { _, id, _ in changes.append(id) }
  board.setBypassed(instance.id, true)
  #expect(changes == [2] && board.instance(instance.id)?.bypassed == true)
  #expect(try board.chain[0].module.value(2) == 0)
  #expect(try board.chain[0].module.value(1) == 0)
  #expect(try board.audioDescriptions()[0]["bypassed"] == nil)
  board.setBypassed(instance.id, false)
  #expect(changes == [2, 2] && board.instance(instance.id)?.bypassed == false)
  #expect(try board.chain[0].module.value(2) == 1)
}

/// Reports how many module instances a fixture package's library creates, keeping the library
/// loaded so that its count survives other code loading and closing it.
private final class InstanceCounter {
  private let handle: UnsafeMutableRawPointer
  private let count: @convention(c) () -> Int32

  init(_ package: TestModulePackage) throws {
    let path = package.directory.appendingPathComponent("custom.so").path
    handle = try #require(dlopen(path, RTLD_NOW | RTLD_LOCAL))
    count = unsafeBitCast(
      try #require(dlsym(handle, "external_test_created_instances")),
      to: (@convention(c) () -> Int32).self)
  }

  deinit { dlclose(handle) }

  var created: Int32 { count() }
}

@MainActor
@Test func editorLibraryListsEveryInstalledModuleWithoutCreatingInstances() throws {
  let fixture = try ChainFixture()
  let family = [
    try TestModulePackage(
      category: "post-fx", id: "org.example.chorus", family: "Zeta Family",
      parent: fixture.root),
    try TestModulePackage(
      category: "post-fx", id: "org.example.delay", family: "Zeta Family",
      parent: fixture.root),
  ]
  let counters = try family.map(InstanceCounter.init)
  let board = PedalCatalog(position: .post, directory: fixture.root)
  var changes = 0
  board.onChange = { _ in changes += 1 }
  #expect(board.error == nil)
  #expect(board.chain.map(\.module.id) == ["org.example.a", "org.example.b", "org.example.rack"])
  #expect(counters.map(\.created) == [0, 0])

  let options = board.editorOptions
  let library = options.filter { $0.id.hasPrefix(PedalCatalog.libraryPrefix) }
  #expect(
    library.map(\.id)
      == [
        "org.example.a", "org.example.b", "org.example.rack", "org.example.chorus",
        "org.example.delay",
      ].map { PedalCatalog.libraryPrefix + $0 })
  #expect(library.map(\.group) == [nil, nil, nil, "Zeta Family", "Zeta Family"])
  #expect(
    options.filter { $0.group == PedalCatalog.currentGroup }.map(\.id) == board.chain.map(\.id))
  #expect(options.last == SettingsOption(id: "", title: "No editor"))

  board.chooseEditor(PedalCatalog.libraryPrefix + "org.example.delay", in: 1)
  #expect(counters.map(\.created) == [0, 1])
  #expect(changes == 0 && board.chain.count == 3 && board.drafts.count == 1)
  #expect(board.editors.spans[1].id == board.drafts[0].id)
}

@MainActor
@Test func libraryChoicesCreateIndependentDraftsOutsideTheChain() throws {
  let fixture = try ChainFixture()
  let board = fixture.board
  let chain = board.chain.map(\.id)
  let library = PedalCatalog.libraryPrefix + "org.example.a"
  board.chooseEditor(library, in: 0)
  board.chooseEditor(library, in: 1)
  #expect(board.drafts.count == 2)
  let first = board.drafts[0]
  let second = board.drafts[1]
  #expect(first.id != second.id && first.module !== second.module)
  #expect(board.drafts.map(\.title) == ["External Test Amp 2", "External Test Amp 3"])
  #expect(board.drafts.allSatisfy { $0.lifecycle == .untouchedDraft && $0.bypassed })
  #expect(board.editors == .pedals([first.id, second.id, nil]))
  try first.module.setValue(2, 0.3)
  #expect(board.instance(first.id)?.lifecycle == .editedDraft)
  #expect(try second.module.value(2) == 0.5)
  #expect(fixture.changes.isEmpty && board.chain.map(\.id) == chain)
  #expect(try board.audioDescriptions().map { $0["slot"] as? String } == chain)
  #expect(board.editorOptions.contains { $0.id == first.id && $0.title == first.title })

  // An untouched draft holds only defaults and goes once its editor leaves the board; an edited
  // one stays until it is removed.
  board.chooseEditor(fixture.id("b"), in: 1)
  board.chooseEditor("", in: 0)
  #expect(board.drafts.map(\.id) == [first.id])
  #expect(board.editors == .pedals([nil, fixture.id("b"), nil]))
  board.showEditor(first.id, in: 2)
  #expect(board.editors == .pedals([nil, fixture.id("b"), first.id]))
  #expect(try first.module.value(2) == 0.3)
  #expect(fixture.changes.isEmpty)

  // Switching a draft on, here through its own power control, appends it once.
  try first.module.setValue(1, 1)
  #expect(fixture.changes.count == 1 && board.drafts.isEmpty)
  #expect(board.chain.map(\.id) == chain + [first.id])
  #expect(board.instance(first.id)?.lifecycle == .chained)
  let added = try #require(fixture.changes.last?.last)
  #expect(added["slot"] as? String == first.id)
  let parameters = try #require(added["parameters"] as? [[String: Any]])
  #expect(parameters.first { $0["id"] as? UInt32 == 1 }?["value"] as? Float == 1)
  #expect(parameters.first { $0["id"] as? UInt32 == 2 }?["value"] as? Float == 0.3)
  #expect(try first.module.value(1) == 1)

  // Once in the chain, switching off bypasses in place and switching on again never re-adds.
  var forwarded: [(String, UInt32, Float)] = []
  board.onParameterChange = { forwarded.append(($0, $1, $2)) }
  board.move(first.id, to: 0)
  board.setBypassed(first.id, true)
  board.setBypassed(first.id, false)
  #expect(forwarded.map(\.0) == [first.id, first.id] && forwarded.map(\.2) == [0, 1])
  #expect(board.chain.map(\.id) == [first.id] + chain)
  #expect(fixture.changes.count == 2)

  // A module without a power parameter is held out of the graph until it is switched on.
  board.chooseEditor(PedalCatalog.libraryPrefix + "org.example.b", in: 0)
  let plain = try #require(board.drafts.first)
  #expect(plain.hostBypassed && plain.bypassed)
  board.setBypassed(plain.id, false)
  #expect(board.drafts.isEmpty && board.chain.last?.id == plain.id)
  let description = try #require(fixture.changes.last?.last)
  #expect(description["slot"] as? String == plain.id && description["bypassed"] == nil)
  #expect(fixture.changes.count == 3)
}

@MainActor
@Test func rejectedActivationKeepsTheDraftOffWithItsSettings() throws {
  let fixture = try ChainFixture()
  let board = fixture.board
  let chain = board.chain.map(\.id)
  board.chooseEditor(PedalCatalog.libraryPrefix + "org.example.a", in: 0)
  board.chooseEditor(PedalCatalog.libraryPrefix + "org.example.b", in: 1)
  let powered = board.drafts[0]
  let plain = board.drafts[1]
  try powered.module.setValue(2, 0.4)
  let editors = board.editors
  board.onChange = { _ in throw AmpError.message("Rejected") }

  #expect(throws: (any Error).self) { try powered.module.setValue(1, 1) }
  board.setBypassed(powered.id, false)
  board.setBypassed(plain.id, false)
  #expect(board.error == "Rejected")
  #expect(board.chain.map(\.id) == chain)
  #expect(board.drafts.map(\.id) == [powered.id, plain.id])
  #expect(board.drafts.allSatisfy { $0.bypassed })
  #expect(try powered.module.value(1) == 0 && powered.module.value(2) == 0.4)
  #expect(board.editors == editors)

  board.onChange = { fixture.changes.append($0) }
  board.setBypassed(powered.id, false)
  #expect(board.error == nil && board.chain.map(\.id) == chain + [powered.id])
  #expect(fixture.changes.count == 1 && board.drafts.map(\.id) == [plain.id])
}

@MainActor
@Test func rackAndPedalDraftsReplaceEachOtherWithoutTouchingTheChain() throws {
  let fixture = try ChainFixture()
  let board = fixture.board
  let chain = board.chain.map(\.id)
  board.chooseEditor(PedalCatalog.libraryPrefix + "org.example.a", in: 0)
  board.chooseEditor(PedalCatalog.libraryPrefix + "org.example.rack", in: 1)
  let rack = try #require(board.drafts.last)
  #expect(board.editors == .rack(rack.id) && board.drafts.map(\.id) == [rack.id])
  #expect(board.instance(rack.id)?.title == "External Test Amp 2")
  board.chooseEditor(PedalCatalog.libraryPrefix + "org.example.b", in: 2)
  let pedal = try #require(board.drafts.last)
  #expect(board.editors == .pedals([nil, nil, pedal.id]) && board.drafts.map(\.id) == [pedal.id])
  board.chooseEditor(PedalCatalog.libraryPrefix + "org.example.rack", in: 0)
  let shown = try #require(board.drafts.last)
  board.setBypassed(shown.id, false)
  #expect(board.editors == .rack(shown.id) && board.chain.map(\.id) == chain + [shown.id])
  #expect(fixture.changes.count == 1)
  board.remove(shown.id)
  #expect(board.chain.map(\.id) == chain)
}

@MainActor
@Test func hiddenChainInstancesKeepTakingEditsAndMIDIWhileDraftsDoNot() throws {
  let fixture = try ChainFixture(position: .pre)
  let empty = fixture.root.appendingPathComponent("empty")
  let audio = AudioController(
    amps: AmpCatalog(directory: empty), preFX: fixture.board,
    postFX: PedalCatalog(position: .post, directory: empty),
    cabinets: CabinetCatalog(directory: empty), specialFX: SpecialFXCatalog(directory: empty))
  defer { audio.stop() }
  let board = fixture.board
  let hidden = board.chain[0]
  board.chooseEditor(PedalCatalog.libraryPrefix + "org.example.a", in: 0)
  let draft = try #require(board.drafts.first)
  #expect(!board.editors.visible.contains(hidden.id))
  var forwarded: [(String, UInt32, Float)] = []
  board.onParameterChange = { forwarded.append(($0, $1, $2)) }
  try hidden.module.setValue(2, 0.25)
  try draft.module.setValue(2, 0.75)
  #expect(forwarded.count == 1 && forwarded[0].0 == hidden.id && forwarded[0].2 == 0.25)
  let targets = audio.midiTargets
  #expect(targets.contains { $0.id == "pre-fx.0/org.example.a/2" && $0.module === hidden.module })
  #expect(!targets.contains { $0.module === draft.module })
  board.setBypassed(draft.id, false)
  #expect(audio.midiTargets.contains { $0.id == "\(draft.id)/org.example.a/2" })
}

@MainActor
@Test func midiTargetsFollowInstancesAcrossReorderingAndEditorChanges() throws {
  let fixture = try ChainFixture(position: .pre)
  let empty = fixture.root.appendingPathComponent("empty")
  let audio = AudioController(
    amps: AmpCatalog(directory: empty), preFX: fixture.board,
    postFX: PedalCatalog(position: .post, directory: empty),
    cabinets: CabinetCatalog(directory: empty), specialFX: SpecialFXCatalog(directory: empty))
  defer { audio.stop() }
  let board = fixture.board
  let first = board.chain[0]
  let targetID = "pre-fx.0/org.example.a/2"
  #expect(first.id == "pre-fx.0")
  #expect(audio.midiTargets.first { $0.id == targetID }?.module === first.module)
  board.move(first.id, to: 2)
  board.showEditor(fixture.id("rack"))
  board.add("org.example.a")
  let targets = audio.midiTargets
  #expect(targets.first { $0.id == targetID }?.module === first.module)
  #expect(Set(targets.map(\.id)).count == targets.count)
  let copy = try #require(board.chain.last)
  #expect(
    targets.contains { $0.module === copy.module && $0.title.contains("External Test Amp 2") })
}

@MainActor
@Test func longSignalChainPanelStaysWithinTheBoard() throws {
  let fixture = try ChainFixture()
  for _ in 0..<12 { fixture.board.add("org.example.b") }
  fixture.board.onChange = { _ in throw AmpError.message(String(repeating: "Rejected. ", count: 30))
  }
  fixture.board.add("org.example.b")
  #expect(fixture.board.chain.count == 15 && fixture.board.error != nil)
  // Height of the Post FX board inside the fixed 1100 × 860 window canvas.
  let board = CGSize(width: 1100, height: 554)
  let below: CGFloat = 120
  _ = NSApplication.shared
  let host = NSHostingView(
    rootView: ZStack(alignment: .top) {
      Color(nsColor: .magenta)
      SignalChainOverlay(catalog: fixture.board) {}
        .frame(width: board.width, height: board.height)
    })
  let window = NSWindow(
    contentRect: NSRect(x: 0, y: 0, width: board.width, height: board.height + below),
    styleMask: .borderless, backing: .buffered, defer: true)
  window.isReleasedWhenClosed = false
  window.contentView = host
  defer {
    window.contentView = nil
    window.close()
  }
  host.layoutSubtreeIfNeeded()
  let bitmap = try #require(host.bitmapImageRepForCachingDisplay(in: host.bounds))
  host.cacheDisplay(in: host.bounds, to: bitmap)
  let scale = CGFloat(bitmap.pixelsHigh) / host.bounds.height
  var covered = 0
  for x in stride(from: 240, through: 860, by: 20) {
    for y in stride(from: board.height + 2, to: board.height + below, by: 6) {
      let color = try #require(
        bitmap.colorAt(x: Int(CGFloat(x) * scale), y: Int(y * scale))?.usingColorSpace(.sRGB))
      if !(color.redComponent > 0.8 && color.greenComponent < 0.4 && color.blueComponent > 0.8) {
        covered += 1
      }
    }
  }
  #expect(covered == 0)
}

@Test func boardUnitsShareOneColumnWidthAndEditorsFitTheirSpans() {
  let bounds = CGSize(width: 1100, height: 520)
  let available = bounds.height - PedalBoardLayout.headerHeight - PedalBoardLayout.headerSpacing
  let pedal = PedalBoardLayout.defaultPedal
  let stout = CGSize(width: 231.5, height: 376)
  let narrow = CGSize(width: 204, height: 380)
  let delay = CGSize(width: 440, height: 380)
  let rack = CGSize(width: 955.5, height: 521.5)
  let wide = CGSize(width: 500, height: 400)
  let boards: [[(units: Int, size: CGSize)]] = [
    [(1, pedal), (1, stout), (1, narrow), (1, pedal)],
    [(2, delay), (1, pedal), (1, narrow)],
    [(4, rack)],
    [(2, delay), (2, wide)],
    [(1, narrow), (1, pedal), (1, pedal), (1, pedal)],
  ]
  let frames = boards.map { PedalBoardLayout.frames($0, in: bounds) }
  let column = frames[0].spans[0]
  let gap = frames[0].spacing
  for (board, frame) in zip(boards, frames) {
    #expect(frame.spacing == gap)
    #expect(frame.spans.reduce(0, +) + gap * CGFloat(board.count - 1) <= 960.001)
    #expect(
      abs(frame.spans.reduce(0, +) + gap * CGFloat(board.count - 1) - (4 * column + 3 * gap))
        < 0.001)
    for ((units, size), (span, editor)) in zip(board, zip(frame.spans, frame.editors)) {
      #expect(abs(span - (CGFloat(units) * column + CGFloat(units - 1) * gap)) < 0.001)
      #expect(editor.width <= span + 0.001 && editor.height <= available + 0.001)
      #expect(abs(editor.width / editor.height - size.width / size.height) < 0.0001)
      #expect(editor.height == frame.editors[0].height)
    }
  }
  // The widest artwork for its columns sets the shared height.
  #expect(abs(frames[0].editors[1].width - column) < 0.001)
  #expect(frames[0].editors[0].height < frames[4].editors[0].height)
  #expect(abs(frames[4].editors[1].width - column) < 0.001)
  #expect(
    abs(frames[1].editors[1].width - column) < 0.001
      && frames[1].editors[0].width < frames[1].spans[0])
  #expect(abs(frames[3].editors[1].width - frames[3].spans[1]) < 0.001)
  // A full-board editor is scaled whole to the available height, at the size a rack had.
  let rackScale = min(1.1, 960 / rack.width, available / rack.height)
  #expect(abs(frames[2].editors[0].height - rack.height * rackScale) < 0.001)
  #expect(abs(frames[2].editors[0].height - available) < 0.001)
}

@MainActor
@Test func editorsFitTheBoardWithoutScrolling() throws {
  let bounds = CGSize(width: 1100, height: 520)
  let fixture = try ChainFixture()
  _ = NSApplication.shared
  let host = NSHostingView(
    rootView: PedalModuleBoard(catalog: fixture.board).background(Theme.window))
  let window = NSWindow(
    contentRect: NSRect(origin: .zero, size: bounds), styleMask: .borderless,
    backing: .buffered, defer: true)
  window.isReleasedWhenClosed = false
  window.contentView = host
  defer {
    window.contentView = nil
    window.close()
  }
  func views<T: NSView>(_ view: NSView, _ type: T.Type) -> [T] {
    (view as? T).map { [$0] } ?? view.subviews.flatMap { views($0, type) }
  }
  for (layout, shown) in [("pedals", 2), ("rack", 1), ("pedals", 1)] {
    if layout == "rack" {
      fixture.board.showEditor(fixture.id("rack"))
    } else {
      fixture.board.chooseEditor(fixture.id("a"), in: 0)
    }
    host.layoutSubtreeIfNeeded()
    let panels = views(host, NativeModulePanel.Container.self)
    #expect(panels.count == shown)
    #expect(views(host, NSScrollView.self).isEmpty)
    for panel in panels {
      let frame = panel.convert(panel.bounds, to: host)
      #expect(frame.minY >= -0.5 && frame.maxY <= host.bounds.maxY + 0.5)
      #expect(frame.minX >= -0.5 && frame.maxX <= host.bounds.maxX + 0.5)
    }
    for panel in panels { panel.stop() }
  }
  #expect(fixture.changes.isEmpty)
}

@Test func widerEditorsHideTheFewestNeighboursPreferringTheRight() {
  typealias Layout = PedalEditorLayout
  func editor(_ id: String, _ width: Int = 1) -> Layout.Span { .editor(id, width: width) }
  let four = Layout.pedals(["a", "b", "c", "d"])

  // Free units are taken before any editor is hidden, nearest on the right first.
  #expect(
    Layout.pedals(["a", nil, nil, "b"]).showing("x", width: 2, at: 1).spans
      == [editor("a"), editor("x", 2), editor("b")])
  #expect(
    Layout.pedals([nil, "a", nil, nil]).showing("x", width: 2, at: 1).spans
      == [.free, editor("x", 2), .free])
  #expect(
    Layout.pedals([nil, nil, "a", nil]).showing("x", width: 2, at: 3).spans
      == [.free, editor("a"), editor("x", 2)])

  // Right-hand neighbours are hidden before left-hand ones, nearest first.
  #expect(four.showing("x", width: 2, at: 1).spans == [editor("a"), editor("x", 2), editor("d")])
  #expect(four.showing("x", width: 3, at: 1).spans == [editor("a"), editor("x", 3)])
  #expect(four.showing("x", width: 2, at: 3).spans == [editor("a"), editor("b"), editor("x", 2)])
  #expect(four.showing("x", width: 3, at: 2).spans == [editor("a"), editor("x", 3)])
  #expect(four.showing("x", width: 4, at: 2).spans == [editor("x", 4)])

  // The fewest editors are hidden, even when that is not the nearest one.
  let mixed = Layout([editor("a"), editor("b"), editor("c", 2)])
  #expect(mixed.showing("x", width: 3, at: 0).spans == [editor("x", 3), editor("b")])
  #expect(mixed.showing("x", width: 2, at: 0).spans == [editor("x", 2), editor("c", 2)])

  // Narrower replacements leave free units after them.
  #expect(
    Layout([editor("r", 4)]).showing("x", width: 1, at: 2).spans
      == [.free, .free, editor("x"), .free])
  #expect(
    Layout([editor("d", 2), editor("e", 2)]).showing("x", width: 1, at: 0).spans
      == [editor("x"), .free, editor("e", 2)])

  // Without a unit: the first free run wide enough, else the first free unit, else the last span.
  #expect(
    Layout.pedals(["a", nil, "b", nil]).showing("x", width: 1, at: nil)
      == Layout.pedals(["a", "x", "b", nil]))
  #expect(
    Layout.pedals(["a", nil, "b", nil]).showing("x", width: 2, at: nil).spans
      == [editor("a"), editor("x", 2), editor("b")])
  #expect(
    Layout.pedals(["a", "b", nil, nil]).showing("x", width: 2, at: nil).spans
      == [editor("a"), editor("b"), editor("x", 2)])
  #expect(
    four.showing("x", width: 2, at: nil).spans == [editor("a"), editor("b"), editor("x", 2)])

  // A shown editor exchanges places, and showing it again without a unit changes nothing.
  let delay = Layout([editor("d", 2), editor("a"), editor("b")])
  #expect(delay.showing("d", width: 2, at: 3).spans == [editor("b"), editor("a"), editor("d", 2)])
  #expect(delay.showing("d", width: 2, at: nil) == delay)
  #expect(delay.showing("d", width: 2, at: 1) == delay)

  #expect(delay.clearing(at: 1).spans == [.free, .free, editor("a"), editor("b")])
  #expect(delay.clearing("b").spans == [editor("d", 2), editor("a"), .free])
  #expect(delay.units == [0, 2, 3] && delay.span(at: 1) == 0 && delay.span(at: 9) == 2)

  // Spans too wide for the board drop free units, then editors, from the right.
  #expect(
    Layout([editor("a"), .free, editor("d", 2), editor("b")]).spans
      == [editor("a"), editor("d", 2), editor("b")])
  #expect(
    Layout([editor("a"), editor("d", 2), editor("b"), editor("c")]).spans
      == [editor("a"), editor("d", 2), editor("b")])
  #expect(Layout([editor("a", 9)]).spans == [editor("a", 4)])
}

@MainActor
@Test func boardsOfMixedWidthsShowEveryCombinationThatFits() throws {
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
  // Discovery order is delay, pedal, rack: the rack does not fit beside the other two.
  let delay = board.chain[0].id
  let pedal = board.chain[1].id
  let rack = board.chain[2].id
  #expect(board.chain.map(\.module.boardWidth) == [2, 1, 4])
  #expect(board.editors.spans == [.editor(delay, width: 2), .editor(pedal, width: 1), .free])

  board.chooseEditor(PedalCatalog.libraryPrefix + "org.example.pedal", in: 3)
  let second = try #require(board.drafts.last).id
  #expect(
    board.editors.spans
      == [.editor(delay, width: 2), .editor(pedal, width: 1), .editor(second, width: 1)])
  board.chooseEditor(PedalCatalog.libraryPrefix + "org.example.delay", in: 2)
  let delay2 = try #require(board.drafts.last).id
  #expect(board.editors.spans == [.editor(delay, width: 2), .editor(delay2, width: 2)])
  #expect(board.drafts.map(\.id) == [delay2])
  board.chooseEditor(rack, in: 2)
  #expect(board.editors == .rack(rack) && board.drafts.isEmpty)
  board.chooseEditor(pedal, in: 1)
  #expect(board.editors == .pedals([nil, pedal, nil, nil]))
  #expect(board.chain.map(\.id) == [delay, pedal, rack])
}
