import AppKit
import Darwin
import LayoutRenderer
import ModuleSDK
import SwiftUI

final class ModuleTiming {
  var bpm = 120.0
  var beat = 0.0
}

final class LoadedModule: Identifiable {
  let id: String
  let name: String
  /// The version string the binary declares.
  let version: String
  let abiVersion: UInt32
  let pluginFamily: String?

  /// The order every catalog lists modules in: plugin family, then name, then ID; modules
  /// without a family sort by name.
  var familyOrderKey: (String, String, String) { (pluginFamily ?? name, name, id) }

  static func orderedByFamily(_ lhs: LoadedModule, _ rhs: LoadedModule) -> Bool {
    lhs.familyOrderKey < rhs.familyOrderKey
  }
  let preferredViewSize: CGSize
  /// Pre and post FX board units the editor spans.
  let boardWidth: Int
  let visualInsets: ModuleManifest.VisualInsets
  let icon: NSImage
  let cabinetSupport: ModuleManifest.CabinetSupport?
  let cabinetPreview: NSImage?
  let cabinetPreviewHeight: Double
  /// The amp's decoded main-window background; nil keeps the plain window colour.
  let windowBackground: CGImage?
  /// The routes a Special FX module can take; one unless `routeSelector` chooses among them.
  let routes: [SpecialFXRoute]
  /// The parameter whose value chooses the route.
  let routeSelector: UInt32?
  /// The module's routes as its audio descriptions carry them, unresolved.
  let routeDescription: [String: Any]
  /// The amp package's own EQ processor, loaded with the amp and keeping its own control state.
  let equalizer: LoadedModule?
  private let library: UnsafeMutableRawPointer
  private let api: UnsafePointer<vs_module_api>
  private let instance: UnsafeMutableRawPointer
  let resourceDirectory: URL
  let binaryURL: URL
  var onParameterChange: ((UInt32, Float) throws -> Void)?
  var onFileChange: (() throws -> Void)?
  let fileSlots: [ModuleFileSlot]
  private(set) var files: [ModuleFileReference] = []
  private(set) var fileProblems: [UInt32: String] = [:]
  private struct FileImport {
    let generation: UUID
    let task: Task<Void, Never>
  }
  @MainActor private var fileImports: [UInt32: FileImport] = [:]
  var timing: ModuleTiming?
  /// The parameter the module declares as its bypass (`VS_MODULE_BYPASS_PARAMETER`): values of at
  /// least 0.5 process, lower values bypass the module. Nil when the host must bypass it.
  let bypassParameter: UInt32?
  let acceptsMIDI: Bool
  let supportsServices: Bool
  /// How each declared parameter's values are shown, in descriptor order, from the module's
  /// parameter metadata when it exports some.
  let parameterDisplays: [VesperaParameterDisplay]
  private(set) var uiAPI: UnsafePointer<vs_module_ui_api>?
  /// The manifest-declared layout the host renders itself; when set, `uiAPI` is not used.
  private(set) var layout: (resources: URL, path: String)?
  private var parameterAPI: UnsafePointer<vs_module_parameter_api>?
  private var stateAPI: UnsafePointer<vs_module_state_api>?
  private enum State { case idle, prepared }
  private var state = State.idle

  /// The display name a package's binary declares, read without creating a module instance, so
  /// listing installed modules never sets up their processing or the plugins they host.
  static func name(manifest: ModuleManifest, directory: URL, category: String) throws -> String {
    guard ModuleManifest.schemaVersions.contains(manifest.schemaVersion),
      manifest.category == category
    else {
      throw AmpError.message("Unsupported module manifest: \(manifest.id)")
    }
    let url = try manifest.resource(manifest.binary, relativeTo: directory)
    guard let library = dlopen(url.path, RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE) else {
      throw AmpError.message("Cannot load \(url.lastPathComponent)")
    }
    defer { if dlsym(library, VS_MODULE_UI_ENTRY_NAME) == nil { dlclose(library) } }
    guard let symbol = dlsym(library, "vespera_module_entry") else {
      throw AmpError.message("Missing module entry: \(url.lastPathComponent)")
    }
    let api = try moduleAPI(symbol, url)
    guard let moduleID = api.pointee.id, let moduleName = api.pointee.name,
      api.pointee.create != nil, api.pointee.destroy != nil,
      api.pointee.prepare != nil, api.pointee.release != nil, api.pointee.process != nil
    else { throw AmpError.message("Invalid module: \(url.lastPathComponent)") }
    let id = String(cString: moduleID)
    guard id == manifest.id else {
      throw AmpError.message("Manifest ID \(manifest.id) does not match binary ID \(id)")
    }
    return String(cString: moduleName)
  }

  /// The descriptor a module entry point returns for this host's module ABI.
  private static func moduleAPI(_ symbol: UnsafeMutableRawPointer, _ url: URL) throws
    -> UnsafePointer<vs_module_api>
  {
    let entry = unsafeBitCast(
      symbol, to: (@convention(c) (UInt32) -> UnsafePointer<vs_module_api>?).self)
    guard let api = entry(UInt32(VS_MODULE_ABI_VERSION)) else {
      if let older = (1..<UInt32(VS_MODULE_ABI_VERSION)).last(where: { entry($0) != nil }) {
        throw AmpError.message(
          "\(url.lastPathComponent) was built for module ABI \(older); this version of vespera "
            + "loads only ABI \(VS_MODULE_ABI_VERSION)")
      }
      throw AmpError.message("Invalid module: \(url.lastPathComponent)")
    }
    guard api.pointee.struct_size >= MemoryLayout<vs_module_api>.size,
      api.pointee.abi_version == VS_MODULE_ABI_VERSION
    else { throw AmpError.message("Invalid module: \(url.lastPathComponent)") }
    return api
  }

  /// The capabilities a module declares, validated as the native runtime validates them.
  private static func capabilities(of api: UnsafePointer<vs_module_api>, _ url: URL) throws
    -> (bypassParameter: UInt32?, midi: Bool)
  {
    let declared = api.pointee.capabilities
    let bypass = UInt32(VS_MODULE_BYPASS_PARAMETER)
    let known = bypass | UInt32(VS_MODULE_HOST_TEMPO) | UInt32(VS_MODULE_MIDI_INPUT)
    guard declared & ~known == 0 else {
      throw AmpError.message("\(url.lastPathComponent) declares unknown capabilities")
    }
    guard declared & bypass != 0 else {
      guard api.pointee.bypass_parameter == 0 else {
        throw AmpError.message("Invalid module: \(url.lastPathComponent)")
      }
      return (nil, declared & UInt32(VS_MODULE_MIDI_INPUT) != 0)
    }
    let id = api.pointee.bypass_parameter
    let parameters = UnsafeBufferPointer(
      start: api.pointee.parameters, count: Int(api.pointee.parameter_count))
    guard parameters.contains(where: { $0.id == id && $0.minimum == 0 && $0.maximum == 1 }) else {
      throw AmpError.message(
        "\(url.lastPathComponent) names bypass parameter \(id), which is not a parameter "
          + "ranging from 0 to 1")
    }
    return (id, declared & UInt32(VS_MODULE_MIDI_INPUT) != 0)
  }

  /// Checks the parameters a module's routes name: a branch mix parameter ranges from 0 to 1, and
  /// a selector parameter from 0 to the last route, takes exactly one value per route when the
  /// metadata describes it, and is neither a mix nor the bypass parameter. Routed modules must
  /// let the host read parameter values.
  private static func validate(
    _ routing: (selector: UInt32?, routes: [SpecialFXRoute]), api: UnsafePointer<vs_module_api>,
    displays: [VesperaParameterDisplay], bypass: UInt32?, library: UnsafeMutableRawPointer,
    _ url: URL
  ) throws {
    let mixes = routing.routes.compactMap { $0.branch?.mix.parameter }
    guard routing.selector != nil || !mixes.isEmpty else { return }
    let parameters = UnsafeBufferPointer(
      start: api.pointee.parameters, count: Int(api.pointee.parameter_count))
    func range(_ id: UInt32) -> (Float, Float)? {
      parameters.first { $0.id == id }.map { ($0.minimum, $0.maximum) }
    }
    for mix in mixes {
      guard let (minimum, maximum) = range(mix), minimum == 0, maximum == 1 else {
        throw AmpError.message(
          "\(url.lastPathComponent): routing mix parameter \(mix) must range from 0 to 1")
      }
    }
    if let selector = routing.selector {
      let last = Float(routing.routes.count - 1)
      guard let (minimum, maximum) = range(selector), minimum == 0, maximum == last else {
        throw AmpError.message(
          "\(url.lastPathComponent): routingSelector parameter \(selector) must range from 0 to "
            + "\(routing.routes.count - 1)")
      }
      if let display = displays.first(where: { $0.identifier == selector }), display.described {
        guard display.kind != .continuous, display.steps == routing.routes.count else {
          throw AmpError.message(
            "\(url.lastPathComponent): routingSelector parameter \(selector) must be a toggle, "
              + "stepped or enumerated parameter with \(routing.routes.count) values")
        }
      }
      guard selector != bypass else {
        throw AmpError.message(
          "\(url.lastPathComponent): the routingSelector parameter cannot be the bypass parameter")
      }
    }
    guard let symbol = dlsym(library, VS_MODULE_PARAMETER_ENTRY_NAME),
      let ext = unsafeBitCast(symbol, to: vs_module_parameter_entry_fn.self)(
        UInt32(VS_MODULE_PARAMETER_ABI_VERSION)),
      ext.pointee.struct_size >= MemoryLayout<vs_module_parameter_api>.size,
      ext.pointee.get_value != nil, ext.pointee.set_value != nil
    else {
      throw AmpError.message(
        "\(url.lastPathComponent): a routed module must export the parameter value extension")
    }
  }

  /// The route the control instance's selector value chooses, or the only route.
  var route: SpecialFXRoute {
    guard let routeSelector, let value = try? value(routeSelector) else { return routes[0] }
    return route(for: value)
  }

  func route(for value: Float) -> SpecialFXRoute {
    routes[SpecialFXRoute.index(for: value, count: routes.count)]
  }

  /// Where a Special FX module's output enters the host chain on its current route.
  var placement: SpecialFXPlacement { route.placement }

  /// Every parameter's display, validated as untrusted module data.
  private static func displays(
    of api: UnsafePointer<vs_module_api>, library: UnsafeMutableRawPointer, _ url: URL
  ) throws -> [VesperaParameterDisplay] {
    var metadata: UnsafeRawPointer?
    if let symbol = dlsym(library, VS_MODULE_METADATA_ENTRY_NAME) {
      let entry = unsafeBitCast(symbol, to: vs_module_metadata_entry_fn.self)
      metadata = entry(UInt32(VS_MODULE_METADATA_ABI_VERSION)).map { UnsafeRawPointer($0) }
    }
    do {
      return try VesperaParameterDisplay.displays(
        forModule: UnsafeRawPointer(api), metadata: metadata)
    } catch {
      throw AmpError.message("\(url.lastPathComponent): \(error.localizedDescription)")
    }
  }

  private static func hasServices(library: UnsafeMutableRawPointer, _ url: URL) throws -> Bool {
    guard let symbol = dlsym(library, VS_MODULE_SERVICES_ENTRY_NAME) else { return false }
    let entry = unsafeBitCast(symbol, to: vs_module_services_entry_fn.self)
    guard let services = entry(UInt32(VS_MODULE_SERVICES_ABI_VERSION)) else { return false }
    guard services.pointee.struct_size >= MemoryLayout<vs_module_services_api>.size,
      services.pointee.abi_version == VS_MODULE_SERVICES_ABI_VERSION,
      services.pointee.attach != nil, services.pointee.status != nil
    else {
      throw AmpError.message("\(url.lastPathComponent): invalid services descriptor")
    }
    return true
  }

  private static func fileSlots(library: UnsafeMutableRawPointer, _ url: URL) throws
    -> [ModuleFileSlot]
  {
    guard let symbol = dlsym(library, VS_MODULE_FILES_ENTRY_NAME) else { return [] }
    let entry = unsafeBitCast(symbol, to: vs_module_files_entry_fn.self)
    guard let api = entry(UInt32(VS_MODULE_FILES_ABI_VERSION)) else { return [] }
    guard api.pointee.struct_size >= MemoryLayout<vs_module_files_api>.size,
      api.pointee.abi_version == VS_MODULE_FILES_ABI_VERSION, api.pointee.set_file != nil,
      api.pointee.slot_count <= 16, api.pointee.slot_size >= MemoryLayout<vs_file_slot>.size,
      api.pointee.slot_size <= 4096,
      api.pointee.slot_count == 0 || api.pointee.slots != nil
    else { throw AmpError.message("\(url.lastPathComponent): invalid files descriptor") }
    var slots: [ModuleFileSlot] = []
    for index in 0..<Int(api.pointee.slot_count) {
      let slot = UnsafeRawPointer(api.pointee.slots!).advanced(
        by: index * Int(api.pointee.slot_size)
      ).loadUnaligned(as: vs_file_slot.self)
      guard slot.id != 0, !slots.contains(where: { $0.id == slot.id }),
        let name = slot.name, strnlen(name, 256) > 0, strnlen(name, 256) < 256,
        let text = String(validatingCString: name), let types = slot.types,
        strnlen(types, 256) > 0, strnlen(types, 256) < 256,
        let extensions = String(validatingCString: types),
        extensions.range(of: "^[a-z0-9]+(,[a-z0-9]+)*$", options: .regularExpression) != nil,
        slot.max_bytes > 0, slot.max_bytes <= FileLibrary.maximumFileBytes,
        slot.flags & ~UInt32(VS_FILE_SLOT_OPTIONAL) == 0
      else { throw AmpError.message("\(url.lastPathComponent): invalid file slot") }
      var bundled: String?
      if let path = slot.bundled {
        guard strnlen(path, 4097) <= 4096, let value = String(validatingCString: path),
          FileLibrary.safeRelative(value)
        else { throw AmpError.message("Invalid bundled file directory") }
        bundled = value
      }
      slots.append(
        ModuleFileSlot(
          id: slot.id, name: text, types: extensions.components(separatedBy: ","),
          bundled: bundled, maximumBytes: slot.max_bytes,
          optional: slot.flags & UInt32(VS_FILE_SLOT_OPTIONAL) != 0))
    }
    return slots
  }

  init(manifest: ModuleManifest, directory: URL, category: String = "amp") throws {
    guard ModuleManifest.schemaVersions.contains(manifest.schemaVersion),
      manifest.category == category
    else {
      throw AmpError.message("Unsupported module manifest: \(manifest.id)")
    }
    let url = try manifest.resource(manifest.binary, relativeTo: directory)
    let iconURL = try manifest.resource(manifest.icon, relativeTo: directory)
    let resources = try manifest.resource(manifest.resources, relativeTo: directory)
    let previewURL = try manifest.cabinetPreview.map {
      try manifest.resource($0, relativeTo: directory)
    }
    let layout = try manifest.layout(relativeTo: directory)
    let windowBackground = try manifest.windowBackground(relativeTo: directory).map {
      try WindowBackgroundImage.decode($0)
    }
    let routing = try manifest.specialFXRoutes()
    let routeDescription = try manifest.routeDescription()
    if manifest.eq != nil && category != "amp" {
      throw AmpError.message("Only amps can declare an EQ: \(manifest.id)")
    }
    let equalizer = try manifest.equalizerManifest.map {
      try LoadedModule(manifest: $0, directory: directory, category: "eq")
    }
    guard let icon = NSImage(contentsOf: iconURL), icon.isValid,
      icon.size.width.isFinite, icon.size.height.isFinite,
      icon.size.width > 0, icon.size.height > 0
    else { throw AmpError.message("Cannot load module icon: \(iconURL.lastPathComponent)") }
    // Native UI classes and autoreleased objects can outlive the last module instance.
    guard let library = dlopen(url.path, RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE) else {
      throw AmpError.message("Cannot load \(url.lastPathComponent)")
    }
    do {
      guard let symbol = dlsym(library, "vespera_module_entry") else {
        throw AmpError.message("Missing module entry: \(url.lastPathComponent)")
      }
      let api = try Self.moduleAPI(symbol, url)
      let capabilities = try Self.capabilities(of: api, url)
      let displays = try Self.displays(of: api, library: library, url)
      let services = try Self.hasServices(library: library, url)
      try Self.validate(
        routing, api: api, displays: displays, bypass: capabilities.bypassParameter,
        library: library, url)
      let fileSlots = try Self.fileSlots(library: library, url)
      if let layout {
        try VesperaLayoutRenderer.validateFileSlots(
          fileSlots.map { NSNumber(value: $0.id) },
          resources: layout.resources.path, layout: layout.path)
      }
      guard let moduleID = api.pointee.id, let moduleName = api.pointee.name,
        api.pointee.create != nil, api.pointee.destroy != nil,
        api.pointee.prepare != nil, api.pointee.release != nil, api.pointee.process != nil
      else { throw AmpError.message("Invalid module: \(url.lastPathComponent)") }
      let id = String(cString: moduleID)
      guard id == manifest.id else {
        throw AmpError.message("Manifest ID \(manifest.id) does not match binary ID \(id)")
      }
      var instance: UnsafeMutableRawPointer?
      guard api.pointee.create(&instance) == VS_OK, let instance else {
        throw AmpError.message("Cannot create \(id)")
      }
      self.library = library
      self.api = api
      self.instance = instance
      self.id = id
      self.name = String(cString: moduleName)
      self.version = api.pointee.version.map { String(cString: $0) } ?? ""
      self.abiVersion = api.pointee.abi_version
      self.bypassParameter = capabilities.bypassParameter
      self.acceptsMIDI = capabilities.midi
      self.supportsServices = services
      self.fileSlots = fileSlots
      self.parameterDisplays = displays
      self.pluginFamily = manifest.pluginFamily.flatMap {
        let family = $0.trimmingCharacters(in: .whitespacesAndNewlines)
        return family.isEmpty ? nil : family
      }
      self.visualInsets = manifest.visualInsets?.validated ?? .init()
      self.boardWidth = manifest.boardUnits ?? ModuleManifest.boardWidths.lowerBound
      let size = manifest.viewSize
      self.preferredViewSize =
        size.map {
          $0.width.isFinite && $0.height.isFinite && $0.width > 0 && $0.height > 0
            ? CGSize(width: $0.width, height: $0.height) : CGSize(width: 215, height: 380)
        } ?? CGSize(width: 215, height: 380)
      self.icon = icon
      self.cabinetSupport = manifest.cabinet
      self.equalizer = equalizer
      self.cabinetPreview = previewURL.flatMap { NSImage(contentsOf: $0) }
      self.cabinetPreviewHeight = max(0, manifest.cabinetPreviewHeight ?? 0)
      self.windowBackground = windowBackground
      self.routes = routing.routes
      self.routeSelector = routing.selector
      self.routeDescription = routeDescription
      self.resourceDirectory = resources
      self.binaryURL = url
      if let symbol = dlsym(library, VS_MODULE_PARAMETER_ENTRY_NAME) {
        let entry = unsafeBitCast(symbol, to: vs_module_parameter_entry_fn.self)
        if let ext = entry(UInt32(VS_MODULE_PARAMETER_ABI_VERSION)),
          ext.pointee.struct_size >= MemoryLayout<vs_module_parameter_api>.size,
          ext.pointee.abi_version == VS_MODULE_PARAMETER_ABI_VERSION,
          ext.pointee.get_value != nil, ext.pointee.set_value != nil
        {
          parameterAPI = ext
        }
      }
      if let symbol = dlsym(library, VS_MODULE_STATE_ENTRY_NAME) {
        let entry = unsafeBitCast(symbol, to: vs_module_state_entry_fn.self)
        if let ext = entry(UInt32(VS_MODULE_STATE_ABI_VERSION)),
          ext.pointee.struct_size >= MemoryLayout<vs_module_state_api>.size,
          ext.pointee.abi_version == VS_MODULE_STATE_ABI_VERSION,
          ext.pointee.size != nil, ext.pointee.save != nil, ext.pointee.load != nil
        {
          stateAPI = ext
        }
      }
      if parameterAPI != nil, let layout {
        self.layout = layout
      } else if parameterAPI != nil, let symbol = dlsym(library, VS_MODULE_UI_ENTRY_NAME) {
        let entry = unsafeBitCast(symbol, to: vs_module_ui_entry_fn.self)
        if let ext = entry(UInt32(VS_MODULE_UI_ABI_VERSION)),
          ext.pointee.struct_size >= MemoryLayout<vs_module_ui_api>.size,
          ext.pointee.abi_version == VS_MODULE_UI_ABI_VERSION,
          let platform = ext.pointee.platform, String(cString: platform) == "macos-nsview",
          ext.pointee.create != nil, ext.pointee.destroy != nil, ext.pointee.refresh != nil
        {
          uiAPI = ext
        }
      }
    } catch {
      if dlsym(library, VS_MODULE_UI_ENTRY_NAME) == nil { dlclose(library) }
      throw error
    }
  }

  deinit {
    release()
    api.pointee.destroy(instance)
    // dlclose can unregister Objective-C classes even when RTLD_NODELETE keeps their code mapped.
    if dlsym(library, VS_MODULE_UI_ENTRY_NAME) == nil { dlclose(library) }
  }

  func prepare() throws {
    guard state == .idle else { return }
    var info = vs_prepare_info(
      struct_size: UInt32(MemoryLayout<vs_prepare_info>.size),
      sample_rate: 48000, max_frames: 128, channels: 2)
    var timing = vs_module_timing(
      struct_size: UInt32(MemoryLayout<vs_module_timing>.size),
      latency_frames: 0, tail_frames: 0)
    guard api.pointee.prepare(instance, &info, &timing) == VS_OK else {
      throw AmpError.message("Cannot prepare \(name)")
    }
    state = .prepared
  }

  func value(_ id: UInt32) throws -> Float {
    var value: Float = 0
    guard let parameterAPI, parameterAPI.pointee.get_value(instance, id, &value) == VS_OK else {
      throw AmpError.message("Cannot read parameter \(id)")
    }
    return value
  }

  var parameters: [ModuleParameter] {
    guard let descriptors = api.pointee.parameters else { return [] }
    return zip(
      UnsafeBufferPointer(start: descriptors, count: Int(api.pointee.parameter_count)),
      parameterDisplays
    ).map { descriptor, display in
      ModuleParameter(
        id: descriptor.id, name: String(cString: descriptor.name),
        unit: descriptor.unit.map { String(cString: $0) } ?? "",
        minimum: descriptor.minimum, maximum: descriptor.maximum, display: display)
    }
  }

  func setValue(_ id: UInt32, _ value: Float) throws {
    guard value.isFinite, let descriptors = api.pointee.parameters,
      let descriptor = UnsafeBufferPointer(
        start: descriptors, count: Int(api.pointee.parameter_count)
      )
      .first(where: { $0.id == id }),
      value >= descriptor.minimum, value <= descriptor.maximum,
      let parameterAPI
    else {
      throw AmpError.message("Cannot set parameter \(id)")
    }
    try onParameterChange?(id, value)
    guard parameterAPI.pointee.set_value(instance, id, value) == VS_OK else {
      throw AmpError.message("Cannot set parameter \(id)")
    }
  }

  func audioDescription() throws -> [String: Any] {
    guard let descriptors = api.pointee.parameters else {
      throw AmpError.message("Missing amp parameters")
    }
    let parameters = try UnsafeBufferPointer(
      start: descriptors, count: Int(api.pointee.parameter_count)
    )
    .map { ["id": $0.id, "value": try value($0.id)] as [String: Any] }
    var description: [String: Any] = [
      "binary": binaryURL.path, "module": id, "version": version, "parameters": parameters,
      "resources": resourceDirectory.path,
    ]
    if let stateAPI {
      description["state"] = Self.stateDescription(
        ModuleStateSnapshot(schemaVersion: stateAPI.pointee.schema_version, data: try saveState()))
    }
    if !files.isEmpty { description["files"] = try fileDescriptions(files) }
    return description
  }

  /// The audio description of this module with `staged` settings in place of its own.
  func audioDescription(_ staged: StagedModuleSettings) throws -> [String: Any] {
    var description: [String: Any] = [
      "binary": binaryURL.path, "module": id, "version": version,
      "parameters": staged.parameters.map { ["id": $0.id, "value": $0.value] as [String: Any] },
      "resources": resourceDirectory.path,
    ]
    if let state = staged.state { description["state"] = Self.stateDescription(state) }
    if let files = staged.files, !files.isEmpty {
      description["files"] = try fileDescriptions(files)
    }
    return description
  }

  private func fileDescriptions(_ files: [ModuleFileReference]) throws -> [[String: Any]] {
    try files.map { file in
      try file.validate()
      let path = try? FileLibrary.resolvedPath(file, resources: resourceDirectory)
      return [
        "slot": file.slot, "path": path?.path ?? "", "state": path == nil ? "missing" : "present",
        "name": file.name,
        "bytes": file.bytes, "sha256": file.sha256,
      ]
    }
  }

  func setFileProblems(_ problems: [UInt32: String]) { fileProblems = problems }

  @MainActor func setFile(_ slot: UInt32, _ reference: ModuleFileReference?) throws {
    guard let descriptor = fileSlots.first(where: { $0.id == slot }),
      reference != nil || descriptor.optional
    else { throw AmpError.message("File slot cannot be cleared") }
    if let reference {
      try reference.validate()
      guard reference.slot == slot, reference.bytes <= descriptor.maximumBytes else {
        throw AmpError.message("File does not fit its slot")
      }
    }
    cancelFileImport(slot)
    let previous = files
    let previousProblems = fileProblems
    files.removeAll { $0.slot == slot }
    if let reference {
      files.append(reference)
      files.sort { $0.slot < $1.slot }
    }
    fileProblems.removeValue(forKey: slot)
    do { try onFileChange?() } catch {
      files = previous
      fileProblems = previousProblems
      throw error
    }
  }

  @MainActor func isImportingFile(_ slot: UInt32) -> Bool { fileImports[slot] != nil }

  @MainActor func cancelFileImport(_ slot: UInt32) {
    fileImports.removeValue(forKey: slot)?.task.cancel()
  }

  @discardableResult
  @MainActor func importFile(
    _ slot: UInt32,
    operation: @escaping @Sendable () async throws -> ModuleFileReference,
    confirm: @escaping @MainActor (ModuleFileReference) -> Bool = { _ in true },
    reportError: @escaping @MainActor (Error) -> Void
  ) -> Task<Void, Never> {
    cancelFileImport(slot)
    let generation = UUID()
    let task = Task { @MainActor [weak self] in
      do {
        let reference = try await operation()
        guard let self, self.fileImports[slot]?.generation == generation else { return }
        let confirmed = confirm(reference)
        guard self.fileImports[slot]?.generation == generation else { return }
        guard confirmed else {
          self.fileImports.removeValue(forKey: slot)
          return
        }
        self.fileImports.removeValue(forKey: slot)
        do { try self.setFile(slot, reference) } catch { reportError(error) }
      } catch {
        guard let self, self.fileImports[slot]?.generation == generation else { return }
        self.fileImports.removeValue(forKey: slot)
        if !Task.isCancelled { reportError(error) }
      }
    }
    fileImports[slot] = FileImport(generation: generation, task: task)
    return task
  }

  private static func stateDescription(_ state: ModuleStateSnapshot) -> [String: Any] {
    let digits = Array("0123456789abcdef".utf8)
    var hex = [UInt8]()
    hex.reserveCapacity(state.data.count * 2)
    for byte in state.data {
      hex.append(digits[Int(byte >> 4)])
      hex.append(digits[Int(byte & 15)])
    }
    return ["schema": state.schemaVersion, "data": String(decoding: hex, as: UTF8.self)]
  }

  func saveState() throws -> Data { try saveState(of: instance) }

  private func saveState(of target: UnsafeMutableRawPointer) throws -> Data {
    var size: UInt64 = 0
    guard let stateAPI, stateAPI.pointee.size(target, &size) == VS_OK else {
      throw AmpError.message("Cannot size the state of \(name)")
    }
    guard size <= ModuleStateSnapshot.maximumBytes else {
      throw AmpError.message("The state of \(name) exceeds the preset limit")
    }
    var data = Data(count: Int(size))
    var written: UInt64 = 0
    let result = data.withUnsafeMutableBytes {
      stateAPI.pointee.save(target, $0.bindMemory(to: UInt8.self).baseAddress, size, &written)
    }
    guard result == VS_OK, written == size else {
      throw AmpError.message("Cannot save the state of \(name)")
    }
    return data
  }

  func restoreState(_ data: Data) throws {
    guard let stateAPI else { throw AmpError.message("No amp state support") }
    try restoreState(
      ModuleStateSnapshot(schemaVersion: stateAPI.pointee.schema_version, data: data))
  }

  private func restoreState(_ snapshot: ModuleStateSnapshot) throws {
    let wasPrepared = state == .prepared
    release()
    let result = load(snapshot, into: instance)
    if wasPrepared { try prepare() }
    try check(result, loading: snapshot)
  }

  private func load(_ snapshot: ModuleStateSnapshot, into target: UnsafeMutableRawPointer)
    -> vs_result
  {
    guard let stateAPI else { return vs_result(VS_ERROR_UNSUPPORTED) }
    let empty: UInt8 = 0
    return snapshot.data.isEmpty
      ? withUnsafePointer(to: empty) {
        stateAPI.pointee.load(target, snapshot.schemaVersion, $0, 0)
      }
      : snapshot.data.withUnsafeBytes {
        stateAPI.pointee.load(
          target, snapshot.schemaVersion, $0.bindMemory(to: UInt8.self).baseAddress,
          UInt64(snapshot.data.count))
      }
  }

  private func check(_ result: vs_result, loading snapshot: ModuleStateSnapshot) throws {
    if result == VS_OK { return }
    if result == VS_ERROR_UNSUPPORTED {
      throw AmpError.message(
        "\(name) \(version) does not support saved state schema \(snapshot.schemaVersion)")
    }
    throw AmpError.message("\(name) rejected its saved state")
  }

  /// Everything a preset keeps of this module: its identity, its opaque state and its parameters.
  func snapshot() throws -> ModuleSnapshot {
    try snapshot(of: instance)
  }

  private func snapshot(of target: UnsafeMutableRawPointer) throws -> ModuleSnapshot {
    let state = try stateAPI.map {
      ModuleStateSnapshot(schemaVersion: $0.pointee.schema_version, data: try saveState(of: target))
    }
    let parameters = try self.parameters.map { parameter in
      var value: Float = 0
      guard let parameterAPI, parameterAPI.pointee.get_value(target, parameter.id, &value) == VS_OK,
        value.isFinite
      else { throw AmpError.message("Cannot read parameter \(parameter.id) of \(name)") }
      return ParameterSnapshot(id: parameter.id, value: value)
    }
    return ModuleSnapshot(
      id: id, version: version, abiVersion: abiVersion, state: state, parameters: parameters,
      files: target == instance && !files.isEmpty ? files : nil)
  }

  /// Whether settings saved by a module built for module ABI `saved` mean the same to modules built
  /// for this host's ABI (`VS_MODULE_SETTINGS_ABI_MINIMUM` through `VS_MODULE_ABI_VERSION`), as
  /// the native runtime's `restores_saved_module_abi` decides. Saving records the current ABI.
  static func restoresSavedABI(_ saved: UInt32) -> Bool {
    saved >= VS_MODULE_SETTINGS_ABI_MINIMUM && saved <= VS_MODULE_ABI_VERSION
  }

  /// The settings of a new instance of this module.
  func defaultSnapshot() throws -> ModuleSnapshot {
    try withScratchInstance { try snapshot(of: $0) }
  }

  /// Validates `snapshot` for this module on a separate instance, leaving this one untouched:
  /// its state is loaded through the module's state interface, then its parameter values are
  /// applied, the order the audio side uses. Returns the settings the module then reports.
  func stage(_ snapshot: ModuleSnapshot) throws -> StagedModuleSettings {
    guard snapshot.id == id else {
      throw AmpError.message("Saved settings for \(snapshot.id) cannot be applied to \(id)")
    }
    guard Self.restoresSavedABI(snapshot.abiVersion) else {
      throw AmpError.message(
        "\(name) uses module ABI \(abiVersion), but the preset requires ABI \(snapshot.abiVersion)")
    }
    let known = Dictionary(uniqueKeysWithValues: parameters.map { ($0.id, $0) })
    guard Set(snapshot.parameters.map(\.id)).count == snapshot.parameters.count else {
      throw AmpError.message("The preset repeats a parameter of \(name)")
    }
    for parameter in snapshot.parameters {
      guard let descriptor = known[parameter.id], parameter.value.isFinite,
        parameter.value >= descriptor.minimum, parameter.value <= descriptor.maximum
      else { throw AmpError.message("The preset has an invalid parameter for \(name)") }
    }
    let files = snapshot.files ?? []
    guard files.count <= 16, Set(files.map(\.slot)).count == files.count else {
      throw AmpError.message("Invalid file slots for \(name)")
    }
    for file in files {
      try file.validate()
      guard let slot = fileSlots.first(where: { $0.id == file.slot }),
        file.bytes <= slot.maximumBytes
      else { throw AmpError.message("Undeclared or oversized file slot") }
    }
    return try withScratchInstance { scratch in
      if let state = snapshot.state {
        guard stateAPI != nil else {
          throw AmpError.message("\(name) \(version) cannot load saved state")
        }
        guard state.data.count <= ModuleStateSnapshot.maximumBytes else {
          throw AmpError.message("The saved state of \(name) exceeds the preset limit")
        }
        try check(load(state, into: scratch), loading: state)
      } else if stateAPI != nil {
        throw AmpError.message("The preset has no saved state for \(name)")
      }
      for parameter in snapshot.parameters {
        guard let parameterAPI,
          parameterAPI.pointee.set_value(scratch, parameter.id, parameter.value) == VS_OK
        else { throw AmpError.message("\(name) rejected parameter \(parameter.id)") }
      }
      let result = try self.snapshot(of: scratch)
      return StagedModuleSettings(
        state: result.state, parameters: result.parameters,
        files: files.isEmpty ? nil : files)
    }
  }

  /// Applies settings from `stage(_:)` to this instance without notifying `onParameterChange`:
  /// the caller has already submitted them to the audio side.
  @MainActor func apply(_ staged: StagedModuleSettings) throws {
    if let state = staged.state { try restoreState(state) }
    for parameter in staged.parameters {
      guard let parameterAPI,
        parameterAPI.pointee.set_value(instance, parameter.id, parameter.value) == VS_OK
      else { throw AmpError.message("\(name) rejected parameter \(parameter.id)") }
    }
    for slot in Array(fileImports.keys) { cancelFileImport(slot) }
    files = staged.files ?? []
  }

  private func withScratchInstance<T>(_ body: (UnsafeMutableRawPointer) throws -> T) throws -> T {
    var scratch: UnsafeMutableRawPointer?
    guard api.pointee.create(&scratch) == VS_OK, let scratch else {
      throw AmpError.message("Cannot create \(name)")
    }
    defer { api.pointee.destroy(scratch) }
    return try body(scratch)
  }

  func release() {
    guard state == .prepared else { return }
    api.pointee.release(instance)
    state = .idle
  }

  func renderProbe() throws -> [Float] {
    guard state == .prepared else { throw AmpError.message("Amp is not prepared") }
    var input = [Float](repeating: 0, count: 128)
    input[0] = 0.5
    var output = [Float](repeating: 0, count: 256)
    let result = input.withUnsafeBufferPointer { source in
      output.withUnsafeMutableBufferPointer { destination in
        let inputs: [UnsafePointer<Float>?] = [source.baseAddress, source.baseAddress]
        let outputs: [UnsafeMutablePointer<Float>?] = [
          destination.baseAddress, destination.baseAddress! + 128,
        ]
        return inputs.withUnsafeBufferPointer { inputs in
          outputs.withUnsafeBufferPointer { outputs in
            var block = vs_process_block(
              struct_size: UInt32(MemoryLayout<vs_process_block>.size),
              inputs: inputs.baseAddress, outputs: outputs.baseAddress, channels: 2, frames: 128,
              frame_position: 0, flags: UInt32(VS_PROCESS_DISCONTINUITY), events: nil,
              event_count: 0, tempo: nil, midi_events: nil, midi_event_count: 0,
              midi_event_size: UInt32(MemoryLayout<vs_midi_event>.size))
            return api.pointee.process(instance, &block)
          }
        }
      }
    }
    guard result == VS_OK, output.allSatisfy({ $0.isFinite }) else {
      throw AmpError.message("Processing failed: \(name)")
    }
    return output
  }
}
