import AppKit
import SwiftUI
import Testing

@testable import VesperaUI

/// Renders the global controls header in each preset state, checking that the preset controls
/// keep the header's footprint. With `VESPERA_CAPTURE_DIR` set, writes each state as a PNG.
@MainActor
@Test func presetHeaderStatesFitTheGlobalControls() throws {
  let fixture = try RigFixture()
  let audio = fixture.audio()
  defer { audio.stop() }
  var names = ["Crunch", "Alpha"]
  let presets = PresetController(
    audio: audio, store: fixture.store(),
    prompts: PresetController.Prompts(
      unsavedChanges: { _ in .discard }, name: { _ in names.removeFirst() },
      confirmDelete: { _ in true }, importFile: { nil }))
  presets.restore()
  _ = NSApplication.shared
  let capture = ProcessInfo.processInfo.environment["VESPERA_CAPTURE_DIR"].map {
    URL(fileURLWithPath: $0, isDirectory: true)
  }

  func render(_ name: String, actions: Bool = false, menu: Bool = false) throws -> CGFloat {
    let size = CGSize(width: 1100, height: menu ? 330 : 170)
    var measured: CGFloat = 0
    let root = VStack(spacing: 0) {
      GlobalControls(
        settings: .constant(audio.globals), stereoAvailable: true, presets: presets
      )
      .background(
        GeometryReader { proxy in
          Color.clear.onAppear { measured = proxy.size.height }
        })
      Spacer(minLength: 0)
    }
    .frame(width: size.width, height: size.height)
    .background(Theme.window)
    .settingsDropdownOverlay()
    .preferredColorScheme(.dark)
    let host = NSHostingView(
      rootView: root.environment(\.presetControlsPresentation, .init(actions: actions, menu: menu)))
    let window = NSWindow(
      contentRect: NSRect(origin: .zero, size: size), styleMask: .borderless, backing: .buffered,
      defer: true)
    window.appearance = NSAppearance(named: .darkAqua)
    window.isReleasedWhenClosed = false
    window.contentView = host
    defer {
      window.contentView = nil
      window.close()
    }
    host.layoutSubtreeIfNeeded()
    RunLoop.main.run(until: Date().addingTimeInterval(0.05))
    host.layoutSubtreeIfNeeded()
    let bitmap = try #require(host.bitmapImageRepForCachingDisplay(in: host.bounds))
    host.cacheDisplay(in: host.bounds, to: bitmap)
    if let capture {
      try FileManager.default.createDirectory(at: capture, withIntermediateDirectories: true)
      try #require(bitmap.representation(using: .png, properties: [:])).write(
        to: capture.appendingPathComponent("\(name).png"))
    }
    return measured
  }

  let baseline = try render("default")
  #expect(baseline > 0)
  #expect(try render("default-actions", actions: true) == baseline)
  presets.saveAs()
  presets.saveAs()
  #expect(presets.title == "Alpha")
  #expect(try render("named-clean") == baseline)
  try audio.amps.activeAmp?.setValue(2, 0.123)
  presets.flush()
  #expect(presets.title == "* Alpha" && presets.canSave)
  #expect(try render("named-dirty") == baseline)
  #expect(try render("named-actions", actions: true) == baseline)
  _ = try render("menu-open", menu: true)

  // In the window, the open menu draws over the module page below the header.
  let size = CGSize(width: 1100, height: 860)
  let host = NSHostingView(
    rootView: VesperaWindow(audio: audio, presets: presets)
      .environment(\.presetControlsPresentation, .init(menu: true)))
  let window = NSWindow(
    contentRect: NSRect(origin: .zero, size: size), styleMask: .borderless, backing: .buffered,
    defer: true)
  window.appearance = NSAppearance(named: .darkAqua)
  window.isReleasedWhenClosed = false
  window.contentView = host
  defer {
    window.contentView = nil
    window.close()
  }
  host.layoutSubtreeIfNeeded()
  RunLoop.main.run(until: Date().addingTimeInterval(0.1))
  host.layoutSubtreeIfNeeded()
  let bitmap = try #require(host.bitmapImageRepForCachingDisplay(in: host.bounds))
  host.cacheDisplay(in: host.bounds, to: bitmap)
  if let capture {
    try #require(bitmap.representation(using: .png, properties: [:])).write(
      to: capture.appendingPathComponent("window-menu-open.png"))
  }
}
