import AVFoundation
import Combine
import Foundation
import SwiftUI

struct AudioChannelInfo: Codable, Identifiable, Hashable {
  let id: UInt32
  let name: String
  var title: String {
    name.isEmpty || name == String(id + 1) ? String(id + 1) : "\(id + 1) — \(name)"
  }
}

struct AudioDeviceInfo: Codable, Identifiable {
  let id: String
  let backend: String
  let name: String
  let pairable: Bool
  let inputs: [AudioChannelInfo]
  let outputs: [AudioChannelInfo]
  let rates: [UInt32]
  let currentRate: UInt32
  let minimumBuffer: UInt32
  let maximumBuffer: UInt32
  let preferredBuffer: UInt32
  let granularity: Int

  func supports(buffer: UInt32) -> Bool {
    guard buffer >= minimumBuffer, buffer <= maximumBuffer else { return false }
    if granularity == -1 { return buffer > 0 && buffer & (buffer - 1) == 0 }
    if granularity > 0 { return (buffer - minimumBuffer) % UInt32(granularity) == 0 }
    return true
  }
}

struct AudioConfiguration: Codable, Equatable {
  var backend = "coreaudio"
  var inputDevice = ""
  var outputDevice = ""
  var inputChannels: [UInt32] = [0]
  var outputChannels: [UInt32] = [0, 1]
  var sampleRate: UInt32 = 48000
  var bufferSize: UInt32 = 128
  var monitor = true

  func validate(devices: [AudioDeviceInfo]) throws {
    guard let input = devices.first(where: { $0.id == inputDevice }),
      let output = devices.first(where: { $0.id == outputDevice })
    else {
      throw AmpError.message("Select an available input and output device.")
    }
    guard input.backend == backend, output.backend == backend,
      inputDevice == outputDevice || (input.pairable && output.pairable)
    else {
      throw AmpError.message("These devices cannot be paired by the audio backend.")
    }
    guard (1...2).contains(inputChannels.count), (1...2).contains(outputChannels.count),
      Set(inputChannels).count == inputChannels.count,
      Set(outputChannels).count == outputChannels.count,
      inputChannels.allSatisfy({ id in input.inputs.contains { $0.id == id } }),
      outputChannels.allSatisfy({ id in output.outputs.contains { $0.id == id } })
    else {
      throw AmpError.message("Select one or two distinct channels for each device.")
    }
    guard input.rates.contains(sampleRate), output.rates.contains(sampleRate),
      input.supports(buffer: bufferSize), output.supports(buffer: bufferSize)
    else {
      throw AmpError.message("Choose a sample rate and buffer size supported by both devices.")
    }
  }
}

struct AudioStatus: Decodable {
  /// What the audio session reports about itself; a state this version does not know decodes
  /// as `.unknown` instead of failing.
  enum State: Equatable, Decodable {
    case running
    case prepared
    case stopped
    case recoveryRequired
    case unknown(String)

    init(from decoder: Decoder) throws {
      let raw = try decoder.singleValueContainer().decode(String.self)
      switch raw {
      case "running": self = .running
      case "prepared": self = .prepared
      case "stopped": self = .stopped
      case "recovery_required": self = .recoveryRequired
      default: self = .unknown(raw)
      }
    }
  }

  let state: State
  let recovery: String
  let lastError: Int
  let blocks: UInt64
  let beat: Double?
  let input: [Float]
  let output: [Float]
  let sampleRate: UInt32?
  let bufferSize: UInt32?
  let latencyFrames: UInt32?
  let midiSource: Int32?
  let midiDropped: UInt64?
  /// Modules of the running graph whose status is not ready.
  let modules: [ModuleRuntimeStatus]?
  let candidate: CandidatePreparationStatus?
}

struct CandidatePreparationStatus: Decodable {
  /// How far a replacement rig has got. States this version does not treat as preparation,
  /// including "idle", "ready" and "cancelled", decode as `.unknown`.
  enum State: Equatable, Decodable {
    case queued
    case preparing
    case waiting
    case unknown(String)

    init(from decoder: Decoder) throws {
      let raw = try decoder.singleValueContainer().decode(String.self)
      switch raw {
      case "queued": self = .queued
      case "preparing": self = .preparing
      case "waiting": self = .waiting
      default: self = .unknown(raw)
      }
    }

    var isCancellable: Bool {
      switch self {
      case .queued, .preparing, .waiting: true
      case .unknown: false
      }
    }
  }

  let generation: UInt64
  let activeGeneration: UInt64
  let state: State
  let error: String
  let modules: [ModuleRuntimeStatus]?
}

/// A status a running module reports through host services, for the bridge slot it fills.
struct ModuleRuntimeStatus: Decodable, Equatable {
  enum Kind: String, Decodable {
    /// Loading or rebuilding; the module still processes.
    case busy
    /// Processing as configured, with a remark.
    case warning
    /// Not processing as configured, such as a missing or unreadable resource.
    case error
  }

  let slot: String
  let status: Kind
  let message: String
  /// From 0 to 1 when the module knows how far it is.
  let progress: Float?
  var fileSlot: UInt32? = nil
}

/// A module status together with the name of the module it concerns.
struct ModuleNotice: Equatable, Identifiable {
  let name: String
  let status: ModuleRuntimeStatus

  var id: String { status.slot }
  var isProblem: Bool { status.status != .busy }

  var text: String {
    var detail = status.message
    if detail.isEmpty {
      detail = status.status == .busy ? "Working" : "Needs attention"
    }
    if status.status == .busy, let progress = status.progress {
      detail += " (\(Int((progress * 100).rounded()))%)"
    }
    return "\(name): \(detail)"
  }
}

struct MIDISourceInfo: Decodable, Identifiable {
  let id: Int32
  let name: String
}

@MainActor
final class AudioController: ObservableObject {
  enum State: Equatable {
    case stopped, requestingPermission, running, suspended, recovering, failed
  }
  let amps: AmpCatalog
  let cabinets: CabinetCatalog
  let preFX: PedalCatalog
  let postFX: PedalCatalog
  let specialFX: SpecialFXCatalog
  @Published private(set) var state = State.stopped
  @Published private(set) var devices: [AudioDeviceInfo] = []
  @Published private(set) var configuration = AudioConfiguration()
  @Published private(set) var globals = GlobalControlSettings()
  @Published private(set) var tempo = TempoSettings()
  private let moduleTiming = ModuleTiming()
  @Published private(set) var inputLevels: [Float] = [0, 0]
  @Published private(set) var outputLevels: [Float] = [0, 0]
  @Published private(set) var status: AudioStatus?
  /// Statuses the running modules report, from the latest bridge status.
  @Published private(set) var moduleStatuses: [ModuleRuntimeStatus] = []
  @Published var error: String?
  /// One-time information for the user, shown until dismissed. Unlike `error`, starting audio or
  /// a later failure never replaces it.
  @Published private(set) var notice: String?
  @Published var showsSettings = false
  @Published private(set) var midiSources: [MIDISourceInfo] = []
  @Published private(set) var midiSource: Int32 = 0
  @Published private(set) var midiChannel: UInt32 = 0
  @Published var midiMappings: [MIDIMapping] = [] {
    didSet {
      _ = try? mappingInput.drain()
      for mapping in oldValue where !midiMappings.contains(mapping) {
        mappingCoordinator.releaseGate(mapping.id)
      }
      defaults.set(
        try? JSONEncoder().encode(midiMappings), forKey: "audio.midiMappings")
    }
  }
  private let mappingInput = MIDIMappingInput()
  /// Where audio settings, globals, tempo and MIDI mappings persist.
  private let defaults: UserDefaults
  /// The MIDI mappings as saved before any was retargeted to a module that moved to Special FX,
  /// stored once and never overwritten.
  static let mappingsBeforeRelocationKey = "audio.midiMappings.beforeSpecialFXRelocation"
  private var bridge: AudioBridge?
  private var timer: Timer?
  /// Global-control changes are not sent as they happen: the latest settings wait here and go
  /// out with the next 30 Hz status poll, so a burst of knob turns collapses into one request.
  /// Starting audio or loading a preset clears this, because their requests already carry the
  /// global controls.
  private var pendingGlobals: GlobalControlSettings?
  private lazy var powerRecovery = AudioPowerRecovery(
    isRunning: { [weak self] in self?.state == .running },
    suspend: { [weak self] in
      guard let self else { return }
      self.mappingCoordinator.releaseGates()
      _ = try self.bridge?.request(.stop)
      self.transition(to: .suspended)
      self.inputLevels = [0, 0]
      self.outputLevels = [0, 0]
    },
    resume: { [weak self] in
      guard let self else { return }
      guard AVCaptureDevice.authorizationStatus(for: .audio) == .authorized else {
        throw AmpError.message("Microphone access is unavailable.")
      }
      try self.refreshDevices()
      try self.activate(self.configuration)
    },
    onWaiting: { [weak self] in self?.transition(to: .recovering) },
    onIdleWake: { [weak self] in
      do { try self?.refreshDevices() } catch { self?.report(error) }
    },
    onFailure: { [weak self] message in
      self?.transition(to: .failed)
      self?.error = message
    })
  /// CoreAudio device queries and the listener for the system's device list changing.
  private lazy var deviceStore = AudioDeviceStore(
    onDevicesChanged: { [weak self] in self?.devicesChanged() })
  /// Runs MIDI mappings against the installed modules.
  private lazy var mappingCoordinator = MIDIMappingCoordinator(
    amps: amps, cabinets: cabinets, preFX: preFX, postFX: postFX, specialFX: specialFX,
    report: { [weak self] error in self?.report(error) })
  /// Resolves, validates and commits whole-rig changes.
  private lazy var stager = RigStager(
    amps: amps, cabinets: cabinets, preFX: preFX, postFX: postFX, specialFX: specialFX,
    submit: { [weak self] request in
      guard let self else { return }
      try self.submit(request)
    },
    commit: { [weak self] staged in
      guard let self else { return }
      self.mappingCoordinator.clearGates()
      self.globals = staged.globals
      self.pendingGlobals = nil
      self.persist()
      self.rigEdited()
    },
    stopIfRunning: { [weak self] in
      guard let self, self.state == .running else { return false }
      self.stop()
      return true
    },
    report: { [weak self] error in self?.report(error) })
  /// Called after anything a preset captures may have changed: module settings, chains, editors,
  /// selections or global controls.
  var onRigEdit: (() -> Void)?
  private var rigObservers: [AnyCancellable] = []
  /// Submits a staged preset to the audio side; throwing rejects it. Replaceable for tests.
  var submitPreset: (([String: Any]) throws -> Void)?
  private static let decoder = JSONDecoder()

  var inputDevice: AudioDeviceInfo? { devices.first { $0.id == configuration.inputDevice } }
  var outputDevice: AudioDeviceInfo? { devices.first { $0.id == configuration.outputDevice } }
  var stereoAvailable: Bool { (inputDevice?.inputs.count ?? 0) >= 2 }

  // MARK: Startup

  init(
    amps: AmpCatalog = AmpCatalog(), preFX: PedalCatalog = PedalCatalog(),
    postFX: PedalCatalog = PedalCatalog(position: .post),
    cabinets: CabinetCatalog = CabinetCatalog(), specialFX: SpecialFXCatalog = SpecialFXCatalog(),
    defaults: UserDefaults = .standard
  ) {
    self.defaults = defaults
    self.amps = amps
    self.cabinets = cabinets
    self.preFX = preFX
    self.postFX = postFX
    self.specialFX = specialFX
    let relocated = loadPersistedState()
    do {
      bridge = try AudioBridge()
      try startBackend()
    } catch {
      report(error)
      transition(to: .failed)
    }
    if relocated > 0 {
      notice =
        relocated == 1
        ? "1 MIDI mapping now targets a module that moved to Special FX."
        : "\(relocated) MIDI mappings now target modules that moved to Special FX."
    }
    wireCatalogs()
    observeDevices()
    startPolling()
  }

  /// Decodes the saved MIDI mappings, audio configuration and global controls, retargeting
  /// mappings whose module moved to Special FX; returns how many moved.
  private func loadPersistedState() -> Int {
    var relocatedMappings = 0
    if let data = defaults.data(forKey: "audio.midiMappings"),
      let saved = try? Self.decoder.decode([MIDIMapping].self, from: data)
    {
      let relocated = MIDIMapping.relocated(saved, special: Set(specialFX.modules.map(\.id)))
      midiMappings = relocated.mappings
      relocatedMappings = relocated.moved
      if relocated.moved > 0 {
        if defaults.data(forKey: Self.mappingsBeforeRelocationKey) == nil {
          defaults.set(data, forKey: Self.mappingsBeforeRelocationKey)
        }
        defaults.set(try? JSONEncoder().encode(relocated.mappings), forKey: "audio.midiMappings")
      }
    }
    if let saved = defaults.data(forKey: "audio.configuration"),
      let config = try? Self.decoder.decode(AudioConfiguration.self, from: saved)
    {
      configuration = config
    }
    if let saved = defaults.data(forKey: "audio.globals"),
      let settings = try? Self.decoder.decode(GlobalControlSettings.self, from: saved)
    {
      globals = settings
    }
    return relocatedMappings
  }

  /// Hands the backend the saved tempo, shares the beat clock with the catalogs and picks the
  /// devices and rate the configuration will use. Requires `bridge` to be connected.
  private func startBackend() throws {
    if let saved = defaults.data(forKey: "audio.tempo"),
      let settings = try? Self.decoder.decode(TempoSettings.self, from: saved), settings.isValid
    {
      tempo = settings
    }
    _ = try bridge?.request(.tempo, ["tempo": try AudioBridge.object(tempo)])
    moduleTiming.bpm = tempo.bpm
    preFX.timing = moduleTiming
    postFX.timing = moduleTiming
    for amp in amps.amps {
      amp.timing = moduleTiming
      amp.equalizer?.timing = moduleTiming
    }
    try refreshDevices()
    chooseDefaults()
  }

  /// The selection, parameter and file callbacks the catalogs and the controller drive each
  /// other through, and the observers that mark the rig edited.
  private func wireCatalogs() {
    cabinets.onChange = { [weak self] amp, cabinet in
      guard let self else { return }
      self.rigEdited()
      guard self.state == .running else { return }
      _ = try self.bridge?.request(try Self.rigRequest(amp: amp, cabinet: cabinet))
    }
    do { try cabinets.setAmp(amps.activeAmp) } catch { report(error) }
    amps.onSelection = { [weak self] amp in try self?.cabinets.setAmp(amp) }
    for cabinet in cabinets.cabinets {
      cabinet.timing = moduleTiming
      cabinet.onParameterChange = { [weak self, weak cabinet] id, value in
        self?.rigEdited()
        guard let self, let cabinet, self.state == .running, self.cabinets.activeCabinet === cabinet
        else { return }
        try self.sendParameter(slot: "cabinet", id: id, value: value)
      }
    }
    specialFX.onChange = { [weak self] modules in
      self?.rigEdited()
      guard let self, self.state == .running else { return }
      _ = try self.bridge?.request(.specialFX, ["modules": modules])
    }
    for module in specialFX.modules {
      module.timing = moduleTiming
      module.onParameterChange = { [weak self, weak module] id, value in
        self?.rigEdited()
        if let self, let module { self.specialFX.parameterChanged(module, id) }
        guard let self, let module, self.state == .running, self.specialFX.isActive(module.id)
        else { return }
        try self.sendParameter(slot: SpecialFXCatalog.slot(module.id), id: id, value: value)
      }
    }
    for catalog in [preFX, postFX] {
      let operation = catalog.position.operation
      catalog.onChange = { [weak self] modules in
        self?.rigEdited()
        guard let self, self.state == .running else { return }
        _ = try self.bridge?.request(operation, ["modules": modules])
      }
      catalog.onParameterChange = { [weak self] slot, id, value in
        self?.rigEdited()
        guard let self, self.state == .running else { return }
        try self.sendParameter(slot: slot, id: id, value: value)
      }
    }
    for amp in amps.amps {
      amp.onParameterChange = { [weak self, weak amp] id, value in
        self?.rigEdited()
        guard let self, let amp, self.state == .running, self.amps.activeAmp === amp else { return }
        try self.sendParameter(slot: nil, id: id, value: value)
      }
      amp.equalizer?.onParameterChange = { [weak self, weak amp] id, value in
        self?.rigEdited()
        guard let self, let amp, self.state == .running, self.amps.activeAmp === amp else { return }
        try self.sendParameter(slot: "eq", id: id, value: value)
      }
    }
    let publishers: [ObservableObjectPublisher] = [
      amps.objectWillChange, cabinets.objectWillChange, specialFX.objectWillChange,
      preFX.objectWillChange, postFX.objectWillChange,
    ]
    rigObservers = publishers.map { $0.sink { [weak self] in self?.rigEdited() } }
    for module in amps.amps.flatMap({ [$0] + ($0.equalizer.map { [$0] } ?? []) })
      + cabinets.cabinets + specialFX.modules
    {
      module.onFileChange = { [weak self] in
        guard let self else { return }
        try self.submit(self.currentRigRequest())
        self.rigEdited()
      }
    }
  }

  /// Starts listening for the system's device list changing, so a disconnect or a new device
  /// refreshes what Audio Settings offers.
  private func observeDevices() { _ = deviceStore }

  /// The recurring bridge poll and the sleep/wake recovery watcher.
  private func startPolling() {
    _ = powerRecovery
    timer = Timer.scheduledTimer(withTimeInterval: 1 / 30, repeats: true) { [weak self] _ in
      MainActor.assumeIsolated { self?.poll() }
    }
  }

  private func report(_ error: Error) { self.error = error.localizedDescription }

  /// Forwards a live parameter change to the audio side; the active amp's own parameters have no
  /// slot. A failure is reported and rethrown, so the control that changed also rejects it.
  private func sendParameter(slot: String?, id: UInt32, value: Float) throws {
    var fields: [String: Any] = ["id": id, "value": value]
    if let slot { fields["slot"] = slot }
    do { _ = try bridge?.request(.parameter, fields) } catch {
      report(error)
      throw error
    }
  }

  /// The one way `state` changes: start passes through requestingPermission into running; a
  /// sleep suspends and a wake recovers; any failure ends in failed; stop ends in stopped.
  private func transition(to next: State) { state = next }

  // MARK: Devices

  func refreshDevices() throws {
    guard let bridge else { throw AmpError.message("Audio backend is unavailable.") }
    let data = try bridge.request(.devices)
    devices = try Self.decoder.decode([AudioDeviceInfo].self, from: data).filter {
      $0.backend == "coreaudio"
    }
  }

  private func devicesChanged() {
    guard !powerRecovery.isPending else { return }
    do {
      try refreshDevices()
      if inputDevice == nil || outputDevice == nil {
        stop()
        error =
          "The selected audio device disconnected. Choose an available device in Audio Settings."
        showsSettings = true
      }
    } catch { report(error) }
  }

  private func chooseDefaults() {
    if inputDevice == nil {
      let defaultInput = AudioDeviceStore.defaultDevice(input: true)
      configuration.inputDevice =
        devices.first { $0.id == defaultInput && !$0.inputs.isEmpty }?.id
        ?? devices.first { !$0.inputs.isEmpty }?.id ?? ""
      configuration.inputChannels = Array((inputDevice?.inputs ?? []).prefix(1).map(\.id))
    }
    if outputDevice == nil {
      let defaultOutput = AudioDeviceStore.defaultDevice(input: false)
      configuration.outputDevice =
        devices.first { $0.id == defaultOutput && !$0.outputs.isEmpty }?.id
        ?? devices.first { !$0.outputs.isEmpty }?.id ?? ""
      configuration.outputChannels = Array((outputDevice?.outputs ?? []).prefix(2).map(\.id))
    }
    if let input = inputDevice, let output = outputDevice {
      let rates = input.rates.filter { output.rates.contains($0) }
      if !rates.contains(configuration.sampleRate) {
        configuration.sampleRate = rates.first ?? 48000
      }
      if !input.supports(buffer: configuration.bufferSize)
        || !output.supports(buffer: configuration.bufferSize)
      {
        configuration.bufferSize = Self.bufferSizes(input: input, output: output).first ?? 128
      }
    }
    globals.stereoInput = configuration.inputChannels.count == 2
  }

  static func bufferSizes(input: AudioDeviceInfo?, output: AudioDeviceInfo?) -> [UInt32] {
    AudioDeviceStore.bufferSizes(input: input, output: output)
  }

  // MARK: Audio lifecycle

  func start() {
    guard state != .requestingPermission, !powerRecovery.isSleeping else { return }
    switch AVCaptureDevice.authorizationStatus(for: .audio) {
    case .authorized:
      apply(configuration)
    case .notDetermined:
      transition(to: .requestingPermission)
      AVCaptureDevice.requestAccess(for: .audio) { [weak self] granted in
        Task { @MainActor in
          guard let self, self.state == .requestingPermission else { return }
          self.transition(to: .stopped)
          if granted { self.apply(self.configuration) } else { self.permissionDenied() }
        }
      }
    default: permissionDenied()
    }
  }

  private func permissionDenied() {
    transition(to: .failed)
    error =
      "Microphone access is disabled. Enable vespera in System Settings → Privacy & Security → Microphone, then start audio again."
    showsSettings = true
  }

  func apply(_ proposed: AudioConfiguration) {
    guard !powerRecovery.isSleeping else { return }
    powerRecovery.cancel()
    guard AVCaptureDevice.authorizationStatus(for: .audio) == .authorized else {
      configuration = proposed
      start()
      return
    }
    do {
      try activate(proposed)
    } catch {
      report(error)
      if state != .running { transition(to: .failed) }
      showsSettings = true
    }
  }

  private func activate(_ proposed: AudioConfiguration) throws {
    try proposed.validate(devices: devices)
    if cabinets.amp !== amps.activeAmp { try cabinets.setAmp(amps.activeAmp) }
    guard let bridge else { throw AmpError.message("Audio backend is unavailable.") }
    var payload = try startConfiguration()
    payload["audio"] = try AudioBridge.object(proposed)
    payload["tempo"] = try AudioBridge.object(tempo)
    let data = try bridge.request(.start, ["configuration": payload])
    apply(status: try Self.decoder.decode(AudioStatus.self, from: data))
    configuration = proposed
    globals.stereoInput = proposed.inputChannels.count == 2
    pendingGlobals = nil
    transition(to: .running)
    error = nil
    persist()
  }

  func stop() {
    powerRecovery.cancel()
    do {
      _ = try bridge?.request(.stop)
      transition(to: .stopped)
      moduleStatuses = []
    } catch {
      report(error)
      transition(to: .failed)
    }
    inputLevels = [0, 0]
    outputLevels = [0, 0]
  }

  func shutdown() {
    powerRecovery.shutdown()
    stop()
    timer?.invalidate()
    timer = nil
    deviceStore.stop()
  }

  private func poll() {
    powerRecovery.poll()
    guard !powerRecovery.isPending else { return }
    pollMappings()
    guard state == .running, let bridge else { return }
    do {
      if let pendingGlobals {
        _ = try bridge.request(.globals, ["globals": try AudioBridge.object(pendingGlobals)])
        self.pendingGlobals = nil
      }
      let current = try Self.decoder.decode(
        AudioStatus.self, from: bridge.request(.status))
      apply(status: current)
      if midiSource != current.midiSource ?? 0 {
        mappingCoordinator.releaseGates()
        midiSource = current.midiSource ?? 0
        try mappingInput.connect(midiSource)
      }
      moduleTiming.beat = current.beat ?? 0
      inputLevels = zip(current.input, inputLevels).map { max($0, $1 * 0.8) }
      outputLevels = zip(current.output, outputLevels).map { max($0, $1 * 0.8) }
      if current.state != .running && !(current.state == .prepared && current.candidate != nil) {
        transition(to: .failed)
        error =
          "Audio stopped: \(current.recovery) (\(current.lastError)). Check Audio Settings and restart."
        inputLevels = [0, 0]
        outputLevels = [0, 0]
      }
    } catch { report(error) }
  }

  func apply(status current: AudioStatus) {
    status = current
    let modules = current.candidate?.modules ?? current.modules ?? []
    if modules != moduleStatuses { moduleStatuses = modules }
    var selected: [(String, LoadedModule)] = []
    if let module = amps.activeAmp { selected.append(("amp", module)) }
    if let module = amps.activeAmp?.equalizer { selected.append(("eq", module)) }
    if let module = cabinets.activeCabinet { selected.append(("cabinet", module)) }
    selected += specialFX.activeModules.map { (SpecialFXCatalog.slot($0.id), $0) }
    selected += (preFX.chain + postFX.chain).map { ($0.id, $0.module) }
    for module in specialFX.modules where !specialFX.isActive(module.id) {
      module.setFileProblems([:])
    }
    for (slot, module) in selected {
      let problems = Dictionary(
        modules.compactMap { status -> (UInt32, String)? in
          guard status.slot == slot, status.status == .error, let fileSlot = status.fileSlot
          else { return nil }
          return (fileSlot, status.message)
        }, uniquingKeysWith: { first, _ in first })
      module.setFileProblems(problems)
    }
  }

  // MARK: Globals and tempo

  func updateGlobals(_ settings: GlobalControlSettings) {
    let previous = globals
    globals = settings
    if settings.stereoInput != previous.stereoInput {
      var proposed = configuration
      let first = proposed.inputChannels.first ?? inputDevice?.inputs.first?.id ?? 0
      proposed.inputChannels = [first]
      if settings.stereoInput, let second = inputDevice?.inputs.first(where: { $0.id != first }) {
        proposed.inputChannels.append(second.id)
      }
      if state == .running {
        apply(proposed)
        globals.stereoInput = configuration.inputChannels.count == 2
      } else {
        configuration = proposed
      }
    }
    pendingGlobals = globals
    persist()
    rigEdited()
  }

  func updateTempo(_ settings: TempoSettings) {
    guard settings.isValid else { return }
    do {
      guard let bridge else { throw AmpError.message("Audio backend is unavailable.") }
      _ = try bridge.request(.tempo, ["tempo": try AudioBridge.object(settings)])
      tempo = settings
      moduleTiming.bpm = settings.bpm
      persist()
    } catch { report(error) }
  }

  func tapTempo() {
    do {
      guard let bridge else { throw AmpError.message("Audio backend is unavailable.") }
      let settings = try Self.decoder.decode(
        TempoSettings.self, from: bridge.request(.tap))
      tempo = settings
      moduleTiming.bpm = settings.bpm
      persist()
    } catch { report(error) }
  }

  private func persist() {
    defaults.set(
      try? JSONEncoder().encode(configuration), forKey: "audio.configuration")
    defaults.set(try? JSONEncoder().encode(globals), forKey: "audio.globals")
    defaults.set(try? JSONEncoder().encode(tempo), forKey: "audio.tempo")
  }

  // MARK: Rig requests

  private func rigEdited() { onRigEdit?() }

  /// One atomic bridge change for the amp and everything it owns or links: cabinet and EQ.
  static func rigRequest(amp: LoadedModule?, cabinet: LoadedModule?) throws -> [String: Any] {
    [
      "op": BridgeOp.rig.rawValue, "amp": try amp?.audioDescription() ?? NSNull(),
      "cabinet": try RigStager.cabinetDescription(cabinet),
      "eq": try RigStager.equalizerDescription(amp),
    ]
  }

  private func submit(_ request: [String: Any]) throws {
    if let submitPreset { return try submitPreset(request) }
    guard state == .running else { return }
    guard let bridge else { throw AmpError.message("Audio backend is unavailable.") }
    _ = try bridge.request(request)
  }

  /// The preset request describing what is currently shown, used to restore the audio side.
  private func currentRigRequest() throws -> [String: Any] {
    try stager.currentRigRequest(globals: globals)
  }

  /// The module sections and global controls audio starts with: what is currently shown.
  func startConfiguration() throws -> [String: Any] {
    try stager.startConfiguration(globals: globals)
  }

  // MARK: Runtime status

  func dismissNotice() { notice = nil }

  var preparationMessage: String? {
    guard let candidate = status?.candidate else { return nil }
    if !candidate.error.isEmpty { return candidate.error }
    switch candidate.state {
    case .queued, .preparing: return "Preparing rig…"
    case .waiting: return "Loading rig…"
    case .unknown: return nil
    }
  }

  var canCancelPreparation: Bool {
    guard let candidate = status?.candidate else { return false }
    return candidate.state.isCancellable
  }

  func cancelPreparation() {
    do {
      guard let bridge else { return }
      let data = try bridge.request(.cancelCandidate)
      apply(status: try Self.decoder.decode(AudioStatus.self, from: data))
    } catch { report(error) }
  }

  /// The modules shown on a tab whose status is not ready, named as the user knows them.
  func moduleNotices(on tab: ModuleTab) -> [ModuleNotice] {
    var names: [String: String] = [:]
    switch tab {
    case .amp: names["amp"] = amps.activeAmp?.name
    case .eq: names["eq"] = amps.activeAmp?.equalizer?.name
    case .cabinet: names["cabinet"] = cabinets.activeCabinet?.name
    case .specialFX:
      for module in specialFX.activeModules {
        names[SpecialFXCatalog.slot(module.id)] = module.name
      }
    case .preFX, .postFX:
      for instance in (tab == .preFX ? preFX : postFX).chain {
        names[instance.id] = instance.title
      }
    }
    return moduleStatuses.compactMap { status in
      names[status.slot].map { ModuleNotice(name: $0, status: status) }
    }
  }

  // MARK: MIDI

  func refreshMIDI() {
    do {
      guard let bridge else { return }
      midiSources = try Self.decoder.decode(
        [MIDISourceInfo].self, from: bridge.request(.midiDevices))
    } catch { report(error) }
  }

  func updateMIDI(source: Int32, channel: UInt32) {
    do {
      guard let bridge else { throw AmpError.message("Audio backend is unavailable.") }
      _ = try bridge.request(.midi, ["source": source, "channel": channel, "slot": "specialFX"])
      do { try mappingInput.connect(source) } catch {
        _ = try? bridge.request(
          .midi, ["source": midiSource, "channel": midiChannel, "slot": "specialFX"])
        try? mappingInput.connect(midiSource)
        throw error
      }
      midiSource = source
      midiChannel = channel
      mappingCoordinator.releaseGates()
    } catch { report(error) }
  }

  func panicMIDI() {
    _ = try? mappingInput.drain()
    mappingCoordinator.releaseGates()
    do { _ = try bridge?.request(.midiPanic) } catch {
      report(error)
    }
  }

  var midiTargets: [MIDITarget] { mappingCoordinator.targets }

  private func pollMappings() {
    let messages: [MIDIMappingInput.Message]
    do { messages = try mappingInput.drain() } catch {
      mappingCoordinator.releaseGates()
      report(error)
      return
    }
    applyMIDIMessages(messages)
  }

  func applyMIDIMessages(_ messages: [MIDIMappingInput.Message]) {
    mappingCoordinator.apply(messages, mappings: midiMappings, channel: midiChannel)
  }

  // MARK: Presets

  /// Everything a preset keeps of the rig as it is shown and processes now.
  func captureRig() throws -> RigSnapshot { try stager.captureRig(globals: globals) }

  /// Resolves and validates `rig` without changing anything. Installed modules the preset does
  /// not mention take their default settings.
  func stageRig(_ rig: RigSnapshot) throws -> RigStager.StagedRig {
    try stager.stageRig(rig, globals: globals)
  }

  /// Loads `rig` as one change; `RigStager.applyRig` describes the commit and rollback.
  @discardableResult
  func applyRig(_ rig: RigSnapshot, beforeCommit: ((RigStager.StagedRig) throws -> Void)? = nil)
    throws -> [String]
  {
    try stager.applyRig(rig, globals: globals, beforeCommit: beforeCommit)
  }

  // MARK: Offline rendering

  /// Renders the current module sections offline through the audio backend's graph builder,
  /// with fresh module instances. Global controls are not applied.
  func renderOffline(input: [[Float]], sampleRate: UInt32 = 48000, blockSize: UInt32 = 128)
    throws -> [[Float]]
  {
    guard let bridge else { throw AmpError.message("Audio backend is unavailable.") }
    var configuration = try startConfiguration()
    configuration.removeValue(forKey: "globals")
    let data = try bridge.request(
      .renderGraph,
      [
        "configuration": configuration, "input": input, "sampleRate": sampleRate,
        "blockSize": blockSize, "bpm": tempo.bpm,
      ])
    struct Rendered: Decodable { let output: [[Float]] }
    return try Self.decoder.decode(Rendered.self, from: data).output
  }
}
