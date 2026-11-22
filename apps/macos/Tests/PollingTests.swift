import Combine
import Foundation
import Testing

@testable import VesperaUI

private func status(_ extra: String = "", beat: Double = 0) throws -> AudioStatus {
  try JSONDecoder().decode(
    AudioStatus.self,
    from: Data(
      """
      {"state": "running", "recovery": "none", "lastError": 0, "blocks": 1, "beat": \(beat),
       "input": [0, 0], "output": [0, 0]\(extra.isEmpty ? "" : ", \(extra)")}
      """.utf8))
}

private func candidate(_ state: String, error: String = "") -> String {
  #""candidate": {"generation": 2, "activeGeneration": 1, "state": "\#(state)", "error": "\#(error)"}"#
}

@MainActor
private func emptyController(_ root: URL) -> AudioController {
  AudioController(
    amps: AmpCatalog(directory: root), preFX: PedalCatalog(directory: root),
    postFX: PedalCatalog(position: .post, directory: root),
    cabinets: CabinetCatalog(directory: root), specialFX: SpecialFXCatalog(directory: root),
    defaults: MemoryDefaults(), takeDirectory: root.appendingPathComponent("Takes"),
    takePrompts: TakeController.Prompts(chooseInput: { nil }, saveOutput: { _ in nil }))
}

@Test func pollingIsFastOnlyWhileSomethingNeedsIt() {
  for (running, pending, needed, cadence) in [
    (false, false, false, PollCadence.idle), (false, true, false, .idle),
    (false, false, true, .running), (false, true, true, .running),
    (true, false, false, .running), (true, false, true, .running),
    (true, true, false, .switching), (true, true, true, .switching),
  ] {
    #expect(
      PollCadence.required(audioRunning: running, switchPending: pending, needsStatus: needed)
        == cadence, "running \(running), pending \(pending), needed \(needed)")
  }
  #expect(PollCadence.running.interval == 1.0 / 30)
  #expect(PollCadence.idle.interval == 1.0 / 4)
  #expect(PollCadence.switching.interval <= 0.005)
}

@MainActor
@Test func stoppedAudioPollsSlowlyAndNothingStartsAfterShutdown() throws {
  let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
  defer { try? FileManager.default.removeItem(at: root) }
  let audio = emptyController(root)
  #expect(audio.state != .running)
  #expect(audio.pollCadence == .idle)
  audio.apply(status: try status(candidate("preparing")))
  #expect(audio.isPreparingRig && audio.pollCadence == .idle)
  audio.apply(status: try status())
  audio.shutdown()
  #expect(audio.pollCadence == nil)
  let before = audio.state
  audio.start()
  #expect(audio.state == before && audio.pollCadence == nil)
}

@MainActor
@Test func statusPublishesOnlyWhatChanged() throws {
  let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
  defer { try? FileManager.default.removeItem(at: root) }
  let audio = emptyController(root)
  defer { audio.stop() }
  var changes = 0
  let observer = audio.objectWillChange.sink { changes += 1 }
  defer { observer.cancel() }

  audio.apply(status: try status(beat: 0.5))
  audio.apply(status: try status(beat: 1.5))
  #expect(changes == 0 && !audio.beatPulse)
  audio.apply(status: try status(beat: 2.05))
  #expect(changes == 1 && audio.beatPulse)
  audio.apply(status: try status(beat: 2.1))
  #expect(changes == 1)

  audio.apply(status: try status(candidate("queued"), beat: 2.1))
  #expect(changes == 2 && audio.isPreparingRig)
  audio.apply(status: try status(candidate("preparing"), beat: 2.1))
  #expect(changes == 2 && audio.isPreparingRig)
  audio.apply(status: try status(candidate("failed", error: "Bad rig"), beat: 2.1))
  #expect(changes == 3 && audio.preparationError == "Bad rig")
  audio.apply(status: try status(candidate("failed", error: "Bad rig"), beat: 2.1))
  #expect(changes == 3)
}

@MainActor
@Test func metersPublishOnlyChangesAndSettleAtTheirFloor() {
  let meters = MeterLevels()
  var changes = 0
  let observer = meters.objectWillChange.sink { changes += 1 }
  defer { observer.cancel() }

  meters.update(input: [0, 0], output: [0, 0], at: 10)
  meters.reset()
  #expect(changes == 0)

  meters.update(input: [0.5, 0], output: [0, 0], at: 10)
  #expect(meters.input == [0.5, 0] && changes == 1)
  meters.update(input: [0, 0], output: [0, 0], at: 10 + 1 / 30)
  #expect(abs(meters.input[0] - 0.4) < 1e-6)
  meters.update(input: [0, 0], output: [0, 0], at: 10 + 2 / 30)
  #expect(abs(meters.input[0] - 0.32) < 1e-5)
  // Polling faster does not make the meter fall faster.
  for step in 1...6 {
    meters.update(input: [0, 0], output: [0, 0], at: 10 + 2 / 30 + Double(step) / 180)
  }
  #expect(abs(meters.input[0] - 0.256) < 1e-4)
  meters.update(input: [0, 0], output: [0.9, 0.2], at: 11)
  #expect(meters.output == [0.9, 0.2])

  meters.update(input: [0, 0], output: [0, 0], at: 20)
  #expect(meters.input == [0, 0] && meters.output == [0, 0])
  let settled = changes
  meters.update(input: [MeterLevels.floor / 2, 0], output: [0, 0], at: 20 + 1 / 30)
  #expect(meters.input == [0, 0] && changes == settled)
  meters.update(input: [MeterLevels.floor, 0], output: [0, 0], at: 20 + 2 / 30)
  #expect(meters.input == [MeterLevels.floor, 0])
  meters.reset()
  #expect(meters.input == [0, 0])
}
