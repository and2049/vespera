import SwiftUI

@MainActor
final class AmpCatalog: ObservableObject {
  var onSelection: ((LoadedModule) throws -> Void)?
  let amps: [LoadedModule]
  @Published private(set) var selectedIndex = 0
  @Published private(set) var error: String?
  /// The active amp's decoded window background. Only the active amp's image is kept; it is
  /// decoded before the selection changes, so the new amp never shows without it.
  @Published private(set) var windowBackground: CGImage?
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
      if !failures.isEmpty { error = ([error].compactMap { $0 } + failures).joined(separator: " ") }
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

  /// Selects an amp whose window background decodes and whose selection the audio side accepts;
  /// otherwise the selection stays and `error` says why.
  func select(_ index: Int) {
    guard amps.indices.contains(index) else { return }
    do {
      let background = try background(of: index)
      try onSelection?(amps[index])
      if index != selectedIndex { activeAmp?.release() }
      selectedIndex = index
      show(background, of: index)
      error = nil
    } catch { self.error = error.localizedDescription }
  }

  func step(_ direction: Int) { select(index(offset: direction)) }

  /// Takes a selection the audio side already processes, without calling `onSelection`. An amp
  /// whose background does not decode is still taken, over the plain window colour.
  func restore(selecting index: Int) {
    guard amps.indices.contains(index) else { return }
    let background = Result { try self.background(of: index) }
    if index != selectedIndex { activeAmp?.release() }
    selectedIndex = index
    switch background {
    case .success(let image):
      show(image, of: index)
      error = nil
    case .failure(let failure):
      show(nil, of: nil)
      error = failure.localizedDescription
    }
  }

  /// The amp whose decoded image `windowBackground` holds.
  private var backgroundIndex: Int?

  private func show(_ image: CGImage?, of index: Int?) {
    windowBackground = image
    backgroundIndex = index
  }

  /// The background of the amp at `index`, reusing the shown image instead of decoding it again.
  private func background(of index: Int) throws -> CGImage? {
    if index == backgroundIndex { return windowBackground }
    return try amps[index].windowBackground.map(WindowBackgroundImage.decode)
  }
}
