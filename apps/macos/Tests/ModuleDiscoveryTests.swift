import Foundation
import Testing

@testable import VesperaUI

@Test func installedModulePathDoesNotUseDevelopmentBuild() {
  #expect(
    ModuleDiscovery.searchDirectories(environment: [:]) == [ModuleDiscovery.installedDirectory])
  #expect(
    ModuleDiscovery.searchDirectories(environment: ["VESPERA_MODULE_PATH": "/one:/two"])
      == [
        URL(fileURLWithPath: "/one", isDirectory: true),
        URL(fileURLWithPath: "/two", isDirectory: true),
      ])
}

@Test func discoveryDeduplicatesPackagesAndIsolatesMalformedManifests() throws {
  let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
  try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
  defer { try? FileManager.default.removeItem(at: directory) }
  for category in ["amp", "pre-fx", "post-fx"] {
    let package = directory.appendingPathComponent(ModuleDiscovery.categoryFolders[category]!)
      .appendingPathComponent(UUID().uuidString + ".vespera")
    try FileManager.default.createDirectory(at: package, withIntermediateDirectories: true)
    let manifest = ModuleManifest(
      schemaVersion: 1, id: "test.\(category)", category: category,
      binary: "custom.so", icon: "icon.svg", resources: ".")
    try JSONEncoder().encode(manifest).write(to: package.appendingPathComponent("module.json"))
    try Data().write(to: package.appendingPathComponent("custom.so"))
    try Data().write(to: package.appendingPathComponent("icon.svg"))
  }
  let relocated = ModuleDiscovery.scan([directory])
  #expect(relocated.failures.isEmpty)
  #expect(Set(relocated.packages.map(\.manifest.category)) == ["amp", "pre-fx", "post-fx"])
  #expect(ModuleDiscovery.scan([directory, directory]).packages.count == 3)

  let broken = directory.appendingPathComponent("amps/broken.vespera")
  try FileManager.default.createDirectory(at: broken, withIntermediateDirectories: true)
  try Data("not json".utf8).write(to: broken.appendingPathComponent("module.json"))
  let scan = ModuleDiscovery.scan([directory])
  #expect(scan.packages.count == 3)
  #expect(scan.failures.count == 1)
}

@Test func packagesLoadOnlyFromTheFolderOfTheirCategory() throws {
  let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
  defer { try? FileManager.default.removeItem(at: directory) }
  func write(_ path: String, category: String) throws {
    let package = directory.appendingPathComponent(path)
    try FileManager.default.createDirectory(at: package, withIntermediateDirectories: true)
    let manifest = ModuleManifest(
      schemaVersion: 1, id: "test.\(path)", category: category,
      binary: "custom.so", icon: "icon.svg", resources: ".")
    try JSONEncoder().encode(manifest).write(to: package.appendingPathComponent("module.json"))
    try Data().write(to: package.appendingPathComponent("custom.so"))
    try Data().write(to: package.appendingPathComponent("icon.svg"))
  }
  try write("amps/right.vespera", category: "amp")
  try write("cabs/wrong.vespera", category: "amp")
  try write("flat.vespera", category: "amp")
  try write("amps/unsuffixed", category: "amp")
  try write("other/ignored.vespera", category: "amp")

  let scan = ModuleDiscovery.scan([directory])
  #expect(scan.packages.map(\.directory.lastPathComponent) == ["right.vespera"])
  #expect(scan.failures.count == 2)
  #expect(
    scan.failures.contains { $0.contains("wrong.vespera is a amp module; move it into amps") })
  #expect(scan.failures.contains { $0.contains("flat.vespera is not in a category folder") })

  let explicit = ModuleDiscovery.scan([directory.appendingPathComponent("flat.vespera")])
  #expect(explicit.packages.count == 1 && explicit.failures.isEmpty)
}

@Test func packageResourcesStayInsidePackage() throws {
  let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
  try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
  defer { try? FileManager.default.removeItem(at: directory) }
  let manifest = ModuleManifest(
    schemaVersion: 1, id: "test", category: "amp",
    binary: "test.so", icon: "icon.svg", resources: "resources")
  #expect(throws: AmpError.self) { try manifest.resource("../outside", relativeTo: directory) }
  #expect(throws: AmpError.self) { try manifest.resource("/outside", relativeTo: directory) }
  try FileManager.default.createSymbolicLink(
    at: directory.appendingPathComponent("escape"),
    withDestinationURL: directory.deletingLastPathComponent())
  #expect(throws: AmpError.self) { try manifest.resource("escape/file", relativeTo: directory) }
}

@Test func boardWidthIsAWholeNumberOfUnitsOnPedalBoardsOnly() throws {
  let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
  try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
  defer { try? FileManager.default.removeItem(at: directory) }
  /// Scans one package whose manifest adds `fields` (JSON members) to a minimal manifest.
  func scan(_ fields: String, category: String = "post-fx") throws -> (
    units: Int?, failures: [String]
  ) {
    let package = directory.appendingPathComponent(UUID().uuidString + ".vespera")
    try FileManager.default.createDirectory(at: package, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: package) }
    try Data(
      """
      {"schemaVersion": 1, "id": "test.width", "category": "\(category)", "binary": "custom.so",
       "icon": "icon.svg", "resources": "."\(fields.isEmpty ? "" : ", " + fields)}
      """.utf8
    ).write(to: package.appendingPathComponent("module.json"))
    try Data().write(to: package.appendingPathComponent("custom.so"))
    try Data().write(to: package.appendingPathComponent("icon.svg"))
    let result = ModuleDiscovery.scan([package])
    return (result.packages.first?.manifest.boardUnits, result.failures)
  }

  #expect(try scan("").units == 1)
  #expect(try scan(#""boardWidth": 1"#).units == 1)
  #expect(try scan(#""boardWidth": 2"#).units == 2)
  #expect(try scan(#""boardWidth": 4.0"#, category: "pre-fx").units == 4)
  #expect(try scan(#""presentation": "rack""#).units == 4)
  #expect(try scan(#""presentation": "pedal""#).units == 1)
  #expect(try scan(#""presentation": "rack", "boardWidth": 2"#).units == 2)

  for (fields, value) in [
    (#""boardWidth": 0"#, "0"), (#""boardWidth": 5"#, "5"), (#""boardWidth": 2.5"#, "2.5"),
    (#""boardWidth": -1"#, "-1"), (#""boardWidth": 1e300"#, "1e+300"),
  ] {
    let result = try scan(fields)
    #expect(result.units == nil)
    #expect(result.failures.count == 1)
    #expect(result.failures.first?.contains("Invalid boardWidth \(value)") == true, "\(fields)")
  }
  let text = try scan(#""boardWidth": "2""#)
  #expect(text.units == nil && text.failures.count == 1)
  let amp = try scan(#""boardWidth": 1"#, category: "amp")
  #expect(amp.units == nil && amp.failures.first?.contains("Invalid boardWidth") == true)
}

/// Scans one package whose manifest adds `fields` to a minimal manifest of `category`.
private func scanManifest(_ fields: String, category: String, in directory: URL) throws -> (
  manifest: ModuleManifest?, failures: [String]
) {
  let package = directory.appendingPathComponent(UUID().uuidString + ".vespera")
  try FileManager.default.createDirectory(at: package, withIntermediateDirectories: true)
  defer { try? FileManager.default.removeItem(at: package) }
  try Data(
    """
    {"schemaVersion": 1, "id": "test.values", "category": "\(category)", "binary": "custom.so",
     "icon": "icon.svg", "resources": ".", \(fields)}
    """.utf8
  ).write(to: package.appendingPathComponent("module.json"))
  try Data().write(to: package.appendingPathComponent("custom.so"))
  try Data().write(to: package.appendingPathComponent("icon.svg"))
  let result = ModuleDiscovery.scan([package])
  return (result.packages.first?.manifest, result.failures)
}

@Test func placementAndFormerCategoriesAgreeWithTheConformanceTool() throws {
  let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
  try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
  defer { try? FileManager.default.removeItem(at: directory) }
  let values = try #require(
    manifestValues.filter { ["placement", "formerCategories"].contains($0.key) }
      as? [String: [String: [Any]]])
  for key in ["placement", "formerCategories"] {
    for (kind, valid) in [("valid", true), ("invalid", false)] {
      for value in try #require(values[key]?[kind]) {
        let text = String(
          decoding: try JSONSerialization.data(withJSONObject: value, options: .fragmentsAllowed),
          as: UTF8.self)
        let result = try scanManifest(
          #""\#(key)": \#(text)"#, category: "special-fx", in: directory)
        #expect((result.manifest != nil) == valid, "\(key): \(text) \(result.failures)")
        #expect(result.failures.isEmpty == valid, "\(key): \(text)")
      }
    }
  }
  for (fields, category, valid) in [
    (#""placement": "after-cabinet""#, "pre-fx", false),
    (#""placement": "before-amp""#, "amp", false),
    (#""placement": null"#, "pre-fx", true),
    (#""formerCategories": ["special-fx"]"#, "pre-fx", true),
    (#""formerCategories": ["pre-fx"]"#, "pre-fx", false),
  ] {
    let result = try scanManifest(fields, category: category, in: directory)
    #expect((result.manifest != nil) == valid, "\(category) \(fields)")
  }
  let invalid = try scanManifest(
    #""placement": "after-amp""#, category: "special-fx", in: directory)
  #expect(
    invalid.failures.first?.contains(
      "use one of before-pre-fx, before-amp, before-cabinet, after-cabinet, after-eq, after-post-fx"
    )
      == true)
  let placed = try scanManifest(#""placement": "after-eq""#, category: "special-fx", in: directory)
  #expect(try placed.manifest?.specialFXPlacement() == .afterEQ)
  let absent = try scanManifest(#""pluginFamily": "F""#, category: "special-fx", in: directory)
  #expect(try absent.manifest?.specialFXPlacement() == .afterCabinet)
}

@Test func formerCategoriesSupersedeOlderCopiesWhateverTheFolderOrder() throws {
  let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
  defer { try? FileManager.default.removeItem(at: directory) }
  func write(_ path: String, id: String, category: String, former: [String]? = nil) throws -> URL {
    let package = directory.appendingPathComponent(path)
    try FileManager.default.createDirectory(at: package, withIntermediateDirectories: true)
    var manifest = ModuleManifest(
      schemaVersion: 1, id: id, category: category, binary: "custom.so", icon: "icon.svg",
      resources: ".")
    manifest.formerCategories = former
    try JSONEncoder().encode(manifest).write(to: package.appendingPathComponent("module.json"))
    try Data().write(to: package.appendingPathComponent("custom.so"))
    try Data().write(to: package.appendingPathComponent("icon.svg"))
    return package
  }
  // The stale copy's folder (pre-fx) sorts before special-fx, so it would otherwise win.
  let stale = try write("pre-fx/old-wah.vespera", id: "test.moved", category: "pre-fx")
  _ = try write(
    "special-fx/wah.vespera", id: "test.moved", category: "special-fx", former: ["pre-fx"])
  let scan = ModuleDiscovery.scan([directory])
  #expect(scan.packages.map(\.directory.lastPathComponent) == ["wah.vespera"])
  #expect(scan.failures.count == 1)
  let line = try #require(scan.failures.first)
  #expect(
    line.hasPrefix(
      "old-wah.vespera is an older pre-fx copy of test.moved, which is now a special-fx module; "
        + "remove /"))
  #expect(line.hasSuffix("/pre-fx/old-wah.vespera"))
  #expect(FileManager.default.fileExists(atPath: stale.path))

  // Two packages naming each other's category both stay unloaded and are both reported.
  _ = try write("amps/a.vespera", id: "test.mutual", category: "amp", former: ["cab"])
  _ = try write("cabs/b.vespera", id: "test.mutual", category: "cab", former: ["amp"])
  // Same-category duplicates stay silent: the first in folder order loads.
  _ = try write("post-fx/one.vespera", id: "test.same", category: "post-fx")
  _ = try write("post-fx/two.vespera", id: "test.same", category: "post-fx")
  // Without formerCategories a different-category duplicate loads first in folder order.
  _ = try write("cabs/plain.vespera", id: "test.plain", category: "cab")
  _ = try write("post-fx/plain.vespera", id: "test.plain", category: "post-fx")
  let all = ModuleDiscovery.scan([directory])
  #expect(!all.packages.contains { $0.manifest.id == "test.mutual" })
  #expect(all.failures.filter { $0.contains("test.mutual") }.count == 2)
  #expect(
    all.packages.filter { $0.manifest.id == "test.same" }.map(\.directory.lastPathComponent) == [
      "one.vespera"
    ])
  #expect(
    all.packages.filter { $0.manifest.id == "test.plain" }.map(\.manifest.category) == ["cab"])
  #expect(all.failures.count == 3)
}

private var manifestValues: [String: Any] {
  let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
    .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
  let data = try! Data(
    contentsOf: root.appendingPathComponent("tests/fixtures/manifest_values.json"))
  return try! JSONSerialization.jsonObject(with: data) as! [String: Any]
}

@Test func routingDeclarationsAgreeWithTheConformanceTool() throws {
  let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
  try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
  defer { try? FileManager.default.removeItem(at: directory) }
  let cases = try #require(manifestValues["routingCases"] as? [[String: Any]])
  #expect(cases.count > 20)
  for item in cases {
    var manifest: [String: Any] = [
      "schemaVersion": item["schemaVersion"]!, "id": "test.routing",
      "category": item["category"] ?? "special-fx", "binary": "custom.so", "icon": "icon.svg",
      "resources": ".",
    ]
    manifest.merge(try #require(item["fields"] as? [String: Any])) { _, field in field }
    let package = directory.appendingPathComponent(UUID().uuidString + ".vespera")
    try FileManager.default.createDirectory(at: package, withIntermediateDirectories: true)
    try JSONSerialization.data(withJSONObject: manifest).write(
      to: package.appendingPathComponent("module.json"))
    try Data().write(to: package.appendingPathComponent("custom.so"))
    try Data().write(to: package.appendingPathComponent("icon.svg"))
    let scan = ModuleDiscovery.scan([package])
    let text = String(decoding: try JSONSerialization.data(withJSONObject: item), as: UTF8.self)
    #expect(scan.packages.isEmpty != (item["valid"] as? Bool == true), "\(text) \(scan.failures)")
  }
}

@Test func routeIndicesFollowTheSharedRoundingRule() throws {
  let values = try #require(manifestValues["routeIndex"] as? [[Double]])
  for value in values {
    #expect(
      SpecialFXRoute.index(for: Float(value[0]), count: Int(value[1])) == Int(value[2]),
      "\(value)")
  }
  #expect(SpecialFXRoute.index(for: .nan, count: 3) == 0)
  #expect(SpecialFXRoute.index(for: .infinity, count: 3) == 2)
}

@Test func routingDescriptionsCarryTheDeclarationUnresolved() throws {
  func manifest(_ fields: String) throws -> ModuleManifest {
    try JSONDecoder().decode(
      ModuleManifest.self,
      from: Data(
        """
        {"schemaVersion": 2, "id": "m", "category": "special-fx", "binary": "b", "icon": "i",
         "resources": "."\(fields)}
        """.utf8))
  }
  let serial = try manifest("").routeDescription()
  #expect(
    serial.keys.sorted() == ["placement"] && serial["placement"] as? String == "after-cabinet")
  let branch = try manifest(
    #", "placement": "after-eq", "routing": {"input": "chain-input", "mix": {"parameter": 8, "law": "crossfade"}}"#
  ).routeDescription()
  #expect(branch.keys.sorted() == ["placement", "routing"])
  #expect(
    NSDictionary(dictionary: branch["routing"] as? [String: Any] ?? [:])
      == ["input": "chain-input", "mix": ["parameter": 8, "law": "crossfade"]] as NSDictionary)
  let selected = try manifest(
    #", "routingSelector": {"parameter": 3, "routes": [{"placement": "before-pre-fx"}, {"placement": "after-eq", "routing": {"input": "chain-input", "mix": {"parameter": 2, "law": "crossfade"}}}]}"#
  )
  let description = try selected.routeDescription()
  #expect(description.keys.sorted() == ["routingSelector"])
  #expect(
    NSDictionary(dictionary: description["routingSelector"] as? [String: Any] ?? [:])
      == [
        "parameter": 3,
        "routes": [
          ["placement": "before-pre-fx"],
          [
            "placement": "after-eq",
            "routing": ["input": "chain-input", "mix": ["parameter": 2, "law": "crossfade"]],
          ],
        ],
      ] as NSDictionary)
  let routes = try selected.specialFXRoutes()
  #expect(routes.selector == 3 && routes.routes.map(\.placement) == [.beforePreFX, .afterEQ])
  #expect(routes.routes.map { $0.branch != nil } == [false, true])
}
