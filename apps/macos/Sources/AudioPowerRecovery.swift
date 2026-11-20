import AppKit

@MainActor
final class AudioPowerRecovery {
  private enum Phase {
    case awake
    case sleepingInactive
    case sleepingActive
    case waiting(deadline: TimeInterval, nextAttempt: TimeInterval, lastError: String?)
  }

  private let notifications: NotificationCenter
  private let now: () -> TimeInterval
  private let isRunning: () -> Bool
  private let suspend: () throws -> Void
  private let resume: () throws -> Void
  private let onWaiting: () -> Void
  private let onIdleWake: () -> Void
  private let onFailure: (String) -> Void
  private var phase = Phase.awake
  private var observers: [NSObjectProtocol] = []

  init(
    notifications: NotificationCenter = NSWorkspace.shared.notificationCenter,
    now: @escaping () -> TimeInterval = { ProcessInfo.processInfo.systemUptime },
    isRunning: @escaping () -> Bool,
    suspend: @escaping () throws -> Void,
    resume: @escaping () throws -> Void,
    onWaiting: @escaping () -> Void,
    onIdleWake: @escaping () -> Void = {},
    onFailure: @escaping (String) -> Void
  ) {
    self.notifications = notifications
    self.now = now
    self.isRunning = isRunning
    self.suspend = suspend
    self.resume = resume
    self.onWaiting = onWaiting
    self.onIdleWake = onIdleWake
    self.onFailure = onFailure
    observers = [
      notifications.addObserver(
        forName: NSWorkspace.willSleepNotification, object: nil, queue: .main
      ) { [weak self] _ in
        MainActor.assumeIsolated { self?.willSleep() }
      },
      notifications.addObserver(
        forName: NSWorkspace.didWakeNotification, object: nil, queue: .main
      ) { [weak self] _ in
        MainActor.assumeIsolated { self?.didWake() }
      },
    ]
  }

  isolated deinit {
    for observer in observers { notifications.removeObserver(observer) }
  }

  var isSleeping: Bool {
    switch phase {
    case .sleepingInactive, .sleepingActive: true
    default: false
    }
  }

  var isPending: Bool {
    if case .awake = phase { return false }
    return true
  }

  func cancel() {
    phase = isSleeping ? .sleepingInactive : .awake
  }

  func shutdown() {
    cancel()
    for observer in observers { notifications.removeObserver(observer) }
    observers.removeAll()
  }

  private func willSleep() {
    guard !isSleeping else { return }
    let shouldResume: Bool
    if case .waiting = phase {
      shouldResume = true
    } else {
      shouldResume = isRunning()
    }
    phase = .sleepingInactive
    guard shouldResume else { return }
    do {
      try suspend()
      phase = .sleepingActive
    } catch {
      onFailure("Could not suspend audio for sleep: \(error.localizedDescription)")
    }
  }

  private func didWake() {
    switch phase {
    case .sleepingActive:
      let time = now()
      phase = .waiting(deadline: time + 10, nextAttempt: time, lastError: nil)
      onWaiting()
      poll()
    case .sleepingInactive:
      phase = .awake
      onIdleWake()
    default:
      break
    }
  }

  func poll() {
    guard case .waiting(let deadline, let nextAttempt, let lastError) = phase else { return }
    let time = now()
    if time >= deadline {
      phase = .awake
      onFailure(
        "Could not resume audio after waking: \(lastError ?? "The selected audio devices are unavailable.") Check Audio Settings and restart."
      )
      return
    }
    guard time >= nextAttempt else { return }
    phase = .waiting(deadline: deadline, nextAttempt: time + 0.5, lastError: lastError)
    do {
      try resume()
      phase = .awake
    } catch {
      phase = .waiting(
        deadline: deadline, nextAttempt: time + 0.5, lastError: error.localizedDescription)
    }
  }
}
