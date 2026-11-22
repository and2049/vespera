import AVFoundation
import Foundation
import Testing

@testable import VesperaUI

/// Writes a mono binary32 WAV file of a 220 Hz tone.
private func writeTone(_ url: URL, frames: Int, sampleRate: UInt32 = 48000) throws {
  var data = Data()
  func append<T: FixedWidthInteger>(_ value: T) {
    withUnsafeBytes(of: value.littleEndian) { data.append(contentsOf: $0) }
  }
  let bytes = UInt32(frames * 4)
  data.append(contentsOf: Array("RIFF".utf8))
  append(UInt32(36) + bytes)
  data.append(contentsOf: Array("WAVEfmt ".utf8))
  append(UInt32(16))
  append(UInt16(3))
  append(UInt16(1))
  append(sampleRate)
  append(sampleRate * 4)
  append(UInt16(4))
  append(UInt16(32))
  data.append(contentsOf: Array("data".utf8))
  append(bytes)
  for frame in 0..<frames {
    let sample = Float(0.5 * sin(2 * Double.pi * 220 * Double(frame) / Double(sampleRate)))
    append(sample.bitPattern)
  }
  try data.write(to: url)
}

/// An `AudioController` on the real audio bridge with fixture modules, never started, whose
/// takes use their own directories and save renders to `destinations` in turn.
@MainActor
private final class BridgeHarness {
  let fixture: RigFixture
  let root: URL
  let temporary: URL
  var destinations: [URL] = []
  var suggestedNames: [String] = []
  lazy var audio = AudioController(
    amps: AmpCatalog(directory: fixture.modules),
    preFX: PedalCatalog(position: .pre, directory: fixture.modules),
    postFX: PedalCatalog(position: .post, directory: fixture.modules),
    cabinets: CabinetCatalog(directory: fixture.modules),
    specialFX: SpecialFXCatalog(directory: fixture.modules), defaults: MemoryDefaults(),
    takeDirectory: root.appendingPathComponent("Takes"), takeTemporaryDirectory: temporary,
    takePrompts: TakeController.Prompts(
      chooseInput: { nil },
      saveOutput: { [unowned self] name in
        self.suggestedNames.append(name)
        return self.destinations.isEmpty ? nil : self.destinations.removeFirst()
      }))

  init() throws {
    fixture = try RigFixture()
    root = fixture.root.appendingPathComponent("takes")
    temporary = root.appendingPathComponent("tmp")
    try FileManager.default.createDirectory(at: temporary, withIntermediateDirectories: true)
  }

  /// Services the main run loop, and with it the controller's status poll, until the render
  /// in progress ends.
  func finish(timeout: TimeInterval = 60) {
    let deadline = Date().addingTimeInterval(timeout)
    while audio.takes.needsStatus || audio.takes.isReamping, Date() < deadline {
      RunLoop.main.run(until: Date().addingTimeInterval(0.01))
    }
  }

  func temporaryFiles() throws -> [String] {
    try FileManager.default.contentsOfDirectory(atPath: temporary.path)
  }
}

@MainActor
@Test func reampingThroughTheRealBridgeSavesTheRenderedFile() throws {
  let harness = try BridgeHarness()
  let audio = harness.audio
  defer { audio.stop() }
  #expect(audio.state != .running)
  let input = harness.root.appendingPathComponent("Take 1.wav")
  try writeTone(input, frames: 4800)
  let tone = (0..<4800).map { Float(0.5 * sin(2 * Double.pi * 220 * Double($0) / 48000)) }
  let expectedFrames = try audio.renderOffline(input: [tone]).first?.count
  #expect(expectedFrames.map { $0 >= 4800 } == true)

  let first = harness.root.appendingPathComponent("Out/First.wav")
  let second = harness.root.appendingPathComponent("Second.wav")
  try FileManager.default.createDirectory(
    at: first.deletingLastPathComponent(), withIntermediateDirectories: true)
  harness.destinations = [first, second]
  for destination in [first, second] {
    audio.takes.startReamp(input)
    guard case .rendering = audio.takes.reamp else {
      Issue.record("render did not start: \(audio.takes.reamp), \(audio.error ?? "")")
      return
    }
    harness.finish()
    guard case .finished(let peak, let saved) = audio.takes.reamp else {
      Issue.record("render did not finish: \(audio.takes.reamp)")
      return
    }
    #expect(saved == destination)
    #expect(peak > 0 && peak.isFinite)
    let file = try AVAudioFile(forReading: destination)
    #expect(Int(file.length) == expectedFrames)
    #expect(file.fileFormat.sampleRate == 48000 && file.fileFormat.channelCount == 2)
    #expect(try harness.temporaryFiles().isEmpty)
  }
  #expect(harness.suggestedNames == ["Take 1 reamp.wav", "Take 1 reamp.wav"])
  for _ in 0..<10 { RunLoop.main.run(until: Date().addingTimeInterval(0.02)) }
  #expect(harness.suggestedNames.count == 2)
  #expect(audio.error == nil)
}

@MainActor
@Test func realReampRejectionsExplainThemselvesAndLeaveNoFiles() throws {
  let harness = try BridgeHarness()
  let audio = harness.audio
  defer { audio.stop() }
  let text = harness.root.appendingPathComponent("Notes.wav")
  try Data("not audio".utf8).write(to: text)
  audio.takes.startReamp(text)
  #expect(
    audio.takes.reamp == .failed("Reamp failed: WAV file Notes.wav: is not a RIFF WAVE file."))
  #expect(audio.error == "WAV file Notes.wav: is not a RIFF WAVE file")

  let empty = harness.root.appendingPathComponent("Empty.wav")
  try writeTone(empty, frames: 0)
  audio.takes.startReamp(empty)
  #expect(audio.takes.reamp == .failed("Reamp failed: Reamp input has no audio."))

  audio.takes.startReamp(harness.root.appendingPathComponent("Missing.wav"))
  #expect(audio.takes.reamp == .failed("Reamp failed: Missing.wav no longer exists."))
  #expect(!audio.takes.needsStatus)
  #expect(try harness.temporaryFiles().isEmpty)
  #expect(harness.suggestedNames.isEmpty)
}

@MainActor
@Test func cancellingARealReampEndsCancelledWithoutFiles() throws {
  let harness = try BridgeHarness()
  let audio = harness.audio
  defer { audio.stop() }
  let input = harness.root.appendingPathComponent("Long.wav")
  try writeTone(input, frames: 48000 * 20)
  try Data(#"{"version": 1, "bpm": 100, "blockSize": 1}"#.utf8)
    .write(to: harness.root.appendingPathComponent("Long.json"))
  audio.takes.startReamp(input)
  guard case .rendering = audio.takes.reamp else {
    Issue.record("render did not start: \(audio.takes.reamp), \(audio.error ?? "")")
    return
  }
  RunLoop.main.run(until: Date().addingTimeInterval(0.05))
  guard case .rendering(_, let progress) = audio.takes.reamp, progress < 1 else {
    Issue.record("render ended before it could be cancelled: \(audio.takes.reamp)")
    return
  }
  audio.takes.cancelReamp()
  if case .cancelling = audio.takes.reamp {
  } else {
    Issue.record("expected cancelling, found \(audio.takes.reamp)")
  }
  harness.finish()
  #expect(audio.takes.reamp == .cancelled)
  #expect(try harness.temporaryFiles().isEmpty)
  #expect(harness.suggestedNames.isEmpty)
  #expect(audio.error == nil)
}
