import Foundation
import Testing

@testable import VesperaUI

@Test func globalReadoutsShowDefaultsAndNoSignedZero() {
  let settings = GlobalControlSettings()
  #expect(GlobalReadout.inputGain.text(settings.inputGain) == "0.0 dB")
  #expect(GlobalReadout.gateThreshold.text(settings.gateThreshold) == "-80 dB")
  #expect(GlobalReadout.transpose.text(settings.transpose) == "0 st")
  #expect(GlobalReadout.doublerDelay.text(settings.doublerTime) == "6.00 ms")
  #expect(GlobalReadout.outputGain.text(settings.outputGain) == "0.0 dB")

  #expect(GlobalReadout.outputGain.text(-0.0) == "0.0 dB")
  #expect(GlobalReadout.outputGain.text(-0.04) == "0.0 dB")
  #expect(GlobalReadout.outputGain.text(0.04) == "0.0 dB")
  #expect(GlobalReadout.outputGain.text(-0.06) == "-0.1 dB")
  #expect(GlobalReadout.gateThreshold.text(-0.4) == "0 dB")
  #expect(GlobalReadout.gateThreshold.text(-0.6) == "-1 dB")
  #expect(GlobalReadout.doublerDelay.text(-0.004) == "0.00 ms")
  #expect(GlobalReadout.doublerDelay.text(-0.006) == "-0.01 ms")
}

@Test func globalReadoutsParseExactTypedValues() {
  #expect(GlobalReadout.inputGain.value(from: "3.4") == 3.4)
  #expect(GlobalReadout.inputGain.value(from: " -12.7 dB ") == -12.7)
  #expect(GlobalReadout.gateThreshold.value(from: "-55") == -55)
  #expect(GlobalReadout.gateThreshold.value(from: "-55 dB") == -55)
  #expect(GlobalReadout.transpose.value(from: "-2 st") == -2)
  #expect(GlobalReadout.doublerDelay.value(from: "6.00 ms") == 6)
  #expect(GlobalReadout.doublerDelay.value(from: "6,5 MS") == 6.5)
  #expect(GlobalReadout.outputGain.value(from: "+0.0 DB") == 0)
}

@Test func globalReadoutsClampToTheirBounds() {
  #expect(GlobalReadout.inputGain.value(from: "100") == 24)
  #expect(GlobalReadout.inputGain.value(from: "-100 dB") == -60)
  #expect(GlobalReadout.gateThreshold.value(from: "3") == 0)
  #expect(GlobalReadout.gateThreshold.value(from: "-200") == -120)
  #expect(GlobalReadout.transpose.value(from: "20") == 12)
  #expect(GlobalReadout.transpose.value(from: "-20 st") == -12)
  #expect(GlobalReadout.doublerDelay.value(from: "0.5") == 1)
  #expect(GlobalReadout.doublerDelay.value(from: "50 ms") == 40)
}

@Test func globalReadoutsSnapToTheirStep() {
  #expect(GlobalReadout.inputGain.value(from: "3.44") == 3.4)
  #expect(GlobalReadout.outputGain.value(from: "3.46") == 3.5)
  #expect(GlobalReadout.gateThreshold.value(from: "-54.4") == -54)
  #expect(GlobalReadout.gateThreshold.value(from: "-54.6") == -55)
  let doubled = GlobalReadout.doublerDelay.value(from: "6.789 ms") ?? .nan
  #expect(abs(doubled - 6.79) < 1e-9)
}

@Test func globalReadoutsRejectInvalidAndNonFiniteText() {
  for text in ["", " ", "dB", "ms", "abc", "6 ms dB", "nan", "-nan", "inf", "-inf", "1e999"] {
    #expect(GlobalReadout.inputGain.value(from: text) == nil)
    #expect(GlobalReadout.doublerDelay.value(from: text) == nil)
  }
  #expect(GlobalReadout.outputGain.value(from: "-0.04")?.sign == .plus)
  #expect(GlobalReadout.outputGain.value(from: "-0.04") == 0)
}

@Test func globalReadoutDragToZeroNeverDisplaysASign() {
  let dragged = GlobalReadout.outputGain.clamped(-0.049) ?? .nan
  #expect(dragged == 0)
  #expect(dragged.sign == .plus)
  #expect(GlobalReadout.outputGain.text(dragged) == "0.0 dB")

  let gate = GlobalReadout.gateThreshold.clamped(-0.4) ?? .nan
  #expect(gate == 0)
  #expect(GlobalReadout.gateThreshold.text(gate) == "0 dB")

  #expect(GlobalReadout.inputGain.clamped(.infinity) == nil)
  #expect(GlobalReadout.inputGain.clamped(.nan) == nil)
}

@Test func typedGlobalValuesRemainValidForPresets() {
  var settings = GlobalControlSettings()
  settings.inputGain = GlobalReadout.inputGain.value(from: "-12.7 dB") ?? .nan
  settings.gateThreshold = GlobalReadout.gateThreshold.value(from: "-55 dB") ?? .nan
  settings.transpose = GlobalReadout.transpose.value(from: "2.5 st") ?? .nan
  settings.doublerTime = GlobalReadout.doublerDelay.value(from: "6.789 ms") ?? .nan
  settings.outputGain = GlobalReadout.outputGain.value(from: "0 dB") ?? .nan
  #expect(settings.transpose == 3)
  #expect(PresetGlobals(settings).isValid)
}

@Test func globalReadoutEditorCommitsAndCancels() {
  var editor = GlobalReadoutEditor()
  editor.begin(-0.0, readout: .outputGain)
  #expect(editor.isEditing)
  #expect(editor.draft == "0.0 dB")

  editor.draft = "-12.5 dB"
  #expect(editor.finish(readout: .outputGain) == -12.5)
  #expect(!editor.isEditing)
  #expect(editor.draft == "")

  editor.begin(6, readout: .doublerDelay)
  #expect(editor.draft == "6.00 ms")
  editor.draft = "not a number"
  #expect(editor.finish(readout: .doublerDelay) == nil)
  #expect(!editor.isEditing)

  editor.begin(3, readout: .inputGain)
  editor.draft = "99 dB"
  editor.cancel()
  #expect(!editor.isEditing)
  #expect(editor.draft == "")
}

@Test func globalReadoutEditorOnlyCommitsOnBlurAfterFocus() {
  var editor = GlobalReadoutEditor()
  editor.begin(0, readout: .outputGain)
  #expect(editor.phase == .awaitingFocus)
  let earlyBlur = editor.focusChanged(to: false)
  #expect(!earlyBlur)
  #expect(editor.phase == .awaitingFocus)
  #expect(editor.isEditing)

  let focus = editor.focusChanged(to: true)
  #expect(!focus)
  #expect(editor.phase == .focused)
  let blur = editor.focusChanged(to: false)
  #expect(blur)

  editor.draft = "-3"
  let typed = editor.finish(readout: .outputGain)
  #expect(typed == -3)
  let blurAfterCommit = editor.focusChanged(to: false)
  #expect(!blurAfterCommit)

  editor.begin(0, readout: .outputGain)
  _ = editor.focusChanged(to: true)
  editor.cancel()
  #expect(editor.phase == .closed)
  let blurAfterCancel = editor.focusChanged(to: false)
  #expect(!blurAfterCancel)
}

@Test func readoutPointerSeparatesClicksFromDrags() {
  var pointer = ReadoutPointer()
  var start = pointer.moved(by: .zero, current: 2)
  #expect(start == nil)
  start = pointer.moved(by: CGSize(width: 1, height: -3.8), current: 2)
  #expect(start == nil)
  var clicked = pointer.ended()
  #expect(clicked)
  #expect(pointer.phase == .idle)

  start = pointer.moved(by: .zero, current: -5)
  #expect(start == nil)
  start = pointer.moved(by: CGSize(width: 0, height: -4), current: -5)
  #expect(start == -5)
  start = pointer.moved(by: CGSize(width: 0, height: -1), current: 7)
  #expect(start == -5)
  clicked = pointer.ended()
  #expect(!clicked)
  #expect(pointer.phase == .idle)

  start = pointer.moved(by: CGSize(width: 3, height: 3), current: 1)
  #expect(start == 1)
  clicked = pointer.ended()
  #expect(!clicked)
}
