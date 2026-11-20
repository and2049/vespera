import CryptoKit
import Darwin
import Foundation

struct ModuleFileSlot: Equatable, Sendable {
  let id: UInt32
  let name: String
  let types: [String]
  let bundled: String?
  let maximumBytes: UInt64
  let optional: Bool
}

struct ModuleFileReference: Codable, Equatable, Sendable {
  enum Scope: String, Codable, Sendable { case library, package }
  var slot: UInt32
  var scope: Scope
  var path: String
  var name: String
  var bytes: UInt64
  var sha256: String

  func validate() throws {
    guard slot != 0, !name.isEmpty, name.utf8.count < 256, !name.contains("\0"),
      bytes <= FileLibrary.maximumFileBytes, sha256.utf8.count == 64,
      sha256.utf8.allSatisfy({ (48...57).contains($0) || (97...102).contains($0) }),
      FileLibrary.safeRelative(path), scope != .library || path == sha256
    else { throw AmpError.message("Invalid file reference") }
  }
}

actor FileLibrary {
  static let shared = FileLibrary()
  static let maximumFileBytes: UInt64 = 64 * 1024 * 1024
  static let maximumLibraryBytes: UInt64 = 1024 * 1024 * 1024
  static let maximumEntries = 1024
  let root: URL
  private let capacityBytes: UInt64
  private let capacityEntries: Int

  init(
    root: URL = FileLibrary.defaultRoot,
    capacityBytes: UInt64 = FileLibrary.maximumLibraryBytes,
    capacityEntries: Int = FileLibrary.maximumEntries
  ) {
    self.root = root.standardizedFileURL
    self.capacityBytes = min(capacityBytes, Self.maximumLibraryBytes)
    self.capacityEntries = max(0, min(capacityEntries, Self.maximumEntries))
  }

  nonisolated static var defaultRoot: URL {
    if let override = ProcessInfo.processInfo.environment["VESPERA_FILE_LIBRARY_PATH"] {
      return URL(fileURLWithPath: override, isDirectory: true)
    }
    return FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
      .appendingPathComponent("vespera/Files", isDirectory: true)
  }

  nonisolated static func safeRelative(_ path: String) -> Bool {
    guard !path.isEmpty, path.utf8.count <= 4096, !path.hasPrefix("/"),
      !path.contains("\0"), !path.contains("\\"), !path.contains(":")
    else { return false }
    return path.split(separator: "/", omittingEmptySubsequences: false)
      .allSatisfy { !$0.isEmpty && $0 != "." && $0 != ".." }
  }

  nonisolated static func contained(_ path: String, in root: URL) throws -> URL {
    guard safeRelative(path) else { throw AmpError.message("Unsafe file path") }
    let base = root.standardizedFileURL.resolvingSymlinksInPath()
    let file = base.appendingPathComponent(path).standardizedFileURL
    guard file.resolvingSymlinksInPath().path.hasPrefix(base.path + "/") else {
      throw AmpError.message("File path leaves its resource directory")
    }
    var current = base
    for component in path.split(separator: "/") {
      current.appendPathComponent(String(component))
      let values = try? current.resourceValues(forKeys: [.isSymbolicLinkKey])
      if values?.isSymbolicLink == true {
        throw AmpError.message("Symbolic links are not file assets")
      }
    }
    return file
  }

  nonisolated static func resolvedPath(
    _ reference: ModuleFileReference, resources: URL, library: URL = defaultRoot
  ) throws -> URL {
    try reference.validate()
    return try contained(reference.path, in: reference.scope == .package ? resources : library)
  }

  private func read(
    _ source: URL, limit: UInt64, consume: (Data) throws -> Void = { _ in }
  ) throws -> (UInt64, String) {
    let descriptor = Darwin.open(source.path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)
    guard descriptor >= 0 else { throw AmpError.message("File is missing or unreadable") }
    let input = FileHandle(fileDescriptor: descriptor, closeOnDealloc: true)
    defer { try? input.close() }
    var info = stat()
    guard fstat(descriptor, &info) == 0, info.st_mode & S_IFMT == S_IFREG,
      info.st_size >= 0, UInt64(info.st_size) <= min(limit, Self.maximumFileBytes)
    else { throw AmpError.message("File is not regular or exceeds its byte limit") }
    var hash = SHA256()
    var used: UInt64 = 0
    while let data = try input.read(upToCount: 65536), !data.isEmpty {
      try Task.checkCancellation()
      guard UInt64(data.count) <= limit - used else {
        throw AmpError.message("The file grew beyond its size limit")
      }
      used += UInt64(data.count)
      hash.update(data: data)
      try consume(data)
    }
    return (used, hash.finalize().map { String(format: "%02x", $0) }.joined())
  }

  nonisolated static func boundedEntries(in root: URL, limit: Int) throws -> [URL] {
    guard let directory = opendir(root.path) else {
      throw AmpError.message("Cannot read file directory")
    }
    defer { closedir(directory) }
    var entries: [URL] = []
    errno = 0
    while let entry = readdir(directory) {
      let name = withUnsafeBytes(of: entry.pointee.d_name) {
        String(validatingCString: $0.baseAddress!.assumingMemoryBound(to: CChar.self))
      }
      guard let name else { throw AmpError.message("File directory contains an invalid name") }
      if name != "." && name != ".." && name != ".lock" {
        guard entries.count < limit else {
          throw AmpError.message("File directory exceeds its \(limit)-entry limit")
        }
        entries.append(root.appendingPathComponent(name))
      }
      errno = 0
    }
    guard errno == 0 else { throw AmpError.message("Cannot read file directory") }
    return entries
  }

  func importFile(_ source: URL, slot: ModuleFileSlot) throws -> ModuleFileReference {
    try Task.checkCancellation()
    guard slot.maximumBytes > 0, slot.maximumBytes <= Self.maximumFileBytes,
      slot.types.contains(source.pathExtension.lowercased())
    else { throw AmpError.message("The file type or size limit is unsupported") }
    let values = try source.resourceValues(forKeys: [
      .isRegularFileKey, .isSymbolicLinkKey, .fileSizeKey,
    ])
    guard values.isRegularFile == true, values.isSymbolicLink != true,
      let size = values.fileSize, size >= 0, UInt64(size) <= slot.maximumBytes
    else { throw AmpError.message("Choose a regular file within the slot's size limit") }
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    let lock = Darwin.open(
      root.appendingPathComponent(".lock").path,
      O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0o600)
    guard lock >= 0 else { throw AmpError.message("Cannot lock the managed file library") }
    defer { Darwin.close(lock) }
    var lockInfo = stat()
    guard fstat(lock, &lockInfo) == 0, lockInfo.st_mode & S_IFMT == S_IFREG,
      lockInfo.st_size == 0, lockInfo.st_nlink == 1,
      flock(lock, LOCK_EX) == 0
    else { throw AmpError.message("Cannot lock the managed file library") }
    defer { flock(lock, LOCK_UN) }
    let entries = try Self.boundedEntries(in: root, limit: capacityEntries)
    guard entries.count <= capacityEntries else {
      throw AmpError.message("The managed file library is full")
    }
    var total: UInt64 = 0
    for entry in entries {
      let info = try entry.resourceValues(forKeys: [
        .fileSizeKey, .isRegularFileKey, .isSymbolicLinkKey,
      ])
      guard info.isRegularFile == true, info.isSymbolicLink != true,
        let count = info.fileSize, count >= 0,
        UInt64(count) <= capacityBytes - min(total, capacityBytes)
      else { throw AmpError.message("The managed file library is invalid or full") }
      total += UInt64(count)
    }
    let temporary = root.appendingPathComponent(".import-\(UUID().uuidString)")
    let temporaryDescriptor = Darwin.open(
      temporary.path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0o600)
    guard temporaryDescriptor >= 0 else {
      throw AmpError.message("Cannot create managed file")
    }
    defer { try? FileManager.default.removeItem(at: temporary) }
    let output = FileHandle(fileDescriptor: temporaryDescriptor, closeOnDealloc: true)
    defer { try? output.close() }
    let (used, digest) = try read(source, limit: slot.maximumBytes) {
      try output.write(contentsOf: $0)
    }
    let destination = try Self.contained(digest, in: root)
    if FileManager.default.fileExists(atPath: destination.path) {
      let existing = try? read(destination, limit: slot.maximumBytes)
      if existing?.0 != used || existing?.1 != digest {
        let oldSize = try destination.resourceValues(forKeys: [.fileSizeKey]).fileSize ?? 0
        guard used <= capacityBytes - (total - min(total, UInt64(max(0, oldSize))))
        else {
          throw AmpError.message("The managed file library is full")
        }
        try output.synchronize()
        guard Darwin.rename(temporary.path, destination.path) == 0 else {
          throw AmpError.message("Cannot repair the managed copy")
        }
      }
    } else {
      guard entries.count < capacityEntries, used <= capacityBytes - total else {
        throw AmpError.message("The managed file library is full (1 GiB / 1024 files)")
      }
      try output.synchronize()
      try FileManager.default.moveItem(at: temporary, to: destination)
    }
    return ModuleFileReference(
      slot: slot.id, scope: .library, path: digest, name: source.lastPathComponent,
      bytes: used, sha256: digest)
  }

  func bundledFile(_ source: URL, resources: URL, slot: ModuleFileSlot) throws
    -> ModuleFileReference
  {
    try Task.checkCancellation()
    let base = resources.standardizedFileURL
    let prefix = base.path + "/"
    guard source.standardizedFileURL.path.hasPrefix(prefix) else {
      throw AmpError.message("Bundled file leaves its package")
    }
    let relative = String(source.standardizedFileURL.path.dropFirst(prefix.count))
    let url = try Self.contained(relative, in: base)
    let info = try url.resourceValues(forKeys: [.fileSizeKey, .isRegularFileKey])
    guard info.isRegularFile == true, let count = info.fileSize, count >= 0,
      UInt64(count) <= min(slot.maximumBytes, Self.maximumFileBytes),
      slot.types.contains(url.pathExtension.lowercased())
    else { throw AmpError.message("Bundled file is invalid or too large") }
    let (size, hash) = try read(url, limit: slot.maximumBytes)
    return ModuleFileReference(
      slot: slot.id, scope: .package, path: relative, name: url.lastPathComponent,
      bytes: size, sha256: hash)
  }
}
