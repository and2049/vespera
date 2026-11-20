import CryptoKit
import Foundation

/// The managed preset library: one reserved Default document, named presets each in its own
/// file, and the last selection. Every write replaces its file atomically.
struct PresetStore {
  enum Selection: Codable, Equatable, Hashable {
    case defaultPreset
    case named(UUID)
  }

  struct Entry: Identifiable, Equatable {
    let id: UUID
    let name: String
  }

  static let defaultName = "Default"
  static let maximumFileBytes = 48 * 1024 * 1024
  static let maximumNameLength = 64

  let directory: URL

  static var standardDirectory: URL {
    FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
      .appendingPathComponent("vespera/Presets", isDirectory: true)
  }

  init(directory: URL = PresetStore.standardDirectory) {
    self.directory = directory
  }

  private var namedDirectory: URL { directory.appendingPathComponent("Named", isDirectory: true) }
  private var defaultURL: URL {
    directory.appendingPathComponent("Default").appendingPathExtension(RigPreset.fileExtension)
  }
  private var selectionURL: URL { directory.appendingPathComponent("selection.json") }
  /// Original files of presets whose load changed what they process. Never listed or pruned.
  var supersededDirectory: URL {
    directory.appendingPathComponent("Superseded", isDirectory: true)
  }

  /// A preset as read, with the exact bytes of its file.
  struct Document {
    let preset: RigPreset
    let data: Data
  }

  func url(for id: UUID) -> URL {
    namedDirectory.appendingPathComponent(id.uuidString).appendingPathExtension(
      RigPreset.fileExtension)
  }

  /// Named presets ordered by name, plus a message for each file that could not be read. Files
  /// that cannot be read are reported, never removed.
  func entries() -> (entries: [Entry], failures: [String]) {
    let files: [URL]
    do {
      files = try FileManager.default.contentsOfDirectory(
        at: namedDirectory, includingPropertiesForKeys: nil)
    } catch {
      if (error as NSError).code == NSFileReadNoSuchFileError { return ([], []) }
      return ([], ["Cannot read presets in \(namedDirectory.path): \(error.localizedDescription)"])
    }
    var entries: [Entry] = []
    var failures: [String] = []
    for file in files where file.pathExtension == RigPreset.fileExtension {
      guard let id = UUID(uuidString: file.deletingPathExtension().lastPathComponent) else {
        continue
      }
      do {
        let name = try Self.read(file).name
        guard (try? Self.validName(name)) == name else {
          throw AmpError.message("\(file.lastPathComponent) has an invalid preset name")
        }
        entries.append(Entry(id: id, name: name))
      } catch {
        failures.append(error.localizedDescription)
      }
    }
    return (Self.sorted(entries), failures)
  }

  static func sorted(_ entries: [Entry]) -> [Entry] {
    entries.sorted {
      let order = $0.name.localizedStandardCompare($1.name)
      return order == .orderedSame
        ? $0.id.uuidString < $1.id.uuidString : order == .orderedAscending
    }
  }

  func loadDefault() throws -> RigPreset? { try loadDefaultDocument()?.preset }

  func loadDefaultDocument() throws -> Document? {
    guard FileManager.default.fileExists(atPath: defaultURL.path) else { return nil }
    let document = try Self.document(defaultURL)
    guard document.preset.name == Self.defaultName else {
      throw AmpError.message("\(defaultURL.lastPathComponent) is not the Default preset")
    }
    return document
  }

  func saveDefault(_ rig: RigSnapshot) throws {
    try write(RigPreset(name: Self.defaultName, rig: rig), to: defaultURL)
  }

  func load(_ id: UUID) throws -> RigPreset { try Self.read(url(for: id)) }

  func loadDocument(_ id: UUID) throws -> Document { try Self.document(url(for: id)) }

  /// Keeps a copy of a preset file's original bytes in `supersededDirectory`, named by their
  /// content, unless that copy already exists.
  @discardableResult
  func preserve(_ data: Data) throws -> URL {
    let digest = SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
    let url = supersededDirectory.appendingPathComponent(String(digest.prefix(16)))
      .appendingPathExtension(RigPreset.fileExtension)
    guard !FileManager.default.fileExists(atPath: url.path) else { return url }
    try ensureDirectory(supersededDirectory)
    try atomicWrite(data, to: url)
    return url
  }

  func save(_ preset: RigPreset, as id: UUID) throws {
    try write(preset, to: url(for: id))
  }

  func delete(_ id: UUID) throws {
    do { try FileManager.default.removeItem(at: url(for: id)) } catch {
      throw AmpError.message(
        "Cannot delete \(url(for: id).path): \(error.localizedDescription)")
    }
  }

  func loadSelection() -> Selection {
    guard let data = try? Data(contentsOf: selectionURL),
      let saved = try? JSONDecoder().decode(SavedSelection.self, from: data),
      saved.schemaVersion == 1
    else { return .defaultPreset }
    return saved.selection
  }

  func saveSelection(_ selection: Selection) throws {
    try ensureDirectory(directory)
    try atomicWrite(
      try JSONEncoder().encode(SavedSelection(schemaVersion: 1, selection: selection)),
      to: selectionURL)
  }

  private struct SavedSelection: Codable {
    let schemaVersion: Int
    let selection: Selection
  }

  static func read(_ url: URL) throws -> RigPreset { try document(url).preset }

  /// Reads and validates a preset file, bounding its size before reading it.
  static func document(_ url: URL) throws -> Document {
    let size: Int
    do {
      size = try url.resourceValues(forKeys: [.fileSizeKey]).fileSize ?? 0
    } catch {
      throw AmpError.message("Cannot read \(url.path): \(error.localizedDescription)")
    }
    guard size <= maximumFileBytes else {
      throw AmpError.message("\(url.lastPathComponent) is too large to be a vespera preset")
    }
    let data: Data
    do { data = try Data(contentsOf: url) } catch {
      throw AmpError.message("Cannot read \(url.path): \(error.localizedDescription)")
    }
    do { return Document(preset: try RigPreset.decode(data), data: data) } catch {
      throw AmpError.message("\(url.lastPathComponent): \(error.localizedDescription)")
    }
  }

  static func validName(_ proposed: String) throws -> String {
    let name = proposed.trimmingCharacters(in: .whitespacesAndNewlines)
    guard !name.isEmpty else { throw AmpError.message("Enter a preset name.") }
    guard name.count <= maximumNameLength, !name.contains(where: \.isNewline) else {
      throw AmpError.message("Preset names are at most \(maximumNameLength) characters.")
    }
    guard name.compare(defaultName, options: .caseInsensitive) != .orderedSame else {
      throw AmpError.message("\(defaultName) is reserved.")
    }
    return name
  }

  /// `name`, or the first of "name 2", "name 3", … that no entry uses.
  static func uniqueName(_ name: String, among entries: [Entry]) -> String {
    let used = Set(entries.map { $0.name.lowercased() } + [defaultName.lowercased()])
    guard used.contains(name.lowercased()) else { return name }
    return (2...).lazy.map { "\(name) \($0)" }.first { !used.contains($0.lowercased()) }!
  }

  private func write(_ preset: RigPreset, to url: URL) throws {
    let data = try preset.encoded()
    guard data.count <= Self.maximumFileBytes else {
      throw AmpError.message("The preset is too large to save")
    }
    try ensureDirectory(url.deletingLastPathComponent())
    try atomicWrite(data, to: url)
  }

  private func ensureDirectory(_ url: URL) throws {
    do { try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true) } catch
    {
      throw AmpError.message("Cannot create \(url.path): \(error.localizedDescription)")
    }
  }

  private func atomicWrite(_ data: Data, to url: URL) throws {
    do { try data.write(to: url, options: .atomic) } catch {
      throw AmpError.message("Cannot save \(url.path): \(error.localizedDescription)")
    }
  }
}
