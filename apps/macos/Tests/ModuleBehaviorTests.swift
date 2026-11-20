import AppKit
import SwiftUI
import Testing

@testable import VesperaUI

@MainActor
@Test func visualMarginsPreserveContentScaleAndDoNotInterceptNeighbors() throws {
  let fixture = try TestModulePackage(category: "amp")
  var manifest = fixture.manifest
  manifest.viewSize = .init(width: 200, height: 100)
  manifest.visualInsets = .init(top: 10, left: 20, bottom: 40, right: 60)
  let module = try LoadedModule(manifest: manifest, directory: fixture.directory)
  _ = NSApplication.shared
  for scale in [0.5, 1.0, 2.0] {
    let host = NSHostingView(rootView: ModulePanel(module: module))
    host.frame = NSRect(x: 0, y: 0, width: 200 * scale, height: 100 * scale)
    host.layoutSubtreeIfNeeded()
    func container(_ view: NSView) -> NativeModulePanel.Container? {
      if let panel = view as? NativeModulePanel.Container { return panel }
      return view.subviews.lazy.compactMap { container($0) }.first
    }
    let panel = try #require(container(host))
    defer { panel.stop() }
    #expect(abs(panel.frame.width - 280 * scale) < 0.001)
    #expect(abs(panel.frame.height - 150 * scale) < 0.001)
    let contentOrigin = panel.convert(NSPoint(x: 20 * scale, y: 10 * scale), to: host)
    #expect(abs(contentOrigin.x) < 0.001)
    #expect(abs(contentOrigin.y) < 0.001)
    let margin = panel.convert(NSPoint(x: 5 * scale, y: 50 * scale), to: panel.superview)
    let body = panel.convert(NSPoint(x: 100 * scale, y: 50 * scale), to: panel.superview)
    #expect(panel.hitTest(margin) == nil)
    #expect(panel.hitTest(body) === panel.lease?.view)
  }
}

@MainActor
@Test func externalModuleRoutesControlsAndRejectsInvalidIdentity() throws {
  let fixture = try TestModulePackage(category: "amp")
  let catalog = AmpCatalog(directory: fixture.directory)
  #expect(catalog.error == nil)
  let module = try #require(catalog.activeAmp)
  #expect(module.id == fixture.manifest.id)
  try module.setValue(1, 1)
  try module.prepare()
  #expect(try module.renderProbe()[0] == 0.25)
  module.release()

  _ = NSApplication.shared
  let panel = try #require(ModulePanelLease(module: module))
  var delivered: (UInt32, Float)?
  module.onParameterChange = { id, value in delivered = (id, value) }
  let event = try #require(
    NSEvent.mouseEvent(
      with: .leftMouseDown, location: .zero, modifierFlags: [], timestamp: 0,
      windowNumber: 0, context: nil, eventNumber: 0, clickCount: 1, pressure: 1))
  panel.view.mouseDown(with: event)
  #expect(delivered?.0 == 2 && delivered?.1 == 0.75)
  #expect(try module.value(2) == 0.75)
  try module.prepare()
  #expect(try module.renderProbe()[0] == 0.375)
  module.release()
  let state = try module.saveState()
  try module.setValue(2, 0.1)
  try module.restoreState(state)
  #expect(try module.value(2) == 0.75)
  #expect(throws: AmpError.self) { try module.setValue(2, .nan) }
  #expect(throws: AmpError.self) { try module.setValue(2, 2) }
  #expect(throws: AmpError.self) { try module.restoreState(Data([0])) }
  #expect(try module.value(2) == 0.75)

  catalog.onSelection = { _ in throw AmpError.message("Rejected") }
  catalog.select(0)
  #expect(catalog.error == "Rejected")
  #expect(catalog.activeAmp === module)

  let mismatched = ModuleManifest(
    schemaVersion: 1, id: "wrong.id", category: "amp",
    binary: fixture.manifest.binary, icon: fixture.manifest.icon, resources: ".")
  #expect(throws: AmpError.self) {
    try LoadedModule(manifest: mismatched, directory: fixture.directory)
  }
  #expect(PedalCatalog(directory: fixture.directory).choices.isEmpty)
}

@MainActor
@Test(arguments: [PedalCatalog.Position.pre, .post])
func repeatedPedalsRetainIndependentStateAndRollbackRejectedEdits(position: PedalCatalog.Position)
  throws
{
  let fixture = try TestModulePackage(category: position.rawValue)
  let board = PedalCatalog(position: position, directory: fixture.directory)
  #expect(board.chain.count == 1)
  #expect(board.error == nil)
  let choice = try #require(board.choices.first)
  let timing = ModuleTiming()
  board.timing = timing
  board.add(choice.id)
  let first = board.chain[0].module
  let second = board.chain[1].module
  #expect(first !== second)
  #expect(first.timing === timing && second.timing === timing)
  #expect(try first.value(1) == 0)
  try first.setValue(2, 0.9)
  try first.setValue(1, 1)
  let movedID = board.chain[0].id
  board.move(movedID, by: 1)
  #expect(board.chain[1].id == movedID)
  #expect(try board.chain[1].module.value(2) == 0.9)
  #expect(try board.chain[1].module.value(1) == 1)
  #expect(try second.value(2) == 0.5)
  #expect(try second.value(1) == 0)
  let identities = board.chain.map(\.id)
  board.onChange = { _ in throw AmpError.message("Rejected") }
  board.move(movedID, by: -1)
  board.remove(identities[0])
  #expect(board.chain.map(\.id) == identities)
  #expect(board.error == "Rejected")
  board.onChange = nil
  var changedSlot: String?
  board.onParameterChange = { slot, _, _ in changedSlot = slot }
  try first.setValue(2, 0.7)
  #expect(changedSlot == movedID)
  board.onParameterChange = { _, _, _ in throw AmpError.message("Rejected") }
  #expect(throws: AmpError.self) { try first.setValue(2, 0.1) }
  #expect(try first.value(2) == 0.7)
  board.onParameterChange = nil
  board.remove(identities[0])
  #expect(board.chain.map(\.id) == [movedID])
  board.add(choice.id)
  let added = try #require(board.chain.last?.module)
  #expect(try added.value(1) == 0)
  #expect(try added.value(2) == 0.5)
  #expect(added.timing === timing)
  try added.setValue(2, 0.3)
  #expect(try first.value(2) == 0.7)
}

@MainActor
@Test func boardWidthLoadsFromManifestWithLegacyRackAndDefaultsToOneUnit() throws {
  let fixture = try TestModulePackage(category: "post-fx")
  func width(_ change: (inout ModuleManifest) -> Void) throws -> Int {
    var manifest = fixture.manifest
    change(&manifest)
    let decoded = try JSONDecoder().decode(
      ModuleManifest.self, from: JSONEncoder().encode(manifest))
    return try LoadedModule(manifest: decoded, directory: fixture.directory, category: "post-fx")
      .boardWidth
  }
  #expect(try width { _ in } == 1)
  #expect(try width { $0.boardWidth = 2 } == 2)
  #expect(try width { $0.presentation = .rack } == 4)
  #expect(try width { $0.presentation = .pedal } == 1)
}

@MainActor
@Test func absentModulesLeaveAnEmptyHost() {
  let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
  let amps = AmpCatalog(directory: directory)
  #expect(amps.activeAmp == nil)
  amps.step(-1)
  #expect(amps.activeAmp == nil)
  let pedals = PedalCatalog(directory: directory)
  #expect(pedals.choices.isEmpty)
  #expect(pedals.chain.isEmpty)
  #expect(pedals.editors.spans == Array(repeating: .free, count: 4))
}

@MainActor
@Test func moduleStatusesReportedByTheBridgeAreShownOnTheirTab() throws {
  let fixture = try TestModulePackage(category: "special-fx", id: "org.example.status")
  let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
  defer { try? FileManager.default.removeItem(at: root) }
  try fixture.link(into: root)
  let special = SpecialFXCatalog(directory: root)
  let module = try #require(special.shownModule)
  let empty = root.appendingPathComponent("empty")
  let audio = AudioController(
    amps: AmpCatalog(directory: empty), preFX: PedalCatalog(directory: empty),
    postFX: PedalCatalog(position: .post, directory: empty),
    cabinets: CabinetCatalog(directory: empty), specialFX: special)
  defer { audio.stop() }
  special.setActive(module.id, true)
  let reply = """
    {"state": "running", "recovery": "none", "lastError": 0, "blocks": 4,
     "input": [0, 0], "output": [0, 0],
     "modules": [{"slot": "specialFX.org.example.status", "status": "error", "message": "ir.wav is unreadable"},
                 {"slot": "amp", "status": "busy", "message": "", "progress": 0.25},
                 {"slot": "pre-fx.9", "status": "warning", "message": "remark"}]}
    """
  audio.apply(status: try JSONDecoder().decode(AudioStatus.self, from: Data(reply.utf8)))
  #expect(audio.moduleStatuses.count == 3)
  let notices = audio.moduleNotices(on: .specialFX)
  #expect(notices.count == 1)
  #expect(notices.first?.text == "\(module.name): ir.wav is unreadable")
  #expect(notices.first?.isProblem == true)
  // No module fills the amp or pedal slots here, so their statuses have nowhere to show.
  #expect(audio.moduleNotices(on: .amp).isEmpty && audio.moduleNotices(on: .preFX).isEmpty)
  let busy = ModuleNotice(
    name: "Reverb",
    status: ModuleRuntimeStatus(slot: "amp", status: .busy, message: "", progress: 0.25))
  #expect(busy.text == "Reverb: Working (25%)" && !busy.isProblem)

  let quiet = """
    {"state": "running", "recovery": "none", "lastError": 0, "blocks": 5,
     "input": [0, 0], "output": [0, 0], "modules": []}
    """
  audio.apply(status: try JSONDecoder().decode(AudioStatus.self, from: Data(quiet.utf8)))
  #expect(audio.moduleNotices(on: .specialFX).isEmpty)
  // Older bridges without module statuses still decode.
  let older = """
    {"state": "running", "recovery": "none", "lastError": 0, "blocks": 6,
     "input": [0, 0], "output": [0, 0]}
    """
  audio.apply(status: try JSONDecoder().decode(AudioStatus.self, from: Data(older.utf8)))
  #expect(audio.moduleStatuses.isEmpty)
}

@MainActor
@Test func audioDescriptionsNameThePackageResources() throws {
  let fixture = try TestModulePackage(category: "special-fx", id: "org.example.resources")
  let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
  defer { try? FileManager.default.removeItem(at: root) }
  try fixture.link(into: root)
  let module = try #require(SpecialFXCatalog(directory: root).shownModule)
  let description = try module.audioDescription()
  #expect(description["resources"] as? String == module.resourceDirectory.path)
  _ = offlineRenderingEnabled
  var slot = description
  slot["slot"] = "specialFX"
  struct Rendered: Decodable { let output: [[Float]] }
  let rendered = try JSONDecoder().decode(
    Rendered.self,
    from: AudioBridge().request([
      "op": "renderGraph", "configuration": ["specialFX": [slot]],
      "input": [[Float](repeating: 0.25, count: 64)],
    ])
  ).output
  #expect(rendered.count == 1 && rendered[0].count == 64)
}

@MainActor
@Test func malformedServicesDescriptorsRejectControlModules() throws {
  for variant in [2, 3, 4, 5] {
    let fixture = try TestModulePackage(category: "special-fx", services: variant)
    do {
      _ = try LoadedModule(
        manifest: fixture.manifest, directory: fixture.directory, category: "special-fx")
      Issue.record("Malformed services descriptor \(variant) loaded")
    } catch {
      #expect(error.localizedDescription.contains("services descriptor"))
    }
  }
  let declined = try TestModulePackage(category: "special-fx", services: 6)
  let module = try LoadedModule(
    manifest: declined.manifest, directory: declined.directory, category: "special-fx")
  #expect(!module.supportsServices)
}

@MainActor
@Test func onlyAudioInstancesAttachServicesAndDerivedTablesRenderOffline() throws {
  let fixture = try TestModulePackage(category: "special-fx", services: 7)
  let module = try LoadedModule(
    manifest: fixture.manifest, directory: fixture.directory, category: "special-fx")
  #expect(module.supportsServices)
  let path = fixture.directory.appendingPathComponent(fixture.manifest.binary).path
  let library = try #require(dlopen(path, RTLD_NOW | RTLD_LOCAL))
  defer { dlclose(library) }
  let symbol = try #require(dlsym(library, "external_test_attached_instances"))
  let attached = unsafeBitCast(symbol, to: (@convention(c) () -> Int32).self)
  #expect(attached() == 0)
  try module.setValue(1, 1)
  try module.setValue(2, 0.7)
  var slot = try module.audioDescription()
  slot["slot"] = "specialFX"
  _ = offlineRenderingEnabled
  struct Rendered: Decodable { let output: [[Float]] }
  let bridge = try AudioBridge()
  var reference: [[Float]]?
  for size in [1, 7, 64] {
    let output = try JSONDecoder().decode(
      Rendered.self,
      from: bridge.request([
        "op": "renderGraph", "configuration": ["specialFX": [slot]], "blockSize": size,
        "input": [[Float](repeating: 0.25, count: 256)],
      ])
    ).output
    #expect(output[0].allSatisfy { $0 == 0.25 * Float(0.7) })
    if let reference { #expect(output == reference) } else { reference = output }
  }
  #expect(attached() == 3)
}

@MainActor
@Test func specialFXLeavingTheChainDropsItsFileProblems() throws {
  let fixture = try TestModulePackage(category: "special-fx", id: "org.example.files")
  let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
  defer { try? FileManager.default.removeItem(at: root) }
  try fixture.link(into: root)
  let special = SpecialFXCatalog(directory: root)
  let module = try #require(special.shownModule)
  let empty = root.appendingPathComponent("empty")
  let audio = AudioController(
    amps: AmpCatalog(directory: empty), preFX: PedalCatalog(directory: empty),
    postFX: PedalCatalog(position: .post, directory: empty),
    cabinets: CabinetCatalog(directory: empty), specialFX: special, defaults: MemoryDefaults())
  defer { audio.stop() }
  func status(_ modules: String) throws -> AudioStatus {
    try JSONDecoder().decode(
      AudioStatus.self,
      from: Data(
        """
        {"state": "running", "recovery": "none", "lastError": 0, "blocks": 1,
         "input": [0, 0], "output": [0, 0], "modules": [\(modules)]}
        """.utf8))
  }
  special.setActive(module.id, true)
  audio.apply(
    status: try status(
      #"{"slot": "specialFX.org.example.files", "status": "error", "message": "missing", "fileSlot": 1}"#
    ))
  #expect(module.fileProblems == [1: "missing"])
  special.setActive(module.id, false)
  audio.apply(status: try status(""))
  #expect(module.fileProblems.isEmpty)
}
