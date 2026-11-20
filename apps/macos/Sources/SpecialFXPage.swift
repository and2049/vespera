import SwiftUI

struct SpecialFXPage: View {
  @ObservedObject var catalog: SpecialFXCatalog
  @ObservedObject var audio: AudioController

  var body: some View {
    VStack(spacing: 8) {
      if let module = catalog.shownModule {
        let active = catalog.isActive(module.id)
        Toggle(
          "In signal chain",
          isOn: Binding(get: { active }, set: { catalog.setActive(module.id, $0) })
        )
        .toggleStyle(SettingsCheckboxStyle())
        .help(module.route.phrase)
        .accessibilityLabel("\(module.name) in signal chain")
        .accessibilityHint(module.route.phrase)
        ModulePanel(module: module).id(module.id)
          .opacity(active ? 1 : 0.6)
      } else {
        Text("Install a Special FX module to get started.")
          .foregroundStyle(Theme.secondary).frame(maxHeight: .infinity)
      }
      if let error = catalog.error {
        Text(error).statusMessage()
      }
      ModuleCarousel(
        modules: catalog.modules, selectedIndex: catalog.shownIndex,
        label: "Special FX editor", marked: catalog.active, onSelect: catalog.show
      )
      .padding(.bottom, 12)
    }
    .padding(.horizontal, 38)
  }
}
