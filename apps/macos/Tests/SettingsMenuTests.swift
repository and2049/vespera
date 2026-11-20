import AppKit
import SwiftUI
import Testing

@testable import VesperaUI

private let familyOptions = [
  SettingsOption(id: "a.echo", title: "Echo", group: "Family A"),
  SettingsOption(id: "a.reverb", title: "Reverb", group: "Family A"),
  SettingsOption(id: "tuner", title: "Tuner"),
  SettingsOption(id: "b.reverb", title: "Reverb", group: " Family B "),
  SettingsOption(id: "gate", title: "Gate", group: " \n "),
  SettingsOption(id: "", title: "Empty slot"),
]

@Test func ungroupedMenusStayFlat() {
  let options = ["short", "long"].map { SettingsOption(id: $0, title: $0) }
  let menu = SettingsMenuContent(options: options, selection: "long")
  #expect(!menu.grouped)
  #expect(menu.rows(at: .root) == options.map(SettingsMenuRow.option))
  #expect(menu.maximumRowCount == 2)
  #expect(menu.initialHighlight(at: .root) == .option("long"))
  #expect(menu.action(for: .option("short"), at: .root) == .choose("short"))
  let unmatched = SettingsMenuContent(options: options, selection: "missing")
  #expect(unmatched.initialHighlight(at: .root) == nil)
  #expect(unmatched.highlight(after: nil, moving: 1, at: .root) == .option("short"))
  #expect(unmatched.highlight(after: nil, moving: -1, at: .root) == .option("long"))
}

@Test func groupedMenusListFamiliesFirst() {
  let menu = SettingsMenuContent(options: familyOptions, selection: "b.reverb")
  #expect(menu.grouped)
  #expect(familyOptions[4].group == nil)
  #expect(
    menu.rows(at: .root) == [
      .group("Family A", count: 2, selected: nil),
      .option(familyOptions[2]),
      .group("Family B", count: 1, selected: familyOptions[3]),
      .option(familyOptions[4]),
      .option(familyOptions[5]),
    ])
  #expect(
    menu.rows(at: .group("Family A")) == [
      .back(group: "Family A"), .option(familyOptions[0]), .option(familyOptions[1]),
    ])
  #expect(
    menu.rows(at: .group("Family B")) == [.back(group: "Family B"), .option(familyOptions[3])])
  #expect(menu.maximumRowCount == 5)
  #expect(menu.resolved(.group("Removed")) == .root)
  #expect(menu.rows(at: .group("Removed")) == menu.rows(at: .root))
}

@Test func groupedMenuSelectionAndNavigation() {
  let menu = SettingsMenuContent(options: familyOptions, selection: "b.reverb")
  #expect(menu.initialHighlight(at: .root) == .group("Family B"))
  #expect(menu.action(for: .group("Family B"), at: .root) == .show(.group("Family B")))
  #expect(menu.initialHighlight(at: .group("Family B"), from: .root) == .option("b.reverb"))
  #expect(menu.initialHighlight(at: .group("Family A"), from: .root) == .option("a.echo"))
  #expect(menu.action(for: .option("a.reverb"), at: .root) == nil)
  #expect(menu.action(for: .option("a.reverb"), at: .group("Family A")) == .choose("a.reverb"))
  #expect(menu.action(for: .back, at: .group("Family A")) == .show(.root))
  #expect(menu.action(for: .back, at: .root) == nil)
  #expect(menu.initialHighlight(at: .root, from: .group("Family A")) == .group("Family A"))
  #expect(menu.highlight(after: .option("a.echo"), moving: -1, at: .group("Family A")) == .back)
  #expect(menu.highlight(after: .back, moving: -1, at: .group("Family A")) == .back)
  #expect(
    menu.highlight(after: .option("a.reverb"), moving: 1, at: .group("Family A"))
      == .option("a.reverb"))
  #expect(menu.highlight(after: .group("Family B"), moving: 1, at: .root) == .option("gate"))

  let empty = SettingsMenuContent(options: familyOptions, selection: "")
  #expect(empty.initialHighlight(at: .root) == .option(""))
  #expect(
    empty.rows(at: .root).allSatisfy {
      if case .group(_, _, let selected) = $0 { selected == nil } else { true }
    })
  #expect(empty.action(for: .option(""), at: .root) == .choose(""))
}

@MainActor
@Test func pedalSlotOptionsGroupByDeclaredFamily() {
  func choice(_ id: String, _ name: String, family: String?) -> PedalModuleChoice {
    var manifest = ModuleManifest(
      schemaVersion: 1, id: id, category: "post-fx", binary: "x.so", icon: "x.svg",
      resources: ".")
    manifest.pluginFamily = family
    return PedalModuleChoice(
      package: ModulePackage(directory: URL(fileURLWithPath: "/nonexistent"), manifest: manifest),
      name: name)
  }
  let options = PedalCatalog.libraryOptions([
    choice("x.reverb", "Reverb", family: " Family X "),
    choice("y.reverb", "Reverb", family: "Family Y"),
    choice("plain", "Plain", family: ""),
  ])
  #expect(options.map(\.id) == ["x.reverb", "y.reverb", "plain"])
  #expect(options.map(\.group) == ["Family X", "Family Y", nil])
  let rows = SettingsMenuContent(options: options, selection: "y.reverb").rows(at: .root)
  #expect(rows.map(\.id) == [.group("Family X"), .group("Family Y"), .option("plain")])
  #expect(PedalCatalog.libraryOptions([]).isEmpty)
}

@MainActor
@Test func activeTabIndicatorKeepsTabGeometry() throws {
  _ = NSApplication.shared
  var brightness: [Bool: CGFloat] = [:]
  var sizes: [Bool: CGSize] = [:]
  for selected in [false, true] {
    let host = NSHostingView(
      rootView: ModuleTabButton(tab: .amp, selected: selected) {}.background(Theme.window))
    host.frame = NSRect(origin: .zero, size: host.fittingSize)
    host.layoutSubtreeIfNeeded()
    sizes[selected] = host.fittingSize
    let bitmap = try #require(host.bitmapImageRepForCachingDisplay(in: host.bounds))
    host.cacheDisplay(in: host.bounds, to: bitmap)
    let scale = CGFloat(bitmap.pixelsWide) / host.bounds.width
    let centre = 2 + ModuleTabButton.indicatorSize / 2
    let color = try #require(
      bitmap.colorAt(
        x: Int(host.bounds.width / 2 * scale), y: Int((host.bounds.height - centre) * scale))?
        .usingColorSpace(.deviceRGB))
    brightness[selected] = color.brightnessComponent
  }
  #expect(sizes[true] == sizes[false])
  #expect(sizes[true] == CGSize(width: 56, height: 56))
  #expect(try #require(brightness[true]) > 0.8)
  #expect(try #require(brightness[false]) < 0.3)
}
