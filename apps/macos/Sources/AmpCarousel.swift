import SwiftUI

struct AmpCarousel: View {
  @ObservedObject var catalog: AmpCatalog
  var showsPanel = true
  var cabinet: LoadedModule? = nil
  var showsEqualizer = false

  var body: some View {
    ZStack(alignment: .bottom) {
      if showsPanel, showsEqualizer, let equalizer = catalog.activeAmp?.equalizer {
        ModulePanel(module: equalizer)
          .id(equalizer.id)
          .frame(maxWidth: .infinity, maxHeight: .infinity)
      } else if showsPanel, !showsEqualizer, let amp = catalog.activeAmp {
        AmpPresentation(amp: amp, cabinet: cabinet)
          .id(amp.id)
          .frame(maxWidth: .infinity, maxHeight: .infinity)
      } else {
        Spacer()
      }
      if showsPanel, cabinet != nil {
        LinearGradient(colors: [.clear, .black.opacity(0.65)], startPoint: .top, endPoint: .bottom)
          .frame(height: 100)
          .allowsHitTesting(false)
      }
      VStack(spacing: 0) {
        if let error = catalog.error {
          Text(error)
            .statusMessage()
            .padding(.horizontal, 50)
        }
        ModuleCarousel(
          modules: catalog.amps, selectedIndex: catalog.selectedIndex,
          label: "Amplifier selection", capturesScroll: false, width: 400,
          onSelect: catalog.select)
      }
    }
    .frame(maxWidth: .infinity, maxHeight: .infinity)
    .background(
      CarouselScrollInput(
        onStep: CarouselNavigation(
          count: catalog.amps.count, selectedIndex: catalog.selectedIndex, onSelect: catalog.select
        ).step))
  }
}
