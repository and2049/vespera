import ModuleSDK
import SwiftUI

struct AudioSettingsView: View {
  @ObservedObject var audio: AudioController
  @State private var draft: AudioConfiguration
  @State private var expandedDropdown: String?

  init(audio: AudioController) {
    self.audio = audio
    var configuration = audio.configuration
    configuration.monitor = true
    _draft = State(initialValue: configuration)
  }

  private var input: AudioDeviceInfo? { audio.devices.first { $0.id == draft.inputDevice } }
  private var output: AudioDeviceInfo? { audio.devices.first { $0.id == draft.outputDevice } }
  private var rates: [UInt32] {
    (input?.rates ?? []).filter { output?.rates.contains($0) == true }.sorted()
  }
  private var buffers: [UInt32] { AudioController.bufferSizes(input: input, output: output) }
  private let metrics = UISize.standard.metrics

  var body: some View {
    SettingsPanel(title: "Audio Settings") {
      SettingsIconButton(systemName: "xmark", label: "Close audio settings") {
        audio.showsSettings = false
      }
    } content: {
      HStack(alignment: .bottom, spacing: metrics.field_gap) {
        SettingsField(label: "Audio Device Type") {
          SettingsCell { Text("CoreAudio") }
        }
        Color.clear.frame(maxWidth: .infinity, maxHeight: 1)
      }
      SettingsDivider()
      HStack(alignment: .top, spacing: metrics.field_gap) {
        VStack(alignment: .leading, spacing: metrics.stack_spacing) {
          SettingsField(label: "Audio Input Device") { devicePicker(input: true) }
          SettingsField(label: "Audio Input Channels") {
            channels(input?.inputs ?? [], selection: $draft.inputChannels)
          }
        }
        VStack(alignment: .leading, spacing: metrics.stack_spacing) {
          SettingsField(label: "Audio Output Device") { devicePicker(input: false) }
          SettingsField(label: "Audio Output Channels") {
            channels(output?.outputs ?? [], selection: $draft.outputChannels)
          }
        }
      }
      SettingsDivider()
      HStack(alignment: .top, spacing: metrics.field_gap) {
        SettingsField(label: "Sample Rate") {
          SettingsDropdown(
            title: "Sample Rate",
            options: rates.map { SettingsOption(id: String($0), title: "\($0) Hz") },
            selection: numericSelection($draft.sampleRate), expanded: $expandedDropdown)
        }.frame(maxWidth: .infinity)
        SettingsField(label: "Audio Buffer Size") {
          SettingsDropdown(
            title: "Audio Buffer Size",
            options: buffers.map { size in
              let duration = String(
                format: "%.1f", Double(size) / Double(max(1, draft.sampleRate)) * 1000)
              return SettingsOption(id: String(size), title: "\(size) samples (\(duration) ms)")
            }, selection: numericSelection($draft.bufferSize), expanded: $expandedDropdown)
        }.frame(maxWidth: .infinity)
      }
      if let error = audio.error {
        Text(error).foregroundStyle(Theme.warning)
          .fixedSize(horizontal: false, vertical: true)
      }
      HStack {
        Spacer()
        Button(audio.state == .running ? "Apply" : "Start Audio") { audio.apply(draft) }
          .buttonStyle(SettingsButtonStyle(primary: true))
          .disabled(
            audio.state == .requestingPermission
              || (try? draft.validate(devices: audio.devices)) == nil)
      }
    }
    .uiSize(.standard)
    .frame(width: 620)
    .settingsDropdownOverlay()
    .onChange(of: draft.inputDevice) { _, _ in
      draft.inputChannels = Array((input?.inputs ?? []).prefix(1).map(\.id))
      normalizeFormat()
    }
    .onChange(of: draft.outputDevice) { _, _ in
      draft.outputChannels = Array((output?.outputs ?? []).prefix(2).map(\.id))
      normalizeFormat()
    }
  }

  private func devicePicker(input isInput: Bool) -> some View {
    SettingsDropdown(
      title: isInput ? "Input device" : "Output device",
      options: audio.devices.filter { isInput ? !$0.inputs.isEmpty : !$0.outputs.isEmpty }
        .map { SettingsOption(id: $0.id, title: $0.name) },
      selection: isInput ? $draft.inputDevice : $draft.outputDevice, expanded: $expandedDropdown)
  }

  private func numericSelection(_ value: Binding<UInt32>) -> Binding<String> {
    Binding(
      get: { String(value.wrappedValue) },
      set: { if let number = UInt32($0) { value.wrappedValue = number } })
  }

  private func channels(_ channels: [AudioChannelInfo], selection: Binding<[UInt32]>) -> some View {
    ScrollView {
      VStack(alignment: .leading, spacing: metrics.horizontal_padding) {
        ForEach(channels) { channel in
          Toggle(
            channel.title,
            isOn: Binding(
              get: { selection.wrappedValue.contains(channel.id) },
              set: { selected in
                if selected {
                  if selection.wrappedValue.count < 2 {
                    selection.wrappedValue.append(channel.id)
                    selection.wrappedValue.sort()
                  }
                } else {
                  selection.wrappedValue.removeAll { $0 == channel.id }
                }
              })
          )
          .toggleStyle(SettingsCheckboxStyle())
          .disabled(
            !selection.wrappedValue.contains(channel.id) && selection.wrappedValue.count == 2)
        }
      }.frame(maxWidth: .infinity, alignment: .leading).padding(metrics.horizontal_padding)
    }
    .frame(height: 125)
    .background(Theme.inset, in: RoundedRectangle(cornerRadius: metrics.corner_radius))
  }

  private func normalizeFormat() {
    if !rates.contains(draft.sampleRate) {
      draft.sampleRate = rates.contains(48000) ? 48000 : rates.first ?? 48000
    }
    if !buffers.contains(draft.bufferSize) {
      draft.bufferSize = buffers.contains(128) ? 128 : buffers.first ?? 128
    }
  }
}
