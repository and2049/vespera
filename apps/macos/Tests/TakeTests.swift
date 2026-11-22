import Foundation
import Testing

@testable import VesperaUI

private func decodeStatus(_ extra: String = "") throws -> AudioStatus {
  try JSONDecoder().decode(
    AudioStatus.self,
    from: Data(
      """
      {"state": "running", "recovery": "none", "lastError": 0, "blocks": 1,
       "input": [0, 0], "output": [0, 0]\(extra.isEmpty ? "" : ", \(extra)")}
      """.utf8))
}

/// A `TakeController` whose bridge, audio state and prompts are fakes, working in its own
/// temporary directory.
@MainActor
private final class TakeHarness {
  let root: URL
  let directory: URL
  let temporary: URL
  let defaults = MemoryDefaults()
  var running = true
  var requests: [[String: Any]] = []
  var rejection: String?
  var reported: [String] = []
  var notices: [String] = []
  /// The JSON value the fake bridge replies with for a request; null when nil.
  var reply: (([String: Any]) -> String)?
  var chosen: URL?
  var destination: URL?
  var suggestedNames: [String] = []
  let date = Date(timeIntervalSince1970: 1_790_000_000)
  var configuration: [String: Any] = ["amp": NSNull(), "globals": ["inputGain": 0]]
  lazy var takes = make()

  init() throws {
    root = FileManager.default.temporaryDirectory
      .appendingPathComponent("redsun/take-tests/\(UUID().uuidString)")
    directory = root.appendingPathComponent("Takes")
    temporary = root.appendingPathComponent("tmp")
    try FileManager.default.createDirectory(at: temporary, withIntermediateDirectories: true)
  }

  deinit { try? FileManager.default.removeItem(at: root) }

  func make() -> TakeController {
    TakeController(
      directory: directory, temporaryDirectory: temporary, defaults: defaults,
      prompts: TakeController.Prompts(
        chooseInput: { [unowned self] in self.chosen },
        saveOutput: { [unowned self] name in
          self.suggestedNames.append(name)
          return self.destination
        }),
      request: { [unowned self] request in
        if let rejection = self.rejection { throw AmpError.message(rejection) }
        self.requests.append(request)
        return Data((self.reply?(request) ?? "null").utf8)
      },
      isAudioRunning: { [unowned self] in self.running },
      configuration: { [unowned self] in self.configuration },
      bpm: { 132 }, blockSize: { 256 },
      report: { [unowned self] error in self.reported.append(error.localizedDescription) },
      notify: { [unowned self] message in self.notices.append(message) },
      now: { [unowned self] in self.date })
  }

  func file(_ name: String, contents: String = "wav") throws -> URL {
    let url = root.appendingPathComponent(name)
    try Data(contents.utf8).write(to: url)
    return url
  }

  func apply(_ extra: String) throws { takes.apply(try decodeStatus(extra)) }

  /// Applies a reamp status naming the render in progress as its output.
  func reamp(_ fields: String) throws {
    try apply(#""reamp": {"output": "\#(output?.path ?? "/none.wav")", \#(fields)}"#)
  }

  /// The temporary output of the render in progress.
  var output: URL? {
    switch takes.reamp {
    case .rendering(let job, _), .cancelling(let job), .saving(let job, _): job.output
    default: nil
    }
  }
}

@Test func recordingAndReampStatusesDecodeEveryState() throws {
  for (raw, state) in [
    ("idle", RecordingStatus.State.idle), ("recording", .recording),
    ("finalizing", .finalizing), ("failed", .failed), ("paused", .unknown("paused")),
  ] {
    let status = try decodeStatus(#""recording": {"state": "\#(raw)"}"#)
    #expect(status.recording?.state == state)
  }
  for (raw, state) in [
    ("idle", ReampStatus.State.idle), ("rendering", .rendering), ("done", .done),
    ("failed", .failed), ("cancelled", .cancelled), ("queued", .unknown("queued")),
  ] {
    let status = try decodeStatus(#""reamp": {"state": "\#(raw)"}"#)
    #expect(status.reamp?.state == state)
  }
  let full = try decodeStatus(
    #"""
    "recording": {"state": "recording", "path": "/tmp/a.wav", "frames": 96000,
                  "sampleRate": 48000, "channels": 2, "droppedFrames": 3},
    "reamp": {"state": "done", "progress": 1, "output": "/tmp/b.wav", "peak": 0.5}
    """#)
  #expect(
    full.recording
      == RecordingStatus(
        state: .recording, path: "/tmp/a.wav", frames: 96000, sampleRate: 48000, channels: 2,
        droppedFrames: 3))
  #expect(
    full.reamp == ReampStatus(state: .done, progress: 1, output: "/tmp/b.wav", peak: 0.5))
  let failed = try decodeStatus(
    #""recording": {"state": "failed", "error": "disk"}, "reamp": {"state": "failed", "error": "x"}"#
  )
  #expect(failed.recording?.error == "disk" && failed.reamp?.error == "x")
  let older = try decodeStatus()
  #expect(older.recording == nil && older.reamp == nil)
}

@MainActor
@Test func recordingStartsInTheTakeDirectoryAndFinishesIntoTheLastTake() throws {
  let harness = try TakeHarness()
  let takes = harness.takes
  #expect(takes.recordControl == .start(enabled: true))
  #expect(!takes.needsStatus)
  takes.toggleRecording()
  let name = TakeController.takeName(harness.date)
  #expect(name.wholeMatch(of: /Take \d{4}-\d{2}-\d{2} \d{2}\.\d{2}\.\d{2}/) != nil)
  let path = harness.directory.appendingPathComponent("\(name).wav").path
  #expect(harness.requests.count == 1)
  #expect(harness.requests.first?["op"] as? String == "record")
  #expect(harness.requests.first?["action"] as? String == "start")
  #expect(harness.requests.first?["path"] as? String == path)
  var isDirectory: ObjCBool = false
  #expect(FileManager.default.fileExists(atPath: harness.directory.path, isDirectory: &isDirectory))
  #expect(isDirectory.boolValue)
  #expect(takes.recordControl == .stop && takes.needsStatus)
  #expect(takes.elapsedText == "0:00")

  try harness.apply(
    #""recording": {"state": "recording", "path": "\#(path)", "frames": 2016000, "sampleRate": 48000, "channels": 1, "droppedFrames": 0}"#
  )
  #expect(takes.elapsedText == "0:42")
  takes.toggleRecording()
  #expect(harness.requests.last?["action"] as? String == "stop")
  #expect(harness.requests.last?["path"] == nil)
  #expect(takes.recordControl == .finalizing)
  takes.toggleRecording()
  #expect(harness.requests.count == 2)

  try harness.apply(
    #""recording": {"state": "finalizing", "path": "\#(path)", "frames": 2064000, "sampleRate": 48000, "channels": 1, "droppedFrames": 0}"#
  )
  #expect(takes.recordControl == .finalizing && takes.needsStatus)
  #expect(takes.lastTake == nil)
  try Data("wav".utf8).write(to: URL(fileURLWithPath: path))
  try harness.apply(
    #""recording": {"state": "idle", "path": "\#(path)", "frames": 2064000, "sampleRate": 48000, "channels": 1, "droppedFrames": 2}"#
  )
  #expect(takes.recording == .idle && !takes.needsStatus)
  let take = try #require(takes.lastTake)
  #expect(
    take == Take(path: path, frames: 2_064_000, sampleRate: 48000, channels: 1, droppedFrames: 2))
  #expect(take.summary == "0:43 · 48 kHz · Mono")
  #expect(take.dropoutWarning == "2 frames were dropped while recording")
  #expect(harness.defaults.data(forKey: TakeController.lastTakeKey) != nil)
  #expect(harness.reported.isEmpty)

  try harness.apply(#""recording": {"state": "idle", "path": "/elsewhere.wav"}"#)
  #expect(takes.lastTake == take)
  takes.startRecording()
  #expect(
    harness.requests.last?["path"] as? String
      == harness.directory.appendingPathComponent("\(name) 2.wav").path)
}

@MainActor
@Test func recordingIsRefusedWhileAudioIsNotRunning() throws {
  let harness = try TakeHarness()
  harness.running = false
  #expect(harness.takes.recordControl == .start(enabled: false))
  harness.takes.toggleRecording()
  #expect(harness.requests.isEmpty)
  #expect(harness.takes.recording == .idle)
  #expect(harness.reported == ["Start audio to record a take."])
}

@MainActor
@Test func recordingFailuresAndRejectionsAreReported() throws {
  let harness = try TakeHarness()
  harness.rejection = "Unknown op"
  harness.takes.startRecording()
  #expect(harness.takes.recording == .idle)
  #expect(harness.reported == ["Unknown op"])

  harness.rejection = nil
  harness.takes.startRecording()
  try harness.apply(#""recording": {"state": "failed", "error": "the disk is full"}"#)
  #expect(harness.takes.recording == .failed("Recording failed: the disk is full."))
  #expect(harness.reported.last == "Recording failed: the disk is full.")
  #expect(harness.takes.recordControl == .start(enabled: true))
  #expect(!harness.takes.needsStatus)
  try harness.apply(#""recording": {"state": "failed", "error": "the disk is full"}"#)
  #expect(harness.reported.count == 2)
  harness.takes.startRecording()
  #expect(harness.takes.recordControl == .stop)
}

@MainActor
@Test func recordRepliesCarryTheStatusAndAFullTakeIsExplainedOnce() throws {
  let harness = try TakeHarness()
  harness.reply = { request in
    let path = harness.directory.appendingPathComponent(
      "\(TakeController.takeName(harness.date)).wav"
    ).path
    let state = request["action"] as? String == "start" ? "recording" : "idle"
    return #"""
      {"state": "running", "recovery": "none", "lastError": 0, "blocks": 1, "input": [0, 0],
       "output": [0, 0], "recording": {"state": "\#(state)", "path": "\#(path)",
       "frames": 96000, "sampleRate": 48000, "channels": 2, "droppedFrames": 0}}
      """#
  }
  harness.takes.startRecording()
  #expect(harness.takes.elapsedText == "0:02")
  let path = try #require(harness.requests.first?["path"] as? String)
  try Data("wav".utf8).write(to: URL(fileURLWithPath: path))
  harness.takes.stopRecording()
  #expect(harness.takes.recording == .idle)
  #expect(harness.takes.lastTake?.summary == "0:02 · 48 kHz · Stereo")
  #expect(harness.notices.isEmpty)

  harness.reply = nil
  harness.takes.startRecording()
  let full =
    #""recording": {"state": "idle", "path": "\#(path)", "frames": 10, "sampleRate": 48000, "channels": 1, "droppedFrames": 0, "stopReason": "full"}"#
  try harness.apply(full)
  try harness.apply(full)
  #expect(harness.takes.recording == .idle)
  #expect(
    harness.notices
      == ["Recording stopped because the take reached the largest size a WAV file can hold."])
  #expect(harness.reported.isEmpty)
}

@MainActor
@Test func aPersistedFinishedRenderNeverResurfaces() throws {
  let harness = try TakeHarness()
  harness.reply = { request in
    let output = request["output"] as? String ?? harness.output?.path ?? ""
    return #"{"state": "rendering", "progress": 0, "output": "\#(output)", "peak": 0}"#
  }
  harness.takes.startReamp(try harness.file("First.wav"))
  let first = try #require(harness.output)
  try harness.reamp(#""state": "failed", "error": "Amp: missing file""#)
  #expect(harness.takes.reamp == .failed("Reamp failed: Amp: missing file."))
  try harness.apply(#""reamp": {"state": "done", "output": "\#(first.path)", "peak": 0.5}"#)
  #expect(harness.suggestedNames.isEmpty)

  let fresh = harness.make()
  fresh.apply(
    try decodeStatus(#""reamp": {"state": "done", "output": "\#(first.path)", "peak": 1}"#))
  #expect(fresh.reamp == .idle)

  harness.takes.startReamp(try harness.file("Second.wav"))
  let second = try #require(harness.output)
  #expect(second != first)
  try harness.apply(#""reamp": {"state": "done", "output": "\#(first.path)", "peak": 0.5}"#)
  #expect(harness.suggestedNames.isEmpty)
  harness.takes.cancelReamp()
  if case .cancelling = harness.takes.reamp {} else { Issue.record("expected cancelling") }
  try harness.reamp(#""state": "cancelled""#)
  #expect(harness.takes.reamp == .cancelled)
}

@MainActor
@Test func enginesWithoutRecordingStatusEndARecordingWithoutATake() throws {
  let harness = try TakeHarness()
  try harness.apply("")
  #expect(harness.takes.recording == .idle && harness.takes.reamp == .idle)
  harness.takes.startRecording()
  try harness.apply("")
  #expect(harness.takes.recording == .idle)
  #expect(harness.takes.lastTake == nil)
  try harness.apply(#""recording": {"state": "stopping"}"#)
  #expect(harness.takes.recording == .idle)
}

@Test func elapsedTimeIsMinutesAndSecondsUntilAnHour() {
  #expect(TakeController.elapsed(0) == "0:00")
  #expect(TakeController.elapsed(59) == "0:59")
  #expect(TakeController.elapsed(59.99) == "0:59")
  #expect(TakeController.elapsed(60) == "1:00")
  #expect(TakeController.elapsed(3599) == "59:59")
  #expect(TakeController.elapsed(3600) == "1:00:00")
  #expect(TakeController.elapsed(3725) == "1:02:05")
  #expect(TakeController.elapsed(-1) == "0:00")
  #expect(TakeController.elapsed(.nan) == "0:00")
  #expect(Take(path: "/a.wav").summary == "0:00")
  #expect(
    Take(path: "/a.wav", frames: 441000, sampleRate: 44100, channels: 2).summary
      == "0:10 · 44.1 kHz · Stereo")
  #expect(
    Take(path: "/a.wav", droppedFrames: 1).dropoutWarning == "1 frame was dropped while recording")
  #expect(Take(path: "/a.wav").dropoutWarning == nil)
}

@MainActor
@Test func theLastTakePersistsAndIsForgottenOnceItsFileIsGone() throws {
  let harness = try TakeHarness()
  harness.takes.startRecording()
  let path = try #require(harness.requests.first?["path"] as? String)
  try Data("wav".utf8).write(to: URL(fileURLWithPath: path))
  try harness.apply(
    #""recording": {"state": "idle", "path": "\#(path)", "frames": 48000, "sampleRate": 48000, "channels": 2, "droppedFrames": 0}"#
  )
  let take = try #require(harness.takes.lastTake)

  #expect(harness.make().lastTake == take)
  try FileManager.default.removeItem(atPath: path)
  harness.takes.refreshLastTake()
  #expect(harness.takes.lastTake == nil)
  #expect(harness.defaults.data(forKey: TakeController.lastTakeKey) == nil)

  harness.defaults.set(try JSONEncoder().encode(take), forKey: TakeController.lastTakeKey)
  #expect(harness.make().lastTake == nil)
  #expect(harness.defaults.data(forKey: TakeController.lastTakeKey) == nil)
  harness.takes.processLastTake()
  #expect(harness.reported == ["The last take no longer exists."])
  #expect(harness.requests.count == 1)
}

@MainActor
@Test func reampSendsTheCurrentRigAndTheSourcesTempo() throws {
  let fixture = try RigFixture()
  let harness = try TakeHarness()
  let audio = AudioController(
    amps: AmpCatalog(directory: fixture.modules),
    preFX: PedalCatalog(position: .pre, directory: fixture.modules),
    postFX: PedalCatalog(position: .post, directory: fixture.modules),
    cabinets: CabinetCatalog(directory: fixture.modules),
    specialFX: SpecialFXCatalog(directory: fixture.modules), defaults: MemoryDefaults(),
    takeDirectory: harness.directory,
    takePrompts: TakeController.Prompts(chooseInput: { nil }, saveOutput: { _ in nil }))
  defer { audio.stop() }
  var requests: [[String: Any]] = []
  audio.submitTakeRequest = { request in
    requests.append(request)
    return Data("null".utf8)
  }
  var globals = audio.globals
  globals.inputGain = 3
  audio.updateGlobals(globals)
  #expect(audio.state != .running)
  #expect(audio.pollCadence == .idle)

  let source = try harness.file("Take 1.wav")
  audio.takes.startReamp(source)
  let request = try #require(requests.first)
  #expect(request["op"] as? String == "reamp")
  #expect(request["action"] as? String == "start")
  #expect(request["input"] as? String == source.path)
  #expect(request["bpm"] as? Double == audio.tempo.bpm)
  #expect(request["blockSize"] as? Int == Int(audio.configuration.bufferSize))
  func canonical(_ object: Any?) throws -> Data {
    try JSONSerialization.data(withJSONObject: try #require(object), options: [.sortedKeys])
  }
  let live = try audio.startConfiguration()
  #expect(try canonical(request["configuration"]) == canonical(live))
  #expect(
    (request["configuration"] as? [String: Any])?["globals"].map { try? canonical($0) }
      == (try canonical(AudioBridge.object(globals))))
  #expect(live["amp"] is [String: Any])
  let output = URL(fileURLWithPath: try #require(request["output"] as? String))
  #expect(
    output.deletingLastPathComponent().standardizedFileURL.path
      == FileManager.default.temporaryDirectory.standardizedFileURL.path)
  #expect(output.pathExtension == "wav")
  #expect(!FileManager.default.fileExists(atPath: output.path))
  #expect(audio.takes.needsStatus)
  #expect(audio.pollCadence == .running)
  audio.apply(
    status: try decodeStatus(
      #""reamp": {"state": "rendering", "progress": 0.25, "output": "\#(output.path)"}"#))
  #expect(audio.takes.reamp == .rendering(.init(source: source, output: output), progress: 0.25))
  audio.apply(
    status: try decodeStatus(#""reamp": {"state": "cancelled", "output": "\#(output.path)"}"#))
  #expect(audio.takes.reamp == .cancelled)
  #expect(audio.pollCadence == .idle)
}

@MainActor
@Test func reampUsesATakesSidecarTempoAndTheCurrentTempoOtherwise() throws {
  let harness = try TakeHarness()
  harness.running = false
  let plain = try harness.file("DI.wav")
  harness.takes.startReamp(plain)
  #expect(harness.requests.last?["bpm"] as? Double == 132)
  #expect(harness.requests.last?["blockSize"] as? Int == 256)
  let configuration = try #require(harness.requests.last?["configuration"] as? [String: Any])
  #expect(NSDictionary(dictionary: configuration).isEqual(to: harness.configuration))
  harness.takes.startReamp(plain)
  #expect(harness.requests.count == 1)
  try harness.reamp(#""state": "cancelled""#)

  let take = try harness.file("Take 2.wav")
  _ = try harness.file(
    "Take 2.json", contents: #"{"bpm": 97.5, "sampleRate": 48000, "blockSize": 64}"#)
  harness.takes.startReamp(take)
  #expect(harness.requests.last?["bpm"] as? Double == 97.5)
  #expect(harness.requests.last?["blockSize"] as? Int == 64)
  try harness.reamp(#""state": "cancelled""#)

  for (index, sidecar) in [
    #"{"bpm": "fast", "blockSize": "big"}"#, #"{"bpm": 20, "blockSize": 0}"#,
    #"{"bpm": 300, "blockSize": 8192}"#, #"{"blockSize": 12.5}"#, "not json",
  ].enumerated() {
    let unreadable = try harness.file("Take \(index + 3).wav")
    _ = try harness.file("Take \(index + 3).json", contents: sidecar)
    harness.takes.startReamp(unreadable)
    #expect(harness.requests.last?["bpm"] as? Double == 132)
    #expect(harness.requests.last?["blockSize"] as? Int == 256)
    try harness.reamp(#""state": "cancelled""#)
  }

  harness.chosen = take
  harness.takes.chooseAndProcess()
  #expect(harness.requests.last?["input"] as? String == take.path)
  #expect(harness.requests.count == 8)
}

@MainActor
@Test func aFinishedReampMovesToTheChosenDestination() throws {
  let harness = try TakeHarness()
  let source = try harness.file("Take 9.wav")
  harness.destination = try harness.file("Kept.wav", contents: "older")
  harness.takes.startReamp(source)
  let output = try #require(harness.output)
  #expect(output.deletingLastPathComponent().path == harness.temporary.path)
  try Data("rendered".utf8).write(to: output)

  try harness.reamp(#""state": "rendering", "progress": 1.5"#)
  #expect(harness.takes.reamp == .rendering(.init(source: source, output: output), progress: 1))
  try harness.apply(#""reamp": {"state": "done", "output": "/elsewhere.wav", "peak": 0.5}"#)
  try harness.apply(#""reamp": {"state": "failed", "output": "/elsewhere.wav"}"#)
  try harness.apply(#""reamp": {"state": "done", "peak": 0.5}"#)
  #expect(harness.suggestedNames.isEmpty)
  #expect(harness.takes.reamp == .rendering(.init(source: source, output: output), progress: 1))
  try harness.apply(#""reamp": {"state": "done", "output": "\#(output.path)", "peak": 0.5}"#)
  #expect(harness.suggestedNames == ["Take 9 reamp.wav"])
  let destination = try #require(harness.destination)
  #expect(harness.takes.reamp == .finished(peak: 0.5, destination: destination))
  #expect(try String(contentsOf: destination, encoding: .utf8) == "rendered")
  #expect(!FileManager.default.fileExists(atPath: output.path))
  #expect(harness.takes.reampMessage?.text == "Saved “Kept.wav” · peak −6.0 dBFS")
  #expect(harness.takes.reampMessage?.isProblem == false)
  #expect(!harness.takes.needsStatus)
  try harness.reamp(#""state": "failed""#)
  #expect(harness.takes.reamp == .finished(peak: 0.5, destination: destination))
}

@MainActor
@Test func aReampWhoseSaveIsCancelledIsDiscarded() throws {
  let harness = try TakeHarness()
  harness.takes.startReamp(try harness.file("Take.wav"))
  let output = try #require(harness.output)
  try Data("rendered".utf8).write(to: output)
  try harness.reamp(#""state": "done", "peak": 2"#)
  #expect(harness.takes.reamp == .finished(peak: 2, destination: nil))
  #expect(!FileManager.default.fileExists(atPath: output.path))
  #expect(
    harness.takes.reampMessage?.text
      == "Not saved · peak +6.0 dBFS, above full scale (not clipped)")
}

@MainActor
@Test func cancellingAReampRemovesItsOutput() throws {
  let harness = try TakeHarness()
  harness.takes.cancelReamp()
  #expect(harness.requests.isEmpty)
  harness.takes.startReamp(try harness.file("Take.wav"))
  let output = try #require(harness.output)
  try Data("partial".utf8).write(to: output)
  harness.takes.cancelReamp()
  #expect(harness.requests.last?["action"] as? String == "cancel")
  #expect(harness.requests.last?["op"] as? String == "reamp")
  #expect(harness.takes.needsStatus)
  try harness.reamp(#""state": "rendering", "progress": 0.5"#)
  if case .cancelling = harness.takes.reamp {} else { Issue.record("expected cancelling") }
  try harness.reamp(#""state": "cancelled""#)
  #expect(harness.takes.reamp == .cancelled)
  #expect(harness.takes.reampMessage == .init(text: "Reamp cancelled", isProblem: false))
  #expect(!FileManager.default.fileExists(atPath: output.path))

  harness.takes.startReamp(try harness.file("Other.wav"))
  let second = try #require(harness.output)
  try Data("done".utf8).write(to: second)
  harness.takes.cancelReamp()
  try harness.reamp(#""state": "done", "peak": 0.5"#)
  #expect(harness.takes.reamp == .cancelled)
  #expect(harness.suggestedNames.isEmpty)
  #expect(!FileManager.default.fileExists(atPath: second.path))
}

@MainActor
@Test func failedReampsExplainThemselvesAndRemoveTheirOutput() throws {
  let harness = try TakeHarness()
  harness.takes.startReamp(try harness.file("Take.wav"))
  let output = try #require(harness.output)
  try Data("partial".utf8).write(to: output)
  try harness.reamp(#""state": "failed", "error": "the input is not a WAV file""#)
  #expect(harness.takes.reamp == .failed("Reamp failed: the input is not a WAV file."))
  #expect(harness.takes.reampMessage?.isProblem == true)
  #expect(!FileManager.default.fileExists(atPath: output.path))

  harness.takes.startReamp(try harness.file("Again.wav"))
  try harness.apply("")
  #expect(harness.takes.reamp == .failed("Reamp failed: the audio engine stopped rendering."))

  harness.rejection = "WAV file Rejected.wav: is not a RIFF WAVE file"
  harness.takes.startReamp(try harness.file("Rejected.wav"))
  #expect(harness.reported == ["WAV file Rejected.wav: is not a RIFF WAVE file"])
  #expect(
    harness.takes.reamp == .failed("Reamp failed: WAV file Rejected.wav: is not a RIFF WAVE file."))
  #expect(harness.takes.reampMessage?.isProblem == true)
  harness.rejection = nil
  harness.takes.startReamp(harness.root.appendingPathComponent("Missing.wav"))
  #expect(harness.reported.last == "Missing.wav no longer exists.")
  #expect(harness.takes.reamp == .failed("Reamp failed: Missing.wav no longer exists."))
  #expect(try FileManager.default.contentsOfDirectory(atPath: harness.temporary.path).isEmpty)

  harness.takes.startReamp(try harness.file("Unsaved.wav"))
  let unsaved = try #require(harness.output)
  try Data("rendered".utf8).write(to: unsaved)
  harness.destination = harness.root.appendingPathComponent("missing/folder/Out.wav")
  try harness.reamp(#""state": "done", "peak": 0.5"#)
  if case .failed(let message) = harness.takes.reamp {
    #expect(message.hasPrefix("Could not save the reamp: "))
  } else {
    Issue.record("expected a failed save")
  }
  #expect(!FileManager.default.fileExists(atPath: unsaved.path))
}

@Test func peaksReadInDecibelsFullScale() {
  #expect(TakeController.peakText(1) == "0.0 dBFS")
  #expect(TakeController.peakText(0.5) == "−6.0 dBFS")
  #expect(TakeController.peakText(2) == "+6.0 dBFS")
  #expect(TakeController.peakText(0) == "−∞ dBFS")
  #expect(TakeController.peakText(.infinity) == "−∞ dBFS")
}

@MainActor
@Test func theSpinnerShowsOnlyWhileARigIsBeingPrepared() throws {
  let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
  defer { try? FileManager.default.removeItem(at: root) }
  let audio = AudioController(
    amps: AmpCatalog(directory: root), preFX: PedalCatalog(directory: root),
    postFX: PedalCatalog(position: .post, directory: root),
    cabinets: CabinetCatalog(directory: root), specialFX: SpecialFXCatalog(directory: root),
    defaults: MemoryDefaults(), takeDirectory: root.appendingPathComponent("Takes"),
    takePrompts: TakeController.Prompts(chooseInput: { nil }, saveOutput: { _ in nil }))
  defer { audio.stop() }
  #expect(!audio.isPreparingRig && audio.preparationError == nil)
  func candidate(_ state: String, error: String = "") throws {
    audio.apply(
      status: try decodeStatus(
        #""candidate": {"generation": 2, "activeGeneration": 1, "state": "\#(state)", "error": "\#(error)"}"#
      ))
  }
  for state in ["queued", "preparing", "waiting"] {
    try candidate(state)
    #expect(audio.isPreparingRig && audio.preparationError == nil)
  }
  for state in ["ready", "idle", "cancelled"] {
    try candidate(state)
    #expect(!audio.isPreparingRig)
  }
  try candidate("failed", error: "The amp could not load")
  #expect(!audio.isPreparingRig && audio.preparationError == "The amp could not load")
  try candidate("preparing", error: "The amp could not load")
  #expect(!audio.isPreparingRig)
  audio.apply(status: try decodeStatus())
  #expect(!audio.isPreparingRig && audio.preparationError == nil)
}
