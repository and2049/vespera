import AppKit
import ModuleSDK
import SwiftUI

struct ControlKnob: View {
  let title: String
  @Binding var value: Double
  let readout: GlobalReadout
  let defaultValue: Double
  var enabled = true
  var centerValue: Double? = nil
  @State private var dragStart: Double?
  @State private var editor = GlobalReadoutEditor()
  @State private var pointer = ReadoutPointer()
  @FocusState private var fieldFocused: Bool

  /// Pointer points of travel that cover the knob's full range while dragging.
  private static let dragTravel = 240.0

  private var range: ClosedRange<Double> { readout.range }
  private var step: Double { readout.step }

  private var position: Double {
    if let centerValue {
      if value <= centerValue {
        return 0.5 * (value - range.lowerBound) / (centerValue - range.lowerBound)
      }
      return 0.5 + 0.5 * (value - centerValue) / (range.upperBound - centerValue)
    }
    return (value - range.lowerBound) / (range.upperBound - range.lowerBound)
  }

  private func update(_ proposed: Double) {
    if let next = readout.clamped(proposed) { value = next }
  }

  /// Shift slows a drag to a tenth of its travel, as module controls do.
  private var fineFactor: Double { NSEvent.modifierFlags.contains(.shift) ? 0.1 : 1 }

  private func beginTextEntry() {
    guard enabled else { return }
    editor.begin(value, readout: readout)
  }

  private func commitTextEntry() {
    guard editor.isEditing else { return }
    if let next = editor.finish(readout: readout) { value = next }
    fieldFocused = false
  }

  private func cancelTextEntry() {
    editor.cancel()
    fieldFocused = false
  }

  private func adjust(_ direction: AccessibilityAdjustmentDirection) {
    guard enabled else { return }
    switch direction {
    case .increment: update(value + step)
    case .decrement: update(value - step)
    @unknown default: break
    }
  }

  var body: some View {
    VStack(spacing: 9) {
      ZStack {
        Circle()
          .trim(from: 0, to: 0.75)
          .stroke(Theme.dialTrack, lineWidth: 3)
          .rotationEffect(.degrees(135))
        Circle()
          .trim(
            from: min(centerValue == nil ? 0 : 0.375, position * 0.75),
            to: max(centerValue == nil ? 0 : 0.375, position * 0.75)
          )
          .stroke(Theme.primary, lineWidth: 3)
          .rotationEffect(.degrees(135))
        Circle()
          .fill(Theme.dialFace)
          .padding(7)
        Rectangle()
          .fill(Theme.fill)
          .frame(width: 2, height: 20)
          .offset(y: -12)
          .rotationEffect(.degrees(-135 + position * 270))
      }
      .frame(width: 58, height: 58)
      .contentShape(Circle())
      .gesture(
        DragGesture(minimumDistance: 0)
          .onChanged { gesture in
            if dragStart == nil { dragStart = value }
            let movement =
              Double(gesture.translation.width - gesture.translation.height) * fineFactor
            update(
              (dragStart ?? value)
                + movement * (range.upperBound - range.lowerBound) / Self.dragTravel)
          }
          .onEnded { _ in dragStart = nil }
      )
      .onTapGesture(count: 2) { value = defaultValue }
      .help("\(title): drag to adjust, shift-drag for fine control, double-click to reset")
      .accessibilityElement(children: .ignore)
      .accessibilityLabel(title)
      .accessibilityValue(readout.text(value))
      .accessibilityAdjustableAction { direction in adjust(direction) }
      .accessibilityAction { beginTextEntry() }
      .accessibilityAction(named: "Type value") { beginTextEntry() }

      Group {
        if editor.isEditing {
          TextField(title, text: $editor.draft)
            .textFieldStyle(.plain)
            .multilineTextAlignment(.center)
            .focused($fieldFocused)
            .focusRing(fieldFocused, radius: UISize.compact.metrics.corner_radius)
            .onSubmit { commitTextEntry() }
            .onExitCommand { cancelTextEntry() }
            .onAppear { fieldFocused = true }
            .onChange(of: fieldFocused) { _, focused in
              if editor.focusChanged(to: focused) { commitTextEntry() }
            }
            .help("Type a value and press Return; Escape cancels")
        } else {
          Text(readout.text(value))
            .monospacedDigit()
            .contentShape(Rectangle())
            .gesture(
              DragGesture(minimumDistance: 0)
                .onChanged { gesture in
                  guard let start = pointer.moved(by: gesture.translation, current: value)
                  else { return }
                  let movement = -Double(gesture.translation.height) * fineFactor
                  update(
                    start + movement * (range.upperBound - range.lowerBound) / Self.dragTravel)
                }
                .onEnded { _ in
                  if pointer.ended() { beginTextEntry() }
                }
            )
            .accessibilityHidden(true)
            .help("Click to type a value; drag vertically to adjust")
        }
      }
      .font(Theme.font(UISize.compact.metrics.font_size, weight: .semibold))
    }
    .opacity(enabled ? 1 : Theme.disabledOpacity)
    .allowsHitTesting(enabled)
    .accessibilityElement(children: .contain)
    .onChange(of: enabled) { _, isEnabled in
      if !isEnabled { cancelTextEntry() }
    }
  }
}

struct CompactSwitch: View {
  let title: String
  @Binding var enabled: Bool

  var body: some View {
    Button {
      enabled.toggle()
    } label: {
      HStack(spacing: 6) {
        Text(title)
        ZStack(alignment: enabled ? .trailing : .leading) {
          Capsule()
            .fill(enabled ? Theme.primary : .clear)
          Capsule()
            .strokeBorder(enabled ? Theme.primary : Theme.label, lineWidth: Theme.borderWidth)
          Circle()
            .fill(enabled ? Theme.window : Theme.label)
            .frame(width: 7, height: 7)
            .padding(2)
        }
        .frame(width: 20, height: 11)
      }
      .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
    .accessibilityLabel(title)
    .accessibilityValue(enabled ? "On" : "Off")
  }
}

struct InputModeControl: View {
  @Binding var stereo: Bool
  let stereoAvailable: Bool

  var body: some View {
    VStack(alignment: .leading, spacing: 20) {
      Text("INPUT MODE")
      HStack(spacing: 9) {
        ZStack(alignment: stereo ? .top : .bottom) {
          Capsule().fill(Theme.highlight)
          Circle()
            .fill(Theme.label)
            .frame(width: 12, height: 12)
            .padding(3)
        }
        .frame(width: 18, height: 43)
        .onTapGesture { stereo.toggle() }
        .accessibilityHidden(true)
        VStack(alignment: .leading, spacing: 10) {
          modeButton("Stereo", selected: stereo) { stereo = true }
          modeButton("Mono", selected: !stereo) { stereo = false }
        }
      }
    }
    .disabled(!stereoAvailable)
    .allowsHitTesting(stereoAvailable)
    .opacity(stereoAvailable ? 1 : Theme.disabledOpacity)
    .help(
      stereoAvailable
        ? "Choose mono or stereo input"
        : "Stereo requires an input device with at least two channels")
  }

  private func modeButton(_ title: String, selected: Bool, action: @escaping () -> Void)
    -> some View
  {
    Button(title, action: action)
      .buttonStyle(.plain)
      .foregroundStyle(selected ? Theme.primary : Theme.muted)
      .accessibilityAddTraits(selected ? [.isSelected] : [])
  }
}

struct GlobalControls: View {
  @Binding var settings: GlobalControlSettings
  let stereoAvailable: Bool
  var inputLevels: [Float] = [0, 0]
  var outputLevels: [Float] = [0, 0]
  var audioRunning = false
  let presets: PresetController

  private enum Meter {
    static let floorDecibels = -60.0
    static let rangeDecibels = 60.0
    static let height: CGFloat = 116
  }

  private var separator: some View {
    Rectangle().fill(Theme.separator).frame(width: 1, height: 106)
  }

  private func meter(_ title: String, stereo: Bool, levels: [Float]) -> some View {
    HStack(spacing: 1) {
      ForEach(0..<(stereo ? 2 : 1), id: \.self) { channel in
        let peak = channel < levels.count ? levels[channel] : 0
        let level =
          peak > 0
          ? max(0, min(1, (20 * log10(Double(peak)) - Meter.floorDecibels) / Meter.rangeDecibels))
          : 0
        ZStack(alignment: .bottom) {
          Rectangle().fill(Theme.meterTrack)
          Rectangle().fill(peak >= 1 ? Theme.clip : Theme.meterLevel)
            .frame(height: Meter.height * level)
        }
        .frame(width: stereo ? 4 : 6, height: Meter.height)
        .accessibilityLabel(
          "\(title) \(stereo ? (channel == 0 ? "left" : "right") : "mono") meter"
        )
      }
    }
    .help("\(title) \(stereo ? "L/R" : "mono") meter — \(audioRunning ? "live" : "audio stopped")")
  }

  var body: some View {
    HStack(alignment: .top, spacing: 20) {
      meter("Input", stereo: stereoAvailable && settings.stereoInput, levels: inputLevels)
      VStack(spacing: 15) {
        Text("INPUT")
        ControlKnob(
          title: "Input gain", value: $settings.inputGain, readout: .inputGain,
          defaultValue: 0, centerValue: 0)
      }
      .frame(width: 64)
      separator
      VStack(spacing: 15) {
        CompactSwitch(title: "GATE", enabled: $settings.gateEnabled)
        ControlKnob(
          title: "Gate threshold", value: $settings.gateThreshold, readout: .gateThreshold,
          defaultValue: -80, enabled: settings.gateEnabled)
      }
      .frame(width: 68)
      VStack(spacing: 15) {
        Text("TRANSPOSE")
        ControlKnob(
          title: "Transpose", value: $settings.transpose, readout: .transpose,
          defaultValue: 0, centerValue: 0)
      }
      .frame(width: 76)
      InputModeControl(stereo: $settings.stereoInput, stereoAvailable: stereoAvailable)
        .frame(width: 84)
      PresetControls(presets: presets)
        .frame(minWidth: 220, maxWidth: .infinity)
      VStack(spacing: 15) {
        CompactSwitch(title: "DOUBLER", enabled: $settings.doublerEnabled)
        ControlKnob(
          title: "Doubler delay", value: $settings.doublerTime, readout: .doublerDelay,
          defaultValue: 6, enabled: settings.doublerEnabled)
      }
      .frame(width: 90)
      separator
      VStack(spacing: 15) {
        Text("OUTPUT")
        ControlKnob(
          title: "Output gain", value: $settings.outputGain, readout: .outputGain,
          defaultValue: 0, centerValue: 0)
      }
      .frame(width: 64)
      meter("Output", stereo: true, levels: outputLevels)
    }
    .font(Theme.font(Theme.captionSize, weight: .semibold))
    .foregroundStyle(Theme.primary)
    .padding(.horizontal, 36)
    .padding(.top, 10)
    .padding(.bottom, 16)
  }
}
