import AppKit
import ModuleSDK
import SwiftUI
import UniformTypeIdentifiers

/// The bottom-left pill: record the input, and open the reamp panel above it. `messages` are
/// status lines stacked above the panel, or above the pill while the panel is closed.
struct TakeControls<Messages: View>: View {
  @ObservedObject var audio: AudioController
  @ObservedObject var takes: TakeController
  @ViewBuilder var messages: Messages
  @State private var panelShown = false
  private let metrics = UISize.compact.metrics

  var body: some View {
    ZStack(alignment: .bottomLeading) {
      if panelShown {
        ControlPanelDismissLayer { panelShown = false }
      }
      VStack(alignment: .leading, spacing: 8) {
        VStack(alignment: .leading, spacing: 4) { messages }
        if panelShown { panel }
        HStack(spacing: metrics.label_gap) {
          pill
          if audio.isPreparingRig {
            ProgressView().controlSize(.small).tint(Theme.secondary)
              .accessibilityLabel("Preparing rig")
          }
        }
      }
    }
    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottomLeading)
    .onChange(of: audio.showsSettings) { _, shown in
      if shown {
        panelShown = false
      }
    }
  }

  private var pill: some View {
    HStack(spacing: metrics.label_gap) {
      recordButton
      if let elapsed = takes.elapsedText {
        Text(elapsed).font(Theme.valueFont(metrics.font_size + 1)).foregroundStyle(Theme.primary)
          .accessibilityLabel("Recording time")
          .accessibilityValue(elapsed)
      }
      ControlPillDivider(metrics: metrics)
      Button {
        panelShown.toggle()
        if panelShown { takes.refreshLastTake() }
      } label: {
        HStack(spacing: metrics.label_gap) {
          Text("REAMP").font(Theme.font(metrics.label_font_size))
            .foregroundStyle(panelShown ? Theme.primary : Theme.label)
          if case .rendering(_, let progress) = takes.reamp {
            Text(percent(progress)).font(Theme.valueFont(metrics.label_font_size))
              .foregroundStyle(Theme.secondary)
          }
          Image(systemName: panelShown ? "chevron.up" : "chevron.down")
            .font(Theme.disclosureChevronFont(metrics)).foregroundStyle(Theme.label)
        }
        .contentShape(Rectangle())
      }
      .accessibilityLabel("Reamp options")
    }
    .controlPill(metrics)
  }

  private var recordButton: some View {
    let control = takes.recordControl
    let symbol: String
    let color: Color
    let enabled: Bool
    switch control {
    case .start(let available):
      symbol = "record.circle"
      color = available ? Theme.label : Theme.muted
      enabled = available
    case .stop:
      symbol = "stop.circle"
      color = Theme.clip
      enabled = true
    case .finalizing:
      symbol = "stop.circle"
      color = Theme.muted
      enabled = false
    }
    let starts = if case .start = control { true } else { false }
    return Button {
      takes.toggleRecording()
    } label: {
      Image(systemName: symbol).font(Theme.iconFont(metrics, grow: 1, weight: .regular))
        .foregroundStyle(color)
    }
    .disabled(!enabled)
    .help(
      starts
        ? (enabled ? "Record the input as a take" : "Start audio to record")
        : enabled ? "Stop recording" : "Finishing the take"
    )
    .accessibilityLabel(starts ? "Record input" : "Stop recording")
  }

  private func percent(_ progress: Double) -> String { "\(Int((progress * 100).rounded()))%" }

  private var panel: some View {
    SettingsPanel(title: "Reamp") {
      VStack(alignment: .leading, spacing: metrics.label_gap) {
        HStack {
          SettingsLabel("Last take")
          Spacer()
          Text(takes.lastTake?.summary ?? "No take yet").font(Theme.valueFont(metrics.font_size))
            .foregroundStyle(takes.lastTake == nil ? Theme.secondary : Theme.primary)
        }
        if let warning = takes.lastTake?.dropoutWarning {
          Text(warning).foregroundStyle(Theme.warning).statusMessage()
        }
      }
      switch takes.reamp {
      case .rendering(_, let progress):
        progressRow(progress, cancelling: false)
      case .cancelling:
        progressRow(nil, cancelling: true)
      case .saving:
        Text("Choose where to save the render").statusMessage()
      case .idle, .finished, .failed, .cancelled:
        Button {
          takes.processLastTake()
        } label: {
          HStack {
            Text("Process last take")
            Spacer()
          }
        }
        .buttonStyle(SettingsButtonStyle())
        .disabled(takes.lastTake == nil)
        Button {
          takes.chooseAndProcess()
        } label: {
          HStack {
            Text("Choose file…")
            Spacer()
          }
        }
        .buttonStyle(SettingsButtonStyle())
      }
      if let message = takes.reampMessage {
        Text(message.text).foregroundStyle(message.isProblem ? Theme.warning : Theme.secondary)
          .statusMessage().lineLimit(1).truncationMode(.middle)
          .help(message.text)
      }
      Text("Renders through the current rig")
        .font(Theme.font(metrics.label_font_size)).foregroundStyle(Theme.secondary)
    }
    .controlPanel { panelShown = false }
  }

  private func progressRow(_ progress: Double?, cancelling: Bool) -> some View {
    VStack(alignment: .leading, spacing: metrics.label_gap) {
      HStack {
        SettingsLabel(cancelling ? "Cancelling" : "Rendering")
        Spacer()
        if let progress {
          Text(percent(progress)).font(Theme.valueFont(metrics.font_size))
            .foregroundStyle(Theme.primary)
        }
      }
      Group {
        if let progress {
          ProgressView(value: progress)
        } else {
          ProgressView()
        }
      }
      .progressViewStyle(.linear).tint(Theme.fill)
      .accessibilityLabel("Reamp progress")
      Button {
        takes.cancelReamp()
      } label: {
        HStack {
          Text("Cancel")
          Spacer()
        }
      }
      .buttonStyle(SettingsButtonStyle())
      .disabled(cancelling)
    }
  }
}

extension TakeController.Prompts {
  /// The open panel for a WAV file and the save panel for a render.
  @MainActor static let standard = TakeController.Prompts(
    chooseInput: {
      let panel = NSOpenPanel()
      panel.title = "Reamp File"
      panel.canChooseDirectories = false
      panel.allowsMultipleSelection = false
      panel.allowedContentTypes = [.wav]
      return panel.runModal() == .OK ? panel.url : nil
    },
    saveOutput: { name in
      let panel = NSSavePanel()
      panel.title = "Save Reamp"
      panel.nameFieldStringValue = name
      panel.allowedContentTypes = [.wav]
      panel.canCreateDirectories = true
      return panel.runModal() == .OK ? panel.url : nil
    })
}
