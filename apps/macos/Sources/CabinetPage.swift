import SwiftUI

struct CabinetStackLayout {
  // The cabinet packages' support plane is a 459.5-point amp-head mount scaled to 620 wide.
  static let mountY: CGFloat = 278.212890625

  static func headSize(viewSize: CGSize, previewHeight: CGFloat) -> CGSize {
    let mountHeight = max(1, viewSize.height - previewHeight)
    let scale = min(620 / viewSize.width, mountY / mountHeight)
    return CGSize(width: viewSize.width * scale, height: viewSize.height * scale)
  }
}

struct AmpPresentation: View {
  let amp: LoadedModule
  let cabinet: LoadedModule?

  var body: some View {
    GeometryReader { geometry in
      let scale = min(
        geometry.size.width / amp.preferredViewSize.width,
        geometry.size.height / amp.preferredViewSize.height)
      ZStack(alignment: .bottom) {
        if let preview = cabinet?.cabinetPreview, amp.cabinetPreviewHeight > 0 {
          CabinetPreview(
            image: preview, width: amp.preferredViewSize.width * scale,
            height: amp.cabinetPreviewHeight * scale
          )
          .allowsHitTesting(false)
        }
        ModulePanel(module: amp)
      }
      .frame(width: geometry.size.width, height: geometry.size.height)
    }
  }
}

struct CabinetPreview: View {
  let image: NSImage
  let width: CGFloat
  let height: CGFloat

  var body: some View {
    Image(nsImage: image)
      .resizable().interpolation(.high)
      .frame(width: width, height: width * image.size.height / image.size.width)
      .frame(width: width, height: height, alignment: .top)
      .clipped()
  }
}

struct CabinetPage: View {
  @ObservedObject var catalog: CabinetCatalog

  private var iconBundle: Bundle {
    #if SWIFT_PACKAGE
      Bundle.module
    #else
      Bundle.main
    #endif
  }

  var body: some View {
    ZStack(alignment: .bottom) {
      GeometryReader { geometry in
        let scale = min(geometry.size.width / 1024, geometry.size.height / 610)
        ZStack(alignment: .top) {
          if let cabinet = catalog.activeCabinet {
            ModulePanel(module: cabinet)
              .id(cabinet.id)
              .frame(width: 1024, height: 610)
          }
          if let amp = catalog.amp {
            let size = CabinetStackLayout.headSize(
              viewSize: amp.preferredViewSize, previewHeight: amp.cabinetPreviewHeight)
            ModulePanel(module: amp)
              .frame(width: size.width, height: size.height)
              .offset(
                y: CabinetStackLayout.mountY - size.height
                  * (1 - amp.cabinetPreviewHeight / amp.preferredViewSize.height)
              )
              .allowsHitTesting(false)
          }
        }
        .frame(width: 1024, height: 610, alignment: .top)
        .clipped()
        .scaleEffect(scale, anchor: .topLeading)
        .frame(width: 1024 * scale, height: 610 * scale, alignment: .topLeading)
        .frame(width: geometry.size.width, height: geometry.size.height, alignment: .bottom)
      }
      LinearGradient(colors: [.clear, .black.opacity(0.65)], startPoint: .top, endPoint: .bottom)
        .frame(height: 100)
        .allowsHitTesting(false)
      HStack(spacing: 12) {
        Button(action: catalog.toggleEnabled) {
          controlIcon("power")
            .opacity(catalog.enabled ? 1 : 0.4)
        }
        .buttonStyle(.plain)
        .disabled(catalog.activeCabinet == nil)
        .help(catalog.enabled ? "Bypass cabinet" : "Enable cabinet")
        .accessibilityLabel("Cabinet power")
        .accessibilityValue(catalog.enabled ? "On" : "Off")
        ModuleCarousel(
          modules: catalog.cabinets, selectedIndex: catalog.selectedIndex,
          label: "Cabinet selection", enabled: !catalog.locked, capturesScroll: false,
          width: 400, onSelect: catalog.select
        )
        Button(action: catalog.toggleLock) {
          controlIcon(catalog.locked ? "link" : "unlink")
        }
        .buttonStyle(.plain)
        .help(catalog.locked ? "Unlink cabinet from amplifier" : "Link cabinet to amplifier")
        .accessibilityLabel(
          catalog.locked ? "Unlink cabinet from amplifier" : "Link cabinet to amplifier")
      }
      .padding(.bottom, 12)
    }
    .background(
      CarouselScrollInput(
        onStep: CarouselNavigation(
          count: catalog.cabinets.count, selectedIndex: catalog.selectedIndex,
          onSelect: catalog.select
        ).step)
    )
    .overlay(alignment: .top) {
      if let error = catalog.error {
        Text(error).statusMessage()
      }
    }
  }

  @ViewBuilder
  private func controlIcon(_ name: String) -> some View {
    if let url = iconBundle.url(forResource: name, withExtension: "svg", subdirectory: "Icons"),
      let image = NSImage(contentsOf: url)
    {
      Image(nsImage: image).resizable().frame(width: 40, height: 40)
        .frame(width: 56, height: 56).contentShape(Rectangle())
    }
  }
}
