import ModuleSDK
import SwiftUI

enum PedalBoardLayout {
  static let pedalHeight: CGFloat = 380
  static let defaultPedal = CGSize(width: 215, height: 380)
  static let maximumWidth: CGFloat = 960
  static let pedalSpacing: CGFloat = 32
  /// The most artwork is enlarged beyond its declared size.
  static let maximumScale: CGFloat = 1.1
  static let maximumHeaderWidth: CGFloat = 320
  static let headerSpacing: CGFloat = 14
  static var headerHeight: CGFloat { UISize.standard.metrics.control_height }

  struct Frames: Equatable {
    /// The gap between adjacent spans.
    var spacing: CGFloat
    /// Each span's width: its columns and the gaps between them.
    var spans: [CGFloat]
    /// Each span's editor or placeholder size, centred in its span.
    var editors: [CGSize]
  }

  /// Frames for spans of `units` board columns showing artwork of `size` within `bounds`.
  ///
  /// Every unit is a column of the same width, set by the bounds alone: a full board of default
  /// pedals and their gaps fills the width. Artwork is normalised to `pedalHeight`, and all
  /// spans share one scale, the largest at which each artwork fits its span and the available
  /// height without exceeding `maximumScale` of its declared size. Free units count as default
  /// pedals. Never taller than bounds.
  static func frames(_ spans: [(units: Int, size: CGSize)], in bounds: CGSize) -> Frames {
    let height = max(0, bounds.height - headerHeight - headerSpacing)
    let width = max(0, min(maximumWidth, bounds.width))
    let capacity = CGFloat(PedalEditorLayout.capacity)
    let board = defaultPedal.width * capacity + pedalSpacing * (capacity - 1)
    let grid = min(maximumScale, width / board, height / pedalHeight)
    let widths = spans.map {
      (defaultPedal.width * CGFloat($0.units) + pedalSpacing * CGFloat($0.units - 1)) * grid
    }
    let normalised = spans.map { $0.size.width * pedalHeight / $0.size.height }
    let scale = spans.indices.map {
      min(widths[$0] / normalised[$0], maximumScale * spans[$0].size.height / pedalHeight)
    }
    .reduce(height / pedalHeight, min)
    return Frames(
      spacing: pedalSpacing * grid, spans: widths,
      editors: normalised.map { CGSize(width: $0 * scale, height: pedalHeight * scale) })
  }
}

struct PedalModuleBoard: View {
  @ObservedObject var catalog: PedalCatalog
  @State private var expanded: String?
  @State private var showsChain = false

  var body: some View {
    VStack(spacing: 12) {
      SignalChainSummary(catalog: catalog, showsChain: $showsChain)
      GeometryReader { geometry in
        editors(in: geometry.size)
          .frame(width: geometry.size.width, height: geometry.size.height, alignment: .top)
      }
      if let error = catalog.error {
        Text(error).statusMessage().padding(.horizontal, 50).lineLimit(2)
      }
    }
    .padding(.top, 8)
    .settingsDropdownOverlay()
    .overlay(alignment: .top) {
      if showsChain {
        SignalChainOverlay(catalog: catalog) { showsChain = false }
      }
    }
    .accessibilityElement(children: .contain)
    .accessibilityLabel("\(catalog.position.title) pedalboard")
  }

  private func editors(in bounds: CGSize) -> some View {
    let spans = catalog.editors.spans
    let units = catalog.editors.units
    let instances = spans.map { $0.id.flatMap(catalog.instance) }
    let frames = PedalBoardLayout.frames(
      spans.indices.map {
        (spans[$0].width, instances[$0]?.module.preferredViewSize ?? PedalBoardLayout.defaultPedal)
      }, in: bounds)
    return HStack(alignment: .top, spacing: frames.spacing) {
      ForEach(spans.indices, id: \.self) { index in
        VStack(spacing: PedalBoardLayout.headerSpacing) {
          HStack(spacing: 6) {
            SettingsDropdown(
              title: "\(catalog.position.title) editor \(units[index] + 1)",
              options: catalog.editorOptions,
              selection: Binding(
                get: { spans[index].id ?? "" },
                set: { catalog.chooseEditor($0, in: units[index]) }),
              expanded: $expanded, placeholder: "Choose an editor", surface: .raised
            )
            if let draft = instances[index], draft.isDraft {
              SettingsIconButton(
                systemName: "power",
                label: "Switch on \(draft.title) and add it to the signal chain"
              ) { catalog.setBypassed(draft.id, false) }
            }
          }
          .frame(maxWidth: PedalBoardLayout.maximumHeaderWidth)
          .frame(height: PedalBoardLayout.headerHeight)
          if let instance = instances[index] {
            ModulePanel(module: instance.module).id(instance.id)
              .frame(width: frames.editors[index].width, height: frames.editors[index].height)
          } else {
            RoundedRectangle(cornerRadius: Theme.panelRadius + 2)
              .strokeBorder(
                Theme.borderSubtle,
                style: StrokeStyle(lineWidth: Theme.borderWidth, dash: [5, 6])
              )
              .overlay { Text("Choose an editor above").statusMessage() }
              .frame(width: frames.editors[index].width, height: frames.editors[index].height)
          }
        }
        .frame(width: frames.spans[index])
        .zIndex(Double(spans.count - index))
      }
    }
  }
}

/// The chain panel over a dimmed board, kept within the board's bounds: the list scrolls rather
/// than the panel growing past the board.
struct SignalChainOverlay: View {
  static let inset: CGFloat = 8
  @ObservedObject var catalog: PedalCatalog
  let dismiss: () -> Void

  var body: some View {
    ZStack(alignment: .top) {
      Theme.scrim.contentShape(Rectangle())
        .onTapGesture(perform: dismiss)
        .accessibilityLabel("Close signal chain")
      SignalChainPanel(catalog: catalog, dismiss: dismiss)
        .frame(width: 640)
        .padding(.vertical, Self.inset)
    }
  }
}

/// The chain in processing order, marking which instances are hidden or bypassed, so that
/// processing without a visible editor stays discoverable.
struct SignalChainSummary: View {
  @ObservedObject var catalog: PedalCatalog
  @Binding var showsChain: Bool

  var body: some View {
    let visible = Set(catalog.editors.visible)
    let hidden = catalog.hiddenInstances.filter { !$0.bypassed }.count
    let bypassed = catalog.chain.filter(\.bypassed).count
    HStack(spacing: 10) {
      Button {
        showsChain = true
      } label: {
        Label("Signal chain", systemImage: "list.bullet")
      }
      .buttonStyle(SettingsButtonStyle())
      .environment(\.uiSize, .compact)
      .help("Show, add, remove, reorder and bypass \(catalog.position.title) effects")
      ViewThatFits(in: .horizontal) {
        HStack(spacing: 6) {
          ForEach(Array(catalog.chain.enumerated()), id: \.element.id) { index, instance in
            if index > 0 {
              Image(systemName: "chevron.right").font(Theme.chainChevronFont())
                .foregroundStyle(Theme.muted).accessibilityHidden(true)
            }
            chip(instance, visible: visible.contains(instance.id))
          }
          if catalog.chain.isEmpty { Text("Empty chain").statusMessage() }
        }
        Text("\(catalog.chain.count) \(catalog.chain.count == 1 ? "effect" : "effects")")
          .statusMessage()
      }
    }
    .frame(maxWidth: PedalBoardLayout.maximumWidth, alignment: .leading)
    .frame(height: UISize.compact.metrics.control_height)
    .accessibilityElement(children: .contain)
    .accessibilityLabel("\(catalog.position.title) signal chain")
    .accessibilityValue(summary(count: catalog.chain.count, hidden: hidden, bypassed: bypassed))
  }

  private func summary(count: Int, hidden: Int, bypassed: Int) -> String {
    var parts = ["\(count) \(count == 1 ? "effect" : "effects")"]
    if hidden > 0 { parts.append("\(hidden) active without editor") }
    if bypassed > 0 { parts.append("\(bypassed) bypassed") }
    if !catalog.drafts.isEmpty { parts.append("\(catalog.drafts.count) off") }
    return parts.joined(separator: " · ")
  }

  private func chip(_ instance: PedalInstance, visible: Bool) -> some View {
    Button {
      catalog.showEditor(instance.id)
    } label: {
      HStack(spacing: 4) {
        Circle().fill(instance.bypassed ? Theme.offBorder : Theme.fill)
          .frame(width: 5, height: 5)
        Text(instance.title).lineLimit(1)
      }
      .font(Theme.font(UISize.compact.metrics.label_font_size))
      .foregroundStyle(visible ? Theme.primary : Theme.secondary)
      .padding(.horizontal, 7)
      .frame(height: 20)
      .background(
        RoundedRectangle(cornerRadius: UISize.compact.metrics.corner_radius).strokeBorder(
          visible ? Theme.border : Theme.borderSubtle,
          style: StrokeStyle(lineWidth: Theme.borderWidth, dash: visible ? [] : [3, 3]))
      )
      .opacity(instance.bypassed ? 0.6 : 1)
      .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
    .help(
      "\(instance.module.pluginFamily.map { "\($0) — " } ?? "")\(instance.title): "
        + "\(instance.bypassed ? "bypassed" : "active"), "
        + "\(visible ? "editor shown" : "editor hidden; click to show")"
    )
    .accessibilityLabel(instance.title)
    .accessibilityValue(
      "\(instance.bypassed ? "Bypassed" : "Active"), \(visible ? "editor shown" : "editor hidden")"
    )
    .accessibilityHint("Shows its editor")
  }
}

struct SignalChainPanel: View {
  @ObservedObject var catalog: PedalCatalog
  let dismiss: () -> Void
  @State private var expanded: String?
  private let metrics = UISize.standard.metrics

  var body: some View {
    SettingsPanel(title: "\(catalog.position.title) signal chain") {
      SettingsIconButton(systemName: "xmark", label: "Close signal chain", action: dismiss)
    } content: {
      ViewThatFits(in: .vertical) {
        list.frame(maxHeight: 360).fixedSize(horizontal: false, vertical: true)
        list.frame(maxHeight: 360)
      }
      SettingsDivider()
      SettingsField(label: "Add effect") {
        SettingsDropdown(
          title: "Add \(catalog.position.title) effect", options: catalog.libraryOptions,
          selection: Binding(get: { "" }, set: { catalog.add($0) }), expanded: $expanded,
          placeholder: "Choose an installed module")
      }
      if let error = catalog.error { Text(error).statusMessage().lineLimit(3) }
    }
    .settingsDropdownOverlay()
  }

  private var list: some View {
    ScrollView {
      VStack(spacing: 2) {
        ForEach(Array(catalog.chain.enumerated()), id: \.element.id) { index, instance in
          row(instance, index: index)
        }
        if catalog.chain.isEmpty {
          Text("No effects").statusMessage()
            .frame(maxWidth: .infinity, minHeight: metrics.control_height)
        }
        if !catalog.drafts.isEmpty {
          Text("Off").statusMessage()
            .frame(maxWidth: .infinity, minHeight: metrics.control_height, alignment: .leading)
          ForEach(catalog.drafts) { draftRow($0) }
        }
      }
    }
  }

  private func draftRow(_ instance: PedalInstance) -> some View {
    let visible = catalog.editors.visible.contains(instance.id)
    return HStack(spacing: 8) {
      Spacer().frame(width: 22)
      Toggle(
        "\(instance.title) active",
        isOn: Binding(get: { false }, set: { if $0 { catalog.setBypassed(instance.id, false) } })
      )
      .toggleStyle(SettingsCheckboxStyle(labelHidden: true))
      .help("Switch on \(instance.title) and add it to the signal chain")
      VStack(alignment: .leading, spacing: 1) {
        Text(instance.title).foregroundStyle(Theme.primary).lineLimit(1)
        Text(instance.module.pluginFamily ?? "No plugin family")
          .font(Theme.font(UISize.compact.metrics.label_font_size))
          .foregroundStyle(Theme.secondary).lineLimit(1)
      }
      .opacity(0.5)
      .frame(maxWidth: .infinity, alignment: .leading)
      SettingsIconButton(
        systemName: visible ? "eye.fill" : "eye",
        label: visible ? "\(instance.title) editor shown" : "Show \(instance.title) editor"
      ) { catalog.showEditor(instance.id) }
      SettingsIconButton(systemName: "trash", label: "Remove \(instance.title)") {
        catalog.remove(instance.id)
      }
    }
    .padding(.horizontal, 6)
    .frame(minHeight: metrics.control_height + 8)
    .background(
      RoundedRectangle(cornerRadius: metrics.corner_radius)
        .fill(visible ? Theme.inset : Color.clear)
    )
    .accessibilityElement(children: .contain)
    .accessibilityLabel("\(instance.title), not in chain")
  }

  private func row(_ instance: PedalInstance, index: Int) -> some View {
    let visible = catalog.editors.visible.contains(instance.id)
    let last = index == catalog.chain.count - 1
    return HStack(spacing: 8) {
      Text("\(index + 1)").monospacedDigit().foregroundStyle(Theme.secondary)
        .frame(width: 22, alignment: .trailing)
      Toggle(
        "\(instance.title) active",
        isOn: Binding(
          get: { !instance.bypassed },
          set: { catalog.setBypassed(instance.id, !$0) })
      )
      .toggleStyle(SettingsCheckboxStyle(labelHidden: true))
      .help(instance.bypassed ? "Enable \(instance.title)" : "Bypass \(instance.title)")
      VStack(alignment: .leading, spacing: 1) {
        Text(instance.title).foregroundStyle(Theme.primary).lineLimit(1)
        Text(instance.module.pluginFamily ?? "No plugin family")
          .font(Theme.font(UISize.compact.metrics.label_font_size))
          .foregroundStyle(Theme.secondary).lineLimit(1)
      }
      .opacity(instance.bypassed ? 0.5 : 1)
      .frame(maxWidth: .infinity, alignment: .leading)
      if instance.bypassed { Text("Bypassed").statusMessage() }
      SettingsIconButton(
        systemName: visible ? "eye.fill" : "eye",
        label: visible ? "\(instance.title) editor shown" : "Show \(instance.title) editor"
      ) { catalog.showEditor(instance.id) }
      SettingsIconButton(systemName: "chevron.up", label: "Move \(instance.title) earlier") {
        catalog.move(instance.id, by: -1)
      }
      .disabled(index == 0)
      SettingsIconButton(systemName: "chevron.down", label: "Move \(instance.title) later") {
        catalog.move(instance.id, by: 1)
      }
      .disabled(last)
      SettingsIconButton(
        systemName: "plus.square.on.square", label: "Duplicate \(instance.title)"
      ) { catalog.duplicate(instance.id) }
      SettingsIconButton(systemName: "trash", label: "Remove \(instance.title)") {
        catalog.remove(instance.id)
      }
    }
    .padding(.horizontal, 6)
    .frame(minHeight: metrics.control_height + 8)
    .background(
      RoundedRectangle(cornerRadius: metrics.corner_radius)
        .fill(visible ? Theme.inset : Color.clear)
    )
    .contentShape(Rectangle())
    .draggable(instance.id) {
      Text(instance.title).padding(6).background(Theme.raised)
    }
    .dropDestination(for: String.self) { ids, _ in
      guard let id = ids.first, catalog.instance(id) != nil else { return false }
      catalog.move(id, to: index)
      return true
    }
    .accessibilityElement(children: .contain)
    .accessibilityLabel("\(index + 1). \(instance.title)")
  }
}
