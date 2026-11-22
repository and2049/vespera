import AppKit
import LayoutRenderer
import ModuleSDK
import SwiftUI

@MainActor
final class ModulePanelLease {
  let module: LoadedModule
  let view: NSView
  private enum Renderer {
    case native(handle: UnsafeMutableRawPointer, api: UnsafePointer<vs_module_ui_api>)
    case layout
  }
  private let renderer: Renderer

  convenience init?(module: LoadedModule) { try? self.init(loading: module) }

  init(loading module: LoadedModule) throws {
    self.module = module
    if let layout = module.layout {
      do {
        view = try VesperaLayoutRenderer.view(
          withResources: layout.resources.path, layout: layout.path,
          parameters: module.parameterDisplays,
          getValue: { id, output in
            guard let value = try? module.value(id) else { return false }
            output.pointee = value
            return true
          },
          setValue: { id, value in (try? module.setValue(id, value)) != nil },
          tempo: { output in
            output.pointee = module.timing?.bpm ?? 120
            return module.timing != nil
          })
        try VesperaLayoutRenderer.configureFiles(
          in: view, slots: module.fileSlots.map { NSNumber(value: $0.id) },
          name: { [weak module] slot in
            module.map { ModuleFileMenu.name($0, slot: slot) } ?? ""
          },
          action: { [weak module] slot, anchor in
            if let module { ModuleFileMenu.show(module, slot: slot, anchor: anchor) }
          })
      } catch {
        throw AmpError.message(
          "\(module.name) — invalid module layout: \(error.localizedDescription)")
      }
      renderer = .layout
      return
    }
    guard let api = module.uiAPI else {
      throw AmpError.message("\(module.name) — no compatible module UI")
    }
    var host = vs_module_ui_host(
      struct_size: UInt32(MemoryLayout<vs_module_ui_host>.size),
      context: Unmanaged.passUnretained(module).toOpaque(),
      get_value: { context, id, output in
        guard let context, let output else { return Int32(VS_ERROR_ARGUMENT) }
        do {
          output.pointee = try Unmanaged<LoadedModule>.fromOpaque(context)
            .takeUnretainedValue().value(id)
          return Int32(VS_OK)
        } catch { return Int32(VS_ERROR_ARGUMENT) }
      },
      set_value: { context, id, value in
        guard let context else { return Int32(VS_ERROR_ARGUMENT) }
        do {
          try Unmanaged<LoadedModule>.fromOpaque(context).takeUnretainedValue().setValue(id, value)
          return Int32(VS_OK)
        } catch { return Int32(VS_ERROR_ARGUMENT) }
      },
      get_tempo: { context, output in
        guard let context, let output,
          output.pointee.struct_size >= UInt32(MemoryLayout<vs_tempo_context>.size)
        else { return Int32(VS_ERROR_ARGUMENT) }
        let timing = Unmanaged<LoadedModule>.fromOpaque(context).takeUnretainedValue().timing
        output.pointee.flags = timing == nil ? 0 : UInt32(VS_TEMPO_VALID)
        output.pointee.bpm = timing?.bpm ?? 120
        output.pointee.beat_position = timing?.beat ?? 0
        return Int32(VS_OK)
      })
    guard let handle = module.resourceDirectory.path.withCString({ api.pointee.create(&host, $0) })
    else {
      throw AmpError.message("\(module.name) — module UI could not be created")
    }
    renderer = .native(handle: handle, api: api)
    view = Unmanaged<NSView>.fromOpaque(handle).takeUnretainedValue()
  }

  isolated deinit {
    if case .native(let handle, let api) = renderer { api.pointee.destroy(handle) }
  }

  func refresh() {
    switch renderer {
    case .native(let handle, let api): api.pointee.refresh(handle)
    case .layout: VesperaLayoutRenderer.refreshView(view)
    }
  }
}

struct ModulePanel: View {
  let module: LoadedModule

  var body: some View {
    GeometryReader { geometry in
      let scale = min(
        geometry.size.width / module.preferredViewSize.width,
        geometry.size.height / module.preferredViewSize.height)
      let insets = module.visualInsets
      NativeModulePanel(module: module)
        .frame(
          width: geometry.size.width + (insets.left + insets.right) * scale,
          height: geometry.size.height + (insets.top + insets.bottom) * scale
        )
        .offset(x: -insets.left * scale, y: -insets.top * scale)
    }
  }
}

struct NativeModulePanel: NSViewRepresentable {
  let module: LoadedModule

  func makeNSView(context: Context) -> Container { Container(module: module) }
  func updateNSView(_ view: Container, context: Context) { view.lease?.refresh() }
  static func dismantleNSView(_ view: Container, coordinator: ()) { view.stop() }

  final class Container: NSView {
    var lease: ModulePanelLease?
    /// Panels poll their module so MIDI- and automation-driven changes show, but only while the
    /// window can be seen.
    enum Polling {
      case paused
      case active(Timer)
      case stopped
    }
    private(set) var polling = Polling.paused
    private var occlusionObserver: NSObjectProtocol?
    private let contentSize: CGSize
    private let insets: ModuleManifest.VisualInsets

    override var isFlipped: Bool { true }

    override func hitTest(_ point: NSPoint) -> NSView? {
      let local = convert(point, from: superview)
      let scale = min(
        bounds.width / (contentSize.width + insets.left + insets.right),
        bounds.height / (contentSize.height + insets.top + insets.bottom))
      let content = NSRect(
        x: insets.left * scale, y: insets.top * scale,
        width: bounds.width - (insets.left + insets.right) * scale,
        height: bounds.height - (insets.top + insets.bottom) * scale)
      return content.contains(local) ? super.hitTest(point) : nil
    }

    init(module: LoadedModule) {
      contentSize = module.preferredViewSize
      insets = module.visualInsets
      super.init(frame: .zero)
      wantsLayer = true
      layer?.masksToBounds = true
      var failure: String?
      do { lease = try ModulePanelLease(loading: module) } catch {
        failure = error.localizedDescription
      }
      if let native = lease?.view {
        native.frame = bounds
        native.autoresizingMask = [.width, .height]
        addSubview(native)
      } else {
        let label = NSTextField(
          labelWithString: failure ?? "\(module.name) — no compatible module UI")
        label.frame = NSRect(x: 24, y: 24, width: 600, height: 24)
        addSubview(label)
      }
    }

    required init?(coder: NSCoder) { nil }

    override func viewDidMoveToWindow() {
      super.viewDidMoveToWindow()
      if let occlusionObserver { NotificationCenter.default.removeObserver(occlusionObserver) }
      occlusionObserver = nil
      if let window {
        occlusionObserver = NotificationCenter.default.addObserver(
          forName: NSWindow.didChangeOcclusionStateNotification, object: window, queue: .main
        ) { [weak self] _ in
          MainActor.assumeIsolated { self?.updatePolling() }
        }
      }
      updatePolling()
    }

    /// Polls while the panel is in a visible window; on resuming it refreshes at once so changes
    /// made while hidden show without waiting for the first tick.
    func updatePolling() {
      let visible = window.map { $0.occlusionState.contains(.visible) } ?? false
      switch polling {
      case .stopped: return
      case .active(let timer) where !visible:
        timer.invalidate()
        polling = .paused
      case .paused where visible && lease != nil:
        lease?.refresh()
        let timer = Timer(timeInterval: 1 / 30, repeats: true) { [weak self] _ in
          MainActor.assumeIsolated { self?.lease?.refresh() }
        }
        timer.tolerance = 1 / 120
        RunLoop.main.add(timer, forMode: .default)
        polling = .active(timer)
      default: break
      }
    }

    func stop() {
      if case .active(let timer) = polling { timer.invalidate() }
      polling = .stopped
      if let occlusionObserver { NotificationCenter.default.removeObserver(occlusionObserver) }
      occlusionObserver = nil
      lease?.view.removeFromSuperview()
      lease = nil
    }
  }
}
