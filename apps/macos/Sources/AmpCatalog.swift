import SwiftUI

@MainActor
final class AmpCatalog: ObservableObject {
  var onSelection: ((LoadedModule) throws -> Void)?
  let amps: [LoadedModule]
  @Published private(set) var selectedIndex = 0
  @Published private(set) var error: String?
  var activeAmp: LoadedModule? { amps.isEmpty ? nil : amps[selectedIndex] }

  init(directory: URL? = nil) {
    let directories = directory.map { [$0] } ?? ModuleDiscovery.searchDirectories()
    let (loaded, failures) = ModuleDiscovery.catalog(
      category: "amp", directories: directories,
      make: { try LoadedModule(manifest: $0.manifest, directory: $0.directory) },
      orderKey: \.familyOrderKey)
    amps = loaded
    if amps.isEmpty {
      error =
        "Install amp .vespera packages in \(directories.map { $0.appendingPathComponent("amps").path }.joined(separator: ", ")) and relaunch. "
        + failures.joined(separator: " ")
    } else {
      select(0)
      if !failures.isEmpty { error = failures.joined(separator: " ") }
    }
  }

  func index(offset: Int) -> Int {
    guard !amps.isEmpty else { return 0 }
    return ((selectedIndex + offset) % amps.count + amps.count) % amps.count
  }

  func offset(for index: Int) -> Int {
    guard !amps.isEmpty else { return 0 }
    let distance = (index - selectedIndex + amps.count) % amps.count
    return distance > amps.count / 2 ? distance - amps.count : distance
  }

  func select(_ index: Int) {
    guard amps.indices.contains(index) else { return }
    do {
      try onSelection?(amps[index])
      if index != selectedIndex { activeAmp?.release() }
      selectedIndex = index
      error = nil
    } catch { self.error = error.localizedDescription }
  }

  func step(_ direction: Int) { select(index(offset: direction)) }

  /// Takes a selection the audio side already processes, without calling `onSelection`.
  func restore(selecting index: Int) {
    guard amps.indices.contains(index) else { return }
    if index != selectedIndex { activeAmp?.release() }
    selectedIndex = index
    error = nil
  }
}
