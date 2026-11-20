import Foundation
import Testing

@testable import VesperaUI

/// Special FX fixtures `ids` linked into a search directory, each with the manifest `placement`
/// at the same index when one is given.
@MainActor
private final class SpecialFXFixture {
  let root: URL
  let packages: [TestModulePackage]

  init(_ ids: [String], placements: [String?] = []) throws {
    root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    var packages: [TestModulePackage] = []
    for (index, id) in ids.enumerated() {
      let package = try TestModulePackage(category: "special-fx", id: id)
      if index < placements.count, let placement = placements[index] {
        var manifest = package.manifest
        manifest.placement = placement
        try JSONEncoder().encode(manifest).write(
          to: package.directory.appendingPathComponent("module.json"))
      }
      try package.link(into: root)
      packages.append(package)
    }
    self.packages = packages
  }

  deinit { try? FileManager.default.removeItem(at: root) }
}

@MainActor
@Test func specialFXShowsEditorsWithoutChangingProcessing() throws {
  let fixture = try SpecialFXFixture(["org.example.special-a", "org.example.special-b"])
  let catalog = SpecialFXCatalog(directory: fixture.root)
  #expect(catalog.modules.map(\.id) == ["org.example.special-a", "org.example.special-b"])
  #expect(catalog.active.isEmpty && catalog.activeModules.isEmpty)
  #expect(catalog.shownModule?.id == "org.example.special-a")
  #expect(try catalog.audioDescriptions().isEmpty)

  var requests: [[[String: Any]]] = []
  catalog.onChange = { requests.append($0) }
  let original = try #require(catalog.shownModule)
  try original.setValue(1, 0.25)
  catalog.show(1)
  let shown = try #require(catalog.shownModule)
  #expect(shown !== original)
  try shown.setValue(1, 0.75)
  catalog.show(0)
  #expect(try catalog.shownModule?.value(1) == 0.25)
  catalog.show(1)
  #expect(try catalog.shownModule?.value(1) == 0.75)
  catalog.show(5)
  #expect(catalog.shownModule === shown)
  #expect(requests.isEmpty && catalog.active.isEmpty)
}

@MainActor
@Test func specialFXActivationCommitsOnlyWhenAccepted() throws {
  let fixture = try SpecialFXFixture(
    ["org.example.special-a", "org.example.special-b"], placements: ["before-pre-fx"])
  let catalog = SpecialFXCatalog(directory: fixture.root)
  var requests: [[[String: Any]]] = []
  catalog.onChange = { requests.append($0) }

  catalog.setActive("org.example.special-b", true)
  catalog.setActive("org.example.special-a", true)
  #expect(catalog.active == ["org.example.special-a", "org.example.special-b"])
  #expect(requests.count == 2)
  #expect(requests[0].map { $0["slot"] as? String } == ["specialFX.org.example.special-b"])
  // Each request carries the whole section in catalog order with every module's placement.
  let section = try #require(requests.last)
  #expect(
    section.map { $0["slot"] as? String }
      == ["specialFX.org.example.special-a", "specialFX.org.example.special-b"])
  #expect(section.map { $0["placement"] as? String } == ["before-pre-fx", "after-cabinet"])
  #expect(section.map { $0["module"] as? String } == catalog.modules.map(\.id))
  #expect(
    try catalog.audioDescriptions().map { $0["slot"] as? String }
      == section.map { $0["slot"] as? String })

  catalog.setActive("org.example.special-a", true)
  catalog.setActive("org.example.missing", true)
  #expect(requests.count == 2)

  catalog.onChange = { _ in throw AmpError.message("Rejected") }
  catalog.setActive("org.example.special-a", false)
  #expect(catalog.active == ["org.example.special-a", "org.example.special-b"])
  #expect(catalog.error == "Rejected")
  catalog.onChange = { requests.append($0) }
  catalog.setActive("org.example.special-a", false)
  #expect(catalog.active == ["org.example.special-b"] && catalog.error == nil)
  #expect(requests.last?.map { $0["slot"] as? String } == ["specialFX.org.example.special-b"])

  // An inactive module's edits change its control instance only.
  let inactive = try #require(catalog.modules.first { $0.id == "org.example.special-a" })
  try inactive.setValue(1, 0.5)
  #expect(try catalog.audioDescriptions().count == 1)

  catalog.restore(shown: 0, active: ["org.example.special-a", "org.example.uninstalled"])
  #expect(catalog.active == ["org.example.special-a"] && catalog.shownIndex == 0)
  #expect(requests.count == 3)
  #expect(catalog.modules.map(\.placement) == [.beforePreFX, .afterCabinet])
}

@MainActor
@Test func specialFXParametersReachOnlyActiveSlotsAndEveryModuleIsAMIDITarget() throws {
  let fixture = try SpecialFXFixture(["org.example.special-a", "org.example.special-b"])
  let empty = fixture.root.appendingPathComponent("empty")
  let audio = AudioController(
    amps: AmpCatalog(directory: empty), preFX: PedalCatalog(directory: empty),
    postFX: PedalCatalog(position: .post, directory: empty),
    cabinets: CabinetCatalog(directory: empty),
    specialFX: SpecialFXCatalog(directory: fixture.root),
    defaults: MemoryDefaults())
  defer { audio.stop() }
  let targets = audio.midiTargets.map(\.id)
  for module in audio.specialFX.modules {
    for parameter in module.parameters {
      #expect(targets.contains("specialFX/\(module.id)/\(parameter.id)"))
    }
  }
  audio.specialFX.setActive("org.example.special-b", true)
  let configuration = try audio.startConfiguration()
  let section = try #require(configuration["specialFX"] as? [[String: Any]])
  #expect(section.map { $0["slot"] as? String } == ["specialFX.org.example.special-b"])
  let rig = try audio.captureRig()
  #expect(rig.specialFX.active == ["org.example.special-b"])
  #expect(rig.specialFX.selected == "org.example.special-a")
}

/// A Special FX fixture whose manifest is schema 2 with `fields` added. With described metadata
/// its parameters are Enabled (1, bypass), Gain (2, 0 to 1), Mode (3, three choices 0 to 2) and
/// Voices (4, 1 to 8).
@MainActor
private func routed(
  _ fields: [String: Any], metadata: TestModulePackage.Metadata = .described,
  id: String = "org.example.routed", parent: URL? = nil
) throws -> TestModulePackage {
  let package = try TestModulePackage(
    category: "special-fx", id: id, parent: parent, metadata: metadata)
  let url = package.directory.appendingPathComponent("module.json")
  var manifest = try #require(
    try JSONSerialization.jsonObject(with: Data(contentsOf: url)) as? [String: Any])
  manifest["schemaVersion"] = 2
  manifest.merge(fields) { _, field in field }
  try JSONSerialization.data(withJSONObject: manifest).write(to: url)
  return package
}

private func branch(_ mix: UInt32) -> [String: Any] {
  ["input": "chain-input", "mix": ["parameter": mix, "law": "crossfade"]]
}

private func selector(_ parameter: UInt32, _ routes: [[String: Any]]) -> [String: Any] {
  ["routingSelector": ["parameter": parameter, "routes": routes]]
}

@MainActor
private func load(_ package: TestModulePackage) throws -> LoadedModule {
  let manifest = try JSONDecoder().decode(
    ModuleManifest.self,
    from: Data(contentsOf: package.directory.appendingPathComponent("module.json")))
  return try LoadedModule(manifest: manifest, directory: package.directory, category: "special-fx")
}

@MainActor
@Test func routedModulesCheckTheParametersTheirRoutesName() throws {
  let three: [[String: Any]] = [
    ["placement": "before-pre-fx"], ["placement": "after-eq", "routing": branch(2)],
    ["placement": "after-post-fx"],
  ]
  let module = try load(try routed(selector(3, three)))
  #expect(module.routeSelector == 3 && module.routes.count == 3)
  try module.setValue(3, 0)
  #expect(module.route == SpecialFXRoute(placement: .beforePreFX, branch: nil))
  #expect(module.route.phrase == "Runs before Pre FX")
  try module.setValue(3, 1)
  #expect(module.placement == .afterEQ && module.route.branch?.mix.parameter == 2)
  #expect(module.route.phrase == "Listens to the input; mixed in after the EQ, before Post FX")
  #expect(module.route(for: 2).placement == .afterPostFX)

  let fixed = try load(try routed(["placement": "after-post-fx", "routing": branch(2)]))
  #expect(fixed.routeSelector == nil && fixed.route.branch != nil)
  #expect(fixed.placement == .afterPostFX)

  let two: [[String: Any]] = [["placement": "before-pre-fx"], ["placement": "after-eq"]]
  for (fields, message) in [
    (selector(4, two), "routingSelector parameter 4 must range from 0 to 1"),
    (selector(3, two), "routingSelector parameter 3 must range from 0 to 1"),
    (selector(2, two), "must be a toggle, stepped or enumerated parameter with 2 values"),
    (selector(1, two), "cannot be the bypass parameter"),
    (selector(9, three), "routingSelector parameter 9 must range from 0 to 2"),
    (["routing": branch(4)], "routing mix parameter 4 must range from 0 to 1"),
    (["routing": branch(9)], "routing mix parameter 9 must range from 0 to 1"),
    (
      selector(3, [three[0], ["placement": "after-eq", "routing": branch(4)], three[2]]),
      "mix parameter 4"
    ),
  ] as [([String: Any], String)] {
    let package = try routed(fields)
    #expect("\(fields)") { try load(package) } throws: {
      $0.localizedDescription.contains(message)
    }
  }
  // Without parameter metadata only the selector's range is known.
  let undescribed = try load(try routed(selector(2, two), metadata: .none))
  #expect(undescribed.routes.count == 2)
}

@MainActor
@Test func routedDescriptionsAndPageWordingFollowTheControlInstance() throws {
  let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
  defer { try? FileManager.default.removeItem(at: root) }
  let modules = root.appendingPathComponent("modules")
  let routes: [[String: Any]] = [
    ["placement": "before-pre-fx"], ["placement": "after-eq", "routing": branch(2)],
    ["placement": "after-post-fx"],
  ]
  let packages = [
    try routed(selector(3, routes), id: "org.example.a-selected", parent: modules),
    try routed(
      ["placement": "after-eq", "routing": branch(2)], id: "org.example.b-branch",
      parent: modules),
    try TestModulePackage(category: "special-fx", id: "org.example.c-serial", parent: modules),
  ]
  _ = packages
  let empty = root.appendingPathComponent("empty")
  let catalog = SpecialFXCatalog(directory: modules)
  #expect(catalog.modules.map(\.id) == packages.map(\.manifest.id))
  let audio = AudioController(
    amps: AmpCatalog(directory: empty), preFX: PedalCatalog(directory: empty),
    postFX: PedalCatalog(position: .post, directory: empty),
    cabinets: CabinetCatalog(directory: empty), specialFX: catalog, defaults: MemoryDefaults())
  defer { audio.stop() }
  for module in catalog.modules { catalog.setActive(module.id, true) }
  let section = try catalog.audioDescriptions()
  #expect(section[0]["placement"] == nil && section[0]["routing"] == nil)
  #expect(
    NSDictionary(dictionary: section[0]["routingSelector"] as? [String: Any] ?? [:])
      == ["parameter": 3, "routes": routes] as NSDictionary)
  #expect(section[1]["routingSelector"] == nil && section[1]["placement"] as? String == "after-eq")
  #expect(
    NSDictionary(dictionary: section[1]["routing"] as? [String: Any] ?? [:])
      == branch(2) as NSDictionary)
  #expect(section[2].keys.contains("placement") && !section[2].keys.contains("routing"))
  #expect(section.map { $0["slot"] as? String } == catalog.modules.map { "specialFX.\($0.id)" })

  // A selector change redraws the page; the description itself stays unresolved.
  let selected = catalog.modules[0]
  var redraws = 0
  let observer = catalog.objectWillChange.sink { redraws += 1 }
  defer { observer.cancel() }
  try selected.setValue(2, 0.3)
  #expect(redraws == 0)
  try selected.setValue(3, 1)
  #expect(redraws == 1 && selected.route.branch != nil)
  #expect(
    NSDictionary(
      dictionary: try catalog.audioDescriptions()[0]["routingSelector"] as? [String: Any] ?? [:])
      == ["parameter": 3, "routes": routes] as NSDictionary)
  let parameters = try #require(
    try catalog.audioDescriptions()[0]["parameters"] as? [[String: Any]])
  #expect(parameters.first { $0["id"] as? UInt32 == 3 }?["value"] as? Float == 1)

  // A rejected selector change keeps the control value and its route.
  selected.onParameterChange = { _, _ in throw AmpError.message("Rejected") }
  #expect(throws: AmpError.self) { try selected.setValue(3, 2) }
  #expect(selected.route.placement == .afterEQ)
}
