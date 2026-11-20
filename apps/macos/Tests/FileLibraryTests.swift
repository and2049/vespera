import Foundation
import Testing

@testable import VesperaUI

@Test func managedFilesAreBoundedDeduplicatedAndIndependentOfTheirSource() async throws {
  let root = FileManager.default.temporaryDirectory.appendingPathComponent("redsun/files-\(UUID())")
  try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
  defer { try? FileManager.default.removeItem(at: root) }
  let library = FileLibrary(root: root.appendingPathComponent("library"))
  let slot = ModuleFileSlot(
    id: 1, name: "Response", types: ["wav"], bundled: nil, maximumBytes: 16, optional: true)
  let source = root.appendingPathComponent("one.wav")
  let second = root.appendingPathComponent("two.wav")
  try Data([1, 2, 3]).write(to: source)
  try Data([1, 2, 3]).write(to: second)
  let first = try await library.importFile(source, slot: slot)
  let same = try await library.importFile(second, slot: slot)
  #expect(first.sha256 == same.sha256 && first.path == same.path)
  #expect(first.name != same.name)
  let copies = try FileManager.default.contentsOfDirectory(
    at: root.appendingPathComponent("library"), includingPropertiesForKeys: nil)
  #expect(copies.filter { $0.lastPathComponent != ".lock" }.count == 1)
  try FileManager.default.removeItem(at: source)
  let copy = try FileLibrary.resolvedPath(
    first, resources: root, library: root.appendingPathComponent("library"))
  #expect(try Data(contentsOf: copy) == Data([1, 2, 3]))
  try Data(repeating: 0, count: 17).write(to: second)
  await #expect(throws: (any Error).self) { try await library.importFile(second, slot: slot) }
  try Data([9]).write(to: copy)
  try Data([1, 2, 3]).write(to: second)
  _ = try await library.importFile(second, slot: slot)
  #expect(try Data(contentsOf: copy) == Data([1, 2, 3]))
  #expect(
    try FileManager.default.contentsOfDirectory(
      atPath: root.appendingPathComponent("library").path
    ).filter { $0 != ".lock" }.count == 1)
}

@Test func fileReferencePathsRejectTraversalAndSymlinkEscapes() throws {
  for path in ["", "/tmp/x", "../x", "x/../y", "x\\y", "x//y", "x:y"] {
    #expect(!FileLibrary.safeRelative(path))
  }
  let root = FileManager.default.temporaryDirectory.appendingPathComponent("redsun/files-\(UUID())")
  try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
  defer { try? FileManager.default.removeItem(at: root) }
  try FileManager.default.createSymbolicLink(
    at: root.appendingPathComponent("escape"), withDestinationURL: root.deletingLastPathComponent())
  #expect(throws: (any Error).self) { try FileLibrary.contained("escape/x", in: root) }
  try Data().write(to: root.appendingPathComponent("first"))
  try Data().write(to: root.appendingPathComponent("second"))
  #expect(throws: (any Error).self) { try FileLibrary.boundedEntries(in: root, limit: 1) }
}

@Test func managedLibraryEnforcesEntryAndTotalByteBudgets() async throws {
  let root = FileManager.default.temporaryDirectory.appendingPathComponent("redsun/files-\(UUID())")
  try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
  defer { try? FileManager.default.removeItem(at: root) }
  let slot = ModuleFileSlot(
    id: 1, name: "Response", types: ["wav"], bundled: nil, maximumBytes: 16, optional: true)
  let source = root.appendingPathComponent("source.wav")
  let bytes = FileLibrary(root: root.appendingPathComponent("bytes"), capacityBytes: 5)
  try Data([1, 2, 3]).write(to: source)
  let first = try await bytes.importFile(source, slot: slot)
  try Data([4, 5, 6]).write(to: source)
  await #expect(throws: (any Error).self) { try await bytes.importFile(source, slot: slot) }
  #expect(
    FileManager.default.fileExists(
      atPath: root.appendingPathComponent("bytes").appendingPathComponent(first.path).path))
  let entries = FileLibrary(
    root: root.appendingPathComponent("entries"), capacityBytes: 32, capacityEntries: 1)
  _ = try await entries.importFile(source, slot: slot)
  try Data([1]).write(to: source)
  await #expect(throws: (any Error).self) { try await entries.importFile(source, slot: slot) }
  #expect(
    try FileManager.default.contentsOfDirectory(
      atPath: root.appendingPathComponent("entries").path
    ).allSatisfy { !$0.hasPrefix(".import-") })
}

@Test func bundledReferencesStayPackageRelativeAndRejectEscapesAndOversize() async throws {
  let root = FileManager.default.temporaryDirectory.appendingPathComponent("redsun/files-\(UUID())")
  let resources = root.appendingPathComponent("resources")
  let directory = resources.appendingPathComponent("impulses")
  try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
  defer { try? FileManager.default.removeItem(at: root) }
  let slot = ModuleFileSlot(
    id: 1, name: "Response", types: ["wav"], bundled: "impulses", maximumBytes: 16, optional: true)
  let source = directory.appendingPathComponent("room.wav")
  try Data([1, 2, 3]).write(to: source)
  let library = FileLibrary(root: root.appendingPathComponent("managed"))
  let reference = try await library.bundledFile(source, resources: resources, slot: slot)
  #expect(reference.scope == .package && reference.path == "impulses/room.wav")
  #expect(
    try FileLibrary.resolvedPath(reference, resources: resources)
      == source.resolvingSymlinksInPath())
  #expect(!FileManager.default.fileExists(atPath: root.appendingPathComponent("managed").path))
  let outside = root.appendingPathComponent("outside.wav")
  try Data([4]).write(to: outside)
  let escape = directory.appendingPathComponent("escape.wav")
  try FileManager.default.createSymbolicLink(at: escape, withDestinationURL: outside)
  await #expect(throws: (any Error).self) {
    try await library.bundledFile(escape, resources: resources, slot: slot)
  }
  try Data(repeating: 0, count: 17).write(to: source)
  await #expect(throws: (any Error).self) {
    try await library.bundledFile(source, resources: resources, slot: slot)
  }
}

private actor ImportGate {
  var loads: [Int: CheckedContinuation<ModuleFileReference, any Error>] = [:]
  var observers: [Int: [CheckedContinuation<Void, Never>]] = [:]
  func load(_ id: Int) async throws -> ModuleFileReference {
    try await withCheckedThrowingContinuation { continuation in
      loads[id] = continuation
      for observer in observers.removeValue(forKey: id) ?? [] { observer.resume() }
    }
  }
  func entered(_ id: Int) async {
    if loads[id] != nil { return }
    await withCheckedContinuation { observers[id, default: []].append($0) }
  }
  func finish(_ id: Int, _ file: ModuleFileReference) {
    loads.removeValue(forKey: id)?.resume(returning: file)
  }
}

@MainActor
@Test func fileImportsSupersedeAndCancelWithoutPublishingStaleSelections() async throws {
  let fixture = try TestModulePackage(category: "special-fx", files: true)
  let module = try LoadedModule(
    manifest: fixture.manifest, directory: fixture.directory, category: "special-fx")
  let gate = ImportGate()
  func file(_ letter: String) -> ModuleFileReference {
    ModuleFileReference(
      slot: 1, scope: .library, path: String(repeating: letter, count: 64),
      name: "\(letter).wav", bytes: 1, sha256: String(repeating: letter, count: 64))
  }
  var changes = 0
  module.onFileChange = { changes += 1 }
  let first = module.importFile(
    1, operation: { try await gate.load(0) },
    reportError: { Issue.record($0) })
  await gate.entered(0)
  let second = module.importFile(
    1, operation: { try await gate.load(1) },
    reportError: { Issue.record($0) })
  await gate.entered(1)
  await gate.finish(1, file("b"))
  await second.value
  await gate.finish(0, file("a"))
  await first.value
  #expect(module.files == [file("b")] && changes == 1)
  let cancelled = module.importFile(
    1, operation: { try await gate.load(2) },
    reportError: { Issue.record($0) })
  await gate.entered(2)
  module.cancelFileImport(1)
  await gate.finish(2, file("c"))
  await cancelled.value
  #expect(module.files == [file("b")] && changes == 1 && !module.isImportingFile(1))
}

@MainActor
@Test func filePresetApplicationSupersedesPendingImports() async throws {
  let fixture = try TestModulePackage(category: "special-fx", files: true)
  let module = try LoadedModule(
    manifest: fixture.manifest, directory: fixture.directory, category: "special-fx")
  let file = ModuleFileReference(
    slot: 1, scope: .library, path: String(repeating: "a", count: 64),
    name: "preset.wav", bytes: 1, sha256: String(repeating: "a", count: 64))
  let oldFile = ModuleFileReference(
    slot: 1, scope: .library, path: String(repeating: "b", count: 64),
    name: "old-pick.wav", bytes: 1, sha256: String(repeating: "b", count: 64))
  var changes = 0
  module.onFileChange = { changes += 1 }
  for replacement in [[file], []] {
    try module.setFile(1, replacement.first)
    let staged = try module.stage(module.snapshot())
    let gate = ImportGate()
    let task = module.importFile(
      1, operation: { try await gate.load(0) }, reportError: { Issue.record($0) })
    await gate.entered(0)
    let before = changes
    try module.apply(staged)
    #expect(!module.isImportingFile(1))
    await gate.finish(0, oldFile)
    await task.value
    #expect(module.files == replacement && changes == before)
  }
}

@MainActor
@Test func fileImportConfirmationCannotCommitOrCancelSupersedingWork() async throws {
  let fixture = try TestModulePackage(category: "special-fx", files: true)
  let module = try LoadedModule(
    manifest: fixture.manifest, directory: fixture.directory, category: "special-fx")
  let file = ModuleFileReference(
    slot: 1, scope: .library, path: String(repeating: "a", count: 64),
    name: "old.wav", bytes: 1, sha256: String(repeating: "a", count: 64))
  let staged = try module.stage(module.snapshot())
  for accepted in [false, true] {
    let gate = ImportGate()
    var next: Task<Void, Never>?
    var changes = 0
    module.onFileChange = { changes += 1 }
    let first = module.importFile(
      1, operation: { try await gate.load(0) },
      confirm: { _ in
        do { try module.apply(staged) } catch { Issue.record(error) }
        next = module.importFile(
          1, operation: { try await gate.load(1) }, reportError: { Issue.record($0) })
        return accepted
      }, reportError: { Issue.record($0) })
    await gate.entered(0)
    await gate.finish(0, file)
    await first.value
    await gate.entered(1)
    #expect(module.files.isEmpty && changes == 0 && module.isImportingFile(1))
    await gate.finish(1, file)
    await next?.value
    #expect(module.files == [file] && changes == 1 && !module.isImportingFile(1))
  }
}

@MainActor
@Test func filePresetSchemaAndMissingReferencesRoundTripWithoutOpaqueStateChanges() throws {
  let fixture = try TestModulePackage(category: "special-fx", files: true)
  let module = try LoadedModule(
    manifest: fixture.manifest, directory: fixture.directory, category: "special-fx")
  #expect(module.fileSlots.count == 1)
  let before = try module.saveState()
  let file = ModuleFileReference(
    slot: 1, scope: .library, path: String(repeating: "a", count: 64),
    name: "missing.wav", bytes: 4, sha256: String(repeating: "a", count: 64))
  try module.setFile(1, file)
  #expect(try module.saveState() == before)
  let snapshot = try module.snapshot()
  #expect(snapshot.files == [file])
  let staged = try module.stage(snapshot)
  try module.setFile(1, nil)
  try module.apply(staged)
  #expect(module.files == [file])
  #expect(ModuleFileMenu.missing(module, file))
  var rig = RigSnapshot(
    globals: PresetGlobals(GlobalControlSettings()), amp: AmpSection(selected: nil, amps: []),
    cabinet: CabinetSection(independent: nil, cabinets: []),
    specialFX: SpecialFXSection(selected: module.id, modules: [snapshot]),
    preFX: BoardSnapshot(chain: [], drafts: [], editors: .init(kind: .pedals, slots: [])),
    postFX: BoardSnapshot(chain: [], drafts: [], editors: .init(kind: .pedals, slots: [])))
  let preset = RigPreset(name: "Files", rig: rig)
  #expect(preset.schemaVersion == 2)
  #expect(try RigPreset.decode(preset.encoded()).rig == rig)
  rig.specialFX.modules[0].files = nil
  #expect(RigPreset(name: "No files", rig: rig).schemaVersion == 1)
}
