import CoreGraphics
import Foundation

/// How one top-row global readout shows and accepts a value: its bounds, drag step, shown
/// decimals and unit.
struct GlobalReadout: Equatable {
  let range: ClosedRange<Double>
  let step: Double
  let decimals: Int
  let unit: String

  /// The text shown for `value`. A number that rounds to zero carries no sign.
  func text(_ value: Double) -> String {
    var number = String(format: "%.*f", decimals, value)
    if number.hasPrefix("-"), number.dropFirst().allSatisfy({ $0 == "0" || $0 == "." }) {
      number.removeFirst()
    }
    return "\(number) \(unit)"
  }

  /// `proposed` within `range`, on a multiple of `step`, with negative zero made positive.
  /// Non-finite values are rejected.
  func clamped(_ proposed: Double) -> Double? {
    guard proposed.isFinite else { return nil }
    let snapped = (min(range.upperBound, max(range.lowerBound, proposed)) / step).rounded() * step
    let normalized = (snapped * 1e9).rounded() / 1e9
    return normalized == 0 ? 0 : normalized
  }

  /// The value typed in `text`, in this readout's units. A trailing unit is optional and
  /// commas read as decimal points. Numbers are clamped to the range and snapped to the step;
  /// text without a finite number reads as nil, leaving the current value unchanged.
  func value(from text: String) -> Double? {
    var body = text.trimmingCharacters(in: .whitespacesAndNewlines)
    if let suffix = body.range(of: unit, options: [.anchored, .backwards, .caseInsensitive]) {
      body = String(body[..<suffix.lowerBound]).trimmingCharacters(in: .whitespacesAndNewlines)
    }
    guard let parsed = Double(body.replacingOccurrences(of: ",", with: ".")) else { return nil }
    return clamped(parsed)
  }
}

extension GlobalReadout {
  static let inputGain = GlobalReadout(range: -60...24, step: 0.1, decimals: 1, unit: "dB")
  static let gateThreshold = GlobalReadout(range: -120...0, step: 1, decimals: 0, unit: "dB")
  static let transpose = GlobalReadout(range: -12...12, step: 1, decimals: 0, unit: "st")
  static let doublerDelay = GlobalReadout(range: 1...40, step: 0.01, decimals: 2, unit: "ms")
  static let outputGain = GlobalReadout(range: -60...24, step: 0.1, decimals: 1, unit: "dB")
}

/// The text-entry state of a readout: the draft being typed and whether the field is open.
///
/// A newly opened field has not yet received keyboard focus, so a focus loss reported before
/// focus arrives is not a blur and must not commit and close the field.
struct GlobalReadoutEditor: Equatable {
  enum Phase: Equatable { case closed, awaitingFocus, focused }

  var draft = ""
  private(set) var phase = Phase.closed

  var isEditing: Bool { phase != .closed }

  mutating func begin(_ value: Double, readout: GlobalReadout) {
    draft = readout.text(value)
    phase = .awaitingFocus
  }

  /// Records the field's focus and returns true when the change is a blur that should commit.
  mutating func focusChanged(to focused: Bool) -> Bool {
    switch (phase, focused) {
    case (.awaitingFocus, true):
      phase = .focused
      return false
    case (.focused, false):
      return true
    default:
      return false
    }
  }

  mutating func cancel() {
    draft = ""
    phase = .closed
  }

  /// Ends editing and returns the typed value, or nil when the text is not a finite number.
  mutating func finish(readout: GlobalReadout) -> Double? {
    let typed = draft
    draft = ""
    phase = .closed
    return readout.value(from: typed)
  }
}

/// One pointer press on a readout, which is either a click that opens text entry or, once it
/// travels past `dragThreshold`, a vertical drag that adjusts the value from where it started.
struct ReadoutPointer: Equatable {
  enum Phase: Equatable {
    case idle
    case pressed(from: Double)
    case dragging(from: Double)
  }

  static let dragThreshold: CGFloat = 4

  private(set) var phase = Phase.idle

  /// Tracks the press and returns the value it adjusts from once it has become a drag.
  mutating func moved(by translation: CGSize, current value: Double) -> Double? {
    switch phase {
    case .idle:
      phase = .pressed(from: value)
      return moved(by: translation, current: value)
    case .pressed(let start):
      guard hypot(translation.width, translation.height) >= Self.dragThreshold else { return nil }
      phase = .dragging(from: start)
      return start
    case .dragging(let start):
      return start
    }
  }

  /// Ends the press and returns true when it was a click rather than a drag.
  mutating func ended() -> Bool {
    defer { phase = .idle }
    if case .dragging = phase { return false }
    return true
  }
}
