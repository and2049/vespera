import ModuleSDK
import SwiftUI

struct TempoSettings: Codable, Equatable {
  var bpm = 120.0
  var enabled = false
  var level = -18.0

  var isValid: Bool {
    bpm.isFinite && (40...240).contains(bpm) && level.isFinite && (-60...0).contains(level)
  }
}

struct TempoControls: View {
  @ObservedObject var audio: AudioController
  private enum Panel { case tempo, midi }
  @State private var panel: Panel?
  @State private var editing = false
  @State private var draft = ""
  @State private var dragStart: Double?
  @FocusState private var focused: Bool
  @FocusState private var optionsFocused: Bool

  private var pulse: Bool {
    guard audio.state == .running, let beat = audio.status?.beat else { return false }
    return beat.truncatingRemainder(dividingBy: 1) < 0.16
  }

  private func setBpm(_ value: Double) {
    guard value.isFinite else { return }
    var next = audio.tempo
    next.bpm = min(240, max(40, value))
    audio.updateTempo(next)
  }

  private func commit() {
    if let value = Double(draft) { setBpm(value) }
    editing = false
    focused = false
  }

  var body: some View {
    ZStack(alignment: .bottomTrailing) {
      if panel != nil {
        Color.black.opacity(0.001)
          .contentShape(Rectangle())
          .onTapGesture {
            panel = nil
          }
          .accessibilityLabel("Dismiss control options")
      }
      VStack(alignment: .trailing, spacing: 8) {
        if panel == .tempo { options }
        if panel == .midi { MIDIControls(audio: audio, dismiss: { panel = nil }) }
        pill
      }
    }
    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottomTrailing)
    .onChange(of: audio.showsSettings) { _, shown in
      if shown {
        panel = nil
      }
    }
  }

  private let metrics = UISize.compact.metrics

  private var pill: some View {
    HStack(spacing: metrics.label_gap) {
      Button("MIDI") {
        panel = panel == .midi ? nil : .midi
      }.font(Theme.font(metrics.label_font_size))
        .foregroundStyle(panel == .midi ? Theme.primary : Theme.label)
        .accessibilityLabel("MIDI settings and mappings")
      Divider().frame(height: metrics.icon_size)
      Circle().fill(Theme.primary.opacity(pulse ? 1 : 0.25)).frame(width: 5, height: 5)
        .accessibilityHidden(true)
      Group {
        if editing {
          TextField("BPM", text: $draft)
            .textFieldStyle(.plain).focused($focused).onSubmit { commit() }
            .onChange(of: focused) { _, value in if !value && editing { commit() } }
        } else {
          Text(audio.tempo.bpm, format: .number.precision(.fractionLength(0...1)))
            .frame(maxWidth: .infinity, alignment: .trailing)
            .contentShape(Rectangle())
            .onTapGesture {
              draft = String(format: "%.1f", audio.tempo.bpm)
              editing = true
              focused = true
            }
            .gesture(
              DragGesture(minimumDistance: 4).onChanged { value in
                if dragStart == nil { dragStart = audio.tempo.bpm }
                setBpm((dragStart ?? audio.tempo.bpm) - Double(value.translation.height) / 2)
              }.onEnded { _ in dragStart = nil })
        }
      }
      .frame(width: 42)
      .font(Theme.valueFont(metrics.font_size + 1))
      .foregroundStyle(Theme.primary)
      .accessibilityLabel("Tempo in beats per minute")
      .accessibilityAdjustableAction { direction in
        setBpm(audio.tempo.bpm + (direction == .increment ? 1 : -1))
      }
      .help("Click to enter BPM; drag vertically to adjust")
      Text("BPM").font(Theme.font(metrics.label_font_size)).foregroundStyle(Theme.secondary)
      Divider().frame(height: metrics.icon_size)
      Button {
        var next = audio.tempo
        next.enabled.toggle()
        audio.updateTempo(next)
      } label: {
        Image(systemName: "metronome").font(Theme.iconFont(metrics, grow: 1, weight: .regular))
          .foregroundStyle(audio.tempo.enabled ? Theme.primary : Theme.secondary)
      }
      .help(audio.tempo.enabled ? "Mute metronome" : "Enable metronome")
      .accessibilityLabel("Metronome")
      .accessibilityValue(audio.tempo.enabled ? "On" : "Off")
      Button {
        panel = panel == .tempo ? nil : .tempo
      } label: {
        Image(systemName: panel == .tempo ? "chevron.up" : "chevron.down")
          .font(Theme.disclosureChevronFont(metrics)).foregroundStyle(Theme.label)
      }
      .accessibilityLabel("Tempo options")
    }
    .buttonStyle(.plain)
    .padding(.horizontal, metrics.horizontal_padding + 4)
    .frame(height: metrics.control_height + 6)
    .background(RoundedRectangle(cornerRadius: Theme.panelRadius - 2).fill(Theme.panel))
    .overlay(
      RoundedRectangle(cornerRadius: Theme.panelRadius - 2)
        .strokeBorder(Theme.divider, lineWidth: Theme.borderWidth)
    )
    .fixedSize(horizontal: true, vertical: false)
  }

  private var options: some View {
    SettingsPanel(title: "Metronome") {
      Text("4/4").font(Theme.valueFont(metrics.label_font_size)).foregroundStyle(Theme.secondary)
    } content: {
      VStack(alignment: .leading, spacing: metrics.label_gap) {
        HStack {
          SettingsLabel("Level")
          Spacer()
          Text("\(Int(audio.tempo.level)) dB").font(Theme.valueFont(metrics.font_size))
            .foregroundStyle(Theme.primary)
        }
        SettingsSlider(
          label: "Metronome level",
          value: Binding(
            get: { audio.tempo.level },
            set: { value in
              var next = audio.tempo
              next.level = value
              audio.updateTempo(next)
            }), range: -60...0, valueDescription: { "\(Int($0)) decibels" })
      }
      Button {
        audio.tapTempo()
      } label: {
        HStack {
          Text("Tap tempo")
          Spacer()
          Text(audio.tempo.bpm, format: .number.precision(.fractionLength(0...1)))
            .font(Theme.valueFont(metrics.font_size)).foregroundStyle(Theme.secondary)
        }
      }.buttonStyle(SettingsButtonStyle())
      Text(
        audio.state == .running
          ? "40–240 BPM · accented first beat" : "Start audio to hear the metronome"
      )
      .font(Theme.font(metrics.label_font_size)).foregroundStyle(Theme.secondary)
    }
    .uiSize(.compact)
    .frame(width: 248)
    .background {
      // A sibling focus anchor receives initial focus for Escape. Making the panel itself
      // focusable would report focus to every descendant button through `isFocused`.
      Color.clear.focusable().focusEffectDisabled().focused($optionsFocused)
        .accessibilityHidden(true)
    }
    .onAppear { optionsFocused = true }
    .onKeyPress(.escape) {
      panel = nil
      return .handled
    }
  }
}
