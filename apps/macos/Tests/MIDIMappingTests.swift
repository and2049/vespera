import CoreMIDI
import Foundation
import Testing

@testable import VesperaUI

@MainActor
@Test func mappingsReachModuleCallbacksAndReleaseRemovedGates() throws {
  let fixture = try TestModulePackage(category: "special-fx", id: "org.example.mapping")
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
  let saved = audio.midiMappings
  defer {
    audio.midiMappings = saved
    audio.stop()
  }
  let target = try #require(
    audio.midiTargets.first { $0.module === module && $0.parameter.id == 1 })
  var received: [Float] = []
  module.onParameterChange = { _, value in received.append(value) }
  var mapping = MIDIMapping()
  mapping.target = target.id
  mapping.number = 7
  audio.midiMappings = [mapping]
  audio.applyMIDIMessages([.init(status: 0xb0, number: 7, value: 127)])
  #expect(try module.value(1) == target.parameter.maximum)
  #expect(received.last == target.parameter.maximum)
  module.onParameterChange = { _, _ in throw AmpError.message("Rejected") }
  audio.applyMIDIMessages([.init(status: 0xb0, number: 7, value: 0)])
  #expect(try module.value(1) == target.parameter.maximum)
  #expect(audio.error == "Rejected")
  module.onParameterChange = { _, value in received.append(value) }
  mapping.kind = .noteGate
  audio.midiMappings = [mapping]
  audio.applyMIDIMessages([.init(status: 0x90, number: 7, value: 90)])
  audio.midiMappings = []
  #expect(try module.value(1) == target.parameter.minimum)
  #expect(received.last == target.parameter.minimum)
}

@MainActor
private func mappingAudio(_ fixture: TestModulePackage, _ root: URL) throws -> (
  AudioController, LoadedModule
) {
  try fixture.link(into: root)
  let special = SpecialFXCatalog(directory: root)
  let module = try #require(special.shownModule)
  let empty = root.appendingPathComponent("empty")
  let audio = AudioController(
    amps: AmpCatalog(directory: empty), preFX: PedalCatalog(directory: empty),
    postFX: PedalCatalog(position: .post, directory: empty),
    cabinets: CabinetCatalog(directory: empty), specialFX: special)
  return (audio, module)
}

@MainActor
@Test func mappingsMoveDescribedParametersByTheValuesTheyTake() throws {
  let fixture = try TestModulePackage(
    category: "special-fx", id: "org.example.described", metadata: .described)
  let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
  defer { try? FileManager.default.removeItem(at: root) }
  let (audio, module) = try mappingAudio(fixture, root)
  let saved = audio.midiMappings
  defer {
    audio.midiMappings = saved
    audio.stop()
  }
  func target(_ id: UInt32) throws -> MIDITarget {
    try #require(audio.midiTargets.first { $0.module === module && $0.parameter.id == id })
  }
  func send(_ id: UInt32, _ kind: MIDIMapping.Kind, _ status: UInt8, _ number: UInt8, _ data: UInt8)
    throws -> Float
  {
    var mapping = MIDIMapping()
    mapping.kind = kind
    mapping.target = try target(id).id
    mapping.number = 20
    mapping.secondNumber = 21
    audio.midiMappings = [mapping]
    audio.applyMIDIMessages([.init(status: status, number: number, value: data)])
    return try module.value(id)
  }
  #expect(try target(1).parameter.display.kind == .toggle)
  #expect(try target(3).parameter.display.kind == .enumerated)
  #expect(try target(3).parameter.display.steps == 3)
  #expect(try send(1, .absolute, 0xb0, 20, 70) == 1)
  #expect(try send(1, .absolute, 0xb0, 20, 60) == 0)
  #expect(try send(3, .absolute, 0xb0, 20, 64) == 1)
  #expect(try send(3, .absolute, 0xb0, 20, 100) == 2)
  #expect(try send(4, .absolute, 0xb0, 20, 0) == 1)
  #expect(try send(4, .pair, 0xb0, 21, 127) == 2)
  #expect(try send(4, .pair, 0xb0, 21, 127) == 3)
  #expect(try send(4, .pair, 0xb0, 20, 127) == 2)
  #expect(try send(4, .relative, 0xb0, 20, 3) == 5)
  #expect(try send(4, .relative, 0xb0, 20, 126) == 3)
  #expect(try send(3, .pair, 0xb0, 20, 127) == 1)
  #expect(try send(2, .absolute, 0xb0, 20, 64) == Float(64) / 127)
  #expect(try target(3).parameter.text(atPosition: 0.5) == "Crunch")
  #expect(try target(2).parameter.text(atPosition: 0.75) == "+6.0 dB")
  #expect(try target(4).parameter.text(atPosition: 1) == "8 voices")
  #expect(try target(1).parameter.text(atPosition: 0.2) == "Off")
}

@MainActor
@Test func mappingsKeepContinuousValuesForModulesWithoutMetadata() throws {
  let fixture = try TestModulePackage(category: "special-fx", id: "org.example.plain")
  let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
  defer { try? FileManager.default.removeItem(at: root) }
  let (audio, module) = try mappingAudio(fixture, root)
  let saved = audio.midiMappings
  defer {
    audio.midiMappings = saved
    audio.stop()
  }
  let enabled = try #require(
    audio.midiTargets.first { $0.module === module && $0.parameter.id == 1 })
  #expect(enabled.parameter.display.kind == .continuous)
  #expect(!enabled.parameter.display.described)
  var mapping = MIDIMapping()
  mapping.target = enabled.id
  mapping.number = 7
  audio.midiMappings = [mapping]
  audio.applyMIDIMessages([.init(status: 0xb0, number: 7, value: 64)])
  #expect(try module.value(1) == Float(64) / 127)
  #expect(enabled.parameter.text(atPosition: 0.5) == "0.50 bool")
}

@Test func mappingBoundariesAndReleases() {
  var mapping = MIDIMapping()
  mapping.number = 12
  mapping.channel = 3
  #expect(mapping.apply(status: 0xb2, number: 12, data: 127, current: 0) == 1)
  #expect(mapping.apply(status: 0xb2, number: 12, data: 0, current: 1) == 0)
  #expect(mapping.apply(status: 0xb1, number: 12, data: 127, current: 0) == nil)
  #expect(mapping.apply(status: 0xb2, number: 13, data: 127, current: 0) == nil)
  #expect(mapping.apply(status: 0x92, number: 12, data: 127, current: 0) == nil)
  mapping.kind = .relative
  #expect(mapping.apply(status: 0xb2, number: 12, data: 127, current: 0) == 0)
  #expect(mapping.apply(status: 0xb2, number: 12, data: 1, current: 1) == 1)
  #expect(mapping.apply(status: 0xb2, number: 12, data: 127, current: 0.5)! < 0.5)
  mapping.kind = .noteGate
  #expect(mapping.apply(status: 0x92, number: 12, data: 70, current: 0) == 1)
  #expect(mapping.apply(status: 0x82, number: 12, data: 70, current: 1) == 0)
  #expect(mapping.apply(status: 0x92, number: 12, data: 0, current: 1) == 0)
  mapping.kind = .noteToggle
  #expect(mapping.apply(status: 0x92, number: 12, data: 70, current: 1) == 0)
  #expect(mapping.apply(status: 0x82, number: 12, data: 70, current: 1) == nil)
  mapping.kind = .notePair
  mapping.secondNumber = 13
  #expect(mapping.apply(status: 0x92, number: 12, data: 70, current: 0.5)! < 0.5)
  #expect(mapping.apply(status: 0x92, number: 13, data: 70, current: 0.5)! > 0.5)
  #expect(mapping.apply(status: 0x92, number: 13, data: 70, current: 0.5, step: 0.25) == 0.75)
  #expect(mapping.apply(status: 0x92, number: 12, data: 70, current: 0.1, step: 0.25) == 0)
  mapping.kind = .relative
  mapping.number = 12
  #expect(mapping.apply(status: 0xb2, number: 12, data: 2, current: 0, step: 0.25) == 0.5)
  #expect(mapping.apply(status: 0xb2, number: 12, data: 127, current: 0.5, step: 0.25) == 0.25)
  mapping.kind = .programValue
  mapping.value = 0.3
  #expect(mapping.apply(status: 0xc2, number: 12, data: 0, current: 0) == Float(0.3))
  mapping.enabled = false
  #expect(mapping.apply(status: 0xc2, number: 12, data: 0, current: 0) == nil)
}

@Test func mappingVirtualSourceRunningStatusOverflowAndReconnect() async throws {
  var client = MIDIClientRef()
  var source = MIDIEndpointRef()
  #expect(MIDIClientCreateWithBlock("vespera mapping test" as CFString, &client, nil) == noErr)
  defer { MIDIClientDispose(client) }
  #expect(MIDISourceCreate(client, "vespera mapping source" as CFString, &source) == noErr)
  defer { MIDIEndpointDispose(source) }
  var identifier: Int32 = 0
  #expect(MIDIObjectGetIntegerProperty(source, kMIDIPropertyUniqueID, &identifier) == noErr)
  let input = MIDIMappingInput()
  try input.connect(identifier)
  func send(_ bytes: [UInt8]) {
    let storage = UnsafeMutableRawPointer.allocate(byteCount: 4096, alignment: 8)
    defer { storage.deallocate() }
    let list = storage.assumingMemoryBound(to: MIDIPacketList.self)
    let first = MIDIPacketListInit(list)
    bytes.withUnsafeBufferPointer { buffer in
      _ = MIDIPacketListAdd(list, 4096, first, 0, bytes.count, buffer.baseAddress!)
    }
    #expect(MIDIReceived(source, list) == noErr)
  }
  send([0xb3, 12, 64, 13, 127, 0x93, 60, 90, 60, 0])
  try await Task.sleep(for: .milliseconds(100))
  let events = try input.drain()
  #expect(events.count == 4)
  #expect(events.map(\.status) == [0xb3, 0xb3, 0x93, 0x93])
  #expect(events.map(\.value) == [64, 127, 90, 0])
  send([0xb0] + Array(repeating: [UInt8(12), UInt8(64)], count: 300).flatMap { $0 })
  try await Task.sleep(for: .milliseconds(100))
  #expect(throws: (any Error).self) { try input.drain() }
  #expect(try input.drain().isEmpty)
  input.disconnect()
  send([0xb0, 12, 100])
  try await Task.sleep(for: .milliseconds(50))
  #expect(try input.drain().isEmpty)
  try input.connect(identifier)
  send([0xc0, 5])
  try await Task.sleep(for: .milliseconds(100))
  #expect(try input.drain().first?.number == 5)
}

@Test func mappingsOfMovedModulesRetargetToSpecialFX() {
  func mapping(_ target: String) -> MIDIMapping {
    var mapping = MIDIMapping()
    mapping.target = target
    return mapping
  }
  let saved = [
    mapping("pre-fx.0/org.example.moved/2"), mapping("post-fx.3/org.example.moved/2"),
    mapping("pre-fx.1/org.example.pedal/2"), mapping("specialFX/org.example.moved/5"),
    mapping("amp/org.example.moved/2"), mapping("pre-fx.2/org.example.moved"),
  ]
  let (relocated, moved) = MIDIMapping.relocated(saved, special: ["org.example.moved"])
  #expect(moved == 2)
  #expect(
    relocated.map(\.target) == [
      "specialFX/org.example.moved/2", "specialFX/org.example.moved/2",
      "pre-fx.1/org.example.pedal/2", "specialFX/org.example.moved/5", "amp/org.example.moved/2",
      "pre-fx.2/org.example.moved",
    ])
  #expect(zip(relocated, saved).allSatisfy { $0.id == $1.id && $0.number == $1.number })
  let again = MIDIMapping.relocated(relocated, special: ["org.example.moved"])
  #expect(again.moved == 0 && again.mappings == relocated)
}

@Test func targetKeysRoundTripTheStoredForm() throws {
  // A mapping saved by an older version of the app: the string predates MIDITargetKey.
  let stored = "pre-fx.3/org.example.pedal/7"
  let key = try #require(MIDITargetKey(storage: stored))
  #expect(key == MIDITargetKey(slot: "pre-fx.3", moduleID: "org.example.pedal", parameterID: 7))
  #expect(key.storage == stored)
  #expect(MIDITargetKey(storage: "pre-fx.2/org.example.moved") == nil)
  #expect(MIDITargetKey(storage: "amp/org.example.moved/x") == nil)
  var saved = MIDIMapping()
  saved.target = stored
  #expect(
    MIDIMapping.relocated([saved], special: ["org.example.pedal"]).mappings.first?.target
      == "specialFX/org.example.pedal/7")
}

@MainActor
@Test func savedMappingsAreRetargetedOnceAndTheOriginalsKept() throws {
  let fixture = try TestModulePackage(category: "special-fx", id: "org.example.moved")
  let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
  defer { try? FileManager.default.removeItem(at: root) }
  try fixture.link(into: root)
  let empty = root.appendingPathComponent("empty")
  func controller(_ defaults: UserDefaults) -> AudioController {
    AudioController(
      amps: AmpCatalog(directory: empty), preFX: PedalCatalog(directory: empty),
      postFX: PedalCatalog(position: .post, directory: empty),
      cabinets: CabinetCatalog(directory: empty), specialFX: SpecialFXCatalog(directory: root),
      defaults: defaults)
  }
  var moved = MIDIMapping()
  moved.target = "pre-fx.0/org.example.moved/1"
  var other = MIDIMapping()
  other.target = "pre-fx.1/org.example.gone/1"
  let defaults = MemoryDefaults()
  let original = try JSONEncoder().encode([moved, other])
  defaults.set(original, forKey: "audio.midiMappings")

  let first = controller(defaults)
  defer { first.stop() }
  #expect(first.midiMappings.map(\.target) == ["specialFX/org.example.moved/1", other.target])
  #expect(first.notice == "1 MIDI mapping now targets a module that moved to Special FX.")
  // A successful start clears `error`; the notice is separate and stays until dismissed.
  first.error = nil
  #expect(first.notice != nil)
  #expect(first.error?.contains("MIDI mapping") != true)
  #expect(defaults.data(forKey: AudioController.mappingsBeforeRelocationKey) == original)
  let stored = try #require(defaults.data(forKey: "audio.midiMappings"))
  #expect(try JSONDecoder().decode([MIDIMapping].self, from: stored) == first.midiMappings)
  #expect(first.midiTargets.contains { $0.id == "specialFX/org.example.moved/1" })

  // Relocated mappings and the kept originals are left alone from then on.
  var newer = moved
  newer.target = "post-fx.5/org.example.moved/1"
  defaults.set(try JSONEncoder().encode([newer]), forKey: "audio.midiMappings")
  let second = controller(defaults)
  defer { second.stop() }
  #expect(second.midiMappings.map(\.target) == ["specialFX/org.example.moved/1"])
  #expect(defaults.data(forKey: AudioController.mappingsBeforeRelocationKey) == original)
  let third = controller(defaults)
  defer { third.stop() }
  #expect(third.notice == nil)
  first.dismissNotice()
  #expect(first.notice == nil)
}
