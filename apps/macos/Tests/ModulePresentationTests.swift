import AppKit
import SwiftUI
import Testing

@testable import VesperaUI

@MainActor
@Test func cabinetPreviewCropsWithoutMagnifyingTexture() throws {
  let image = NSImage(size: NSSize(width: 100, height: 10), flipped: false) { _ in
    for stripe in 0..<10 {
      (stripe.isMultiple(of: 2) ? NSColor.white : NSColor.black).setFill()
      NSRect(x: stripe * 10, y: 0, width: 10, height: 10).fill()
    }
    return true
  }
  for height: CGFloat in [10, 40] {
    let host = NSHostingView(rootView: CabinetPreview(image: image, width: 100, height: height))
    host.frame = NSRect(x: 0, y: 0, width: 100, height: height)
    host.layoutSubtreeIfNeeded()
    let bitmap = try #require(host.bitmapImageRepForCachingDisplay(in: host.bounds))
    host.cacheDisplay(in: host.bounds, to: bitmap)
    let scale = CGFloat(bitmap.pixelsWide) / 100
    for stripe in 0..<10 {
      let color = try #require(
        bitmap.colorAt(x: Int(CGFloat(stripe * 10 + 5) * scale), y: Int(5 * scale))?
          .usingColorSpace(.deviceRGB))
      #expect(abs(color.redComponent - (stripe.isMultiple(of: 2) ? 1 : 0)) < 0.01)
    }
  }
}

@Test func cabinetHeadMountPreservesAspectAndScale() {
  let viewSize = CGSize(width: 1024, height: 590)
  for previewHeight: CGFloat in [31.5, 130.5] {
    let size = CabinetStackLayout.headSize(viewSize: viewSize, previewHeight: previewHeight)
    #expect(abs(size.width / size.height - viewSize.width / viewSize.height) < 0.000001)
    #expect(size.width <= 620)
    #expect(
      abs(
        (viewSize.height - previewHeight) * size.width / viewSize.width - CabinetStackLayout.mountY)
        < 0.000001)
  }
  #expect(CabinetStackLayout.headSize(viewSize: viewSize, previewHeight: 130.5).width == 620)
}

@MainActor
@Test func moduleFamiliesStayGrouped() throws {
  let fixture = try TestModulePackage(category: "amp")
  var manifest = fixture.manifest
  #expect(try LoadedModule(manifest: manifest, directory: fixture.directory).pluginFamily == nil)
  manifest.pluginFamily = " \n "
  #expect(try LoadedModule(manifest: manifest, directory: fixture.directory).pluginFamily == nil)
  manifest.pluginFamily = " Family Z "
  let last = try LoadedModule(manifest: manifest, directory: fixture.directory)
  manifest.pluginFamily = "Family A"
  let first = try LoadedModule(manifest: manifest, directory: fixture.directory)
  let second = try LoadedModule(manifest: manifest, directory: fixture.directory)
  let sorted = [last, first, second].sorted(by: LoadedModule.orderedByFamily)
  #expect(sorted.map(\.pluginFamily) == ["Family A", "Family A", "Family Z"])
  let decoded = try JSONDecoder().decode(ModuleManifest.self, from: JSONEncoder().encode(manifest))
  #expect(decoded.pluginFamily == "Family A")
}
