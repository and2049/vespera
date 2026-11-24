import SwiftUI

struct SettingsOption: Identifiable, Equatable {
  let id: String
  let title: String
  /// Options sharing a group are reached through one expandable top-level menu row.
  let group: String?

  init(id: String, title: String, group: String? = nil) {
    self.id = id
    self.title = title
    let group = group?.trimmingCharacters(in: .whitespacesAndNewlines)
    self.group = group?.isEmpty == false ? group : nil
  }
}

enum SettingsMenuLevel: Equatable {
  case root
  case group(String)
}

enum SettingsMenuRow: Identifiable, Equatable {
  case back(group: String)
  case group(String, count: Int, selected: SettingsOption?)
  case option(SettingsOption)

  enum ID: Hashable {
    case back
    case group(String)
    case option(String)
  }

  var id: ID {
    switch self {
    case .back: .back
    case .group(let name, _, _): .group(name)
    case .option(let option): .option(option.id)
    }
  }
}

enum SettingsMenuAction: Equatable {
  case choose(String)
  case show(SettingsMenuLevel)
}

/// Menu structure for a dropdown. Ungrouped options are listed directly; grouped options are
/// listed beneath one row per group, in the order each group first appears in `options`.
struct SettingsMenuContent {
  let options: [SettingsOption]
  let selection: String

  var grouped: Bool { options.contains { $0.group != nil } }
  var selectedOption: SettingsOption? { options.first { $0.id == selection } }

  func resolved(_ level: SettingsMenuLevel) -> SettingsMenuLevel {
    if case .group(let name) = level, !options.contains(where: { $0.group == name }) {
      return .root
    }
    return level
  }

  func rows(at level: SettingsMenuLevel) -> [SettingsMenuRow] {
    switch resolved(level) {
    case .root:
      var listed = Set<String>()
      return options.compactMap { option in
        guard let group = option.group else { return .option(option) }
        guard listed.insert(group).inserted else { return nil }
        let members = options.filter { $0.group == group }
        return .group(
          group, count: members.count, selected: members.first { $0.id == selection })
      }
    case .group(let name):
      return [.back(group: name)] + options.filter { $0.group == name }.map(SettingsMenuRow.option)
    }
  }

  var maximumRowCount: Int {
    var levels = [SettingsMenuLevel.root]
    for group in options.compactMap(\.group) where !levels.contains(.group(group)) {
      levels.append(.group(group))
    }
    return levels.map { rows(at: $0).count }.max() ?? 0
  }

  /// The row highlighted on showing `level`: the group just left, otherwise the row holding
  /// the selection, otherwise the first option of a group.
  func initialHighlight(at level: SettingsMenuLevel, from previous: SettingsMenuLevel? = nil)
    -> SettingsMenuRow.ID?
  {
    let rows = rows(at: level)
    if case .group(let name)? = previous, rows.contains(where: { $0.id == .group(name) }) {
      return .group(name)
    }
    if let row = rows.first(where: { $0.holdsSelection(selection) }) { return row.id }
    guard case .group = resolved(level) else { return nil }
    return rows.first { $0.id != .back }?.id
  }

  func highlight(
    after current: SettingsMenuRow.ID?, moving direction: Int, at level: SettingsMenuLevel
  ) -> SettingsMenuRow.ID? {
    let rows = rows(at: level)
    guard !rows.isEmpty else { return nil }
    guard let index = rows.firstIndex(where: { $0.id == current }) else {
      return (direction > 0 ? rows.first : rows.last)?.id
    }
    return rows[min(max(index + direction, 0), rows.count - 1)].id
  }

  func action(for id: SettingsMenuRow.ID, at level: SettingsMenuLevel) -> SettingsMenuAction? {
    guard let row = rows(at: level).first(where: { $0.id == id }) else { return nil }
    switch row {
    case .back: return .show(.root)
    case .group(let name, _, _): return .show(.group(name))
    case .option(let option): return .choose(option.id)
    }
  }
}

extension SettingsMenuRow {
  fileprivate func holdsSelection(_ selection: String) -> Bool {
    switch self {
    case .back: false
    case .group(_, _, let selected): selected != nil
    case .option(let option): option.id == selection
    }
  }
}

private struct DropdownAnchor {
  let key: String
  let bounds: Anchor<CGRect>
  let options: [SettingsOption]
  let selection: String
  let size: UISize
  let browse: Bool
  let choose: (String) -> Void
  let dismiss: () -> Void
}

private struct DropdownPreference: PreferenceKey {
  static var defaultValue: [DropdownAnchor] { [] }
  static func reduce(value: inout [DropdownAnchor], nextValue: () -> [DropdownAnchor]) {
    value.append(contentsOf: nextValue())
  }
}

struct SettingsPanel<Trailing: View, Content: View>: View {
  let title: String
  @ViewBuilder var trailing: Trailing
  @ViewBuilder var content: Content
  @Environment(\.uiSize) private var size

  var body: some View {
    let metrics = size.metrics
    VStack(alignment: .leading, spacing: 0) {
      HStack(spacing: metrics.label_gap) {
        Text(title).font(Theme.font(metrics.title_font_size)).foregroundStyle(Theme.primary)
        Spacer(minLength: metrics.field_gap)
        trailing
      }
      .padding(.horizontal, metrics.panel_padding)
      .padding(.vertical, metrics.stack_spacing)
      .background(Theme.header)
      VStack(alignment: .leading, spacing: metrics.stack_spacing) { content }
        .padding(metrics.panel_padding)
    }
    .font(Theme.font(metrics.font_size))
    .foregroundStyle(Theme.label)
    .background(Theme.panel)
    .clipShape(RoundedRectangle(cornerRadius: Theme.panelRadius))
    .overlay {
      RoundedRectangle(cornerRadius: Theme.panelRadius)
        .strokeBorder(Theme.border, lineWidth: Theme.borderWidth)
    }
  }
}

extension SettingsPanel where Trailing == EmptyView {
  init(title: String, @ViewBuilder content: () -> Content) {
    self.init(title: title, trailing: { EmptyView() }, content: content)
  }
}

struct SettingsField<Content: View>: View {
  let label: String
  @ViewBuilder var content: Content
  @Environment(\.uiSize) private var size

  var body: some View {
    VStack(alignment: .leading, spacing: size.metrics.label_gap) {
      SettingsLabel(label)
      content
    }
  }
}

struct SettingsLabel: View {
  let text: String
  @Environment(\.uiSize) private var size

  init(_ text: String) { self.text = text }

  var body: some View {
    Text(text).font(Theme.font(size.metrics.label_font_size)).foregroundStyle(Theme.label)
      .lineLimit(1)
  }
}

struct SettingsDivider: View {
  var body: some View { Rectangle().fill(Theme.divider).frame(height: 1) }
}

/// Inset cell for read-only values and empty table cells.
struct SettingsCell<Content: View>: View {
  var placeholder = false
  @ViewBuilder var content: Content
  @Environment(\.uiSize) private var size

  var body: some View {
    let metrics = size.metrics
    content
      .lineLimit(1).truncationMode(.middle)
      .foregroundStyle(Theme.primary)
      .padding(.horizontal, metrics.horizontal_padding)
      .frame(maxWidth: .infinity, minHeight: metrics.control_height, alignment: .leading)
      .background(
        placeholder ? Theme.placeholder : Theme.inset,
        in: RoundedRectangle(cornerRadius: metrics.corner_radius))
  }
}

struct SettingsEmptyCell: View {
  @Environment(\.uiSize) private var size

  var body: some View {
    RoundedRectangle(cornerRadius: size.metrics.corner_radius).fill(Theme.placeholder)
      .frame(height: size.metrics.control_height)
      .accessibilityHidden(true)
  }
}

struct SettingsDropdown: View {
  /// `raised` matches the preset selector for dropdowns placed on dark boards rather than panels.
  enum Surface {
    case inset
    case raised
  }

  let title: String
  let options: [SettingsOption]
  @Binding var selection: String
  @Binding var expanded: String?
  var placeholder = "Select device"
  var surface = Surface.inset
  var browse = false
  var helpText: String?
  @Environment(\.uiSize) private var size
  @Environment(\.isEnabled) private var enabled
  @State private var hovered = false

  var body: some View {
    let metrics = size.metrics
    let open = expanded == title
    let selected = options.first { $0.id == selection }
    Button {
      expanded = open ? nil : title
    } label: {
      if browse {
        CarouselControlIcon(name: "browse")
          .opacity(hovered || open ? 1 : 0.7)
      } else {
        HStack(spacing: 0) {
          Text(selected?.title ?? placeholder)
            .lineLimit(1).truncationMode(.tail)
            .frame(maxWidth: .infinity, alignment: .leading)
          Image(systemName: open ? "chevron.up" : "chevron.down")
            .font(Theme.chevronFont(metrics))
            .foregroundStyle(Theme.secondary)
            .frame(width: size.dropdownAccessoryWidth - metrics.horizontal_padding)
        }
        .font(Theme.font(metrics.font_size))
        .foregroundStyle(Theme.primary)
        .padding(.leading, metrics.horizontal_padding)
        .padding(.trailing, metrics.horizontal_padding)
        .frame(height: metrics.control_height)
        .background(
          fill(active: enabled && (hovered || open)),
          in: RoundedRectangle(cornerRadius: metrics.corner_radius)
        )
        .overlay {
          RoundedRectangle(cornerRadius: metrics.corner_radius)
            .strokeBorder(
              Theme.borderHover.opacity(enabled && (hovered || open) ? 1 : 0),
              lineWidth: Theme.borderWidth)
        }
        .contentShape(Rectangle())
      }
    }
    .buttonStyle(FocusRingButtonStyle(radius: metrics.corner_radius))
    .disabled(options.isEmpty)
    .opacity(enabled && !options.isEmpty ? 1 : Theme.disabledOpacity)
    .onHover { hovered = $0 }
    .accessibilityLabel(title)
    .accessibilityValue(
      selected.map { option in option.group.map { "\(option.title), \($0)" } ?? option.title }
        ?? "No selection"
    )
    .help(
      helpText ?? selected.flatMap { option in option.group.map { "\($0) — \(option.title)" } }
        ?? ""
    )
    .anchorPreference(key: DropdownPreference.self, value: .bounds) { anchor in
      open
        ? [
          DropdownAnchor(
            key: title, bounds: anchor, options: options, selection: selection, size: size,
            browse: browse,
            choose: {
              selection = $0
              expanded = nil
            }, dismiss: { expanded = nil })
        ] : []
    }
  }

  private func fill(active: Bool) -> Color {
    switch surface {
    case .inset: active ? Theme.insetHover : Theme.inset
    case .raised: active ? Theme.track : Theme.raised
    }
  }
}

private struct FocusRingButtonStyle: ButtonStyle {
  let radius: CGFloat
  @Environment(\.isFocused) private var focused

  func makeBody(configuration: Configuration) -> some View {
    configuration.label
      .opacity(configuration.isPressed ? Theme.pressedOpacity : 1)
      .focusRing(focused, radius: radius)
  }
}

extension View {
  func settingsDropdownOverlay() -> some View {
    overlayPreferenceValue(DropdownPreference.self) { anchors in
      GeometryReader { geometry in
        if let dropdown = anchors.first {
          Color.clear
            .contentShape(Rectangle())
            .onTapGesture { dropdown.dismiss() }
            .accessibilityLabel("Dismiss menu")
          SettingsDropdownMenu(
            dropdown: dropdown, bounds: geometry[dropdown.bounds], container: geometry.size
          )
          .id(dropdown.key)
        }
      }
    }
    // Menus open in the nearest overlay only, so nested overlays never show one menu twice.
    .transformPreference(DropdownPreference.self) { $0.removeAll() }
  }
}

/// Grouped menus may extend past a narrow dropdown so that group names remain readable.
private let groupedMenuMinimumWidth: CGFloat = 240

private struct SettingsDropdownMenu: View {
  let dropdown: DropdownAnchor
  let bounds: CGRect
  let container: CGSize
  @State private var level = SettingsMenuLevel.root
  @State private var highlighted: SettingsMenuRow.ID?
  @FocusState private var focused: Bool

  private var content: SettingsMenuContent {
    SettingsMenuContent(options: dropdown.options, selection: dropdown.selection)
  }

  private func menuHeight(rows: Int) -> CGFloat {
    min(
      CGFloat(rows) * CGFloat(dropdown.size.metrics.menu_row_height) + 2 * Theme.menuInset,
      Theme.menuMaxHeight)
  }

  var body: some View {
    let size = dropdown.size
    let metrics = size.metrics
    let radius = metrics.corner_radius + 1
    let content = content
    let rows = content.rows(at: level)
    let height = menuHeight(rows: rows.count)
    let below = bounds.maxY + Theme.menuOffset
    let opensBelow =
      !dropdown.browse
      && below + menuHeight(rows: content.maximumRowCount)
        <= container.height - 2 * Theme.menuOffset
    let y =
      opensBelow ? below : max(2 * Theme.menuOffset, bounds.minY - height - Theme.menuOffset)
    let width =
      content.grouped || dropdown.browse
      ? min(max(bounds.width, groupedMenuMinimumWidth), container.width - 2 * Theme.menuOffset)
      : bounds.width
    let anchorX = dropdown.browse ? bounds.maxX - width : bounds.minX
    let x =
      content.grouped || dropdown.browse
      ? max(Theme.menuOffset, min(anchorX, container.width - Theme.menuOffset - width))
      : bounds.minX
    ScrollViewReader { proxy in
      ScrollView {
        VStack(spacing: 0) {
          ForEach(rows) { row in
            Button {
              activate(row.id)
            } label: {
              label(row, size: size)
                .padding(.leading, metrics.horizontal_padding - Theme.menuInset)
                .frame(height: metrics.menu_row_height)
                .background(
                  highlighted == row.id ? Theme.highlight : Color.clear,
                  in: RoundedRectangle(cornerRadius: metrics.corner_radius - 2)
                )
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .modifier(MenuRowAccessibility(row: row, selection: dropdown.selection))
            .onHover { if $0 { highlighted = row.id } }
            .id(row.id)
          }
        }.padding(Theme.menuInset)
      }
      .onAppear {
        highlighted = content.initialHighlight(at: level)
        focused = true
        if let highlighted { proxy.scrollTo(highlighted, anchor: .center) }
      }
      .onChange(of: highlighted) { _, value in
        if let value { proxy.scrollTo(value) }
      }
    }
    .font(Theme.font(metrics.font_size))
    .background(Theme.inset, in: RoundedRectangle(cornerRadius: radius))
    .overlay {
      RoundedRectangle(cornerRadius: radius).strokeBorder(
        Theme.border, lineWidth: Theme.borderWidth)
    }
    .focusable().focusEffectDisabled().focused($focused)
    .onKeyPress(.escape) {
      dropdown.dismiss()
      return .handled
    }
    .onKeyPress(.upArrow) {
      highlighted = content.highlight(after: highlighted, moving: -1, at: level)
      return .handled
    }
    .onKeyPress(.downArrow) {
      highlighted = content.highlight(after: highlighted, moving: 1, at: level)
      return .handled
    }
    .onKeyPress(.rightArrow) {
      guard case .group? = highlighted else { return .ignored }
      activate(highlighted)
      return .handled
    }
    .onKeyPress(.leftArrow) {
      guard case .group = level else { return .ignored }
      activate(.back)
      return .handled
    }
    .onKeyPress(.return) {
      activate(highlighted)
      return .handled
    }
    .onKeyPress(.space) {
      activate(highlighted)
      return .handled
    }
    .onKeyPress(.tab) {
      dropdown.dismiss()
      return .ignored
    }
    .frame(width: width, height: height)
    .offset(x: x, y: y)
  }

  @ViewBuilder
  private func label(_ row: SettingsMenuRow, size: UISize) -> some View {
    let metrics = size.metrics
    let chevron = Theme.chevronFont(metrics)
    switch row {
    case .back(let group):
      HStack(spacing: 0) {
        Image(systemName: "chevron.left").font(chevron)
          .frame(width: metrics.checkbox_size, alignment: .leading)
        Text(group).lineLimit(1).truncationMode(.tail)
          .frame(maxWidth: .infinity, alignment: .leading)
      }
      .foregroundStyle(Theme.secondary)
      .padding(.trailing, metrics.horizontal_padding - Theme.menuInset)
    case .group(let name, _, let selected):
      HStack(spacing: 0) {
        Text(name).lineLimit(1).truncationMode(.tail)
          .foregroundStyle(selected == nil ? Theme.label : Theme.primary)
          .frame(maxWidth: .infinity, alignment: .leading)
        if selected != nil {
          CheckMark().fill(Theme.fill)
            .frame(width: metrics.checkbox_size, height: metrics.checkbox_size)
        }
        Image(systemName: "chevron.right").font(chevron)
          .foregroundStyle(Theme.secondary)
          .frame(width: size.dropdownAccessoryWidth)
      }
    case .option(let option):
      let selected = option.id == dropdown.selection
      HStack(spacing: 0) {
        Text(option.title).lineLimit(1).truncationMode(.tail)
          .foregroundStyle(selected ? Theme.primary : Theme.label)
          .frame(maxWidth: .infinity, alignment: .leading)
        CheckMark().fill(Theme.fill)
          .frame(width: metrics.checkbox_size, height: metrics.checkbox_size)
          .opacity(selected ? 1 : 0)
          .frame(width: size.dropdownAccessoryWidth)
      }
    }
  }

  private func activate(_ id: SettingsMenuRow.ID?) {
    guard let id, let action = content.action(for: id, at: level) else { return }
    switch action {
    case .choose(let option):
      dropdown.choose(option)
    case .show(let next):
      let previous = level
      level = next
      highlighted = content.initialHighlight(at: next, from: previous)
    }
  }
}

private struct MenuRowAccessibility: ViewModifier {
  let row: SettingsMenuRow
  let selection: String

  func body(content: Content) -> some View {
    switch row {
    case .back(let group):
      content.accessibilityLabel("Back").accessibilityHint("Leaves \(group)")
    case .group(let name, let count, let selected):
      content.accessibilityLabel(name)
        .accessibilityValue(selected?.title ?? "")
        .accessibilityHint("Shows \(count) \(count == 1 ? "option" : "options")")
        .accessibilityAddTraits(selected == nil ? [] : .isSelected)
    case .option(let option):
      content.accessibilityAddTraits(option.id == selection ? .isSelected : [])
    }
  }
}

/// The reference checkbox: a light filled square with a dark tick when on.
struct SettingsCheckboxStyle: ToggleStyle {
  var labelHidden = false
  @Environment(\.isEnabled) private var enabled
  @Environment(\.uiSize) private var size

  func makeBody(configuration: Configuration) -> some View {
    let metrics = size.metrics
    let shape = RoundedRectangle(cornerRadius: metrics.checkbox_radius)
    Button {
      configuration.isOn.toggle()
    } label: {
      HStack(spacing: metrics.label_gap) {
        ZStack {
          shape.fill(configuration.isOn ? Theme.fill : Theme.insetHover)
          if configuration.isOn {
            CheckMark().fill(Theme.onFill)
          } else {
            shape.strokeBorder(Theme.offBorder, lineWidth: Theme.borderWidth)
          }
        }
        .frame(width: metrics.checkbox_size, height: metrics.checkbox_size)
        if !labelHidden {
          configuration.label.font(Theme.font(metrics.font_size)).foregroundStyle(Theme.label)
        }
      }
      .opacity(enabled ? 1 : Theme.disabledOpacity).contentShape(Rectangle())
    }
    .buttonStyle(FocusRingButtonStyle(radius: metrics.checkbox_radius + 2))
    .accessibilityValue(configuration.isOn ? "On" : "Off")
  }
}

struct SettingsButtonStyle: ButtonStyle {
  var primary = false
  @Environment(\.isEnabled) private var enabled
  @Environment(\.isFocused) private var focused
  @Environment(\.uiSize) private var size

  func makeBody(configuration: Configuration) -> some View {
    let metrics = size.metrics
    configuration.label
      .font(Theme.font(metrics.font_size))
      .lineLimit(1)
      .foregroundStyle(primary ? Theme.onFill : Theme.primary)
      .padding(.horizontal, metrics.horizontal_padding + 4)
      .frame(minHeight: metrics.control_height)
      .background(
        primary ? Theme.fill : Theme.control,
        in: RoundedRectangle(cornerRadius: metrics.corner_radius)
      )
      .focusRing(focused, radius: metrics.corner_radius)
      .opacity(
        !enabled ? Theme.disabledOpacity : configuration.isPressed ? Theme.pressedOpacity : 1)
  }
}

struct SettingsIconButton: View {
  let systemName: String
  let label: String
  let action: () -> Void
  @Environment(\.uiSize) private var size
  @Environment(\.isEnabled) private var enabled
  @State private var hovered = false

  var body: some View {
    let metrics = size.metrics
    Button(action: action) {
      Image(systemName: systemName)
        .font(Theme.iconFont(metrics))
        .foregroundStyle(hovered && enabled ? Theme.primary : Theme.secondary)
        .frame(width: metrics.control_height, height: metrics.control_height)
        .opacity(enabled ? 1 : Theme.disabledOpacity)
        .contentShape(Rectangle())
    }
    .buttonStyle(FocusRingButtonStyle(radius: metrics.corner_radius))
    .onHover { hovered = $0 }
    .accessibilityLabel(label)
    .help(label)
  }
}

struct SettingsSegmentedControl: View {
  let title: String
  let options: [SettingsOption]
  @Binding var selection: String
  @Environment(\.uiSize) private var size

  var body: some View {
    let metrics = size.metrics
    HStack(spacing: 2) {
      ForEach(options) { option in
        let selected = option.id == selection
        Button {
          selection = option.id
        } label: {
          Text(option.title)
            .foregroundStyle(selected ? Theme.primary : Theme.secondary)
            .padding(.horizontal, metrics.horizontal_padding)
            .frame(maxHeight: .infinity)
            .background(
              selected ? Theme.highlight : Color.clear,
              in: RoundedRectangle(cornerRadius: metrics.corner_radius - 2)
            )
            .contentShape(Rectangle())
        }
        .buttonStyle(FocusRingButtonStyle(radius: metrics.corner_radius - 2))
        .accessibilityAddTraits(selected ? .isSelected : [])
      }
    }
    .font(Theme.font(metrics.font_size))
    .padding(2)
    .frame(height: metrics.control_height)
    .background(Theme.inset, in: RoundedRectangle(cornerRadius: metrics.corner_radius))
    .accessibilityElement(children: .contain)
    .accessibilityLabel(title)
  }
}

struct SettingsNumberField: View {
  let label: String
  @Binding var value: Int
  let range: ClosedRange<Int>
  @Environment(\.uiSize) private var size
  @FocusState private var focused: Bool

  var body: some View {
    numberCell(
      TextField(
        label,
        value: Binding(
          get: { value }, set: { value = min(range.upperBound, max(range.lowerBound, $0)) }),
        format: .number), size: size, focused: $focused)
  }
}

struct SettingsDecimalField: View {
  let label: String
  @Binding var value: Double
  let range: ClosedRange<Double>
  @Environment(\.uiSize) private var size
  @FocusState private var focused: Bool

  var body: some View {
    numberCell(
      TextField(
        label,
        value: Binding(
          get: { value },
          set: { value = $0.isFinite ? min(range.upperBound, max(range.lowerBound, $0)) : 0 }),
        format: .number), size: size, focused: $focused)
  }
}

@MainActor
private func numberCell(_ field: TextField<Text>, size: UISize, focused: FocusState<Bool>.Binding)
  -> some View
{
  let metrics = size.metrics
  return
    field
    .textFieldStyle(.plain).multilineTextAlignment(.center)
    .font(Theme.valueFont(metrics.font_size))
    .foregroundStyle(Theme.primary)
    .focused(focused)
    .frame(height: metrics.control_height)
    .background(Theme.inset, in: RoundedRectangle(cornerRadius: metrics.corner_radius))
    .focusRing(focused.wrappedValue, radius: metrics.corner_radius)
}

struct SettingsSlider: View {
  let label: String
  @Binding var value: Double
  let range: ClosedRange<Double>
  var step = 1.0
  var valueDescription: (Double) -> String = { String(format: "%.0f", $0) }
  @FocusState private var focused: Bool

  private func set(_ proposed: Double) {
    value = min(range.upperBound, max(range.lowerBound, proposed))
  }

  var body: some View {
    let knob: CGFloat = 12
    GeometryReader { geometry in
      let width = max(1, geometry.size.width - knob)
      let span = range.upperBound - range.lowerBound
      let position = CGFloat((value - range.lowerBound) / span) * width
      ZStack(alignment: .leading) {
        Capsule().fill(Theme.track).frame(height: 3)
        Capsule().fill(Theme.label).frame(width: position + knob / 2, height: 3)
        Circle().fill(Theme.primary).frame(width: knob, height: knob)
          .overlay {
            Circle().strokeBorder(Theme.focus.opacity(focused ? 1 : 0), lineWidth: Theme.focusWidth)
              .padding(-3)
          }
          .offset(x: position)
      }
      .frame(height: 22)
      .contentShape(Rectangle())
      .gesture(
        DragGesture(minimumDistance: 0).onChanged { gesture in
          set(range.lowerBound + Double((gesture.location.x - knob / 2) / width) * span)
        })
    }
    .frame(height: 22)
    .accessibilityElement()
    .accessibilityLabel(label)
    .accessibilityValue(valueDescription(value))
    .accessibilityAdjustableAction { direction in
      set(value + (direction == .increment ? step : -step))
    }
    .focusable().focusEffectDisabled().focused($focused)
    .onKeyPress(.leftArrow) {
      set(value - step)
      return .handled
    }
    .onKeyPress(.rightArrow) {
      set(value + step)
      return .handled
    }
  }
}
