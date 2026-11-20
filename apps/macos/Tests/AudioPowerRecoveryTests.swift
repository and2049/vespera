import AppKit
import Testing

@testable import VesperaUI

@MainActor
private final class PowerFixture {
  let notifications = NotificationCenter()
  var time: TimeInterval = 100
  var running = true
  var available = true
  var stopFails = false
  var calls: [String] = []
  var failures: [String] = []
  var idleWakes = 0
  lazy var recovery = AudioPowerRecovery(
    notifications: notifications,
    now: { [unowned self] in self.time },
    isRunning: { [unowned self] in self.running },
    suspend: { [unowned self] in
      self.calls.append("stop")
      if self.stopFails { throw AmpError.message("Stream could not quiesce") }
      self.running = false
    },
    resume: { [unowned self] in
      self.calls.append("start")
      if !self.available { throw AmpError.message("Selected device unavailable") }
      self.running = true
    },
    onWaiting: { [unowned self] in self.calls.append("waiting") },
    onIdleWake: { [unowned self] in self.idleWakes += 1 },
    onFailure: { [unowned self] in self.failures.append($0) })

  func sleep() {
    _ = recovery
    notifications.post(name: NSWorkspace.willSleepNotification, object: nil)
  }

  func wake() {
    notifications.post(name: NSWorkspace.didWakeNotification, object: nil)
  }
}

@Test @MainActor func audioResumesAfterSystemSleep() {
  let fixture = PowerFixture()
  defer { fixture.recovery.shutdown() }
  fixture.sleep()
  #expect(fixture.calls == ["stop"])
  #expect(!fixture.running)
  #expect(fixture.recovery.isSleeping)
  fixture.recovery.poll()
  #expect(fixture.calls == ["stop"])
  fixture.wake()
  #expect(fixture.calls == ["stop", "waiting", "start"])
  #expect(fixture.running)
  #expect(!fixture.recovery.isPending)
  #expect(fixture.failures.isEmpty)
}

@Test @MainActor func audioWakeWaitsForSelectedDevice() {
  let fixture = PowerFixture()
  defer { fixture.recovery.shutdown() }
  fixture.sleep()
  fixture.available = false
  fixture.wake()
  #expect(fixture.recovery.isPending)
  fixture.time += 0.25
  fixture.recovery.poll()
  #expect(fixture.calls.filter { $0 == "start" }.count == 1)
  fixture.time += 0.25
  fixture.recovery.poll()
  #expect(fixture.calls.filter { $0 == "start" }.count == 2)
  fixture.available = true
  fixture.time += 0.5
  fixture.recovery.poll()
  #expect(fixture.running)
  #expect(!fixture.recovery.isPending)
  #expect(fixture.failures.isEmpty)
}

@Test @MainActor func audioWakeRecoveryHasDeadline() {
  let fixture = PowerFixture()
  defer { fixture.recovery.shutdown() }
  fixture.sleep()
  fixture.available = false
  fixture.wake()
  fixture.time += 10
  fixture.recovery.poll()
  #expect(!fixture.recovery.isPending)
  #expect(fixture.failures.count == 1)
  #expect(fixture.failures.first?.contains("Selected device unavailable") == true)
  fixture.available = true
  fixture.time += 1
  fixture.recovery.poll()
  fixture.wake()
  #expect(!fixture.running)
  #expect(fixture.calls.filter { $0 == "start" }.count == 1)
}

@Test @MainActor func audioStoppedBeforeSleepStaysStopped() {
  let fixture = PowerFixture()
  defer { fixture.recovery.shutdown() }
  fixture.running = false
  fixture.sleep()
  fixture.wake()
  #expect(fixture.calls.isEmpty)
  #expect(fixture.idleWakes == 1)
  #expect(!fixture.recovery.isPending)
}

@Test @MainActor func audioManualStopCancelsWakeRecovery() {
  for cancelWhileSleeping in [true, false] {
    let fixture = PowerFixture()
    defer { fixture.recovery.shutdown() }
    fixture.sleep()
    fixture.available = false
    if !cancelWhileSleeping { fixture.wake() }
    fixture.recovery.cancel()
    let calls = fixture.calls
    fixture.available = true
    fixture.wake()
    fixture.time += 1
    fixture.recovery.poll()
    #expect(fixture.calls == calls)
    #expect(!fixture.running)
  }
}

@Test @MainActor func audioPowerNotificationsAreIdempotent() {
  let fixture = PowerFixture()
  defer { fixture.recovery.shutdown() }
  fixture.sleep()
  fixture.sleep()
  fixture.available = false
  fixture.wake()
  fixture.wake()
  #expect(fixture.calls == ["stop", "waiting", "start"])
  fixture.sleep()
  fixture.available = true
  fixture.wake()
  #expect(fixture.calls == ["stop", "waiting", "start", "stop", "waiting", "start"])
  #expect(fixture.running)
}

@Test @MainActor func audioFailedSuspendDoesNotRestart() {
  let fixture = PowerFixture()
  defer { fixture.recovery.shutdown() }
  fixture.stopFails = true
  fixture.sleep()
  fixture.wake()
  #expect(fixture.calls == ["stop"])
  #expect(fixture.failures.count == 1)
  #expect(fixture.failures.first?.contains("quiesce") == true)
}

@Test @MainActor func audioShutdownRemovesPowerObservers() {
  let fixture = PowerFixture()
  fixture.sleep()
  fixture.recovery.shutdown()
  fixture.wake()
  fixture.sleep()
  fixture.time += 1
  fixture.recovery.poll()
  #expect(fixture.calls == ["stop"])
}
