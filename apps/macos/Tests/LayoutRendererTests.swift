import AppKit
import LayoutRenderer
import Testing

@testable import VesperaUI

/// A pre-FX fixture package whose binary has DSP and parameters but no UI entry point; its
/// manifest declares a host-rendered layout drawn from generated artwork.
@MainActor
private final class LayoutPackage {
  let fixture: TestModulePackage
  var manifest: ModuleManifest
  var directory: URL { fixture.directory }

  static let red = NSColor(srgbRed: 1, green: 0, blue: 0, alpha: 1)
  static let green = NSColor(srgbRed: 0, green: 1, blue: 0, alpha: 1)
  static let blue = NSColor(srgbRed: 0, green: 0, blue: 1, alpha: 1)

  init(
    layout: [String: Any]? = nil, metadata: TestModulePackage.Metadata = .none,
    files: Bool = false
  ) throws {
    fixture = try TestModulePackage(
      category: "pre-fx", id: "org.example.layout", metadata: metadata, files: files)
    manifest = fixture.manifest
    manifest.ui = .init(layout: "layout.json")
    try Self.image(directory.appendingPathComponent("background.png"), 100, 50, [.gray])
    try Self.image(
      directory.appendingPathComponent("knob.png"), 10, 30, [Self.red, Self.green, Self.blue])
    try Self.image(directory.appendingPathComponent("off.png"), 10, 10, [.black])
    try Self.image(directory.appendingPathComponent("on.png"), 10, 10, [.white])
    try write(layout: layout ?? Self.layout())
    try save()
  }

  static func layout(widgets: [[String: Any]]? = nil) -> [String: Any] {
    [
      "version": 1, "crop": [0, 0, 100, 50], "background": "background.png",
      "backgroundRect": [0, 0, 100, 50],
      "widgets": widgets ?? [
        [
          "name": "Gain", "type": "rotaryFilmStripKnob", "rect": [10, 10, 20, 20],
          "images": ["knob.png"], "id": 2, "initial": 0.5, "label": "Gain",
        ],
        [
          "name": "Power", "type": "imageButton", "rect": [60, 10, 20, 20],
          "images": ["off.png", "on.png"], "id": 1, "label": "Power",
        ],
        [
          "name": "Led", "type": "imageButton", "rect": [60, 10, 20, 20], "images": ["", "on.png"],
          "id": 1, "readonly": true,
        ],
      ],
    ]
  }

  func write(layout: [String: Any]) throws {
    try JSONSerialization.data(withJSONObject: layout).write(
      to: directory.appendingPathComponent("layout.json"))
  }

  func save() throws {
    try JSONEncoder().encode(manifest).write(to: directory.appendingPathComponent("module.json"))
  }

  func validate() -> String? {
    do {
      try VesperaLayoutRenderer.validateResources(directory.path, layout: "layout.json")
      return nil
    } catch { return error.localizedDescription }
  }

  /// Fills horizontal bands top to bottom, one per color.
  static func image(_ url: URL, _ width: Int, _ height: Int, _ bands: [NSColor]) throws {
    let rep = try #require(
      NSBitmapImageRep(
        bitmapDataPlanes: nil, pixelsWide: width, pixelsHigh: height, bitsPerSample: 8,
        samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB,
        bytesPerRow: 0, bitsPerPixel: 0))
    NSGraphicsContext.saveGraphicsState()
    defer { NSGraphicsContext.restoreGraphicsState() }
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
    let band = Double(height) / Double(bands.count)
    for (index, color) in bands.enumerated() {
      color.setFill()
      NSRect(x: 0, y: Double(height) - Double(index + 1) * band, width: Double(width), height: band)
        .fill()
    }
    try #require(rep.representation(using: .png, properties: [:])).write(to: url)
  }
}

@MainActor
private func render(_ view: NSView) throws -> NSBitmapImageRep {
  view.layoutSubtreeIfNeeded()
  let rep = try #require(view.bitmapImageRepForCachingDisplay(in: view.bounds))
  view.cacheDisplay(in: view.bounds, to: rep)
  return rep
}

@MainActor
@Test func fileWidgetsRequireVersionTwoDeclaredSlotsAndHostCallbacks() throws {
  var layout = LayoutPackage.layout(widgets: [
    ["type": "file", "slot": 1, "rect": [10, 10, 80, 20], "placeholder": "Response"]
  ])
  layout["version"] = 2
  let package = try LayoutPackage(layout: layout, files: true)
  #expect(package.validate() == nil)
  let module = try LoadedModule(
    manifest: package.manifest, directory: package.directory, category: "pre-fx")
  #expect(module.fileSlots.count == 1)
  let view = try VesperaLayoutRenderer.view(
    withResources: package.directory.path, layout: "layout.json", parameters: nil,
    getValue: { _, _ in false }, setValue: { _, _ in false }, tempo: { _ in false })
  var invoked: UInt32 = 0
  try VesperaLayoutRenderer.configureFiles(
    in: view, slots: [1], name: { _ in "response.wav" },
    action: { slot, _ in invoked = slot })
  let button = try #require(view.subviews.first as? NSButton)
  #expect(button.title == "response.wav")
  #expect(button.accessibilityPerformPress())
  #expect(invoked == 1)
  #expect(throws: (any Error).self) {
    try VesperaLayoutRenderer.configureFiles(
      in: view, slots: [2], name: { _ in "" }, action: { _, _ in })
  }
  layout["version"] = 1
  try package.write(layout: layout)
  #expect(package.validate() != nil)
}

/// The color at a point in layout units (top-left origin) of a 100×50 layout.
@MainActor
private func color(_ rep: NSBitmapImageRep, _ x: Double, _ y: Double) throws -> NSColor {
  let pixel = try #require(
    rep.colorAt(
      x: Int(x / 100 * Double(rep.pixelsWide)), y: Int(y / 50 * Double(rep.pixelsHigh))))
  return try #require(pixel.usingColorSpace(.sRGB))
}

private func matches(_ color: NSColor, _ expected: NSColor) -> Bool {
  let expected = expected.usingColorSpace(.sRGB)!
  return abs(color.alphaComponent - 1) < 0.01
    && abs(color.redComponent - expected.redComponent) < 0.25
    && abs(color.greenComponent - expected.greenComponent) < 0.25
    && abs(color.blueComponent - expected.blueComponent) < 0.25
}

@MainActor
private func mouse(
  _ type: NSEvent.EventType, x: Double = 0, y: Double = 0, clicks: Int = 1,
  timestamp: TimeInterval = 0, shift: Bool = false
) throws -> NSEvent {
  try #require(
    NSEvent.mouseEvent(
      with: type, location: NSPoint(x: x, y: y), modifierFlags: shift ? [.shift] : [],
      timestamp: timestamp,
      windowNumber: 0, context: nil, eventNumber: 0, clickCount: clicks, pressure: 1))
}

@MainActor
@Test func hostRendersDeclaredLayoutForModuleWithoutUICode() throws {
  _ = NSApplication.shared
  let package = try LayoutPackage()
  let scan = ModuleDiscovery.scan([package.directory])
  #expect(scan.failures.isEmpty)
  #expect(scan.packages.first?.manifest.ui == .init(layout: "layout.json"))
  let module = try LoadedModule(
    manifest: package.manifest, directory: package.directory, category: "pre-fx")
  #expect(module.uiAPI == nil)
  #expect(module.layout?.path == "layout.json")

  let lease = try ModulePanelLease(loading: module)
  lease.view.frame = NSRect(x: 0, y: 0, width: 100, height: 50)
  #expect(lease.view.subviews.count == 3)
  var image = try render(lease.view)
  #expect(matches(try color(image, 20, 20), LayoutPackage.green))
  #expect(matches(try color(image, 70, 20), .black))
  #expect(matches(try color(image, 50, 40), .gray))

  try module.setValue(2, 1)
  try module.setValue(1, 1)
  lease.refresh()
  image = try render(lease.view)
  #expect(matches(try color(image, 20, 20), LayoutPackage.blue))
  #expect(matches(try color(image, 70, 20), .white))

  try module.setValue(2, 0)
  lease.refresh()
  #expect(matches(try color(try render(lease.view), 20, 20), LayoutPackage.red))
}

@MainActor
@Test func layoutControlsRouteToModuleParameters() throws {
  _ = NSApplication.shared
  let package = try LayoutPackage()
  let module = try LoadedModule(
    manifest: package.manifest, directory: package.directory, category: "pre-fx")
  let lease = try ModulePanelLease(loading: module)
  let view = lease.view
  view.frame = NSRect(x: 0, y: 0, width: 200, height: 100)
  view.layoutSubtreeIfNeeded()
  var delivered: [(UInt32, Float)] = []
  module.onParameterChange = { delivered.append(($0, $1)) }

  let power = try #require(view.hitTest(NSPoint(x: 140, y: 50)))
  #expect(power === view.subviews[1])
  power.mouseDown(with: try mouse(.leftMouseDown))
  #expect(try module.value(1) == 1)
  power.mouseDown(with: try mouse(.leftMouseDown))
  #expect(try module.value(1) == 0)
  view.subviews[2].mouseDown(with: try mouse(.leftMouseDown))
  #expect(try module.value(1) == 0)

  let knob = view.subviews[0]
  #expect(knob.accessibilityLabel() == "Gain")
  #expect(knob.accessibilityRole() == .slider)
  knob.setAccessibilityValue(0.25)
  #expect(try module.value(2) == 0.25)
  #expect(knob.accessibilityPerformIncrement())
  #expect(abs(try module.value(2) - 0.26) < 1e-6)
  #expect((knob.accessibilityValue() as? NSNumber)?.floatValue == (try module.value(2)))
  knob.setAccessibilityValue(7)
  #expect(try module.value(2) == 1)

  knob.mouseDown(with: try mouse(.leftMouseDown, clicks: 2))
  #expect(try module.value(2) == 0.5)
  knob.mouseDown(with: try mouse(.leftMouseDown))
  knob.mouseDragged(with: try mouse(.leftMouseDragged, y: 36))
  #expect(abs(try module.value(2) - 0.6) < 1e-5)
  #expect(delivered.map(\.0) == [1, 1, 2, 2, 2, 2, 2])

  module.onParameterChange = { _, _ in throw AmpError.message("rejected") }
  knob.setAccessibilityValue(0.1)
  #expect(abs(try module.value(2) - 0.6) < 1e-5)
}

@MainActor
@Test func invalidLayoutsFailWithReadableErrors() throws {
  let package = try LayoutPackage()
  #expect(package.validate() == nil)
  func failure(_ layout: [String: Any]) throws -> String {
    try package.write(layout: layout)
    return try #require(package.validate())
  }
  var layout = LayoutPackage.layout()
  layout["widgets"] = [
    ["name": "Laser", "type": "laserKnob", "rect": [0, 0, 1, 1], "id": 2]
  ]
  #expect(try failure(layout).contains("widget 0 (Laser): unknown type \"laserKnob\""))
  layout = LayoutPackage.layout()
  layout["version"] = nil
  #expect(try failure(layout).contains("version"))
  layout["version"] = 3
  #expect(try failure(layout).contains("version 3 is not supported"))
  layout["version"] = 1
  layout["crop"] = [0, 0, 0, 50]
  #expect(try failure(layout).contains("crop"))
  layout["crop"] = [0, 0, 100, 50]
  layout["widgets"] = Array(
    repeating: ["type": "meter", "rect": [0, 0, 1, 1], "id": 1] as [String: Any], count: 513)
  #expect(try failure(layout).contains("at most 512"))
  func widget(_ changes: [String: Any]) -> [String: Any] {
    var result = LayoutPackage.layout()
    var knob = (result["widgets"] as! [[String: Any]])[0]
    knob.merge(changes) { $1 }
    result["widgets"] = [knob]
    return result
  }
  #expect(try failure(widget(["rect": [0, 0, 1]])).contains("rect"))
  #expect(try failure(widget(["id": -1])).contains("id must be a parameter ID"))
  #expect(try failure(widget(["minimum": 2])).contains("minimum exceeds maximum"))
  #expect(try failure(widget(["curve": [0, 1, 0]])).contains("curve"))
  #expect(try failure(widget(["frames": 0])).contains("frames"))
  #expect(try failure(widget(["images": [] as [String]])).contains("needs an image"))
  #expect(try failure(widget(["label": 3])).contains("label must be a string"))

  #expect(try failure(widget(["images": ["absent.png"]])).contains("missing asset absent.png"))
  #expect(try failure(widget(["images": ["../knob.png"]])).contains("relative path"))
  #expect(try failure(widget(["images": ["/etc/hosts"]])).contains("relative path"))
  let outside = FileManager.default.temporaryDirectory.appendingPathComponent(
    UUID().uuidString + ".png")
  try LayoutPackage.image(outside, 10, 10, [.black])
  defer { try? FileManager.default.removeItem(at: outside) }
  try FileManager.default.createSymbolicLink(
    at: package.directory.appendingPathComponent("linked.png"), withDestinationURL: outside)
  #expect(try failure(widget(["images": ["linked.png"]])).contains("escapes"))
  try Data("not an image".utf8).write(to: package.directory.appendingPathComponent("text.png"))
  #expect(try failure(widget(["images": ["text.png"]])).contains("not a supported image"))

  try Data("{".utf8).write(to: package.directory.appendingPathComponent("layout.json"))
  #expect(package.validate()?.hasPrefix("layout.json: ") == true)
}

@MainActor
@Test func discoveryRejectsInvalidLayoutDeclarations() throws {
  let package = try LayoutPackage()
  var layout = LayoutPackage.layout()
  layout["widgets"] = [["type": "laserKnob", "rect": [0, 0, 1, 1], "id": 2]]
  try package.write(layout: layout)
  var scan = ModuleDiscovery.scan([package.directory])
  #expect(scan.packages.isEmpty)
  #expect(scan.failures.first?.contains("Invalid module layout layout.json") == true)

  try package.write(layout: LayoutPackage.layout())
  for path in ["../layout.json", "/layout.json", "missing.json"] {
    package.manifest.ui = .init(layout: path)
    try package.save()
    scan = ModuleDiscovery.scan([package.directory])
    #expect(scan.packages.isEmpty)
    #expect(scan.failures.count == 1)
  }

  let art = package.directory.appendingPathComponent("art")
  try FileManager.default.createDirectory(at: art, withIntermediateDirectories: true)
  package.manifest = ModuleManifest(
    ui: .init(layout: "layout.json"), schemaVersion: 1, id: package.manifest.id,
    category: "pre-fx", binary: "custom.so", icon: "custom.svg", resources: "art")
  try package.save()
  scan = ModuleDiscovery.scan([package.directory])
  #expect(scan.failures.first?.contains("must be inside its resources") == true)
}

@MainActor
@Test func missingLayoutAssetFailsClosedWhenPanelOpens() throws {
  _ = NSApplication.shared
  let package = try LayoutPackage()
  let module = try LoadedModule(
    manifest: package.manifest, directory: package.directory, category: "pre-fx")
  try FileManager.default.removeItem(at: package.directory.appendingPathComponent("knob.png"))
  #expect(ModulePanelLease(module: module) == nil)
  do {
    _ = try ModulePanelLease(loading: module)
    Issue.record("A panel opened without its artwork")
  } catch {
    #expect(error.localizedDescription.contains("invalid module layout"))
    #expect(error.localizedDescription.contains("missing asset knob.png"))
  }
  let panel = NativeModulePanel.Container(module: module)
  defer { panel.stop() }
  #expect(panel.lease == nil)
  let label = try #require(panel.subviews.first as? NSTextField)
  #expect(label.stringValue.contains("missing asset knob.png"))
}

@MainActor
@Test func ampEqualizerCompanionRendersItsDeclaredLayout() throws {
  _ = NSApplication.shared
  let fixture = try TestModulePackage(category: "amp", equalizer: "org.example.test-amp.eq")
  let directory = fixture.directory
  try LayoutPackage.image(directory.appendingPathComponent("background.png"), 100, 50, [.gray])
  try LayoutPackage.image(
    directory.appendingPathComponent("knob.png"), 10, 30,
    [LayoutPackage.red, LayoutPackage.green, LayoutPackage.blue])
  try LayoutPackage.image(directory.appendingPathComponent("off.png"), 10, 10, [.black])
  try LayoutPackage.image(directory.appendingPathComponent("on.png"), 10, 10, [.white])
  try JSONSerialization.data(withJSONObject: LayoutPackage.layout()).write(
    to: directory.appendingPathComponent("eq-layout.json"))
  var manifest = fixture.manifest
  manifest.eq?.ui = .init(layout: "eq-layout.json")
  try JSONEncoder().encode(manifest).write(to: directory.appendingPathComponent("module.json"))
  #expect(ModuleDiscovery.scan([directory]).failures.isEmpty)

  let module = try LoadedModule(manifest: manifest, directory: directory)
  #expect(module.uiAPI != nil && module.layout == nil)
  let equalizer = try #require(module.equalizer)
  #expect(equalizer.uiAPI == nil)
  #expect(equalizer.layout?.path == "eq-layout.json")
  let lease = try ModulePanelLease(loading: equalizer)
  lease.view.frame = NSRect(x: 0, y: 0, width: 100, height: 50)
  lease.view.subviews[1].mouseDown(with: try mouse(.leftMouseDown))
  #expect(try equalizer.value(1) == 1)
  #expect(try module.value(1) == 0)

  var layout = LayoutPackage.layout()
  layout["version"] = 3
  try JSONSerialization.data(withJSONObject: layout).write(
    to: directory.appendingPathComponent("eq-layout.json"))
  let scan = ModuleDiscovery.scan([directory])
  #expect(scan.packages.isEmpty)
  #expect(scan.failures.first?.contains("eq-layout.json: version 3 is not supported") == true)
  manifest.eq?.ui = .init(layout: "../eq-layout.json")
  try JSONEncoder().encode(manifest).write(to: directory.appendingPathComponent("module.json"))
  #expect(ModuleDiscovery.scan([directory]).failures.count == 1)
}

@MainActor
@Test func anchorPlacesLetterboxedLayoutsAndInterpolationIsValidated() throws {
  _ = NSApplication.shared
  let package = try LayoutPackage()
  func knobTop(_ anchor: String?) throws -> CGFloat {
    var layout = LayoutPackage.layout()
    layout["anchor"] = anchor
    layout["interpolation"] = "high"
    try package.write(layout: layout)
    let module = try LoadedModule(
      manifest: package.manifest, directory: package.directory, category: "pre-fx")
    let lease = try ModulePanelLease(loading: module)
    lease.view.frame = NSRect(x: 0, y: 0, width: 100, height: 150)
    lease.view.layoutSubtreeIfNeeded()
    return lease.view.subviews[0].frame.minY
  }
  #expect(try knobTop(nil) == 60)
  #expect(try knobTop("center") == 60)
  #expect(try knobTop("top") == 10)
  #expect(try knobTop("bottom") == 110)

  var layout = LayoutPackage.layout()
  layout["anchor"] = "left"
  try package.write(layout: layout)
  #expect(package.validate()?.contains("anchor must be") == true)
  layout["anchor"] = nil
  layout["interpolation"] = "bicubic"
  try package.write(layout: layout)
  #expect(package.validate()?.contains("interpolation must be") == true)
  layout["interpolation"] = nil
  var widgets = layout["widgets"] as! [[String: Any]]
  widgets[0]["interpolation"] = 2
  layout["widgets"] = widgets
  try package.write(layout: layout)
  #expect(package.validate()?.contains("widget 0 (Gain): interpolation must be") == true)
}

@MainActor
@Test func cycleStepTapSideEffectMomentaryImageAndTextStyle() throws {
  _ = NSApplication.shared
  let package = try LayoutPackage()
  try package.write(
    layout: LayoutPackage.layout(widgets: [
      [
        "name": "Cycle", "type": "threeWayImageSwitch", "rect": [10, 10, 20, 20],
        "images": ["off.png", "on.png", "knob.png"], "id": 2, "minimum": 0, "maximum": 1,
        "step": -0.5,
      ],
      [
        "name": "Tap", "type": "imageButton", "rect": [60, 10, 20, 20],
        "images": ["off.png", "on.png"], "id": 2, "tap": true, "momentary": true,
        "sets": [[1, 1]],
      ],
      [
        "name": "Readout", "type": "textEditor", "rect": [0, 35, 100, 15], "id": 2,
        "readonly": true, "textStyle": ["font": "monospaced", "size": 8, "color": [0.12, 1]],
      ],
    ]))
  #expect(package.validate() == nil)
  let module = try LoadedModule(
    manifest: package.manifest, directory: package.directory, category: "pre-fx")
  let lease = try ModulePanelLease(loading: module)
  lease.view.frame = NSRect(x: 0, y: 0, width: 100, height: 50)
  lease.view.layoutSubtreeIfNeeded()
  let cycle = lease.view.subviews[0]
  let tap = lease.view.subviews[1]

  try module.setValue(2, 0.5)
  cycle.mouseDown(with: try mouse(.leftMouseDown))
  #expect(try module.value(2) == 0)
  cycle.mouseDown(with: try mouse(.leftMouseDown))
  #expect(try module.value(2) == 1)

  try module.setValue(2, 0)
  #expect(matches(try color(try render(lease.view), 70, 20), .black))
  tap.mouseDown(with: try mouse(.leftMouseDown, timestamp: 10))
  #expect(matches(try color(try render(lease.view), 70, 20), .white))
  tap.mouseUp(with: try mouse(.leftMouseUp, timestamp: 10))
  #expect(matches(try color(try render(lease.view), 70, 20), .black))
  #expect(try module.value(1) == 0)
  tap.mouseDown(with: try mouse(.leftMouseDown, timestamp: 10.1))
  #expect(try module.value(2) == 0)
  #expect(try module.value(1) == 0)
  tap.mouseDown(with: try mouse(.leftMouseDown, timestamp: 10.6))
  #expect(abs(try module.value(2) - 0.4) < 1e-5)
  #expect(try module.value(1) == 1)

  var layout = LayoutPackage.layout()
  func widget(_ changes: [String: Any]) throws -> String? {
    var knob = (LayoutPackage.layout()["widgets"] as! [[String: Any]])[0]
    knob.merge(changes) { $1 }
    layout["widgets"] = [knob]
    try package.write(layout: layout)
    return package.validate()
  }
  #expect(try widget(["step": 0])?.contains("step must not be zero") == true)
  #expect(try widget(["sets": [[1]]])?.contains("sets must contain") == true)
  #expect(try widget(["sets": [[-1, 0]]])?.contains("sets must contain") == true)
  #expect(try widget(["momentary": "yes"])?.contains("momentary must be") == true)
  for style: Any in [
    "small", ["font": "serif"], ["size": 0], ["align": "justified"], ["color": [0.5]],
    ["color": [2, 1]],
  ] {
    #expect(try widget(["textStyle": style])?.contains("textStyle needs") == true)
  }
  #expect(try widget(["textStyle": ["align": "left", "color": [1, 0, 0, 1]]]) == nil)
}

@MainActor
@Test func degenerateFramesDrawNothingInsteadOfRaising() throws {
  _ = NSApplication.shared
  let package = try LayoutPackage()
  try package.write(
    layout: LayoutPackage.layout(widgets: [
      [
        "name": "Styled", "type": "textEditor", "rect": [0, 35, 70, 14], "id": 2,
        "readonly": true, "textStyle": ["font": "monospaced", "size": 8, "color": [0.12, 1]],
      ],
      ["name": "Plain", "type": "textEditor", "rect": [75, 35, 20, 14], "id": 2],
      [
        "name": "Knob", "type": "rotaryFilmStripKnob", "rect": [10, 10, 20, 20],
        "images": ["knob.png"], "id": 2,
      ],
    ]))
  #expect(package.validate() == nil)
  let module = try LoadedModule(
    manifest: package.manifest, directory: package.directory, category: "pre-fx")
  let lease = try ModulePanelLease(loading: module)
  for size in [
    CGSize(width: 100, height: 50), .zero, CGSize(width: 0.5, height: 0.5),
    CGSize(width: 1, height: 1),
    CGSize(width: 2, height: 2), CGSize(width: 5, height: 900),
  ] {
    lease.view.frame = NSRect(origin: .zero, size: size)
    lease.view.layoutSubtreeIfNeeded()
    let bounds = NSRect(x: 0, y: 0, width: max(size.width, 1), height: max(size.height, 1))
    let rep = try #require(lease.view.bitmapImageRepForCachingDisplay(in: bounds))
    lease.view.cacheDisplay(in: lease.view.bounds, to: rep)
  }
}

@MainActor
private func key(_ code: UInt16, shift: Bool = false) throws -> NSEvent {
  try #require(
    NSEvent.keyEvent(
      with: .keyDown, location: .zero, modifierFlags: shift ? [.shift] : [], timestamp: 0,
      windowNumber: 0, context: nil, characters: "", charactersIgnoringModifiers: "",
      isARepeat: false, keyCode: code))
}

@MainActor
private func open(_ package: LayoutPackage, width: Double = 100, height: Double = 50) throws -> (
  LoadedModule, ModulePanelLease
) {
  let module = try LoadedModule(
    manifest: package.manifest, directory: package.directory, category: "pre-fx")
  let lease = try ModulePanelLease(loading: module)
  lease.view.frame = NSRect(x: 0, y: 0, width: width, height: height)
  lease.view.layoutSubtreeIfNeeded()
  return (module, lease)
}

@MainActor
private func version2(_ widgets: [[String: Any]], _ changes: [String: Any] = [:]) -> [String: Any] {
  var layout = LayoutPackage.layout(widgets: widgets)
  layout["version"] = 2
  layout.merge(changes) { $1 }
  return layout
}

@MainActor
@Test func toggleWidgetsToggleThroughAccessibilityAndKeys() throws {
  _ = NSApplication.shared
  let package = try LayoutPackage()
  try package.write(
    layout: LayoutPackage.layout(widgets: [
      [
        "name": "Power", "type": "imageButton", "rect": [60, 10, 20, 20],
        "images": ["off.png", "on.png"], "id": 1, "label": "Power",
      ],
      [
        "name": "Mode", "type": "threeWayImageSwitch", "rect": [10, 10, 20, 20],
        "images": ["off.png", "on.png", "knob.png"], "id": 2, "minimum": 0, "maximum": 1,
        "step": 0.5, "label": "Mode",
      ],
    ]))
  let (module, lease) = try open(package)
  let power = lease.view.subviews[0]
  let mode = lease.view.subviews[1]

  #expect(power.accessibilityRole() == .checkBox)
  #expect(power.accessibilityValueDescription() == "Off")
  #expect(power.accessibilityPerformIncrement())
  #expect(try module.value(1) == 1)
  #expect(power.accessibilityValueDescription() == "On")
  #expect((power.accessibilityValue() as? NSNumber)?.intValue == 1)
  #expect(power.accessibilityPerformIncrement())
  #expect(try module.value(1) == 0)
  #expect(power.accessibilityPerformDecrement())
  #expect(try module.value(1) == 1)
  #expect(power.accessibilityPerformPress())
  #expect(try module.value(1) == 0)
  power.setAccessibilityValue(0.8)
  #expect(try module.value(1) == 1)
  power.keyDown(with: try key(49))
  #expect(try module.value(1) == 0)
  power.keyDown(with: try key(126))
  #expect(try module.value(1) == 1)

  try module.setValue(2, 0)
  #expect(mode.accessibilityRole() == .slider)
  #expect(mode.accessibilityPerformIncrement())
  #expect(try module.value(2) == 0.5)
  #expect(mode.accessibilityPerformIncrement())
  #expect(mode.accessibilityPerformIncrement())
  #expect(try module.value(2) == 1)
  #expect(mode.accessibilityPerformPress())
  #expect(try module.value(2) == 0)
  #expect(mode.accessibilityPerformDecrement())
  #expect(try module.value(2) == 0)
}

@MainActor
@Test func continuousControlsHaveStepsKeysAndValueDescriptions() throws {
  _ = NSApplication.shared
  let package = try LayoutPackage()
  try package.write(
    layout: version2([
      [
        "name": "Gain", "type": "rotaryFilmStripKnob", "rect": [10, 10, 20, 20],
        "images": ["knob.png"], "id": 2, "initial": 0.5, "label": "Gain",
        "help": "Input gain.", "step": 0.05, "fineStep": 0.002,
      ]
    ]))
  let (module, lease) = try open(package)
  let knob = lease.view.subviews[0]
  #expect(knob.toolTip == "Input gain.")
  #expect(knob.accessibilityHelp() == "Input gain.")
  try module.setValue(2, 0.5)
  #expect(knob.accessibilityValueDescription() == "50%")
  #expect(knob.accessibilityPerformIncrement())
  #expect(abs(try module.value(2) - 0.55) < 1e-6)
  knob.keyDown(with: try key(125, shift: true))
  #expect(abs(try module.value(2) - 0.548) < 1e-6)
  knob.keyDown(with: try key(121))
  #expect(abs(try module.value(2) - 0.048) < 1e-6)
  knob.keyDown(with: try key(116))
  #expect(abs(try module.value(2) - 0.548) < 1e-6)
  knob.keyDown(with: try key(119))
  #expect(try module.value(2) == 1)
  knob.keyDown(with: try key(115))
  #expect(try module.value(2) == 0)
  knob.keyDown(with: try key(117))
  #expect(try module.value(2) == 0.5)
  try module.setValue(2, 0.3)
  knob.keyDown(with: try key(51))
  #expect(try module.value(2) == 0.5)

  try package.write(layout: LayoutPackage.layout())
  let (legacy, legacyLease) = try open(package)
  let legacyKnob = legacyLease.view.subviews[0]
  try legacy.setValue(2, 0.5)
  legacyKnob.keyDown(with: try key(126, shift: true))
  #expect(abs(try legacy.value(2) - 0.501) < 1e-6)
  legacyKnob.keyDown(with: try key(116))
  #expect(abs(try legacy.value(2) - 0.601) < 1e-6)
}

@MainActor
@Test func pixelRatioAndDragTravelSetDragDistances() throws {
  _ = NSApplication.shared
  let package = try LayoutPackage()
  func drag(_ layout: [String: Any]) throws -> Float {
    try package.write(layout: layout)
    let (module, lease) = try open(package, width: 200, height: 100)
    let knob = lease.view.subviews[0]
    try module.setValue(2, 0.5)
    knob.mouseDown(with: try mouse(.leftMouseDown))
    knob.mouseDragged(with: try mouse(.leftMouseDragged, y: 36))
    return try module.value(2)
  }
  let knob: [String: Any] = [
    "name": "Gain", "type": "rotaryFilmStripKnob", "rect": [10, 10, 20, 20],
    "images": ["knob.png"], "id": 2,
  ]
  #expect(abs(try drag(version2([knob])) - 0.6) < 1e-5)
  #expect(abs(try drag(version2([knob], ["pixelRatio": 2])) - 0.55) < 1e-5)
  var traveled = knob
  traveled["dragTravel"] = 90
  #expect(abs(try drag(version2([traveled], ["pixelRatio": 2])) - 0.7) < 1e-5)
}

@MainActor
@Test func travellingSliderHandleFollowsAnotherParameterWithinItsHitRect() throws {
  _ = NSApplication.shared
  let package = try LayoutPackage()
  try package.write(
    layout: version2([
      [
        "name": "Band", "type": "imageSlider", "rect": [40, 10, 10, 30],
        "images": ["off.png", "on.png"], "id": 2, "handle": [20, 10],
        "handleTravel": [45, 35, 45, 15], "handleAnchor": [10, 5], "imageFollows": [1, 0, 1],
        "label": "Band", "initial": 0.5,
      ]
    ]))
  #expect(package.validate() == nil)
  let (module, lease) = try open(package)
  let slider = lease.view.subviews[0]
  #expect(slider.frame == NSRect(x: 35, y: 10, width: 20, height: 30))
  try module.setValue(1, 0)
  try module.setValue(2, 0)
  lease.refresh()
  var image = try render(lease.view)
  #expect(matches(try color(image, 38, 35), .black))
  #expect(matches(try color(image, 38, 15), .gray))
  try module.setValue(2, 1)
  lease.refresh()
  image = try render(lease.view)
  #expect(matches(try color(image, 38, 15), .black))
  #expect(matches(try color(image, 38, 35), .gray))
  try module.setValue(1, 1)
  lease.refresh()
  #expect(matches(try color(try render(lease.view), 38, 15), .white))

  #expect(lease.view.hitTest(NSPoint(x: 45, y: 25)) === slider)
  #expect(lease.view.hitTest(NSPoint(x: 37, y: 25)) !== slider)

  lease.view.frame = NSRect(x: 0, y: 0, width: 200, height: 100)
  lease.view.layoutSubtreeIfNeeded()
  try module.setValue(2, 0.5)
  slider.mouseDown(with: try mouse(.leftMouseDown))
  slider.mouseDragged(with: try mouse(.leftMouseDragged, y: 10))
  #expect(abs(try module.value(2) - 0.75) < 1e-5)
  slider.mouseDown(with: try mouse(.leftMouseDown, clicks: 2))
  #expect(try module.value(2) == 0.5)
}

@MainActor
@Test func travelAxisSlidersDragAlongArbitraryHandleTravel() throws {
  _ = NSApplication.shared
  let package = try LayoutPackage()
  let horizontal: [String: Any] = [
    "name": "Rate", "type": "imageSlider", "rect": [10, 10, 80, 10], "images": ["off.png"],
    "id": 2, "handle": [4, 10], "handleTravel": [12, 15, 88, 15], "dragAxis": "travel",
    "label": "Rate", "initial": 0.5, "step": 0.1, "fineStep": 0.01,
  ]
  func drag(
    _ changes: [String: Any], x: Double, y: Double = 0, shift: Bool = false
  ) throws -> Float {
    try package.write(layout: version2([horizontal.merging(changes) { $1 }]))
    #expect(package.validate() == nil)
    let (module, lease) = try open(package, width: 200, height: 100)
    let slider = lease.view.subviews[0]
    try module.setValue(2, 0.5)
    slider.mouseDown(with: try mouse(.leftMouseDown))
    slider.mouseDragged(with: try mouse(.leftMouseDragged, x: x, y: y, shift: shift))
    return try module.value(2)
  }
  #expect(abs(try drag([:], x: 38) - 0.75) < 1e-5)
  #expect(abs(try drag([:], x: -38) - 0.25) < 1e-5)
  #expect(try drag([:], x: 0, y: 50) == 0.5)
  #expect(abs(try drag([:], x: 38, shift: true) - 0.525) < 1e-5)
  #expect(abs(try drag(["invert": true], x: 38) - 0.25) < 1e-5)
  #expect(abs(try drag(["dragTravel": 38], x: 19) - 0.75) < 1e-5)
  #expect(try drag([:], x: 500) == 1)
  #expect(try drag([:], x: -500) == 0)
  #expect(abs(try drag(["handleTravel": [88, 15, 12, 15]], x: 38) - 0.25) < 1e-5)
  let diagonal: [String: Any] = ["rect": [10, 0, 30, 40], "handleTravel": [10, 40, 40, 0]]
  #expect(abs(try drag(diagonal, x: 6, y: 8) - 0.6) < 1e-5)
  #expect(abs(try drag(diagonal, x: -6, y: -8) - 0.4) < 1e-5)
  #expect(try drag(diagonal, x: 8, y: -6) == 0.5)
  #expect(abs(try drag(["handleTravel": [40, 0, 10, 40]], x: -6, y: -8) - 0.6) < 1e-5)
  #expect(abs(try drag(["handleTravel": [12, 15, 13, 15]], x: 1) - 1) < 1e-5)
  #expect(
    abs(
      try drag(["dragAxis": "vertical", "handleTravel": [45, 35, 45, 15]], x: 0, y: 10)
        - 0.75) < 1e-5)
  #expect(try drag(["dragAxis": "vertical", "handleTravel": [45, 35, 45, 15]], x: 40) == 0.5)

  try package.write(layout: version2([horizontal]))
  let (module, lease) = try open(package)
  let slider = lease.view.subviews[0]
  #expect(slider.frame == NSRect(x: 10, y: 10, width: 80, height: 10))
  #expect(slider.accessibilityRole() == .slider)
  try module.setValue(2, 0)
  lease.refresh()
  var image = try render(lease.view)
  #expect(matches(try color(image, 12, 15), .black))
  #expect(matches(try color(image, 87, 15), .gray))
  try module.setValue(2, 1)
  lease.refresh()
  image = try render(lease.view)
  #expect(matches(try color(image, 87, 15), .black))
  #expect(matches(try color(image, 12, 15), .gray))
  #expect(lease.view.hitTest(lease.view.convert(NSPoint(x: 50, y: 15), to: nil)) === slider)
  #expect(lease.view.hitTest(lease.view.convert(NSPoint(x: 50, y: 25), to: nil)) === lease.view)

  try module.setValue(2, 0.5)
  slider.keyDown(with: try key(124))
  #expect(abs(try module.value(2) - 0.6) < 1e-6)
  slider.keyDown(with: try key(123, shift: true))
  #expect(abs(try module.value(2) - 0.59) < 1e-6)
  slider.keyDown(with: try key(125))
  #expect(abs(try module.value(2) - 0.49) < 1e-6)
  #expect(slider.accessibilityPerformIncrement())
  #expect(abs(try module.value(2) - 0.59) < 1e-6)
  slider.mouseDown(with: try mouse(.leftMouseDown, clicks: 2))
  #expect(try module.value(2) == 0.5)
}

@MainActor
@Test func clickToEditReadoutsFormatParseAndShareAFittedSize() throws {
  _ = NSApplication.shared
  let package = try LayoutPackage()
  let gain: [String: Any] = ["range": [-12, 12], "decimals": 1, "unit": " dB", "signed": true]
  let cutoff: [String: Any] = [
    "range": [1000, 20000], "skew": 0.6, "unit": " Hz", "kiloUnit": " kHz", "off": "maximum",
  ]
  let style: [String: Any] = ["font": "monospacedDigit", "size": 20, "fit": true]
  try package.write(
    layout: version2([
      [
        "name": "Gain value", "type": "textEditor", "editing": "click", "rect": [0, 35, 50, 15],
        "id": 2, "format": gain, "textStyle": style, "label": "Gain value", "help": "Type a gain.",
      ],
      [
        "name": "Cutoff value", "type": "textEditor", "editing": "click", "rect": [50, 35, 12, 15],
        "id": 2, "format": cutoff, "textStyle": style,
      ],
      [
        "name": "Gain", "type": "rotaryFilmStripKnob", "rect": [10, 10, 20, 20],
        "images": ["knob.png"], "id": 2, "format": gain, "label": "Gain",
      ],
    ]))
  #expect(package.validate() == nil)
  let (module, lease) = try open(package)
  let fields = try lease.view.subviews[0...1].map {
    try #require($0.subviews.first as? NSTextField)
  }
  #expect(lease.view.subviews[0].isAccessibilityElement() == false)
  #expect(fields[0].accessibilityLabel() == "Gain value")
  #expect(fields[0].toolTip == "Type a gain.")
  #expect(fields[0].isEditable)
  try module.setValue(2, 0.5)
  lease.refresh()
  #expect(fields[0].stringValue == "0.0 dB")
  #expect(fields[1].stringValue == "6.98 kHz")
  #expect(lease.view.subviews[2].accessibilityValueDescription() == "0.0 dB")
  for nearCentre: Float in [0.499, 0.501] {
    try module.setValue(2, nearCentre)
    lease.refresh()
    #expect(fields[0].stringValue == "0.0 dB")
  }
  try module.setValue(2, 0.75)
  lease.refresh()
  #expect(fields[0].stringValue == "+6.0 dB")
  try module.setValue(2, 0.25)
  lease.refresh()
  #expect(fields[0].stringValue == "-6.0 dB")
  try module.setValue(2, 1)
  lease.refresh()
  #expect(fields[1].stringValue == "OFF")
  try module.setValue(2, 0.999)
  lease.refresh()
  #expect(fields[1].stringValue == "20.0 kHz")

  let size = try #require(fields[0].font?.pointSize)
  #expect(size == fields[1].font?.pointSize)
  #expect(size < 10)

  func type(_ text: String, _ field: Int) -> Float? {
    fields[field].stringValue = text
    _ = fields[field].target?.perform(fields[field].action, with: fields[field])
    return try? module.value(2)
  }
  #expect(abs(type("-3,5 dB", 0)! - Float((-3.5 + 12) / 24)) < 1e-6)
  #expect(fields[0].stringValue == "-3.5 dB")
  #expect(type("99", 0) == 1)
  try module.setValue(2, 0.3)
  #expect(type("loud", 0) == 0.3)
  #expect(fields[0].stringValue == "-4.8 dB")
  #expect(type("off", 1) == 1)
  #expect(type("20000", 1) == Float(1).nextDown)
  #expect(fields[1].stringValue == "20.0 kHz")
  #expect(abs(type("5", 1)! - Float(pow((5000.0 - 1000) / 19000, 0.6))) < 1e-6)
  #expect(abs(type("2.5k", 1)! - Float(pow((2500.0 - 1000) / 19000, 0.6))) < 1e-6)
  #expect(type("100", 1) == 0)
}

@MainActor
@Test func formatOffEndsAndToleranceAndReadOnlyReadouts() throws {
  _ = NSApplication.shared
  let package = try LayoutPackage()
  let corner: [String: Any] = [
    "range": [20, 500], "skew": 0.6, "unit": " Hz", "kiloUnit": " kHz", "off": "minimum",
  ]
  var tolerant = corner
  tolerant["offTolerance"] = 1e-4
  try package.write(
    layout: version2([
      [
        "name": "Exact", "type": "textEditor", "editing": "click", "rect": [0, 35, 50, 15],
        "id": 2, "format": corner,
      ],
      [
        "name": "Tolerant", "type": "textEditor", "editing": "click", "rect": [50, 35, 50, 15],
        "id": 2, "format": tolerant, "readonly": true,
      ],
      [
        "name": "Percent", "type": "textEditor", "rect": [0, 0, 50, 15], "id": 2,
        "format": ["range": [0, 100], "unit": "%"],
      ],
    ]))
  let (module, lease) = try open(package)
  let exact = try #require(lease.view.subviews[0].subviews.first as? NSTextField)
  let tolerant2 = try #require(lease.view.subviews[1].subviews.first as? NSTextField)
  #expect(!tolerant2.isEditable)
  #expect(lease.view.hitTest(NSPoint(x: 75, y: 40)) === lease.view)
  for (value, first, second) in [
    (Float(0), "OFF", "OFF"), (Float(1e-7), "20 Hz", "OFF"), (Float(0.5), "171 Hz", "171 Hz"),
    (Float(1), "500 Hz", "500 Hz"),
  ] {
    try module.setValue(2, value)
    lease.refresh()
    #expect(exact.stringValue == first)
    #expect(tolerant2.stringValue == second)
  }
  try module.setValue(2, 0.125)
  #expect(lease.view.subviews[2].accessibilityValueDescription() == "13%")
  exact.stringValue = "20"
  _ = exact.target?.perform(exact.action, with: exact)
  #expect(try module.value(2) == Float.leastNonzeroMagnitude)
  #expect(exact.stringValue == "20 Hz")
}

@MainActor
@Test func version2IndicatorsLabelsRasterAndViewHelp() throws {
  _ = NSApplication.shared
  let package = try LayoutPackage()
  try """
  <svg xmlns="http://www.w3.org/2000/svg" width="100" height="50">
  <rect width="100" height="50" fill="#ff0000"/></svg>
  """.write(
    to: package.directory.appendingPathComponent("labels.svg"), atomically: true, encoding: .utf8)
  let indicator: [String: Any] = [
    "name": "Led", "type": "image", "rect": [60, 10, 20, 20], "images": ["off.png", "on.png"],
    "id": 1,
  ]
  var layout = version2([indicator], ["labels": "labels.svg", "labelsRect": [0, 0, 100, 50]])
  layout["help"] = "Drag knobs."
  try package.write(layout: layout)
  var (module, lease) = try open(package)
  #expect(lease.view.toolTip == "Drag knobs.")
  #expect(matches(try color(try render(lease.view), 50, 40), LayoutPackage.red))

  try LayoutPackage.image(
    package.directory.appendingPathComponent("labels.png"), 100, 50, [LayoutPackage.green])
  (module, lease) = try open(package)
  let led = lease.view.subviews[0]
  #expect(matches(try color(try render(lease.view), 50, 40), LayoutPackage.green))
  #expect(lease.view.hitTest(NSPoint(x: 70, y: 20)) === lease.view)
  #expect(!led.isAccessibilityElement())
  led.mouseDown(with: try mouse(.leftMouseDown, clicks: 2))
  led.keyDown(with: try key(126))
  #expect(try module.value(1) == 0)
  try module.setValue(1, 1)
  lease.refresh()
  #expect(matches(try color(try render(lease.view), 70, 20), .white))

  layout["version"] = 1
  layout["widgets"] = LayoutPackage.layout()["widgets"]
  try package.write(layout: layout)
  (module, lease) = try open(package)
  #expect(matches(try color(try render(lease.view), 50, 40), LayoutPackage.red))
}

@MainActor
@Test func version2FieldsAreValidated() throws {
  let package = try LayoutPackage()
  let knob: [String: Any] = [
    "name": "Gain", "type": "rotaryFilmStripKnob", "rect": [10, 10, 20, 20],
    "images": ["knob.png"], "id": 2,
  ]
  let slider: [String: Any] = [
    "name": "Band", "type": "imageSlider", "rect": [40, 10, 10, 30], "images": ["off.png"],
    "id": 2, "handle": [10, 10], "handleTravel": [45, 35, 45, 15],
  ]
  let readout: [String: Any] = [
    "name": "Value", "type": "textEditor", "rect": [0, 35, 50, 15], "id": 2, "editing": "click",
  ]
  func failure(_ widget: [String: Any], _ changes: [String: Any], version: Int = 2) throws
    -> String?
  {
    var layout = version2([widget.merging(changes) { $1 }])
    layout["version"] = version
    try package.write(layout: layout)
    return package.validate()
  }
  for field: (String, Any) in [
    ("dragTravel", 90), ("fineStep", 0.1), ("format", ["unit": "%"]),
    ("imageFollows", [1, 0, 1]),
  ] {
    #expect(
      try failure(knob, [field.0: field.1], version: 1)?.contains(
        "widget 0 (Gain): \(field.0) requires layout version 2") == true)
  }
  #expect(try failure(slider, [:], version: 1)?.contains("handleTravel requires") == true)
  #expect(
    try failure(knob, ["dragAxis": "vertical"], version: 1)?.contains(
      "dragAxis requires layout version 2") == true)
  #expect(try failure(readout, [:], version: 1)?.contains("editing requires") == true)
  var layout = version2([knob], ["pixelRatio": 2])
  layout["version"] = 1
  try package.write(layout: layout)
  #expect(package.validate()?.contains("pixelRatio requires layout version 2") == true)
  layout["version"] = 3
  try package.write(layout: layout)
  #expect(package.validate()?.contains("version 3 is not supported") == true)
  for ratio: Any in [0, 65, "2"] {
    try package.write(layout: version2([knob], ["pixelRatio": ratio]))
    #expect(package.validate()?.contains("pixelRatio must be") == true)
  }
  try package.write(layout: version2([knob], ["help": 3]))
  #expect(package.validate()?.contains("help must be a string") == true)

  #expect(try failure(knob, [:]) == nil)
  #expect(try failure(slider, [:]) == nil)
  #expect(try failure(readout, [:]) == nil)
  for changes: [String: Any] in [
    ["dragAxis": "vertical"], ["dragAxis": "travel"],
    ["dragAxis": "travel", "handleTravel": [10, 15, 90, 15]],
    ["dragAxis": "travel", "handleTravel": [90, 15, 10, 15]],
    ["dragAxis": "travel", "handleTravel": [45, 15, 45, 35]],
    ["dragAxis": "travel", "handleTravel": [45, 15, 46, 15]],
    ["handleTravel": [45, 35, 45, 34.999]],
  ] {
    #expect(try failure(slider, changes) == nil, "\(changes)")
  }
  var plainSlider = slider
  plainSlider["handleTravel"] = nil
  #expect(try failure(plainSlider, ["dragAxis": "vertical"]) == nil)
  let cases: [([String: Any], [String: Any], String)] = [
    (knob, ["help": 1], "help must be a string"),
    (knob, ["dragTravel": 0], "dragTravel must be"),
    (knob, ["fineStep": -1], "fineStep must be positive"),
    (knob, ["step": -0.1], "step must be positive"),
    (knob, ["handleTravel": [0, 1, 0, 0]], "handleTravel applies only to imageSlider"),
    (knob, ["imageFollows": [1, 0, 1]], "imageFollows does not apply"),
    (slider, ["handleTravel": [45, 15, 45, 35]], "maximum above the minimum"),
    (slider, ["handleTravel": [45, 35, 45]], "handleTravel must be"),
    (slider, ["handleTravel": [10, 15, 90, 15]], "maximum above the minimum unless dragAxis"),
    (slider, ["dragAxis": "vertical", "handleTravel": [10, 15, 90, 15]], "maximum above"),
    (slider, ["dragAxis": "travel", "handleTravel": [45, 15, 45, 15]], "differ by at least 1"),
    (slider, ["dragAxis": "travel", "handleTravel": [45, 15, 45.5, 15.5]], "differ by at least"),
    (slider, ["dragAxis": "travel", "handleTravel": [45, 15, 45]], "handleTravel must be"),
    (plainSlider, ["dragAxis": "travel"], "dragAxis \"travel\" needs handleTravel"),
    (slider, ["dragAxis": "horizontal"], "dragAxis must be"),
    (slider, ["dragAxis": 1], "dragAxis must be"),
    (slider, ["dragAxis": ""], "dragAxis must be"),
    (slider, ["dragAxis": NSNull()], "dragAxis must be"),
    (knob, ["dragAxis": "vertical"], "dragAxis applies only to imageSlider"),
    (slider, ["handleAnchor": [5]], "handleAnchor must be"),
    (slider, ["imageFollows": [1, 1, 1]], "imageFollows must be"),
    (slider, ["imageFollows": [-1, 0, 1]], "imageFollows must be"),
    (knob, ["format": "dB"], "format must be an object"),
    (knob, ["format": ["range": [1, 1]]], "format range"),
    (knob, ["format": ["skew": 0]], "format skew"),
    (knob, ["format": ["decimals": 7]], "format decimals"),
    (knob, ["format": ["unit": 3]], "format unit"),
    (knob, ["format": ["off": "both"]], "format off"),
    (knob, ["format": ["offTolerance": 0.1]], "format offTolerance"),
    (knob, ["format": ["unit": "%"], "unit": "%"], "format replaces unit"),
    (knob, ["editing": "click"], "editing applies only to textEditor"),
    (readout, ["editing": "always"], "editing must be"),
    (readout, ["choices": ["A", "B"]], "cannot be combined with choices"),
    (readout, ["editing": "doubleClick", "textStyle": ["fit": true]], "textStyle fit"),
  ]
  for (widget, changes, message) in cases {
    #expect(try failure(widget, changes)?.contains(message) == true, "\(changes)")
  }
}

@MainActor
@Test func version2WidgetsSurviveDegenerateFrames() throws {
  _ = NSApplication.shared
  let package = try LayoutPackage()
  try package.write(
    layout: version2(
      [
        [
          "name": "Band", "type": "imageSlider", "rect": [40, 10, 10, 30],
          "images": ["off.png", "on.png"], "id": 2, "handle": [20, 10],
          "handleTravel": [45, 35, 45, 15], "imageFollows": [1, 0, 1],
        ],
        [
          "name": "Value", "type": "textEditor", "editing": "click", "rect": [0, 35, 50, 15],
          "id": 2, "textStyle": ["size": 200, "fit": true],
          "format": ["range": [-12, 12], "decimals": 6, "unit": " dB"],
        ],
        [
          "name": "Plain", "type": "textEditor", "editing": "click", "rect": [60, 35, 20, 15],
          "id": 2,
        ],
      ], ["pixelRatio": 2]))
  #expect(package.validate() == nil)
  let (_, lease) = try open(package)
  for size in [
    CGSize(width: 100, height: 50), .zero, CGSize(width: 0.5, height: 0.5),
    CGSize(width: 1, height: 1), CGSize(width: 5, height: 900), CGSize(width: 4000, height: 2000),
  ] {
    lease.view.frame = NSRect(origin: .zero, size: size)
    lease.view.layoutSubtreeIfNeeded()
    let bounds = NSRect(x: 0, y: 0, width: max(size.width, 1), height: max(size.height, 1))
    let rep = try #require(lease.view.bitmapImageRepForCachingDisplay(in: bounds))
    lease.view.cacheDisplay(in: lease.view.bounds, to: rep)
  }
}

@MainActor
@Test func describedParametersSupplyReadoutsThatLayoutTextOverrides() throws {
  _ = NSApplication.shared
  let readout: (UInt32, [String: Any]) -> [String: Any] = { id, extra in
    ["type": "textEditor", "editing": "click", "rect": [0, 0, 20, 10], "id": id].merging(extra) {
      $1
    }
  }
  let widgets: [[String: Any]] = [
    readout(2, [:]),
    readout(2, ["format": ["range": [0, 10], "decimals": 2]]),
    readout(3, ["maximum": 2]),
    readout(4, ["minimum": 1, "maximum": 8]),
    readout(2, ["unit": " x"]),
    [
      "name": "Gain", "type": "rotaryFilmStripKnob", "rect": [10, 10, 20, 20],
      "images": ["knob.png"], "id": 2, "label": "Gain",
    ],
  ]
  let package = try LayoutPackage(layout: version2(widgets), metadata: .described)
  #expect(package.validate() == nil)
  let (module, lease) = try open(package)
  let fields = try lease.view.subviews[0...4].map {
    try #require($0.subviews.first as? NSTextField)
  }
  try module.setValue(2, 0.75)
  lease.refresh()
  #expect(fields[0].stringValue == "+6.0 dB")
  #expect(fields[1].stringValue == "7.50")
  #expect(fields[2].stringValue == "Crunch")
  #expect(fields[3].stringValue == "2 voices")
  #expect(fields[4].stringValue == "1 x")
  #expect(lease.view.subviews[5].accessibilityValueDescription() == "+6.0 dB")
  try module.setValue(2, 0.5)
  lease.refresh()
  #expect(fields[0].stringValue == "0.0 dB")

  func type(_ text: String, _ field: Int, _ id: UInt32) -> Float? {
    fields[field].stringValue = text
    _ = fields[field].target?.perform(fields[field].action, with: fields[field])
    return try? module.value(id)
  }
  #expect(type("-3 dB", 0, 2) == Float(0.375))
  #expect(type("lead", 2, 3) == 2)
  #expect(fields[2].stringValue == "Lead")
  #expect(type("Fuzz", 2, 3) == 2)
  #expect(type("5.4", 3, 4) == 5)
  #expect(fields[3].stringValue == "5 voices")

  // Version 1 layouts and modules without metadata keep the layout's own default text.
  let first = LayoutPackage.layout(widgets: [
    ["type": "textEditor", "rect": [0, 0, 20, 10], "id": 2, "label": "Gain"]
  ])
  let older = try LayoutPackage(layout: first, metadata: .described)
  #expect(older.validate() == nil)
  let (olderModule, olderLease) = try open(older)
  try olderModule.setValue(2, 0.75)
  olderLease.refresh()
  #expect(olderLease.view.subviews[0].accessibilityValueDescription() == "75%")
  let plain = try LayoutPackage(layout: version2(widgets))
  let (plainModule, plainLease) = try open(plain)
  try plainModule.setValue(2, 0.75)
  plainLease.refresh()
  #expect((plainLease.view.subviews[0].subviews.first as? NSTextField)?.stringValue == "75%")
  #expect(plainLease.view.subviews[5].accessibilityValueDescription() == "75%")
}

@MainActor
@Test func malformedParameterMetadataRejectsTheModule() throws {
  let package = try LayoutPackage(metadata: .malformed)
  #expect(throws: AmpError.self) {
    _ = try LoadedModule(
      manifest: package.manifest, directory: package.directory, category: "pre-fx")
  }
  do {
    _ = try LoadedModule(
      manifest: package.manifest, directory: package.directory, category: "pre-fx")
  } catch {
    #expect(error.localizedDescription.contains("parameter metadata"))
  }
}

@MainActor
@Test func refreshRedrawsOnlyWhatChanged() throws {
  _ = NSApplication.shared
  let package = try LayoutPackage()
  try package.write(
    layout: version2([
      [
        "name": "Gain", "type": "rotaryFilmStripKnob", "rect": [10, 10, 20, 20],
        "images": ["knob.png"], "id": 2, "initial": 0.5, "label": "Gain",
      ],
      [
        "name": "Power", "type": "imageButton", "rect": [60, 10, 20, 20],
        "images": ["off.png", "on.png"], "id": 1, "label": "Power",
      ],
      [
        "name": "Gain value", "type": "textEditor", "editing": "click", "rect": [0, 35, 50, 15],
        "id": 2, "label": "Gain value",
      ],
      [
        "name": "Lamp", "type": "imageButton", "rect": [60, 32, 10, 10], "images": ["on.png"],
        "id": 1, "readonly": true, "condition": [1, 0.5, 1],
      ],
    ]))
  #expect(package.validate() == nil)
  let (module, lease) = try open(package)
  // Panels are layer-backed in their container; layers record invalidation without a window.
  let controls = lease.view.subviews
  for view in [lease.view] + controls { view.wantsLayer = true }
  #expect(controls.allSatisfy { $0.layer != nil })
  let (gain, power, readout, lamp) = (controls[0], controls[1], controls[2], controls[3])
  let field = try #require(readout.subviews.first as? NSTextField)
  func settle() {
    for view in [lease.view] + controls { view.layer?.displayIfNeeded() }
  }
  func redrawn() -> [NSView] { controls.filter { $0.layer?.needsDisplay() == true } }
  func backgroundRedrawn() -> Bool { lease.view.layer?.needsDisplay() == true }

  try module.setValue(1, 0)
  lease.refresh()
  settle()
  #expect(redrawn().isEmpty && !backgroundRedrawn())
  let text = field.stringValue
  lease.refresh()
  #expect(redrawn().isEmpty && !backgroundRedrawn())
  #expect(field.stringValue == text && lamp.isHidden)

  try module.setValue(2, 0.25)
  lease.refresh()
  #expect(redrawn() == [gain, readout] && !backgroundRedrawn())
  #expect(field.stringValue != text)
  #expect(matches(try color(try render(lease.view), 20, 20), LayoutPackage.green))

  settle()
  try module.setValue(1, 1)
  lease.refresh()
  #expect(redrawn().contains(power) && !redrawn().contains(gain))
  #expect(!lamp.isHidden)
  #expect(matches(try color(try render(lease.view), 70, 20), .white))
  settle()
  lease.refresh()
  #expect(redrawn().isEmpty && !backgroundRedrawn())
}

@MainActor
@Test func panelsPollOnlyInAVisibleWindow() throws {
  _ = NSApplication.shared
  let package = try LayoutPackage()
  let module = try LoadedModule(
    manifest: package.manifest, directory: package.directory, category: "pre-fx")
  let panel = NativeModulePanel.Container(module: module)
  #expect(panel.lease != nil)
  guard case .paused = panel.polling else {
    Issue.record("a panel outside a window polled: \(panel.polling)")
    return
  }
  let window = NSWindow(
    contentRect: NSRect(x: 0, y: 0, width: 100, height: 50), styleMask: .borderless,
    backing: .buffered, defer: true)
  window.isReleasedWhenClosed = false
  window.contentView = panel
  #expect(!window.occlusionState.contains(.visible))
  guard case .paused = panel.polling else {
    Issue.record("a panel in a window that is not on screen polled: \(panel.polling)")
    return
  }
  panel.stop()
  guard case .stopped = panel.polling else {
    Issue.record("a stopped panel kept polling: \(panel.polling)")
    return
  }
  panel.updatePolling()
  guard case .stopped = panel.polling else {
    Issue.record("a stopped panel resumed: \(panel.polling)")
    return
  }
  #expect(panel.lease == nil)
}
