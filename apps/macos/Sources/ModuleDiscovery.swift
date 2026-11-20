import CoreGraphics
import Foundation
import ImageIO
import LayoutRenderer
import zlib

/// One way a Special FX module can process: serially at `placement`, or, with `branch`, from the
/// chain input with its output mixed in at `placement`.
struct SpecialFXRoute: Equatable {
  static let selectableCounts = 2...8

  let placement: SpecialFXPlacement
  let branch: ModuleManifest.Routing?

  /// The route a selector value chooses among `count`: the nearest index, ties upward, clamped.
  /// The audio side and `vespera-module-check` use the same rule.
  static func index(for value: Float, count: Int) -> Int {
    let rounded = (Double(value) + 0.5).rounded(.down)
    guard rounded.isFinite else { return rounded > 0 ? count - 1 : 0 }
    return Int(min(max(rounded, 0), Double(count - 1)))
  }

  /// How the Special FX page describes the route.
  var phrase: String {
    branch == nil
      ? "Runs \(placement.phrase)" : "Listens to the input; mixed in \(placement.phrase)"
  }
}

/// Where in the host chain a Special FX module processes: one of the six gaps around the Pre FX
/// chain, amp, cabinet, EQ and Post FX chain. A gap exists whether or not its neighbours do.
enum SpecialFXPlacement: String, CaseIterable, Codable {
  case beforePreFX = "before-pre-fx"
  case beforeAmp = "before-amp"
  case beforeCabinet = "before-cabinet"
  case afterCabinet = "after-cabinet"
  case afterEQ = "after-eq"
  case afterPostFX = "after-post-fx"

  /// Where the module runs, completing "Runs …".
  var phrase: String {
    switch self {
    case .beforePreFX: "before Pre FX"
    case .beforeAmp: "after Pre FX, before the amp"
    case .beforeCabinet: "after the amp, before the cabinet"
    case .afterCabinet: "after the cabinet, before the EQ"
    case .afterEQ: "after the EQ, before Post FX"
    case .afterPostFX: "after Post FX"
    }
  }
}

struct ModuleManifest: Codable {
  var pluginFamily: String? = nil
  struct CabinetSupport: Codable {
    let supported: Bool
    var defaultModule: String? = nil
  }
  var cabinet: CabinetSupport? = nil
  /// A package-internal EQ processor owned by an amp. It is never discovered or selected alone.
  struct Equalizer: Codable {
    let id: String
    let binary: String
    var ui: UserInterface? = nil
  }
  var eq: Equalizer? = nil
  var cabinetPreview: String? = nil
  var cabinetPreviewHeight: Double? = nil
  struct ViewSize: Codable {
    let width: Double
    let height: Double
  }
  var viewSize: ViewSize? = nil
  enum Presentation: String, Codable {
    case pedal, rack
  }
  /// Superseded by `boardWidth`; `rack` spans the whole board when `boardWidth` is absent.
  var presentation: Presentation? = nil
  /// Board units a pre or post FX editor spans. Decoded as any number so that discovery can
  /// reject a fractional width by name.
  var boardWidth: Double? = nil
  static let boardWidths = 1...4
  /// The editor's board units: `boardWidth`, else the `presentation` equivalent; nil when
  /// `boardWidth` is not an integer within `boardWidths`.
  var boardUnits: Int? {
    guard let boardWidth else { return presentation == .rack ? Self.boardWidths.upperBound : 1 }
    guard boardWidth.rounded() == boardWidth,
      Double(Self.boardWidths.lowerBound)...Double(Self.boardWidths.upperBound) ~= boardWidth
    else { return nil }
    return Int(boardWidth)
  }
  struct VisualInsets: Codable, Equatable {
    var top: Double = 0
    var left: Double = 0
    var bottom: Double = 0
    var right: Double = 0

    var validated: VisualInsets {
      [top, left, bottom, right].allSatisfy { $0.isFinite && $0 >= 0 }
        ? self : VisualInsets()
    }
  }
  var visualInsets: VisualInsets? = nil
  /// A declarative UI the host renders from package resources instead of a module-owned view.
  struct UserInterface: Codable, Equatable {
    /// Package-relative path of the layout file, inside `resources`.
    let layout: String
  }
  var ui: UserInterface? = nil
  /// An amp's main-window background: a package-relative static PNG inside `resources`. Absent or
  /// null keeps the plain window colour.
  var windowBackground: String? = nil
  /// A Special FX module's `SpecialFXPlacement` raw value; absent or null means after the
  /// cabinet. Decoded as text so that discovery can reject an unknown value by name.
  var placement: String? = nil
  /// Categories in which packages of this ID are older copies of this package.
  var formerCategories: [String]? = nil
  /// A Special FX module that listens to the chain input and is mixed in by the host.
  struct Routing: Codable, Equatable {
    struct Mix: Codable, Equatable {
      let parameter: UInt32
      let law: String
    }
    let input: String
    let mix: Mix
  }
  var routing: Routing? = nil
  /// One route a selector chooses: where the module's output enters the chain and how.
  struct Route: Codable, Equatable {
    var placement: String? = nil
    var routing: Routing? = nil
  }
  /// A module parameter whose value chooses one of `routes`.
  struct RoutingSelector: Codable, Equatable {
    let parameter: UInt32
    let routes: [Route]
  }
  var routingSelector: RoutingSelector? = nil
  static let schemaVersions = 1...2
  let schemaVersion: Int
  let id: String
  let category: String
  let binary: String
  let icon: String
  let resources: String

  func resource(_ path: String, relativeTo directory: URL) throws -> URL {
    guard !path.isEmpty, !path.hasPrefix("/"), !path.split(separator: "/").contains("..") else {
      throw AmpError.message("Module resource must be a relative path: \(path)")
    }
    let root = directory.resolvingSymlinksInPath().standardizedFileURL
    let result = path.split(separator: "/").reduce(root) { directory, component in
      directory.appendingPathComponent(String(component)).resolvingSymlinksInPath()
        .standardizedFileURL
    }
    guard result == root || result.path.hasPrefix(root.path + "/") else {
      throw AmpError.message("Module resource escapes its package: \(path)")
    }
    return result
  }

  /// The declared layout's resources directory and its path relative to that directory.
  func layout(relativeTo directory: URL) throws -> (resources: URL, path: String)? {
    guard let ui else { return nil }
    let resources = try resource(self.resources, relativeTo: directory)
    let layout = try resource(ui.layout, relativeTo: directory)
    guard layout.path.hasPrefix(resources.path + "/") else {
      throw AmpError.message("Module layout must be inside its resources: \(ui.layout)")
    }
    return (resources, String(layout.path.dropFirst(resources.path.count + 1)))
  }

  /// The declared placement, defaulting to after the cabinet; only Special FX may declare one.
  func specialFXPlacement() throws -> SpecialFXPlacement {
    guard let placement else { return .afterCabinet }
    let values = SpecialFXPlacement.allCases.map(\.rawValue).joined(separator: ", ")
    guard category == "special-fx" else {
      throw AmpError.message("Only special-fx modules can declare a placement: \(id)")
    }
    guard let value = SpecialFXPlacement(rawValue: placement) else {
      throw AmpError.message("Invalid placement \"\(placement)\": use one of \(values)")
    }
    return value
  }

  /// The routes a Special FX module can take: its one static route, or those its selector
  /// parameter chooses from. Parameters are checked once the module is loaded.
  func specialFXRoutes() throws -> (selector: UInt32?, routes: [SpecialFXRoute]) {
    guard routing != nil || routingSelector != nil else {
      return (nil, [SpecialFXRoute(placement: try specialFXPlacement(), branch: nil)])
    }
    guard category == "special-fx" else {
      throw AmpError.message("Only special-fx modules can declare routing: \(id)")
    }
    guard schemaVersion >= 2 else { throw AmpError.message("routing requires schemaVersion 2") }
    func branch(_ routing: Routing?) throws -> Routing? {
      guard let routing else { return nil }
      guard routing.input == "chain-input" else {
        throw AmpError.message("Invalid routing input \"\(routing.input)\": use chain-input")
      }
      guard routing.mix.law == "crossfade" else {
        throw AmpError.message("Invalid routing mix law \"\(routing.mix.law)\": use crossfade")
      }
      return routing
    }
    guard let selector = routingSelector else {
      return (
        nil, [SpecialFXRoute(placement: try specialFXPlacement(), branch: try branch(routing))]
      )
    }
    guard placement == nil, routing == nil else {
      throw AmpError.message(
        "routingSelector cannot be combined with a top-level placement or routing")
    }
    guard SpecialFXRoute.selectableCounts.contains(selector.routes.count) else {
      throw AmpError.message(
        "routingSelector must list \(SpecialFXRoute.selectableCounts.lowerBound) to "
          + "\(SpecialFXRoute.selectableCounts.upperBound) routes")
    }
    let values = SpecialFXPlacement.allCases.map(\.rawValue).joined(separator: ", ")
    let routes = try selector.routes.map { route in
      guard let name = route.placement, let placement = SpecialFXPlacement(rawValue: name) else {
        throw AmpError.message(
          "Each routingSelector route needs a placement: use one of \(values)")
      }
      return SpecialFXRoute(placement: placement, branch: try branch(route.routing))
    }
    guard !routes.contains(where: { $0.branch?.mix.parameter == selector.parameter }) else {
      throw AmpError.message("The routingSelector parameter cannot be a route's mix parameter")
    }
    return (selector.parameter, routes)
  }

  /// Rejects members routing declarations do not define, as the audio side does, so that the
  /// declarations descriptions carry are exactly what the manifest says.
  static func validateRoutingKeys(_ manifest: Any) throws {
    func check(_ value: Any?, _ name: String, _ allowed: [String]) throws {
      guard let object = value as? [String: Any] else { return }
      if let unknown = object.keys.sorted().first(where: { !allowed.contains($0) }) {
        throw AmpError.message(
          "Unknown key \"\(unknown)\" in \(name): use \(allowed.joined(separator: " and "))")
      }
    }
    func routing(_ value: Any?, _ name: String) throws {
      try check(value, name, ["input", "mix"])
      try check((value as? [String: Any])?["mix"], "\(name).mix", ["parameter", "law"])
    }
    let manifest = manifest as? [String: Any]
    try routing(manifest?["routing"], "routing")
    let selector = manifest?["routingSelector"]
    try check(selector, "routingSelector", ["parameter", "routes"])
    for (index, route) in ((selector as? [String: Any])?["routes"] as? [Any] ?? []).enumerated() {
      let name = "routingSelector.routes[\(index)]"
      try check(route, name, ["placement", "routing"])
      try routing((route as? [String: Any])?["routing"], "\(name).routing")
    }
  }

  /// What an audio description carries for the module's routes, unresolved: the static
  /// placement and routing, or the selector for the audio side to resolve.
  func routeDescription() throws -> [String: Any] {
    func object<T: Encodable>(_ value: T) throws -> Any {
      try JSONSerialization.jsonObject(with: JSONEncoder().encode(value))
    }
    if let routingSelector { return ["routingSelector": try object(routingSelector)] }
    var description: [String: Any] = ["placement": try specialFXPlacement().rawValue]
    if let routing { description["routing"] = try object(routing) }
    return description
  }

  /// Checks `formerCategories`: distinct known categories other than the package's own.
  func validateFormerCategories() throws {
    guard let formerCategories else { return }
    let known = ModuleDiscovery.categoryFolders.keys.sorted()
    guard Set(formerCategories).count == formerCategories.count,
      formerCategories.allSatisfy({ $0 != category && known.contains($0) })
    else {
      throw AmpError.message(
        "Invalid formerCategories: list distinct categories other than \(category) from "
          + known.joined(separator: ", "))
    }
  }

  /// The declared window background after the checks discovery makes for every package: an amp's
  /// regular file inside its resources whose PNG header is within the size budget.
  func windowBackground(relativeTo directory: URL) throws -> URL? {
    guard let windowBackground else { return nil }
    guard category == "amp" else {
      throw AmpError.message("Only amps can declare a windowBackground: \(id)")
    }
    let resources = try resource(self.resources, relativeTo: directory)
    let url = try resource(windowBackground, relativeTo: directory)
    guard url.path.hasPrefix(resources.path + "/") else {
      throw AmpError.message("Window background must be inside its resources: \(windowBackground)")
    }
    guard (try? url.resourceValues(forKeys: [.isRegularFileKey]).isRegularFile) == true else {
      throw AmpError.message("Missing window background: \(url.path)")
    }
    _ = try WindowBackgroundImage.header(of: url)
    return url
  }

  /// The manifest the package's EQ companion loads with, sharing the amp's resources.
  var equalizerManifest: ModuleManifest? {
    eq.map {
      ModuleManifest(
        pluginFamily: pluginFamily, viewSize: viewSize, ui: $0.ui, schemaVersion: schemaVersion,
        id: $0.id, category: "eq", binary: $0.binary, icon: icon, resources: resources)
    }
  }
}

struct ModulePackage {
  let directory: URL
  let manifest: ModuleManifest
}

enum ModuleDiscovery {
  /// The folder of a module search directory that holds each category's packages.
  static let categoryFolders = [
    "amp": "amps", "cab": "cabs", "pre-fx": "pre-fx", "special-fx": "special-fx",
    "post-fx": "post-fx",
  ]

  static var installedDirectory: URL {
    FileManager.default.homeDirectoryForCurrentUser
      .appendingPathComponent("Library/Application Support/vespera/modules", isDirectory: true)
  }

  /// The `.vespera` packages of a search directory with the category each must declare: those
  /// inside its category folders. A search directory that is itself a package is taken as is.
  private static func candidates(in directory: URL, failures: inout [String]) throws
    -> [(package: URL, category: String?)]
  {
    if directory.pathExtension == "vespera" { return [(directory, nil)] }
    func packages(_ folder: URL) throws -> [URL] {
      try FileManager.default.contentsOfDirectory(at: folder, includingPropertiesForKeys: nil)
        .filter { $0.pathExtension == "vespera" }
        .sorted { $0.lastPathComponent < $1.lastPathComponent }
    }
    let folders = categoryFolders.values.sorted().joined(separator: ", ")
    for misplaced in try packages(directory) {
      failures.append(
        "\(misplaced.lastPathComponent) is not in a category folder; move it into one of "
          + "\(folders) inside \(directory.path)")
    }
    var result: [(URL, String?)] = []
    for (category, name) in categoryFolders.sorted(by: { $0.value < $1.value }) {
      let folder = directory.appendingPathComponent(name, isDirectory: true)
      guard FileManager.default.fileExists(atPath: folder.path) else { continue }
      result += try packages(folder).map { ($0, category) }
    }
    return result
  }

  static func searchDirectories(environment: [String: String] = ProcessInfo.processInfo.environment)
    -> [URL]
  {
    if let paths = environment["VESPERA_MODULE_PATH"], !paths.isEmpty {
      return paths.split(separator: ":").map { URL(fileURLWithPath: String($0), isDirectory: true) }
    }
    return [installedDirectory]
  }

  /// One catalog's share of the installed packages: the packages of `category`, built by `make`
  /// and ordered by `orderKey` (plugin family, then name, then ID), with every per-package
  /// failure collected.
  static func catalog<T>(
    category: String, directories: [URL], make: (ModulePackage) throws -> T,
    orderKey: (T) -> (String, String, String)
  ) -> (items: [T], failures: [String]) {
    let discovery = scan(directories)
    var items: [T] = []
    var failures = discovery.failures
    for package in discovery.packages where package.manifest.category == category {
      do { items.append(try make(package)) } catch { failures.append(error.localizedDescription) }
    }
    return (items.sorted { orderKey($0) < orderKey($1) }, failures)
  }

  private static func describe(_ error: DecodingError) -> String {
    func path(_ context: DecodingError.Context, _ key: CodingKey? = nil) -> String {
      (context.codingPath + (key.map { [$0] } ?? [])).map(\.stringValue).joined(separator: ".")
    }
    switch error {
    case .typeMismatch(_, let context), .valueNotFound(_, let context):
      return "\(path(context)) has the wrong type"
    case .keyNotFound(let key, let context): return "\(path(context, key)) is missing"
    case .dataCorrupted(let context): return context.debugDescription
    @unknown default: return error.localizedDescription
    }
  }

  /// Valid packages with an ID seen before load only once: the first of them, unless another
  /// category's package of that ID names its category in `formerCategories`. Such a superseded
  /// package is reported for the user to remove and never loaded; it is never deleted.
  static func scan(_ directories: [URL]) -> (packages: [ModulePackage], failures: [String]) {
    var valid: [ModulePackage] = []
    var failures: [String] = []
    for directory in directories {
      guard FileManager.default.fileExists(atPath: directory.path) else { continue }
      do {
        let candidates = try candidates(in: directory, failures: &failures)
        for (package, expected) in candidates {
          do {
            let data = try Data(contentsOf: package.appendingPathComponent("module.json"))
            let manifest: ModuleManifest
            do {
              manifest = try JSONDecoder().decode(ModuleManifest.self, from: data)
            } catch let error as DecodingError {
              throw AmpError.message("Invalid module manifest: \(describe(error))")
            }
            if manifest.schemaVersion == 1,
              manifest.routing != nil || manifest.routingSelector != nil
            {
              throw AmpError.message("routing requires schemaVersion 2")
            }
            guard ModuleManifest.schemaVersions.contains(manifest.schemaVersion),
              !manifest.id.isEmpty,
              categoryFolders[manifest.category] != nil
            else {
              throw AmpError.message("Unsupported module manifest: \(package.lastPathComponent)")
            }
            if let expected, manifest.category != expected {
              throw AmpError.message(
                "\(package.lastPathComponent) is a \(manifest.category) module; move it into "
                  + "\(categoryFolders[manifest.category] ?? manifest.category)")
            }
            if let cabinet = manifest.cabinet {
              guard manifest.category == "amp",
                cabinet.supported
                  ? !(cabinet.defaultModule?.isEmpty ?? true) : cabinet.defaultModule == nil
              else { throw AmpError.message("Invalid cabinet capability declaration") }
            }
            if let eq = manifest.eq {
              guard manifest.category == "amp", !eq.id.isEmpty, eq.id != manifest.id,
                eq.binary != manifest.binary
              else { throw AmpError.message("Invalid EQ declaration") }
            }
            _ = try manifest.specialFXRoutes()
            try ModuleManifest.validateRoutingKeys(try JSONSerialization.jsonObject(with: data))
            try manifest.validateFormerCategories()
            if let width = manifest.boardWidth {
              guard ["pre-fx", "post-fx"].contains(manifest.category), manifest.boardUnits != nil
              else {
                throw AmpError.message(
                  "Invalid boardWidth \(NSNumber(value: width)): pre-fx and post-fx modules may "
                    + "declare a whole number of board units from \(ModuleManifest.boardWidths.lowerBound) "
                    + "to \(ModuleManifest.boardWidths.upperBound)")
              }
            }
            let paths =
              [manifest.binary, manifest.icon, manifest.resources]
              + (manifest.eq.map { [$0.binary] } ?? [])
            for path in paths {
              let resource = try manifest.resource(path, relativeTo: package)
              guard FileManager.default.fileExists(atPath: resource.path) else {
                throw AmpError.message("Missing module resource: \(resource.path)")
              }
            }
            _ = try manifest.windowBackground(relativeTo: package)
            if let preview = manifest.cabinetPreview {
              let resource = try manifest.resource(preview, relativeTo: package)
              guard FileManager.default.fileExists(atPath: resource.path) else {
                throw AmpError.message("Missing cabinet preview: \(resource.path)")
              }
            }
            for declaring in [manifest] + (manifest.equalizerManifest.map { [$0] } ?? []) {
              guard let layout = try declaring.layout(relativeTo: package) else { continue }
              do {
                try VesperaLayoutRenderer.validateResources(
                  layout.resources.path, layout: layout.path)
              } catch {
                throw AmpError.message("Invalid module layout \(error.localizedDescription)")
              }
            }
            valid.append(ModulePackage(directory: package, manifest: manifest))
          } catch { failures.append("\(package.lastPathComponent): \(error.localizedDescription)") }
        }
      } catch { failures.append(error.localizedDescription) }
    }
    var packages: [ModulePackage] = []
    var identities = Set<String>()
    for package in valid {
      let manifest = package.manifest
      if let successor = valid.first(where: {
        $0.manifest.id == manifest.id && $0.manifest.category != manifest.category
          && $0.manifest.formerCategories?.contains(manifest.category) == true
      }) {
        failures.append(
          "\(package.directory.lastPathComponent) is an older \(manifest.category) copy of "
            + "\(manifest.id), which is now a \(successor.manifest.category) module; remove "
            + package.directory.path)
        continue
      }
      guard identities.insert(manifest.id).inserted else { continue }
      packages.append(package)
    }
    return (packages, failures)
  }
}

/// The static PNG an amp may declare as the main window's background.
enum WindowBackgroundImage {
  static let maximumSide = 4096
  static let maximumPixels = 8_000_000
  private static let signature: [UInt8] = [0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]
  /// The signature, the image header chunk and its CRC.
  static let headerSize = 33

  struct Header: Equatable {
    let width: Int
    let height: Int
    let bitDepth: Int
    let colourType: Int
    let interlaced: Bool

    /// The bytes of filtered scanlines the image data must inflate to.
    var filteredSize: Int {
      let channels = [0: 1, 2: 3, 3: 1, 4: 2, 6: 4][colourType] ?? 0
      func size(_ width: Int, _ height: Int) -> Int {
        width == 0 || height == 0 ? 0 : height * (1 + (width * channels * bitDepth + 7) / 8)
      }
      guard interlaced else { return size(width, height) }
      let passes = [
        (0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2),
        (0, 1, 1, 2),
      ]
      return passes.reduce(0) { total, pass in
        total
          + size(
            (width - pass.0 + pass.2 - 1) / pass.2, (height - pass.1 + pass.3 - 1) / pass.3)
      }
    }
  }

  private static func failure(_ url: URL, _ problem: String) -> AmpError {
    .message("Window background \(url.lastPathComponent) \(problem)")
  }

  private static func bigEndian(_ bytes: UnsafeRawBufferPointer, _ offset: Int) -> Int {
    (offset..<offset + 4).reduce(0) { $0 << 8 | Int(bytes[$1]) }
  }

  private static func crc(_ bytes: UnsafeRawBufferPointer, _ range: Range<Int>) -> Int {
    Int(
      crc32(
        0, bytes.baseAddress!.advanced(by: range.lowerBound).assumingMemoryBound(to: Bytef.self),
        uInt(range.count)))
  }

  /// Validates the signature and image header within the window background budget. This is the
  /// only check discovery makes on the image, and the one `vespera-module-check` makes too.
  static func header(of url: URL) throws -> Header {
    let handle = try FileHandle(forReadingFrom: url)
    defer { try? handle.close() }
    return try header(in: handle.read(upToCount: headerSize) ?? Data(), url)
  }

  static func header(in data: Data, _ url: URL) throws -> Header {
    try data.withUnsafeBytes { bytes in
      guard bytes.count >= signature.count, bytes.prefix(signature.count).elementsEqual(signature)
      else { throw failure(url, "is not a PNG image") }
      guard bytes.count >= headerSize else { throw failure(url, "is truncated") }
      guard bigEndian(bytes, 8) == 13, bytes[12..<16].elementsEqual("IHDR".utf8),
        crc(bytes, 12..<29) == bigEndian(bytes, 29)
      else { throw failure(url, "has no valid PNG image header") }
      let header = Header(
        width: bigEndian(bytes, 16), height: bigEndian(bytes, 20), bitDepth: Int(bytes[24]),
        colourType: Int(bytes[25]), interlaced: bytes[28] == 1)
      let depths = [0: [1, 2, 4, 8, 16], 2: [8, 16], 3: [1, 2, 4, 8], 4: [8, 16], 6: [8, 16]]
      guard depths[header.colourType]?.contains(header.bitDepth) == true, bytes[26] == 0,
        bytes[27] == 0, bytes[28] <= 1
      else { throw failure(url, "has an invalid PNG image header") }
      guard header.width > 0, header.height > 0 else { throw failure(url, "has no pixels") }
      guard header.width <= maximumSide, header.height <= maximumSide,
        header.width * header.height <= maximumPixels
      else {
        throw failure(
          url,
          "is \(header.width)×\(header.height) pixels; window backgrounds may be at most "
            + "\(maximumSide) pixels per side and \(maximumPixels) pixels in total")
      }
      return header
    }
  }

  /// Validates every chunk and inflates all image data, because ImageIO substitutes blank pixels
  /// for truncated or corrupt data instead of failing.
  private static func validateChunks(_ data: Data, _ header: Header, _ url: URL) throws {
    var stream = z_stream()
    guard inflateInit_(&stream, zlibVersion(), Int32(MemoryLayout<z_stream>.size)) == Z_OK else {
      throw failure(url, "cannot be decompressed")
    }
    defer { inflateEnd(&stream) }
    var scratch = [UInt8](repeating: 0, count: 65536)
    var inflated = Z_OK
    var previous = ""
    var offset = signature.count
    try data.withUnsafeBytes { bytes in
      while true {
        guard offset + 8 <= bytes.count else { throw failure(url, "is truncated") }
        let length = bigEndian(bytes, offset)
        let type = String(decoding: bytes[offset + 4..<offset + 8], as: UTF8.self)
        guard length <= Int(Int32.max), offset + 12 + length <= bytes.count else {
          throw failure(url, "is truncated")
        }
        guard crc(bytes, offset + 4..<offset + 8 + length) == bigEndian(bytes, offset + 8 + length)
        else { throw failure(url, "has a corrupt \(type) chunk") }
        switch type {
        case "acTL": throw failure(url, "is animated; window backgrounds must be static")
        case "IDAT":
          guard previous == "IDAT" || stream.total_in == 0, inflated == Z_OK else {
            throw failure(url, "has unexpected image data")
          }
          stream.next_in = UnsafeMutablePointer(
            mutating: bytes.baseAddress!.advanced(by: offset + 8).assumingMemoryBound(
              to: Bytef.self))
          stream.avail_in = uInt(length)
          while stream.avail_in > 0 && inflated == Z_OK {
            inflated = scratch.withUnsafeMutableBufferPointer {
              stream.next_out = $0.baseAddress
              stream.avail_out = uInt($0.count)
              return inflate(&stream, Z_NO_FLUSH)
            }
            guard inflated == Z_OK || inflated == Z_STREAM_END,
              Int(stream.total_out) <= header.filteredSize
            else { throw failure(url, "has corrupt image data") }
          }
        case "IEND":
          guard inflated == Z_STREAM_END, Int(stream.total_out) == header.filteredSize else {
            throw failure(url, "has incomplete image data")
          }
          return
        default: break
        }
        previous = type
        offset += 12 + length
      }
    }
  }

  /// Fully decodes the image once into an immutable bitmap, so drawing never decodes it again.
  static func decode(_ url: URL) throws -> CGImage {
    let data = try Data(contentsOf: url, options: .alwaysMapped)
    let header = try header(in: data.prefix(headerSize), url)
    try validateChunks(data, header, url)
    guard let source = CGImageSourceCreateWithData(data as CFData, nil),
      CGImageSourceGetCount(source) == 1,
      let image = CGImageSourceCreateImageAtIndex(
        source, 0, [kCGImageSourceShouldCacheImmediately: true] as CFDictionary),
      image.width == header.width, image.height == header.height,
      let space = CGColorSpace(name: CGColorSpace.sRGB),
      let context = CGContext(
        data: nil, width: header.width, height: header.height, bitsPerComponent: 8,
        bytesPerRow: 0, space: space, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
    else { throw failure(url, "cannot be decoded") }
    context.draw(image, in: CGRect(x: 0, y: 0, width: header.width, height: header.height))
    guard let decoded = context.makeImage() else { throw failure(url, "cannot be decoded") }
    return decoded
  }
}
