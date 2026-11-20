import Foundation
import Testing

@testable import VesperaUI

@MainActor
@Test func cabinetLinkingRetainsSettingsAndRollsBackFailedChanges() throws {
  let first = try TestModulePackage(category: "cab", id: "org.example.cab-a")
  let second = try TestModulePackage(category: "cab", id: "org.example.cab-b")
  let head = try TestModulePackage(category: "amp")
  let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
  try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
  defer { try? FileManager.default.removeItem(at: root) }
  for fixture in [first, second] { try fixture.link(into: root) }
  let catalog = CabinetCatalog(directory: root)
  #expect(catalog.cabinets.count == 2)
  var manifest = head.manifest
  manifest.cabinet = .init(supported: true, defaultModule: first.manifest.id)
  let ampA = try LoadedModule(manifest: manifest, directory: head.directory)
  manifest.cabinet = .init(supported: true, defaultModule: second.manifest.id)
  let ampB = try LoadedModule(manifest: manifest, directory: head.directory)
  let legacy = try LoadedModule(manifest: head.manifest, directory: head.directory)
  manifest.cabinet = .init(supported: false)
  let unsupported = try LoadedModule(manifest: manifest, directory: head.directory)
  try catalog.setAmp(ampA)
  #expect(catalog.locked && catalog.selectedID == first.manifest.id)
  let indexB = try #require(catalog.cabinets.firstIndex { $0.id == second.manifest.id })
  catalog.select(indexB)
  #expect(catalog.selectedID == first.manifest.id)
  func swipe(_ direction: Int) {
    CarouselNavigation(
      count: catalog.cabinets.count, selectedIndex: catalog.selectedIndex,
      onSelect: catalog.select
    ).step(direction)
  }
  swipe(1)
  #expect(catalog.selectedID == first.manifest.id)
  catalog.toggleLock()
  swipe(-1)
  #expect(catalog.selectedID == second.manifest.id)
  swipe(1)
  #expect(catalog.selectedID == first.manifest.id)
  swipe(1)
  let selected = try #require(catalog.activeCabinet)
  try selected.setValue(1, 1)
  #expect(catalog.enabled)
  catalog.toggleEnabled()
  #expect(!catalog.enabled)
  #expect(try selected.value(1) == 0)
  selected.onParameterChange = { _, _ in throw AmpError.message("Rejected bypass") }
  catalog.toggleEnabled()
  #expect(!catalog.enabled && catalog.error != nil)
  selected.onParameterChange = nil
  catalog.toggleEnabled()
  #expect(catalog.enabled && catalog.error == nil)
  try selected.setValue(2, 0.83)
  try selected.setValue(1, 0)
  try catalog.setAmp(ampA)
  #expect(catalog.activeCabinet === selected)
  for amp in [unsupported, legacy] {
    try catalog.setAmp(amp)
    #expect(!catalog.supported && catalog.activeCabinet == nil && !catalog.locked)
    try catalog.setAmp(ampB)
    #expect(catalog.activeCabinet === selected)
    #expect(try selected.value(2) == 0.83 && selected.value(1) == 0)
  }
  try catalog.setAmp(ampA)
  catalog.onChange = { _, _ in throw AmpError.message("Rejected rig") }
  catalog.toggleLock()
  #expect(!catalog.locked && catalog.activeCabinet === selected)
  #expect(throws: AmpError.self) { try catalog.setAmp(unsupported) }
  #expect(catalog.amp === ampA && catalog.supported)
  let indexA = try #require(catalog.cabinets.firstIndex { $0.id == first.manifest.id })
  catalog.select(indexA)
  #expect(catalog.activeCabinet === selected)
  swipe(1)
  #expect(catalog.activeCabinet === selected)
  catalog.onChange = nil
  catalog.toggleLock()
  #expect(catalog.locked && catalog.selectedID == first.manifest.id)
  try catalog.setAmp(ampB)
  #expect(catalog.activeCabinet === selected)
  manifest.cabinet = .init(supported: true, defaultModule: "missing.cabinet")
  let missing = try LoadedModule(manifest: manifest, directory: head.directory)
  let empty = CabinetCatalog(directory: root.appendingPathComponent("absent"))
  #expect(throws: AmpError.self) { try empty.setAmp(ampA) }
  #expect(empty.amp == nil && empty.activeCabinet == nil)
  #expect(throws: AmpError.self) { try catalog.setAmp(missing) }
  #expect(catalog.amp === ampB && catalog.activeCabinet === selected)
}
