import SwiftUI

/// One control instance per installed Special FX module. Which editor is shown and which modules
/// process are independent: showing an editor never changes processing, and each module's own
/// power control is separate from whether it is in the signal chain at all.
@MainActor
final class SpecialFXCatalog: ObservableObject {
  static let slotPrefix = "specialFX."

  let modules: [LoadedModule]
  @Published private(set) var shownIndex = 0
  /// IDs of the modules in the signal chain.
  @Published private(set) var active: Set<String> = []
  @Published private(set) var error: String?
  /// Receives the whole Special FX section a change of `active` would process; throwing rejects
  /// the change.
  var onChange: (([[String: Any]]) throws -> Void)?

  var shownModule: LoadedModule? {
    modules.indices.contains(shownIndex) ? modules[shownIndex] : nil
  }
  /// The modules in the signal chain, in catalog order.
  var activeModules: [LoadedModule] { modules.filter { active.contains($0.id) } }

  init(directory: URL? = nil) {
    let (loaded, failures) = ModuleDiscovery.catalog(
      category: "special-fx",
      directories: directory.map { [$0] } ?? ModuleDiscovery.searchDirectories(),
      make: {
        try LoadedModule(
          manifest: $0.manifest, directory: $0.directory, category: "special-fx")
      },
      orderKey: \.familyOrderKey)
    modules = loaded
    if !failures.isEmpty { error = failures.joined(separator: " ") }
  }

  static func slot(_ id: String) -> String { slotPrefix + id }

  func isActive(_ id: String) -> Bool { active.contains(id) }

  /// Redraws what depends on a module's route after a parameter change that may select another.
  func parameterChanged(_ module: LoadedModule, _ id: UInt32) {
    if id == module.routeSelector { objectWillChange.send() }
  }

  /// Shows a module's editor. Never changes processing.
  func show(_ index: Int) {
    guard modules.indices.contains(index) else { return }
    shownIndex = index
  }

  /// Adds a module to the signal chain or removes it, once the audio side accepts the section.
  /// A rejected change leaves the chain as it was and reports why.
  func setActive(_ id: String, _ on: Bool) {
    guard modules.contains(where: { $0.id == id }), isActive(id) != on else { return }
    var next = active
    if on { next.insert(id) } else { next.remove(id) }
    do {
      try onChange?(try audioDescriptions(active: next))
      active = next
      error = nil
    } catch { self.error = error.localizedDescription }
  }

  /// Takes a shown editor and chain the audio side already processes, without calling
  /// `onChange`.
  func restore(shown: Int?, active: Set<String>) {
    if let shown, modules.indices.contains(shown) { shownIndex = shown }
    self.active = active.filter { id in modules.contains { $0.id == id } }
    error = nil
  }

  /// One description per module in `active` (the current chain by default), in catalog order,
  /// with staged settings where given.
  func audioDescriptions(
    active: Set<String>? = nil, staged: [String: StagedModuleSettings] = [:]
  ) throws -> [[String: Any]] {
    let active = active ?? self.active
    return try modules.filter { active.contains($0.id) }.map { module in
      var description =
        try staged[module.id].map { try module.audioDescription($0) }
        ?? module.audioDescription()
      description["slot"] = Self.slot(module.id)
      description.merge(module.routeDescription) { _, route in route }
      return description
    }
  }
}
