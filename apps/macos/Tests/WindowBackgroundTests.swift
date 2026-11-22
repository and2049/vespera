import AppKit
import SwiftUI
import Testing

@testable import VesperaUI

/// PNG fixtures shared with the `vespera-module-check` window background test.
private let fixtures = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
  .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
  .appendingPathComponent("tests/fixtures/window_background")

private let headerAccepted = [
  "valid", "widest", "pixel-budget", "truncated-data", "corrupt-data", "bad-stream",
  "short-data", "animated",
]
private let headerRejected = [
  "too-wide": "at most 4096 pixels per side and 8000000 pixels in total",
  "too-tall": "at most 4096 pixels per side", "too-many-pixels": "is 4000×2001 pixels",
  "empty-dimension": "has no pixels", "truncated-header": "is truncated",
  "not-png": "is not a PNG image",
]

private func fixture(_ name: String) -> URL {
  fixtures.appendingPathComponent(name + ".png")
}

@Test func headerChecksAgreeWithTheConformanceTool() throws {
  for name in headerAccepted {
    #expect(throws: Never.self, "\(name)") { try WindowBackgroundImage.header(of: fixture(name)) }
  }
  #expect(
    try WindowBackgroundImage.header(of: fixture("pixel-budget"))
      == .init(width: 4000, height: 2000, bitDepth: 8, colourType: 0, interlaced: false))
  for (name, message) in headerRejected {
    #expect("\(name)") {
      try WindowBackgroundImage.header(of: fixture(name))
    } throws: { error in
      error.localizedDescription.contains(message)
    }
  }
}

@Test func decodingRejectsImageDataImageIOWouldBlankOut() throws {
  let image = try WindowBackgroundImage.decode(fixture("valid"))
  #expect(image.width == 8 && image.height == 4)
  let rep = NSBitmapImageRep(cgImage: image)
  #expect(rep.colorAt(x: 1, y: 1)?.usingColorSpace(.sRGB)?.redComponent == 1)
  #expect(rep.colorAt(x: 6, y: 1)?.usingColorSpace(.sRGB)?.blueComponent == 1)
  #expect(try WindowBackgroundImage.decode(fixture("widest")).width == 4096)
  for (name, message) in [
    ("truncated-data", "is truncated"), ("corrupt-data", "has a corrupt IDAT chunk"),
    ("bad-stream", "has corrupt image data"), ("short-data", "has incomplete image data"),
    ("animated", "is animated"), ("too-wide", "at most 4096"), ("not-png", "is not a PNG"),
  ] {
    #expect("\(name)") {
      try WindowBackgroundImage.decode(fixture(name))
    } throws: { error in
      error.localizedDescription.contains(message)
    }
  }
}

@Test func interlacedImageDataSizeCoversEveryPass() {
  let header = WindowBackgroundImage.Header(
    width: 3, height: 2, bitDepth: 8, colourType: 6, interlaced: true)
  // Passes 1, 4 and 6 each hold one pixel of row 0, pass 7 all of row 1; the rest are empty.
  #expect(header.filteredSize == 3 * (1 + 4) + (1 + 12))
  #expect(
    WindowBackgroundImage.Header(width: 3, height: 2, bitDepth: 1, colourType: 0, interlaced: false)
      .filteredSize == 4)
}

@Test func discoveryValidatesDeclaredBackgrounds() throws {
  let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
  try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
  defer { try? FileManager.default.removeItem(at: directory) }
  /// Scans one package with `art` resources holding `images`, whose manifest adds `fields`.
  func scan(
    _ fields: String, category: String = "amp", images: [String] = [],
    prepare: (URL) throws -> Void = { _ in }
  ) throws -> (manifest: ModuleManifest?, failures: [String]) {
    let package = directory.appendingPathComponent(UUID().uuidString + ".vespera")
    try FileManager.default.createDirectory(
      at: package.appendingPathComponent("art"), withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: package) }
    try Data(
      """
      {"schemaVersion": 1, "id": "test.background", "category": "\(category)",
       "binary": "custom.so", "icon": "icon.svg", "resources": "art"\(fields.isEmpty ? "" : ", " + fields)}
      """.utf8
    ).write(to: package.appendingPathComponent("module.json"))
    try Data().write(to: package.appendingPathComponent("custom.so"))
    try Data().write(to: package.appendingPathComponent("icon.svg"))
    for image in images {
      try FileManager.default.copyItem(
        at: fixture(image), to: package.appendingPathComponent("art/\(image).png"))
    }
    try prepare(package)
    let result = ModuleDiscovery.scan([package])
    return (result.packages.first?.manifest, result.failures)
  }

  let old = try scan("")
  #expect(old.failures.isEmpty && old.manifest?.windowBackground == nil)
  for category in ["amp", "pre-fx"] {
    let null = try scan(#""windowBackground": null"#, category: category)
    #expect(null.failures.isEmpty && null.manifest?.windowBackground == nil)
  }
  for image in headerAccepted {
    let result = try scan(#""windowBackground": "art/\#(image).png""#, images: [image])
    #expect(result.failures.isEmpty, "\(image)")
    #expect(result.manifest?.windowBackground == "art/\(image).png")
  }
  for (image, message) in headerRejected {
    let result = try scan(#""windowBackground": "art/\#(image).png""#, images: [image])
    #expect(result.manifest == nil)
    #expect(result.failures.first?.contains(message) == true, "\(image): \(result.failures)")
  }
  for (fields, category, message) in [
    (#""windowBackground": "art/valid.png""#, "pre-fx", "Only amps can declare a windowBackground"),
    (#""windowBackground": "art/valid.png""#, "special-fx", "Only amps"),
    (#""windowBackground": """#, "amp", "must be a relative path"),
    (#""windowBackground": "../valid.png""#, "amp", "must be a relative path"),
    (#""windowBackground": "/tmp/valid.png""#, "amp", "must be a relative path"),
    (#""windowBackground": "art/absent.png""#, "amp", "Missing window background"),
    (#""windowBackground": "art""#, "amp", "must be inside its resources"),
    (#""windowBackground": "icon.svg""#, "amp", "must be inside its resources"),
    (#""windowBackground": 1"#, "amp", "windowBackground has the wrong type"),
    (
      #""windowBackground": {"path": "art/valid.png"}"#, "amp",
      "windowBackground has the wrong type"
    ),
  ] {
    let result = try scan(fields, category: category, images: ["valid"])
    #expect(result.manifest == nil, "\(fields)")
    #expect(result.failures.count == 1)
    #expect(
      result.failures.first?.contains(message) == true,
      "\(fields): \(result.failures.first ?? "")")
  }
  let escape = try scan(#""windowBackground": "art/escape.png""#) {
    try FileManager.default.createSymbolicLink(
      at: $0.appendingPathComponent("art/escape.png"), withDestinationURL: fixture("valid"))
  }
  #expect(escape.failures.first?.contains("escapes its package") == true)
  let folder = try scan(#""windowBackground": "art/folder.png""#) {
    try FileManager.default.createDirectory(
      at: $0.appendingPathComponent("art/folder.png"), withIntermediateDirectories: true)
  }
  #expect(folder.failures.first?.contains("Missing window background") == true)
  let inside = try scan(#""windowBackground": "art/link.png""#, images: ["valid"]) {
    try FileManager.default.createSymbolicLink(
      atPath: $0.appendingPathComponent("art/link.png").path, withDestinationPath: "valid.png")
  }
  #expect(inside.failures.isEmpty)
}

@Test func equalizerManifestDoesNotInheritTheBackground() throws {
  let decoded = try JSONDecoder().decode(
    ModuleManifest.self,
    from: Data(
      """
      {"schemaVersion": 1, "id": "amp", "category": "amp", "binary": "a.so", "icon": "i.svg",
       "resources": ".", "windowBackground": "window.png", "eq": {"id": "eq", "binary": "e.so"}}
      """.utf8))
  #expect(decoded.windowBackground == "window.png")
  #expect(decoded.equalizerManifest?.windowBackground == nil)
  #expect(decoded.equalizerManifest?.category == "eq")
}

/// An amp fixture declaring `image` from the shared fixtures as its window background.
private func amp(
  _ image: String?, id: String = "org.example.test-amp", equalizer: String? = nil,
  parent: URL? = nil
) throws -> (package: TestModulePackage, manifest: ModuleManifest) {
  let package = try TestModulePackage(
    category: "amp", id: id, equalizer: equalizer, parent: parent)
  var manifest = package.manifest
  if let image {
    try FileManager.default.copyItem(
      at: fixture(image), to: package.directory.appendingPathComponent("window.png"))
    manifest.windowBackground = "window.png"
  }
  try JSONEncoder().encode(manifest).write(
    to: package.directory.appendingPathComponent("module.json"))
  return (package, manifest)
}

@MainActor
@Test func ampsCheckOnlyTheBackgroundHeaderWhenLoaded() throws {
  let declaring = try amp("valid", equalizer: "org.example.test-eq")
  let module = try LoadedModule(
    manifest: declaring.manifest, directory: declaring.package.directory)
  let url = try #require(module.windowBackground)
  let image = try WindowBackgroundImage.decode(url)
  #expect(image.width == 8 && image.height == 4)
  #expect(module.equalizer?.windowBackground == nil)
  let plain = try LoadedModule(
    manifest: declaring.package.manifest, directory: declaring.package.directory)
  #expect(plain.windowBackground == nil)

  for name in ["corrupt-data", "short-data", "animated"] {
    let broken = try amp(name)
    let loaded = try LoadedModule(manifest: broken.manifest, directory: broken.package.directory)
    #expect(throws: AmpError.self, "\(name)") {
      try WindowBackgroundImage.decode(try #require(loaded.windowBackground))
    }
  }
  let oversized = try amp("too-wide")
  #expect(throws: AmpError.self) {
    try LoadedModule(manifest: oversized.manifest, directory: oversized.package.directory)
  }
  let pedal = ModuleManifest(
    windowBackground: "window.png", schemaVersion: 1, id: declaring.manifest.id,
    category: "pre-fx", binary: "custom.so", icon: "custom.svg", resources: ".")
  #expect(throws: AmpError.self) {
    try LoadedModule(manifest: pedal, directory: declaring.package.directory, category: "pre-fx")
  }
}

@MainActor
@Test func backgroundFollowsTheCommittedAmpSelection() throws {
  let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
  defer { try? FileManager.default.removeItem(at: root) }
  let declaring = try amp("valid", id: "org.example.amp-a", parent: root)
  let plain = try amp(nil, id: "org.example.amp-b", parent: root)
  let broken = try amp("corrupt-data", id: "org.example.amp-c", parent: root)
  _ = (declaring, plain, broken)
  let catalog = AmpCatalog(directory: root)
  #expect(
    catalog.amps.map(\.id) == ["org.example.amp-a", "org.example.amp-b", "org.example.amp-c"])
  #expect(catalog.error == nil)
  let first = try #require(catalog.windowBackground)
  #expect(first.width == 8)

  catalog.select(1)
  #expect(catalog.windowBackground == nil)
  catalog.onSelection = { _ in throw AmpError.message("rejected") }
  catalog.select(0)
  #expect(catalog.selectedIndex == 1 && catalog.windowBackground == nil)
  catalog.restore(selecting: 0)
  #expect(catalog.windowBackground != nil && catalog.error == nil)
  catalog.select(0)
  #expect(catalog.windowBackground != nil && catalog.error == "rejected")
  catalog.onSelection = nil
  catalog.select(1)
  #expect(catalog.windowBackground == nil)

  var offered: [String] = []
  catalog.onSelection = { offered.append($0.id) }
  catalog.select(2)
  #expect(catalog.error?.contains("has a corrupt IDAT chunk") == true)
  #expect(catalog.selectedIndex == 1 && offered.isEmpty)
  catalog.select(0)
  #expect(catalog.selectedIndex == 0 && catalog.windowBackground != nil && catalog.error == nil)
  catalog.restore(selecting: 2)
  #expect(catalog.selectedIndex == 2 && catalog.windowBackground == nil)
  #expect(catalog.error?.contains("has a corrupt IDAT chunk") == true)

  let empty = root.appendingPathComponent("empty")
  #expect(AmpCatalog(directory: empty).windowBackground == nil)
}

@MainActor
@Test func aCorruptBackgroundOnTheFirstAmpIsReportedAtLaunch() throws {
  let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
  defer { try? FileManager.default.removeItem(at: root) }
  let broken = try amp("corrupt-data", id: "org.example.amp-a", parent: root)
  let valid = try amp("valid", id: "org.example.amp-b", parent: root)
  defer { _ = (broken, valid) }
  let catalog = AmpCatalog(directory: root)
  #expect(catalog.amps.count == 2 && catalog.selectedIndex == 0)
  #expect(catalog.windowBackground == nil)
  #expect(catalog.error?.contains("has a corrupt IDAT chunk") == true)
  catalog.select(1)
  #expect(catalog.windowBackground != nil && catalog.error == nil)
}

/// 80×40 pixels: black ten-pixel top and bottom bands around red, green, blue and white
/// twenty-pixel columns.
private func stripes(alpha: CGFloat = 1) throws -> CGImage {
  let context = try #require(
    CGContext(
      data: nil, width: 80, height: 40, bitsPerComponent: 8, bytesPerRow: 0,
      space: CGColorSpace(name: CGColorSpace.sRGB)!,
      bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue))
  context.setFillColor(CGColor(srgbRed: 0, green: 0, blue: 0, alpha: alpha))
  context.fill(CGRect(x: 0, y: 0, width: 80, height: 40))
  let columns: [(CGFloat, CGFloat, CGFloat)] = [(1, 0, 0), (0, 1, 0), (0, 0, 1), (1, 1, 1)]
  for (index, colour) in columns.enumerated() {
    context.setFillColor(
      CGColor(srgbRed: colour.0, green: colour.1, blue: colour.2, alpha: alpha))
    context.fill(CGRect(x: index * 20, y: 10, width: 20, height: 20))
  }
  return try #require(context.makeImage())
}

@MainActor
private func render<Content: View>(_ view: Content, _ size: CGSize) throws -> NSBitmapImageRep {
  let host = NSHostingView(rootView: view.frame(width: size.width, height: size.height))
  host.frame = NSRect(origin: .zero, size: size)
  host.layoutSubtreeIfNeeded()
  let bitmap = try #require(host.bitmapImageRepForCachingDisplay(in: host.bounds))
  host.cacheDisplay(in: host.bounds, to: bitmap)
  return bitmap
}

/// The colour at a point in view coordinates from the top left, as sRGB components.
private func colour(_ bitmap: NSBitmapImageRep, _ x: CGFloat, _ y: CGFloat, width: CGFloat)
  throws -> [Int]
{
  let scale = CGFloat(bitmap.pixelsWide) / width
  let colour = try #require(
    bitmap.colorAt(x: Int(x * scale), y: Int(y * scale))?.usingColorSpace(.sRGB))
  return [colour.redComponent, colour.greenComponent, colour.blueComponent].map {
    Int(($0 * 255).rounded())
  }
}

/// Whether sampled components match, allowing for image interpolation near edges.
private func near(_ sampled: [Int], _ expected: [Int]) -> Bool {
  zip(sampled, expected).allSatisfy { abs($0 - $1) <= 8 }
}

@MainActor
@Test func backgroundFillsCentredWithoutStretching() throws {
  let image = try stripes()
  /// A colour as the same rendering path produces it from an sRGB fill.
  func rendered(_ red: CGFloat, _ green: CGFloat, _ blue: CGFloat) throws -> [Int] {
    let fill = Color(cgColor: CGColor(srgbRed: red, green: green, blue: blue, alpha: 1))
    return try colour(render(fill, CGSize(width: 10, height: 10)), 5, 5, width: 10)
  }
  let (black, red, green, blue, white) = try (
    rendered(0, 0, 0), rendered(1, 0, 0), rendered(0, 1, 0), rendered(0, 0, 1), rendered(1, 1, 1)
  )
  // Square: the full height shows, cropping a quarter of the width from each side.
  let square = try render(WindowBackgroundView(image: image), CGSize(width: 100, height: 100))
  #expect(near(try colour(square, 25, 50, width: 100), green))
  #expect(near(try colour(square, 75, 50, width: 100), blue))
  #expect(near(try colour(square, 50, 5, width: 100), black))
  // Wide: the full width shows, cropping the black rows away.
  let wide = try render(WindowBackgroundView(image: image), CGSize(width: 600, height: 100))
  for (x, expected) in [(75, red), (225, green), (375, blue), (525, white)] {
    #expect(near(try colour(wide, CGFloat(x), 5, width: 600), expected))
    #expect(near(try colour(wide, CGFloat(x), 95, width: 600), expected))
  }
  // Tall: the full height shows, cropping seven eighths of the width.
  let tall = try render(WindowBackgroundView(image: image), CGSize(width: 100, height: 400))
  #expect(near(try colour(tall, 25, 200, width: 100), green))
  #expect(near(try colour(tall, 75, 200, width: 100), blue))
  #expect(near(try colour(tall, 50, 50, width: 100), black))
  #expect(near(try colour(tall, 50, 350, width: 100), black))

  let clipped = try render(
    HStack(spacing: 0) {
      WindowBackgroundView(image: image).frame(width: 100, height: 100)
      Color.black.frame(width: 100, height: 100)
    }, CGSize(width: 200, height: 100))
  #expect(near(try colour(clipped, 125, 50, width: 200), black))
  #expect(near(try colour(clipped, 75, 50, width: 200), blue))
}

@MainActor
@Test func plainWindowColourIsUnchangedWithoutAnImage() throws {
  let size = CGSize(width: 120, height: 90)
  let before = try render(Color.clear.background(Theme.window), size)
  let fallback = try render(Color.clear.background(WindowBackgroundView(image: nil)), size)
  let transparent = try render(
    Color.clear.background(WindowBackgroundView(image: try stripes(alpha: 0))), size)
  #expect(fallback.tiffRepresentation == before.tiffRepresentation)
  #expect(transparent.tiffRepresentation == before.tiffRepresentation)
}

@MainActor
@Test func backgroundIsHiddenFromAccessibility() throws {
  let decorative = NSHostingView(
    rootView: WindowBackgroundView(image: try stripes()).frame(width: 100, height: 100))
  decorative.frame = NSRect(x: 0, y: 0, width: 100, height: 100)
  decorative.layoutSubtreeIfNeeded()
  #expect((decorative.accessibilityChildren() ?? []).isEmpty)
}
