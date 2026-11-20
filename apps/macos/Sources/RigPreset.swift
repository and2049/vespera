import Foundation

struct ParameterSnapshot: Codable, Equatable {
  var id: UInt32
  var value: Float
}

/// A module's opaque saved state, interpreted only by the module's own state interface.
struct ModuleStateSnapshot: Codable, Equatable {
  static let maximumBytes: UInt64 = 1024 * 1024
  var schemaVersion: UInt32
  var data: Data
}

/// One module's saved settings. Identifies the module by ID, never by installed path.
struct ModuleSnapshot: Codable, Equatable {
  var id: String
  /// The version that saved the settings. Another installed version may load them only if its
  /// state interface accepts the saved schema.
  var version: String
  var abiVersion: UInt32
  var state: ModuleStateSnapshot?
  var parameters: [ParameterSnapshot]
  var files: [ModuleFileReference]? = nil
}

/// Settings validated on a separate module instance, ready to commit without further checks.
struct StagedModuleSettings: Equatable {
  let state: ModuleStateSnapshot?
  let parameters: [ParameterSnapshot]
  var files: [ModuleFileReference]? = nil
}

/// The global controls a preset carries. Input mode is excluded: it selects hardware channels.
struct PresetGlobals: Codable, Equatable {
  var inputGain: Double
  var gateEnabled: Bool
  var gateThreshold: Double
  var transpose: Double
  var doublerEnabled: Bool
  var doublerTime: Double
  var outputGain: Double

  init(_ settings: GlobalControlSettings) {
    inputGain = settings.inputGain
    gateEnabled = settings.gateEnabled
    gateThreshold = settings.gateThreshold
    transpose = settings.transpose
    doublerEnabled = settings.doublerEnabled
    doublerTime = settings.doublerTime
    outputGain = settings.outputGain
  }

  /// `current` with these values in place, keeping its input mode.
  func applied(to current: GlobalControlSettings) -> GlobalControlSettings {
    var result = current
    result.inputGain = inputGain
    result.gateEnabled = gateEnabled
    result.gateThreshold = gateThreshold
    result.transpose = transpose
    result.doublerEnabled = doublerEnabled
    result.doublerTime = doublerTime
    result.outputGain = outputGain
    return result
  }

  var isValid: Bool {
    let values = [inputGain, gateThreshold, transpose, doublerTime, outputGain]
    return values.allSatisfy(\.isFinite) && (-60...24).contains(inputGain)
      && (-120...0).contains(gateThreshold) && (-12...12).contains(transpose)
      && transpose.rounded() == transpose && (1...40).contains(doublerTime)
      && (-60...24).contains(outputGain)
  }
}

/// An amp's settings together with those of the EQ it owns.
struct AmpSnapshot: Codable, Equatable {
  var module: ModuleSnapshot
  var equalizer: ModuleSnapshot?
}

struct AmpSection: Codable, Equatable {
  var selected: String?
  /// Every installed amp, so settings kept for amps that are not selected survive a load.
  var amps: [AmpSnapshot]
}

struct CabinetSection: Codable, Equatable {
  /// Module ID of an independently chosen cabinet; nil while the cabinet follows the amp.
  var independent: String?
  var cabinets: [ModuleSnapshot]
}

struct SpecialFXSection: Codable, Equatable {
  /// The shown editor; it does not affect processing when `active` is present.
  var selected: String?
  /// IDs of the modules in the signal chain, ascending. Absent in presets written before Special
  /// FX could be combined, where the selected module alone processed.
  var active: [String]? = nil
  var modules: [ModuleSnapshot]

  /// The processing modules, with the legacy reading of `selected` when `active` is absent.
  var activeModules: [String] { active ?? selected.map { [$0] } ?? [] }

  /// Whether a host reading only `selected` processes the same modules: exactly the selected
  /// module, or nothing when no Special FX module was installed.
  var isLegacyRepresentable: Bool {
    guard let active else { return true }
    return (selected.map { active == [$0] } ?? false) || (active.isEmpty && modules.isEmpty)
  }
}

struct PedalInstanceSnapshot: Codable, Equatable {
  enum Lifecycle: String, Codable {
    case untouchedDraft, editedDraft, chained
  }
  /// The instance identity: its audio slot and MIDI target key.
  var id: String
  var ordinal: Int
  var lifecycle: Lifecycle
  var hostBypassed: Bool
  var module: ModuleSnapshot
}

/// The shown editors. `pedals` slots are the board's spans left to right: an instance ID for an
/// editor spanning its module's board width, nil for one free unit, with free units missing
/// from the end implied. `rack`, which is no longer written, holds one editor ID filling the
/// board.
struct EditorLayoutSnapshot: Codable, Equatable {
  enum Kind: String, Codable { case pedals, rack }
  var kind: Kind
  var slots: [String?]
}

struct BoardSnapshot: Codable, Equatable {
  /// The processing chain in signal order.
  var chain: [PedalInstanceSnapshot]
  var drafts: [PedalInstanceSnapshot]
  var editors: EditorLayoutSnapshot
}

/// Everything a preset restores. Tempo, input mode, audio devices and MIDI mappings are not part
/// of a rig.
struct RigSnapshot: Codable, Equatable {
  var globals: PresetGlobals
  var amp: AmpSection
  var cabinet: CabinetSection
  var specialFX: SpecialFXSection
  var preFX: BoardSnapshot
  var postFX: BoardSnapshot

  /// Bounds applied to every preset before any module sees it.
  static let maximumInstances = 128
  static let maximumStateBytes = 16 * 1024 * 1024

  var modules: [ModuleSnapshot] {
    amp.amps.flatMap { [$0.module] + ($0.equalizer.map { [$0] } ?? []) } + cabinet.cabinets
      + specialFX.modules
      + [preFX, postFX].flatMap { ($0.chain + $0.drafts).map(\.module) }
  }

  func validate() throws {
    guard globals.isValid else {
      throw AmpError.message("The preset's global controls are invalid")
    }
    for module in modules {
      let files = module.files ?? []
      guard files.count <= 16, Set(files.map(\.slot)).count == files.count else {
        throw AmpError.message("The preset repeats a file slot or has more than 16")
      }
      for file in files { try file.validate() }
    }
    for board in [preFX, postFX] {
      guard board.chain.count + board.drafts.count <= Self.maximumInstances else {
        throw AmpError.message("The preset has more than \(Self.maximumInstances) effects")
      }
    }
    for (count, kind) in [
      (amp.amps.count, "amps"), (cabinet.cabinets.count, "cabinets"),
      (specialFX.modules.count, "special FX"), (specialFX.active?.count ?? 0, "active special FX"),
    ] where count > Self.maximumInstances {
      throw AmpError.message("The preset lists more than \(Self.maximumInstances) \(kind)")
    }
    if let active = specialFX.active, Set(active).count != active.count {
      throw AmpError.message("The preset lists an active special FX module twice")
    }
    var total = 0
    for module in modules {
      total += module.state?.data.count ?? 0
      guard module.parameters.count <= 4096, (module.state?.data.count ?? 0) <= 1024 * 1024
      else { throw AmpError.message("The preset's settings for \(module.id) are too large") }
    }
    guard total <= Self.maximumStateBytes else {
      throw AmpError.message("The preset's module state exceeds the preset limit")
    }
  }
}

/// A preset file: a named rig, versioned independently of the native graph preset format.
struct RigPreset: Codable, Equatable {
  static let format = "dev.vespera.rig-preset"
  /// The newest schema this host reads. Files are written with the oldest schema that holds the
  /// rig: 3 for Special FX the legacy `selected` cannot express, else 2 for file references,
  /// else 1.
  static let schemaVersion = 3
  static let fileExtension = "vesperapreset"
  var format = RigPreset.format
  var schemaVersion = RigPreset.schemaVersion
  var name: String
  var rig: RigSnapshot

  init(name: String, rig: RigSnapshot) {
    self.name = name
    self.rig = rig
    schemaVersion = Self.schema(for: rig)
  }

  static func schema(for rig: RigSnapshot) -> Int {
    if !rig.specialFX.isLegacyRepresentable { return 3 }
    return rig.modules.contains { !($0.files ?? []).isEmpty } ? 2 : 1
  }

  static func decode(_ data: Data) throws -> RigPreset {
    struct Header: Decodable {
      let format: String?
      let schemaVersion: Int?
    }
    let header: Header
    do { header = try JSONDecoder().decode(Header.self, from: data) } catch {
      throw AmpError.message("The file is not a vespera preset")
    }
    guard header.format == format else {
      throw AmpError.message("The file is not a vespera preset")
    }
    guard let version = header.schemaVersion, (1...schemaVersion).contains(version) else {
      throw AmpError.message(
        "The preset uses schema \(header.schemaVersion.map(String.init) ?? "unknown"); this version of vespera reads schema \(schemaVersion)"
      )
    }
    let preset: RigPreset
    do { preset = try JSONDecoder().decode(RigPreset.self, from: data) } catch {
      throw AmpError.message("The preset file is damaged or incomplete")
    }
    try preset.rig.validate()
    guard
      preset.schemaVersion >= 2
        || !preset.rig.modules.contains(where: { !($0.files ?? []).isEmpty })
    else {
      throw AmpError.message("File references require preset schema 2")
    }
    guard preset.schemaVersion >= 3 || preset.rig.specialFX.isLegacyRepresentable else {
      throw AmpError.message("Multiple special FX require preset schema 3")
    }
    return preset
  }

  func encoded() throws -> Data {
    try rig.validate()
    let encoder = JSONEncoder()
    encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
    var preset = self
    preset.schemaVersion = Self.schema(for: rig)
    return try encoder.encode(preset)
  }
}
