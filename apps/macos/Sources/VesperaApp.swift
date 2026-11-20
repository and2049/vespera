import AppKit
import ModuleSDK
import SwiftUI

enum ModuleTab: String, CaseIterable, Identifiable {
  case preFX = "pre-fx"
  case amp = "amp"
  case cabinet = "cabinet"
  case specialFX = "special-fx"
  case eq = "eq"
  case postFX = "post-fx"

  var id: String { rawValue }

  /// Tabs in signal order; amp-owned sections appear only when the selected amp provides them.
  static func available(cabinet: Bool, equalizer: Bool) -> [ModuleTab] {
    allCases.filter { ($0 != .cabinet || cabinet) && ($0 != .eq || equalizer) }
  }

  var title: String {
    switch self {
    case .preFX: "Pre FX"
    case .amp: "Amp"
    case .cabinet: "Cabinet"
    case .specialFX: "Special FX"
    case .eq: "EQ"
    case .postFX: "Post FX"
    }
  }

  var shortcut: KeyEquivalent {
    switch self {
    case .preFX: "1"
    case .amp: "2"
    case .cabinet: "3"
    case .specialFX: "4"
    case .eq: "5"
    case .postFX: "6"
    }
  }

  var image: NSImage {
    guard
      let url = Bundle.module.url(
        forResource: rawValue, withExtension: "svg", subdirectory: "Icons"),
      let image = NSImage(contentsOf: url)
    else {
      preconditionFailure("Missing tab icon: \(rawValue)")
    }
    return image
  }
}

/// Statuses of the shown tab's modules that are not ready, one line each, as inline status text:
/// secondary while a module is busy, in the warning colour for remarks and errors.
struct ModuleStatusLine: View {
  let notices: [ModuleNotice]

  var body: some View {
    VStack(alignment: .leading, spacing: 2) {
      ForEach(notices) { notice in
        Text(notice.text).foregroundStyle(notice.isProblem ? Theme.warning : Theme.secondary)
          .statusMessage().lineLimit(1)
      }
    }
    .frame(maxWidth: 360, alignment: .leading)
    .allowsHitTesting(false)
    .accessibilityElement(children: .combine)
  }
}

struct ModuleTabButton: View {
  static let indicatorSize: CGFloat = 4
  let tab: ModuleTab
  let selected: Bool
  let action: () -> Void
  @State private var hovered = false

  var body: some View {
    Button(action: action) {
      Image(nsImage: tab.image)
        .resizable()
        .frame(width: 40, height: 40)
        .opacity(hovered ? 1 : 0.85)
        .frame(width: 56, height: 56)
        .overlay(alignment: .bottom) {
          Circle().fill(Theme.fill)
            .frame(width: ModuleTabButton.indicatorSize, height: ModuleTabButton.indicatorSize)
            .padding(.bottom, 2)
            .opacity(selected ? 1 : 0)
            .accessibilityHidden(true)
        }
        .background(
          RoundedRectangle(cornerRadius: UISize.standard.metrics.corner_radius)
            .fill(Theme.hoverWash.opacity(hovered ? 1 : 0))
        )
        .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
    .onHover { hovered = $0 }
    .help(tab.title)
    .accessibilityLabel(tab.title)
    .accessibilityAddTraits(selected ? [.isSelected] : [])
    .keyboardShortcut(tab.shortcut, modifiers: .command)
  }
}

/// The selected amp's background behind the whole window content: the window colour with the image
/// filling it uniformly, centred and cropped. Purely decorative.
struct WindowBackgroundView: View {
  let image: CGImage?

  var body: some View {
    GeometryReader { geometry in
      ZStack {
        Theme.window
        if let image {
          Image(decorative: image, scale: 1)
            .resizable()
            .interpolation(.high)
            .aspectRatio(contentMode: .fill)
            .frame(width: geometry.size.width, height: geometry.size.height)
            .clipped()
        }
      }
    }
    .allowsHitTesting(false)
    .accessibilityHidden(true)
  }
}

enum WindowLayout {
  static let canvas = CGSize(width: 1100, height: 860)
  static let initial = CGSize(width: 880, height: 688)
  static let minimum = CGSize(width: 550, height: 430)
}

struct VesperaWindow: View {
  @ObservedObject var audio: AudioController
  let presets: PresetController
  @ObservedObject private var cabinets: CabinetCatalog
  @ObservedObject private var amps: AmpCatalog
  @State private var selectedTab: ModuleTab = .amp

  init(audio: AudioController, presets: PresetController) {
    self.audio = audio
    self.presets = presets
    self.cabinets = audio.cabinets
    self.amps = audio.amps
  }

  private var tabs: [ModuleTab] {
    ModuleTab.available(
      cabinet: cabinets.supported, equalizer: amps.activeAmp?.equalizer != nil)
  }

  var body: some View {
    GeometryReader { geometry in
      let scale = min(
        geometry.size.width / WindowLayout.canvas.width,
        geometry.size.height / WindowLayout.canvas.height)
      canvas
        .frame(width: WindowLayout.canvas.width, height: WindowLayout.canvas.height)
        .scaleEffect(scale, anchor: .topLeading)
        .frame(
          width: WindowLayout.canvas.width * scale,
          height: WindowLayout.canvas.height * scale, alignment: .topLeading
        )
        .frame(width: geometry.size.width, height: geometry.size.height)
    }
    .background(WindowBackgroundView(image: amps.activeAmp?.windowBackground))
    .preferredColorScheme(.dark)
    .onChange(of: tabs) { _, tabs in
      if !tabs.contains(selectedTab) { selectedTab = .amp }
    }
  }

  private var canvas: some View {
    VStack(spacing: 0) {
      HStack(spacing: 16) {
        ForEach(tabs) { tab in
          ModuleTabButton(tab: tab, selected: selectedTab == tab) {
            selectedTab = tab
          }
        }
      }
      .frame(maxWidth: .infinity)
      .padding(.vertical, 12)
      .overlay(alignment: .trailing) {
        Button {
          audio.showsSettings = true
        } label: {
          Image(systemName: audio.error == nil ? "gearshape" : "exclamationmark.circle")
            .font(Theme.iconFont(UISize.standard.metrics, grow: 2, weight: .regular))
            .foregroundStyle(audio.error == nil ? Theme.secondary : Theme.warning)
        }
        .buttonStyle(.plain).padding(.trailing, 36)
        .help("Audio Settings").accessibilityLabel("Audio Settings")
      }

      GlobalControls(
        settings: Binding(get: { audio.globals }, set: { audio.updateGlobals($0) }),
        stereoAvailable: audio.stereoAvailable, inputLevels: audio.inputLevels,
        outputLevels: audio.outputLevels, audioRunning: audio.state == .running,
        presets: presets)

      if selectedTab == .amp {
        AmpCarousel(catalog: audio.amps, cabinet: cabinets.activeCabinet)
      } else if selectedTab == .cabinet {
        CabinetPage(catalog: cabinets)
      } else if selectedTab == .specialFX {
        SpecialFXPage(catalog: audio.specialFX, audio: audio)
      } else if selectedTab == .eq {
        AmpCarousel(catalog: audio.amps, showsEqualizer: true)
      } else {
        VStack(spacing: 0) {
          if selectedTab == .preFX {
            PedalModuleBoard(catalog: audio.preFX)
          } else {
            PedalModuleBoard(catalog: audio.postFX)
          }
          AmpCarousel(catalog: audio.amps, showsPanel: false)
            .frame(height: 84)
        }
      }
    }
    .settingsDropdownOverlay()
    .overlay(alignment: .bottomTrailing) {
      TempoControls(audio: audio).padding(.trailing, 24).padding(.bottom, 24)
    }
    .overlay(alignment: .bottomLeading) {
      VStack(alignment: .leading, spacing: 4) {
        if let notice = audio.notice {
          HStack {
            Text(notice).statusMessage()
            Button("Dismiss", action: audio.dismissNotice).buttonStyle(.plain)
          }
          .foregroundStyle(Theme.secondary)
          .frame(maxWidth: 360, alignment: .leading)
        }
        ModuleStatusLine(notices: audio.moduleNotices(on: selectedTab))
        if let message = audio.preparationMessage {
          HStack {
            Text(message).statusMessage()
            if audio.canCancelPreparation {
              Button("Cancel", action: audio.cancelPreparation).buttonStyle(.plain)
            }
          }
          .foregroundStyle(Theme.secondary)
          .frame(maxWidth: 360, alignment: .leading)
        }
      }
      .padding(.leading, 24).padding(.bottom, 24)
    }
    .overlay {
      if audio.showsSettings {
        ZStack {
          Theme.scrim.contentShape(Rectangle())
          AudioSettingsView(audio: audio)
        }
      }
    }
  }
}

@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate, NSWindowDelegate {
  private var window: NSWindow?
  private let audio = AudioController()
  private lazy var presets = PresetController(
    audio: audio, store: PresetStore(directory: Self.presetDirectory), prompts: .standard)

  /// `VESPERA_PRESET_PATH` points a development or test instance at its own preset library.
  private static var presetDirectory: URL {
    ProcessInfo.processInfo.environment["VESPERA_PRESET_PATH"].flatMap {
      $0.isEmpty ? nil : URL(fileURLWithPath: $0, isDirectory: true)
    } ?? PresetStore.standardDirectory
  }

  func applicationDidFinishLaunching(_ notification: Notification) {
    let menu = NSMenu()
    let appItem = NSMenuItem()
    let appMenu = NSMenu()
    let settingsItem = NSMenuItem(
      title: "Audio Settings…", action: #selector(openAudioSettings), keyEquivalent: ",")
    settingsItem.target = self
    appMenu.addItem(settingsItem)
    appMenu.addItem(
      withTitle: "Quit vespera", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
    appItem.submenu = appMenu
    menu.addItem(appItem)
    NSApp.mainMenu = menu

    let window = NSWindow(
      contentRect: NSRect(origin: .zero, size: WindowLayout.initial),
      styleMask: [.titled, .closable, .miniaturizable, .resizable],
      backing: .buffered,
      defer: false
    )
    window.title = "vespera"
    window.appearance = NSAppearance(named: .darkAqua)
    window.backgroundColor = NSColor(VS_UI_SURFACE_WINDOW)
    presets.restore()
    let content = NSHostingView(rootView: VesperaWindow(audio: audio, presets: presets))
    content.sizingOptions = []
    window.contentView = content
    window.contentMinSize = WindowLayout.minimum
    window.contentAspectRatio = WindowLayout.canvas
    window.isReleasedWhenClosed = false
    window.delegate = self
    window.center()
    window.makeKeyAndOrderFront(nil)
    self.window = window
    NSApp.activate(ignoringOtherApps: true)
    ComponentGalleryWindow.openIfRequested()
    audio.start()
  }

  func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }

  /// Closing the window quits, so it goes through the same unsaved-changes check as Quit.
  func windowShouldClose(_ sender: NSWindow) -> Bool {
    NSApp.terminate(nil)
    return false
  }

  func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
    presets.prepareToTerminate() ? .terminateNow : .terminateCancel
  }

  @objc private func openAudioSettings() { audio.showsSettings = true }
  func applicationWillTerminate(_ notification: Notification) { audio.shutdown() }
}

@main
struct VesperaApp {
  @MainActor
  static func main() {
    let app = NSApplication.shared
    let delegate = AppDelegate()
    app.setActivationPolicy(.regular)
    app.delegate = delegate
    withExtendedLifetime(delegate) {
      app.run()
    }
  }
}
