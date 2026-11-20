import Foundation
import Testing

@testable import VesperaUI

final class TestModulePackage {
  /// The parameter metadata the fixture binary exports.
  enum Metadata: Int {
    case none = 0
    /// Gain (parameter 2) shown as signed decibels over -12 to 12, a labelled Mode (3: Clean,
    /// Crunch, Lead) and a stepped Voices (4: 1 to 8).
    case described = 1
    /// The described metadata with an element size too small to hold an entry.
    case malformed = 2
  }

  let directory: URL
  let manifest: ModuleManifest

  /// Links the package into the folder of its category inside the search directory `root`.
  func link(into root: URL) throws {
    let folder = root.appendingPathComponent(
      ModuleDiscovery.categoryFolders[manifest.category] ?? manifest.category)
    try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
    try FileManager.default.createSymbolicLink(
      at: folder.appendingPathComponent(directory.lastPathComponent),
      withDestinationURL: directory)
  }

  /// `bypass` is the parameter the module declares as its bypass, or nil for none, whatever the
  /// name of parameter 1 ("Enabled"); `firstUnit` is that parameter's unit. A `parent` directory
  /// holds several packages for one discovery scan.
  /// `opaqueState` builds the opaque-state fixture instead, whose saved state holds settings
  /// that are not parameters.
  init(
    category: String, id: String = "org.example.test-amp", equalizer: String? = nil,
    bypass: UInt32? = 1, firstUnit: String = "bool", family: String? = nil,
    presentation: ModuleManifest.Presentation? = nil, boardWidth: Double? = nil,
    parent: URL? = nil,
    opaqueState: Bool = false, cabinet: ModuleManifest.CabinetSupport? = nil,
    metadata: Metadata = .none, services: Int = 0, files: Bool = false
  ) throws {
    directory =
      (parent?.appendingPathComponent(ModuleDiscovery.categoryFolders[category] ?? category)
      ?? FileManager.default.temporaryDirectory)
      .appendingPathComponent(UUID().uuidString).appendingPathExtension("vespera")
    var manifest = ModuleManifest(
      schemaVersion: 1, id: id, category: category,
      binary: "custom.so", icon: "custom.svg", resources: ".")
    manifest.eq = equalizer.map { .init(id: $0, binary: "eq.so") }
    manifest.presentation = presentation
    manifest.boardWidth = boardWidth
    manifest.pluginFamily = family
    manifest.cabinet = cabinet
    self.manifest = manifest
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    do {
      let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
        .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
      let compiler = Process()
      compiler.executableURL = URL(fileURLWithPath: "/usr/bin/xcrun")
      compiler.arguments =
        [
          "clang++", "-std=c++20", "-dynamiclib", "-I", root.appendingPathComponent("include").path,
          "-DEXTERNAL_MODULE_ID=\"\(id)\"", "-DOPAQUE_MODULE_ID=\"\(id)\"",
          "-DEXTERNAL_FIRST_UNIT=\"\(firstUnit)\"", "-DEXTERNAL_SERVICES=\(services)",
          "-DEXTERNAL_FILES=\(files ? 1 : 0)",
        ] + (bypass.map { ["-DEXTERNAL_BYPASS_PARAMETER=\($0)"] } ?? [])
        + (metadata == .none ? [] : ["-DEXTERNAL_METADATA=\(metadata.rawValue)"]) + [
          root.appendingPathComponent(
            opaqueState
              ? "tests/fixtures/opaque_state_module.cpp"
              : "apps/macos/Tests/Fixtures/external_dsp.cpp"
          ).path,
          "-o", directory.appendingPathComponent("custom.so").path,
        ]
      if category == "amp" {
        compiler.arguments! += [
          "-DExternalTestPanel=ExternalTestPanel_\(UUID().uuidString.replacingOccurrences(of: "-", with: ""))",
          "-fobjc-arc", "-framework", "AppKit",
          root.appendingPathComponent("apps/macos/Tests/Fixtures/external_ui.mm").path,
        ]
      }
      try compiler.run()
      compiler.waitUntilExit()
      try #require(compiler.terminationStatus == 0)
      if let equalizer {
        let companion = Process()
        companion.executableURL = compiler.executableURL
        companion.arguments = [
          "clang++", "-std=c++20", "-dynamiclib", "-I", root.appendingPathComponent("include").path,
          "-DEXTERNAL_MODULE_ID=\"\(equalizer)\"", "-DEXTERNAL_BYPASS_PARAMETER=1",
          root.appendingPathComponent("apps/macos/Tests/Fixtures/external_dsp.cpp").path,
          "-o", directory.appendingPathComponent("eq.so").path,
        ]
        try companion.run()
        companion.waitUntilExit()
        try #require(companion.terminationStatus == 0)
      }
      try """
      <svg xmlns="http://www.w3.org/2000/svg" width="80" height="40">
      <rect width="80" height="40" fill="white"/></svg>
      """.write(
        to: directory.appendingPathComponent("custom.svg"), atomically: true, encoding: .utf8)
      try JSONEncoder().encode(manifest).write(to: directory.appendingPathComponent("module.json"))
    } catch {
      try? FileManager.default.removeItem(at: directory)
      throw error
    }
  }

  deinit { try? FileManager.default.removeItem(at: directory) }
}

extension PedalEditorLayout {
  /// One-unit editors and free units, left to right, with free units filling the rest.
  static func pedals(_ slots: [String?]) -> PedalEditorLayout {
    PedalEditorLayout(slots.map { $0.map { .editor($0, width: 1) } ?? .free })
  }

  static func rack(_ id: String) -> PedalEditorLayout {
    PedalEditorLayout([.editor(id, width: capacity)])
  }
}
