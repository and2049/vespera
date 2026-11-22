import Combine
import Foundation

/// What the audio side reports about recording the input; absent from the status of engines
/// that cannot record, which is the same as idle.
struct RecordingStatus: Decodable, Equatable {
  /// A state this version does not know decodes as `.unknown` instead of failing.
  enum State: Equatable, Decodable {
    case idle
    case recording
    case finalizing
    case failed
    case unknown(String)

    init(from decoder: Decoder) throws {
      let raw = try decoder.singleValueContainer().decode(String.self)
      switch raw {
      case "idle": self = .idle
      case "recording": self = .recording
      case "finalizing": self = .finalizing
      case "failed": self = .failed
      default: self = .unknown(raw)
      }
    }
  }

  let state: State
  /// Present while recording or finalizing, and kept for the last finished take while idle.
  var path: String? = nil
  var frames: UInt64? = nil
  var sampleRate: UInt32? = nil
  var channels: UInt32? = nil
  var droppedFrames: UInt64? = nil
  var error: String? = nil
  /// "full" while idle after a take that ended because its file reached the WAV size limit.
  var stopReason: String? = nil
}

/// What the audio side reports about rendering a file through a rig offline; a finished state
/// stays until the next render starts. Absent from older engines, which is the same as idle.
struct ReampStatus: Decodable, Equatable {
  /// A state this version does not know decodes as `.unknown` instead of failing.
  enum State: Equatable, Decodable {
    case idle
    case rendering
    case done
    case failed
    case cancelled
    case unknown(String)

    init(from decoder: Decoder) throws {
      let raw = try decoder.singleValueContainer().decode(String.self)
      switch raw {
      case "idle": self = .idle
      case "rendering": self = .rendering
      case "done": self = .done
      case "failed": self = .failed
      case "cancelled": self = .cancelled
      default: self = .unknown(raw)
      }
    }
  }

  let state: State
  /// From 0 to 1.
  var progress: Double? = nil
  /// The render's destination, in every state but idle.
  var output: String? = nil
  /// The rendered file's largest absolute sample, linear.
  var peak: Double? = nil
  var error: String? = nil
}

/// A recorded input file and what the engine reported about it.
struct Take: Codable, Equatable {
  let path: String
  var frames: UInt64 = 0
  var sampleRate: UInt32 = 0
  var channels: UInt32 = 0
  var droppedFrames: UInt64 = 0

  var url: URL { URL(fileURLWithPath: path) }
  var duration: Double { sampleRate > 0 ? Double(frames) / Double(sampleRate) : 0 }

  /// Duration, sample rate and channel layout, such as "0:42 · 48 kHz · Mono".
  var summary: String {
    var parts = [TakeController.elapsed(duration)]
    if sampleRate > 0 {
      parts.append(
        sampleRate % 1000 == 0
          ? "\(sampleRate / 1000) kHz" : String(format: "%.1f kHz", Double(sampleRate) / 1000))
    }
    switch channels {
    case 0: break
    case 1: parts.append("Mono")
    case 2: parts.append("Stereo")
    default: parts.append("\(channels) channels")
    }
    return parts.joined(separator: " · ")
  }

  var dropoutWarning: String? {
    guard droppedFrames > 0 else { return nil }
    return droppedFrames == 1
      ? "1 frame was dropped while recording"
      : "\(droppedFrames) frames were dropped while recording"
  }
}

/// Records the live input into takes and renders takes or other files through the current rig.
/// The `AudioController` that owns it feeds it each decoded status and supplies, through the
/// hooks, the bridge, whether audio runs, and the rig and tempo a render uses.
@MainActor
final class TakeController: ObservableObject {
  enum Recording: Equatable {
    case idle
    /// The take being written, as of the latest status.
    case recording(Take)
    /// Stopped; the engine is still completing the file.
    case finalizing(Take)
    case failed(String)
  }

  /// A render's source and the temporary file the engine writes.
  struct ReampJob: Equatable {
    let source: URL
    let output: URL
  }

  enum Reamp: Equatable {
    case idle
    case rendering(ReampJob, progress: Double)
    /// Cancel was requested; the engine has not confirmed it yet.
    case cancelling(ReampJob)
    /// Rendered; the user is choosing where to save it.
    case saving(ReampJob, peak: Double)
    /// Rendered, then saved to `destination`, or discarded when that is nil.
    case finished(peak: Double, destination: URL?)
    case failed(String)
    case cancelled
  }

  /// The record button: starting is possible only while audio runs.
  enum RecordControl: Equatable {
    case start(enabled: Bool)
    case stop
    case finalizing
  }

  /// Questions put to the user, replaceable for tests.
  struct Prompts {
    /// A WAV file to render; nil cancels.
    var chooseInput: () -> URL?
    /// Where to save a render, starting from a suggested file name; nil discards it.
    var saveOutput: (String) -> URL?
  }

  struct Message: Equatable {
    let text: String
    let isProblem: Bool
  }

  static let lastTakeKey = "takes.lastTake"

  /// `~/Music/vespera/Takes`.
  static var standardDirectory: URL {
    FileManager.default.urls(for: .musicDirectory, in: .userDomainMask)[0]
      .appendingPathComponent("vespera/Takes", isDirectory: true)
  }

  @Published private(set) var recording = Recording.idle
  @Published private(set) var reamp = Reamp.idle
  @Published private(set) var lastTake: Take?

  let directory: URL
  private let temporaryDirectory: URL
  private let defaults: UserDefaults
  private let prompts: Prompts
  private let request: ([String: Any]) throws -> Data
  private let isAudioRunning: () -> Bool
  /// The rig configuration, globals included, that live audio would start with now.
  private let configuration: () throws -> [String: Any]
  private let bpm: () -> Double
  private let blockSize: () -> Int
  private let report: (Error) -> Void
  /// Information for the user that needs no action.
  private let notify: (String) -> Void
  private let now: () -> Date
  private let files = FileManager.default
  private static let decoder = JSONDecoder()

  init(
    directory: URL, temporaryDirectory: URL = FileManager.default.temporaryDirectory,
    defaults: UserDefaults, prompts: Prompts,
    request: @escaping ([String: Any]) throws -> Data,
    isAudioRunning: @escaping () -> Bool,
    configuration: @escaping () throws -> [String: Any],
    bpm: @escaping () -> Double,
    blockSize: @escaping () -> Int,
    report: @escaping (Error) -> Void,
    notify: @escaping (String) -> Void,
    now: @escaping () -> Date = Date.init
  ) {
    self.directory = directory
    self.temporaryDirectory = temporaryDirectory
    self.defaults = defaults
    self.prompts = prompts
    self.request = request
    self.isAudioRunning = isAudioRunning
    self.configuration = configuration
    self.bpm = bpm
    self.blockSize = blockSize
    self.report = report
    self.notify = notify
    self.now = now
    if let data = defaults.data(forKey: Self.lastTakeKey),
      let take = try? JSONDecoder().decode(Take.self, from: data)
    {
      lastTake = take
    }
    refreshLastTake()
  }

  // MARK: Presentation

  var recordControl: RecordControl {
    switch recording {
    case .recording: .stop
    case .finalizing: .finalizing
    case .idle, .failed: .start(enabled: isAudioRunning())
    }
  }

  /// Elapsed recording time while a take is being written.
  var elapsedText: String? {
    switch recording {
    case .recording(let take), .finalizing(let take): Self.elapsed(take.duration)
    case .idle, .failed: nil
    }
  }

  /// Whether the status must be polled even while audio is stopped.
  var needsStatus: Bool { Self.needsStatus(recording: recording, reamp: reamp) }

  nonisolated static func needsStatus(recording: Recording, reamp: Reamp) -> Bool {
    switch (recording, reamp) {
    case (.recording, _), (.finalizing, _), (_, .rendering), (_, .cancelling): true
    default: false
    }
  }

  /// Publishes `needsStatus` as it changes, with the new value.
  var needsStatusPublisher: AnyPublisher<Bool, Never> {
    $recording.combineLatest($reamp).map(Self.needsStatus).removeDuplicates()
      .eraseToAnyPublisher()
  }

  var isReamping: Bool {
    switch reamp {
    case .rendering, .cancelling, .saving: true
    case .idle, .finished, .failed, .cancelled: false
    }
  }

  /// The outcome of the last render, for one line beneath the reamp controls.
  var reampMessage: Message? {
    switch reamp {
    case .idle, .rendering, .cancelling, .saving: return nil
    case .finished(let peak, let destination):
      var text = destination.map { "Saved “\($0.lastPathComponent)”" } ?? "Not saved"
      text += " · peak \(Self.peakText(peak))"
      if peak > 1 { text += ", above full scale (not clipped)" }
      return Message(text: text, isProblem: false)
    case .failed(let message): return Message(text: message, isProblem: true)
    case .cancelled: return Message(text: "Reamp cancelled", isProblem: false)
    }
  }

  /// Seconds as m:ss, or h:mm:ss from one hour.
  nonisolated static func elapsed(_ seconds: Double) -> String {
    let total = seconds.isFinite ? max(0, Int(seconds)) : 0
    let hours = total / 3600
    let minutes = total % 3600 / 60
    return hours > 0
      ? String(format: "%d:%02d:%02d", hours, minutes, total % 60)
      : String(format: "%d:%02d", minutes, total % 60)
  }

  /// A linear peak in dBFS to one decimal, such as "−3.2 dBFS" or "+1.3 dBFS".
  nonisolated static func peakText(_ linear: Double) -> String {
    guard linear > 0, linear.isFinite else { return "−∞ dBFS" }
    let decibels = (20 * log10(linear) * 10).rounded() / 10
    let magnitude = String(format: "%.1f", abs(decibels))
    return decibels > 0 ? "+\(magnitude) dBFS" : decibels < 0 ? "−\(magnitude) dBFS" : "0.0 dBFS"
  }

  /// "Take YYYY-MM-DD HH.mm.ss" in local time.
  nonisolated static func takeName(_ date: Date) -> String {
    let formatter = DateFormatter()
    formatter.locale = Locale(identifier: "en_US_POSIX")
    formatter.calendar = Calendar(identifier: .gregorian)
    formatter.dateFormat = "yyyy-MM-dd HH.mm.ss"
    return "Take \(formatter.string(from: date))"
  }

  // MARK: Recording

  func toggleRecording() {
    switch recording {
    case .recording: stopRecording()
    case .finalizing: break
    case .idle, .failed: startRecording()
    }
  }

  func startRecording() {
    switch recording {
    case .recording, .finalizing: return
    case .idle, .failed: break
    }
    do {
      guard isAudioRunning() else { throw AmpError.message("Start audio to record a take.") }
      try files.createDirectory(at: directory, withIntermediateDirectories: true)
      let url = nextTakeURL()
      let reply = try request([
        "op": BridgeOp.record.rawValue, "action": "start", "path": url.path,
      ])
      recording = .recording(Take(path: url.path))
      applyRecording(reply: reply)
    } catch { report(error) }
  }

  func stopRecording() {
    guard case .recording(let take) = recording else { return }
    do {
      let reply = try request(["op": BridgeOp.record.rawValue, "action": "stop"])
      recording = .finalizing(take)
      applyRecording(reply: reply)
    } catch { report(error) }
  }

  /// Record requests reply with the whole audio status.
  private func applyRecording(reply: Data) {
    guard let status = try? Self.decoder.decode(AudioStatus.self, from: reply) else { return }
    apply(recording: status.recording)
  }

  private func nextTakeURL() -> URL {
    let name = Self.takeName(now())
    var url = directory.appendingPathComponent("\(name).wav")
    var counter = 2
    while files.fileExists(atPath: url.path) {
      url = directory.appendingPathComponent("\(name) \(counter).wav")
      counter += 1
    }
    return url
  }

  /// Forgets the last take when its file no longer exists.
  func refreshLastTake() {
    guard let take = lastTake, !files.fileExists(atPath: take.path) else { return }
    setLastTake(nil)
  }

  private func setLastTake(_ take: Take?) {
    lastTake = take
    if let take, let data = try? JSONEncoder().encode(take) {
      defaults.set(data, forKey: Self.lastTakeKey)
    } else {
      defaults.removeObject(forKey: Self.lastTakeKey)
    }
  }

  // MARK: Status

  func apply(_ status: AudioStatus) {
    apply(recording: status.recording)
    apply(reamp: status.reamp)
  }

  private func apply(recording status: RecordingStatus?) {
    let state = status?.state ?? .idle
    switch (recording, state) {
    case (.recording(let take), .recording), (.finalizing(let take), .recording):
      recording = .recording(updated(take, status))
    case (.recording(let take), .finalizing), (.finalizing(let take), .finalizing):
      recording = .finalizing(updated(take, status))
    case (.recording(let take), .idle), (.finalizing(let take), .idle):
      let finished = updated(take, status)
      recording = .idle
      if files.fileExists(atPath: finished.path) { setLastTake(finished) }
      if status?.stopReason == "full" {
        notify("Recording stopped because the take reached the largest size a WAV file can hold.")
      }
    case (.recording, .failed), (.finalizing, .failed):
      let message = Self.sentence(
        "Recording failed: \(status?.error ?? "the take could not be written")")
      recording = .failed(message)
      report(AmpError.message(message))
    default: break
    }
  }

  private func updated(_ take: Take, _ status: RecordingStatus?) -> Take {
    guard let status else { return take }
    return Take(
      path: status.path ?? take.path, frames: status.frames ?? take.frames,
      sampleRate: status.sampleRate ?? take.sampleRate, channels: status.channels ?? take.channels,
      droppedFrames: status.droppedFrames ?? take.droppedFrames)
  }

  private func apply(reamp status: ReampStatus?) {
    let job: ReampJob
    let cancelling: Bool
    switch reamp {
    case .rendering(let current, _):
      job = current
      cancelling = false
    case .cancelling(let current):
      job = current
      cancelling = true
    case .idle, .saving, .finished, .failed, .cancelled: return
    }
    guard let status, status.state != .idle else {
      discard(job)
      reamp = .failed("Reamp failed: the audio engine stopped rendering.")
      return
    }
    // The engine keeps a finished render's state until the next start, so a status that names
    // another output describes an earlier render, not this one.
    guard let output = status.output, URL(fileURLWithPath: output).path == job.output.path else {
      return
    }
    switch status.state {
    case .rendering:
      if !cancelling {
        reamp = .rendering(job, progress: min(1, max(0, status.progress ?? 0)))
      }
    case .done:
      if cancelling {
        discard(job)
        reamp = .cancelled
      } else {
        deliver(job, peak: status.peak ?? 0)
      }
    case .failed:
      discard(job)
      reamp = .failed(
        Self.sentence("Reamp failed: \(status.error ?? "the file could not be rendered")"))
    case .cancelled:
      discard(job)
      reamp = .cancelled
    case .idle, .unknown: break
    }
  }

  /// `text` ending in exactly one full stop, whether or not the engine's message had one.
  private static func sentence(_ text: String) -> String {
    var text = text
    while text.hasSuffix(".") { text.removeLast() }
    return text + "."
  }

  // MARK: Reamping

  func processLastTake() {
    refreshLastTake()
    guard let take = lastTake else {
      report(AmpError.message("The last take no longer exists."))
      return
    }
    startReamp(take.url)
  }

  func chooseAndProcess() {
    guard !isReamping, let url = prompts.chooseInput() else { return }
    startReamp(url)
  }

  /// Renders `source` through the current rig into a temporary file; once the engine reports it
  /// done, the user chooses where to keep it.
  func startReamp(_ source: URL) {
    guard !isReamping else { return }
    let job = ReampJob(
      source: source,
      output: temporaryDirectory.appendingPathComponent("vespera reamp \(UUID().uuidString).wav"))
    do {
      guard files.fileExists(atPath: source.path) else {
        throw AmpError.message("\(source.lastPathComponent) no longer exists.")
      }
      let sidecar = Self.sidecar(source)
      let reply = try request([
        "op": BridgeOp.reamp.rawValue, "action": "start", "input": source.path,
        "output": job.output.path, "configuration": try configuration(),
        "bpm": sidecar.bpm ?? bpm(), "blockSize": sidecar.blockSize ?? blockSize(),
      ])
      reamp = .rendering(job, progress: 0)
      applyReamp(reply: reply)
    } catch {
      discard(job)
      report(error)
      reamp = .failed(Self.sentence("Reamp failed: \(error.localizedDescription)"))
    }
  }

  func cancelReamp() {
    guard case .rendering(let job, _) = reamp else { return }
    do {
      let reply = try request(["op": BridgeOp.reamp.rawValue, "action": "cancel"])
      reamp = .cancelling(job)
      applyReamp(reply: reply)
    } catch { report(error) }
  }

  /// Reamp requests reply with the reamp status alone.
  private func applyReamp(reply: Data) {
    guard let status = try? Self.decoder.decode(ReampStatus.self, from: reply) else { return }
    apply(reamp: status)
  }

  /// The tempo and block size a take was recorded with, from the JSON sidecar the engine writes
  /// beside it; nil for files without one or values the engine would not accept.
  nonisolated static func sidecar(_ source: URL) -> (bpm: Double?, blockSize: Int?) {
    let url = source.deletingPathExtension().appendingPathExtension("json")
    guard let data = try? Data(contentsOf: url),
      let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
    else { return (nil, nil) }
    let bpm = (object["bpm"] as? NSNumber)?.doubleValue
    let blockSize = (object["blockSize"] as? NSNumber)?.doubleValue
    return (
      bpm.flatMap { $0.isFinite && TempoSettings(bpm: $0).isValid ? $0 : nil },
      blockSize.flatMap { (1...4096).contains($0) && $0.rounded() == $0 ? Int($0) : nil }
    )
  }

  private func deliver(_ job: ReampJob, peak: Double) {
    reamp = .saving(job, peak: peak)
    let name = "\(job.source.deletingPathExtension().lastPathComponent) reamp.wav"
    guard let destination = prompts.saveOutput(name) else {
      discard(job)
      reamp = .finished(peak: peak, destination: nil)
      return
    }
    do {
      if files.fileExists(atPath: destination.path) { try files.removeItem(at: destination) }
      try files.moveItem(at: job.output, to: destination)
      reamp = .finished(peak: peak, destination: destination)
    } catch {
      discard(job)
      report(error)
      reamp = .failed("Could not save the reamp: \(error.localizedDescription)")
    }
  }

  /// Removes a render's output and the partial file the engine writes beside it.
  private func discard(_ job: ReampJob) {
    try? files.removeItem(at: job.output)
    try? files.removeItem(
      at: job.output.deletingLastPathComponent()
        .appendingPathComponent(".\(job.output.lastPathComponent).partial"))
  }
}
