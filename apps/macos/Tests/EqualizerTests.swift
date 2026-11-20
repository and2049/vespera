import Foundation
import Testing

@testable import VesperaUI

private func linkedRoot(_ packages: [TestModulePackage]) throws -> URL {
  let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
  try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
  for package in packages { try package.link(into: root) }
  return root
}

@MainActor
@Test func ampOwnedEqualizersRetainStateRouteMappingsAndRollBackWithTheAmp() throws {
  let first = try TestModulePackage(
    category: "amp", id: "org.example.amp-a", equalizer: "org.example.amp-a.eq")
  let second = try TestModulePackage(
    category: "amp", id: "org.example.amp-b", equalizer: "org.example.amp-b.eq")
  let legacy = try TestModulePackage(category: "amp", id: "org.example.legacy")
  let root = try linkedRoot([first, second, legacy])
  defer { try? FileManager.default.removeItem(at: root) }
  let amps = AmpCatalog(directory: root)
  #expect(amps.error == nil && amps.amps.count == 3)
  #expect(SpecialFXCatalog(directory: root).modules.isEmpty)
  #expect(PedalCatalog(directory: root).choices.isEmpty)
  #expect(CabinetCatalog(directory: root).cabinets.isEmpty)
  func index(_ id: String) throws -> Int { try #require(amps.amps.firstIndex { $0.id == id }) }
  let ampA = amps.amps[try index(first.manifest.id)]
  let ampB = amps.amps[try index(second.manifest.id)]
  let old = amps.amps[try index(legacy.manifest.id)]
  let eqA = try #require(ampA.equalizer)
  let eqB = try #require(ampB.equalizer)
  #expect(eqA.id == "org.example.amp-a.eq" && eqB.id == "org.example.amp-b.eq")
  #expect(eqA !== eqB && old.equalizer == nil)
  #expect(eqA.resourceDirectory == ampA.resourceDirectory)

  let empty = root.appendingPathComponent("empty")
  let audio = AudioController(
    amps: amps, preFX: PedalCatalog(directory: empty),
    postFX: PedalCatalog(position: .post, directory: empty),
    cabinets: CabinetCatalog(directory: empty), specialFX: SpecialFXCatalog(directory: empty))
  let savedMappings = audio.midiMappings
  defer {
    audio.midiMappings = savedMappings
    audio.stop()
  }
  #expect(eqA.onParameterChange != nil && eqA.timing != nil && eqA.timing === ampA.timing)

  amps.select(try index(first.manifest.id))
  try eqA.setValue(1, 1)
  try eqA.setValue(2, 0.8)
  try ampA.setValue(1, 0)
  #expect(try eqA.value(1) == 1 && ampA.value(1) == 0)
  amps.select(try index(second.manifest.id))
  #expect(amps.activeAmp === ampB)
  try eqB.setValue(2, 0.2)
  amps.select(try index(legacy.manifest.id))
  #expect(audio.midiTargets.allSatisfy { !$0.id.hasPrefix("eq/") })
  let legacyRig = try AudioController.rigRequest(amp: old, cabinet: nil)
  #expect((legacyRig["eq"] as? [[String: Any]])?.isEmpty == true)
  amps.select(try index(first.manifest.id))
  #expect(try eqA.value(2) == 0.8 && eqB.value(2) == 0.2 && eqA.value(1) == 1)

  let rig = try AudioController.rigRequest(amp: ampA, cabinet: nil)
  let eqEntries = try #require(rig["eq"] as? [[String: Any]])
  #expect(eqEntries.count == 1 && eqEntries[0]["slot"] as? String == "eq")
  #expect((eqEntries[0]["binary"] as? String)?.hasSuffix("/eq.so") == true)
  let values = try #require(eqEntries[0]["parameters"] as? [[String: Any]])
  #expect(values.contains { $0["id"] as? UInt32 == 2 && $0["value"] as? Float == 0.8 })

  let target = try #require(
    audio.midiTargets.first { $0.module === eqA && $0.parameter.id == 2 })
  #expect(target.id == "eq/\(eqA.id)/2")
  var mapping = MIDIMapping()
  mapping.target = target.id
  mapping.number = 21
  audio.midiMappings = [mapping]
  audio.applyMIDIMessages([.init(status: 0xb0, number: 21, value: 0)])
  #expect(try eqA.value(2) == 0)
  amps.select(try index(second.manifest.id))
  audio.applyMIDIMessages([.init(status: 0xb0, number: 21, value: 127)])
  #expect(try eqA.value(2) == 0 && eqB.value(2) == 0.2)
  amps.select(try index(first.manifest.id))

  let routing = eqA.onParameterChange
  eqA.onParameterChange = { _, _ in throw AmpError.message("Rejected EQ") }
  #expect(throws: AmpError.self) { try eqA.setValue(2, 0.6) }
  #expect(try eqA.value(2) == 0)
  audio.applyMIDIMessages([.init(status: 0xb0, number: 21, value: 127)])
  #expect(try eqA.value(2) == 0 && audio.error == "Rejected EQ")
  eqA.onParameterChange = routing

  audio.cabinets.onChange = { _, _ in throw AmpError.message("Rejected rig") }
  amps.select(try index(second.manifest.id))
  #expect(amps.activeAmp === ampA && amps.error == "Rejected rig")
  #expect(audio.midiTargets.contains { $0.module === eqA })
  #expect(try eqA.value(1) == 1 && eqB.value(2) == 0.2)
}

@Test func tabsFollowSignalOrderAndHideAbsentAmpSections() {
  #expect(
    ModuleTab.available(cabinet: true, equalizer: true) == [
      .preFX, .amp, .cabinet, .specialFX, .eq, .postFX,
    ])
  #expect(
    ModuleTab.available(cabinet: false, equalizer: false) == [.preFX, .amp, .specialFX, .postFX])
  #expect(ModuleTab.eq.shortcut == "5" && ModuleTab.postFX.shortcut == "6")
  #expect(ModuleTab.allCases.map(\.shortcut) == ["1", "2", "3", "4", "5", "6"])
}

@MainActor
@Test func invalidEqualizerDeclarationsAreRejected() throws {
  let amp = try TestModulePackage(
    category: "amp", id: "org.example.amp-c", equalizer: "org.example.amp-c.eq")
  let manifestURL = amp.directory.appendingPathComponent("module.json")
  let valid = try Data(contentsOf: manifestURL)
  defer { try? valid.write(to: manifestURL) }
  func scan(_ edit: (inout ModuleManifest) -> Void) throws -> [String] {
    var manifest = try JSONDecoder().decode(ModuleManifest.self, from: valid)
    edit(&manifest)
    try JSONEncoder().encode(manifest).write(to: manifestURL)
    return ModuleDiscovery.scan([amp.directory]).failures
  }
  #expect(try scan { _ in }.isEmpty)
  #expect(try !scan { $0.eq = .init(id: "", binary: "eq.so") }.isEmpty)
  #expect(try !scan { $0.eq = .init(id: $0.id, binary: "eq.so") }.isEmpty)
  #expect(try !scan { $0.eq = .init(id: "org.example.amp-c.eq", binary: "missing.so") }.isEmpty)
  #expect(try !scan { $0.eq = .init(id: "org.example.amp-c.eq", binary: "../eq.so") }.isEmpty)
  #expect(try !scan { $0.eq = .init(id: "org.example.amp-c.eq", binary: $0.binary) }.isEmpty)
  #expect(
    try !scan {
      $0 = ModuleManifest(
        schemaVersion: 1, id: "org.example.eq", category: "eq", binary: "eq.so",
        icon: "custom.svg", resources: ".")
    }.isEmpty)
  var mismatched = try JSONDecoder().decode(ModuleManifest.self, from: valid)
  mismatched.eq = .init(id: "org.example.other.eq", binary: "eq.so")
  #expect(throws: AmpError.self) {
    try LoadedModule(manifest: mismatched, directory: amp.directory)
  }
  let pedal = try TestModulePackage(category: "pre-fx", id: "org.example.pedal")
  var pedalManifest = pedal.manifest
  pedalManifest.eq = .init(id: "org.example.pedal.eq", binary: "custom.so")
  #expect(throws: AmpError.self) {
    try LoadedModule(manifest: pedalManifest, directory: pedal.directory, category: "pre-fx")
  }
}
