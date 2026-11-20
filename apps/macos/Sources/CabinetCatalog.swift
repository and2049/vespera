import SwiftUI

@MainActor
final class CabinetCatalog: ObservableObject {
  enum Selection: Equatable {
    case linked
    case independent(String)
  }
  let cabinets: [LoadedModule]
  @Published private(set) var selection = Selection.linked
  @Published private(set) var amp: LoadedModule?
  @Published private(set) var error: String?
  var onChange: ((LoadedModule?, LoadedModule?) throws -> Void)?

  var supported: Bool { amp?.cabinetSupport?.supported == true }
  var locked: Bool { selection == .linked }
  var selectedID: String? {
    guard supported else { return nil }
    switch selection {
    case .linked: return amp?.cabinetSupport?.defaultModule
    case .independent(let id): return id
    }
  }
  var activeCabinet: LoadedModule? { cabinets.first { $0.id == selectedID } }
  var selectedIndex: Int { cabinets.firstIndex { $0.id == selectedID } ?? 0 }
  var enabled: Bool {
    guard let cabinet = activeCabinet else { return false }
    guard let bypass = cabinet.bypassParameter else { return true }
    return ((try? cabinet.value(bypass)) ?? 0) >= 0.5
  }

  func toggleEnabled() {
    guard let cabinet = activeCabinet, let bypass = cabinet.bypassParameter else { return }
    do {
      try cabinet.setValue(bypass, enabled ? 0 : 1)
      error = nil
      objectWillChange.send()
    } catch { self.error = error.localizedDescription }
  }

  init(directory: URL? = nil) {
    let (loaded, failures) = ModuleDiscovery.catalog(
      category: "cab", directories: directory.map { [$0] } ?? ModuleDiscovery.searchDirectories(),
      make: {
        try LoadedModule(manifest: $0.manifest, directory: $0.directory, category: "cab")
      },
      orderKey: \.familyOrderKey)
    cabinets = loaded
    if !failures.isEmpty { error = failures.joined(separator: " ") }
  }

  /// The cabinet that processes behind `amp` under `selection`.
  func cabinet(for amp: LoadedModule?, selection: Selection) throws -> LoadedModule? {
    let nextID: String?
    if amp?.cabinetSupport?.supported == true {
      guard let defaultID = amp?.cabinetSupport?.defaultModule, !defaultID.isEmpty else {
        throw AmpError.message("The amp does not declare a default cabinet.")
      }
      switch selection {
      case .linked: nextID = defaultID
      case .independent(let id): nextID = id
      }
    } else {
      nextID = nil
    }
    let cabinet = cabinets.first { $0.id == nextID }
    if nextID != nil && cabinet == nil {
      throw AmpError.message("The selected amp's cabinet package is unavailable.")
    }
    return cabinet
  }

  func setAmp(_ next: LoadedModule?) throws {
    let cabinet = try cabinet(for: next, selection: selection)
    try onChange?(next, cabinet)
    amp = next
    error = nil
  }

  /// Takes a selection the audio side already processes, without calling `onChange`.
  func restore(selection: Selection, amp: LoadedModule?) {
    self.selection = selection
    self.amp = amp
    error = nil
  }

  func select(_ index: Int) {
    guard supported, !locked, cabinets.indices.contains(index) else { return }
    do {
      try onChange?(amp, cabinets[index])
      selection = .independent(cabinets[index].id)
      error = nil
    } catch { self.error = error.localizedDescription }
  }

  func toggleLock() {
    guard supported else { return }
    if locked {
      guard let selectedID else { return }
      selection = .independent(selectedID)
      error = nil
    } else {
      do {
        let id = amp?.cabinetSupport?.defaultModule
        let cabinet = cabinets.first { $0.id == id }
        if id != nil && cabinet == nil {
          throw AmpError.message("The matching cabinet package is unavailable.")
        }
        try onChange?(amp, cabinet)
        selection = .linked
        error = nil
      } catch { self.error = error.localizedDescription }
    }
  }
}
