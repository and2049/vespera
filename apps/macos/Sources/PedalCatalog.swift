import SwiftUI

struct PedalModuleChoice: Identifiable {
  let package: ModulePackage
  let name: String
  var pluginFamily: String? {
    let family = package.manifest.pluginFamily?.trimmingCharacters(in: .whitespacesAndNewlines)
    return family?.isEmpty == false ? family : nil
  }
  var id: String { package.manifest.id }

  /// The order every catalog lists modules in, matching `LoadedModule.familyOrderKey`.
  var familyOrderKey: (String, String, String) { (pluginFamily ?? name, name, id) }

  func instantiate() throws -> LoadedModule {
    try LoadedModule(
      manifest: package.manifest, directory: package.directory, category: package.manifest.category)
  }
}

/// One effect on a board. Its identity and settings persist across reordering and across changes
/// to which editors are shown.
struct PedalInstance: Identifiable {
  enum Lifecycle: Equatable {
    /// Chosen from the library and shown switched off, outside the processing chain. Discarded
    /// once its editor leaves the board, since it holds nothing but defaults.
    case untouchedDraft
    /// A draft whose settings were changed. Kept outside the chain, switched off, until it is
    /// switched on or removed.
    case editedDraft
    /// In the processing chain. Switching it off bypasses it in place.
    case chained
  }

  let id: String
  let module: LoadedModule
  /// Distinguishes repeated instances of one module: 1 for the first, 2 for the next, and so on.
  let ordinal: Int
  var lifecycle: Lifecycle
  /// Bypass of a module without a bypass parameter; such a module is kept out of the audio graph.
  var hostBypassed = false

  var title: String { ordinal > 1 ? "\(module.name) \(ordinal)" : module.name }
  var isDraft: Bool { lifecycle != .chained }
  var bypassed: Bool {
    module.bypassParameter.map { ((try? module.value($0)) ?? 1) < 0.5 } ?? hostBypassed
  }
}

/// Which instances have their editors on screen, left to right across the board's units.
/// Independent of processing order: showing, hiding or rearranging editors never changes the
/// chain.
struct PedalEditorLayout: Equatable {
  enum Span: Equatable {
    /// One unoccupied unit.
    case free
    /// An instance's editor, spanning its module's board width.
    case editor(String, width: Int)

    var width: Int {
      switch self {
      case .free: 1
      case .editor(_, let width): width
      }
    }

    var id: String? {
      switch self {
      case .free: nil
      case .editor(let id, _): id
      }
    }
  }

  static let capacity = ModuleManifest.boardWidths.upperBound

  /// Left to right, together exactly `capacity` units wide.
  private(set) var spans: [Span]

  /// Fits `spans` to the board: when they are too wide, free units are dropped from the right,
  /// then editors from the right; free units fill any remaining room on the right.
  init(_ spans: [Span] = []) {
    var spans = spans.map { span -> Span in
      guard case .editor(let id, let width) = span else { return span }
      return .editor(id, width: min(max(width, 1), Self.capacity))
    }
    while Self.width(spans) > Self.capacity {
      spans.remove(at: spans.lastIndex(of: .free) ?? spans.count - 1)
    }
    self.spans = spans + Array(repeating: .free, count: Self.capacity - Self.width(spans))
  }

  private init(fitted spans: [Span]) { self.spans = spans }

  private static func width(_ spans: [Span]) -> Int { spans.map(\.width).reduce(0, +) }

  var visible: [String] { spans.compactMap(\.id) }

  /// The board unit each span starts at.
  var units: [Int] { spans.indices.map { Self.width(Array(spans[..<$0])) } }

  /// The span covering `unit`, which is clamped to the board.
  func span(at unit: Int) -> Int {
    let unit = min(max(unit, 0), Self.capacity - 1)
    return units.lastIndex { $0 <= unit }!
  }

  var freeUnits: Int { spans.filter { $0 == .free }.count }

  /// Shows an editor `width` units wide in the span at `unit`, or with no unit given, where it
  /// stays shown or else in the first free run wide enough, the first free unit, or the last span.
  ///
  /// An editor already shown elsewhere exchanges places with the span at `unit`. Otherwise that
  /// span's units are freed and the editor starts at `unit`. When the board then lacks room, the
  /// fewest other editors are hidden, preferring those to the editor's right, nearest first, and
  /// then those to its left, nearest first. The editor then takes free units in the same order.
  func showing(_ id: String, width: Int, at unit: Int?) -> PedalEditorLayout {
    if let existing = spans.firstIndex(where: { $0.id == id }) {
      guard let unit else { return self }
      var spans = spans
      spans.swapAt(existing, span(at: unit))
      return PedalEditorLayout(fitted: spans)
    }
    var layout = self
    let target: Int
    if let unit {
      layout = clearing(at: unit)
      target = layout.span(at: unit)
    } else {
      target =
        self.spans.indices.first { start in
          self.spans[start...].prefix(width).count == width
            && self.spans[start...].prefix(width).allSatisfy { $0 == .free }
        } ?? self.spans.firstIndex(of: .free) ?? self.spans.count - 1
    }
    var spans = layout.spans
    spans[target] = .editor(id, width: min(max(width, 1), Self.capacity))
    return Self.fitting(spans, around: target)
  }

  private static func fitting(_ spans: [Span], around target: Int) -> PedalEditorLayout {
    var spans = spans
    var target = target
    func preference(_ index: Int) -> (Int, Int) {
      (index > target ? 0 : 1, abs(index - target))
    }
    let excess = width(spans) - capacity - spans.filter { $0 == .free }.count
    if excess > 0 {
      let candidates = spans.indices.filter { $0 != target && spans[$0] != .free }
        .sorted { preference($0) < preference($1) }
      let hidden = Set(fewest(candidates, covering: excess) { spans[$0].width })
      var remaining: [Span] = []
      for (index, span) in spans.enumerated() {
        if index == target { target = remaining.count }
        remaining += hidden.contains(index) ? Array(repeating: .free, count: span.width) : [span]
      }
      spans = remaining
    }
    let overflow = width(spans) - capacity
    if overflow > 0 {
      let dropped = spans.indices.filter { spans[$0] == .free }
        .sorted { preference($0) < preference($1) }.prefix(overflow)
      for index in dropped.sorted(by: >) { spans.remove(at: index) }
    } else if overflow < 0 {
      spans.insert(contentsOf: Array(repeating: .free, count: -overflow), at: target + 1)
    }
    return PedalEditorLayout(fitted: spans)
  }

  /// The fewest `candidates` whose widths add up to at least `excess`, preferring candidates
  /// earlier in the list.
  private static func fewest(_ candidates: [Int], covering excess: Int, width: (Int) -> Int)
    -> [Int]
  {
    func combinations(_ count: Int, from start: Int) -> [[Int]] {
      guard count > 0 else { return [[]] }
      guard start < candidates.count else { return [] }
      return (start..<candidates.count).flatMap { index in
        combinations(count - 1, from: index + 1).map { [candidates[index]] + $0 }
      }
    }
    for count in 1...max(1, candidates.count) {
      if let found = combinations(count, from: 0).first(where: {
        $0.map(width).reduce(0, +) >= excess
      }) {
        return found
      }
    }
    return candidates
  }

  /// Leaves `id`'s units free.
  func clearing(_ id: String) -> PedalEditorLayout {
    guard let index = spans.firstIndex(where: { $0.id == id }) else { return self }
    return clearing(span: index)
  }

  /// Leaves the units of the span at `unit` free.
  func clearing(at unit: Int) -> PedalEditorLayout { clearing(span: span(at: unit)) }

  private func clearing(span index: Int) -> PedalEditorLayout {
    var spans = spans
    spans.replaceSubrange(index...index, with: Array(repeating: .free, count: spans[index].width))
    return PedalEditorLayout(fitted: spans)
  }
}

@MainActor
final class PedalCatalog: ObservableObject {
  enum Position: String {
    case pre = "pre-fx"
    case post = "post-fx"
    var title: String { self == .pre ? "Pre FX" : "Post FX" }
    var operation: BridgeOp { self == .pre ? .preFX : .postFX }
    /// Instances in a board's initial chain. These are the instances legacy index-based
    /// identities, and the MIDI mappings saved against them, refer to.
    var initialInstances: Int { self == .pre ? 4 : 3 }
  }
  /// Editor menu IDs with this prefix choose an installed module, creating a new draft.
  static let libraryPrefix = "library:"
  /// Editor menu group holding the board's existing instances, so they can be shown again.
  static let currentGroup = "Current effects"

  let position: Position
  /// Installed modules, listed from package metadata without creating module instances.
  let choices: [PedalModuleChoice]
  /// The processing chain in signal order.
  @Published private(set) var chain: [PedalInstance] = []
  /// Instances chosen from the library that have not been switched on, oldest first. Never part
  /// of the processing chain or its audio descriptions.
  @Published private(set) var drafts: [PedalInstance] = []
  @Published private(set) var editors = PedalEditorLayout()
  @Published private(set) var error: String?
  /// Receives the full chain's audio descriptions for each chain change; throwing rejects it.
  var onChange: (([[String: Any]]) throws -> Void)?
  var onParameterChange: ((String, UInt32, Float) throws -> Void)?
  var timing: ModuleTiming? {
    didSet { for instance in chain + drafts { instance.module.timing = timing } }
  }

  init(position: Position = .pre, directory: URL? = nil) {
    self.position = position
    let (loaded, loadFailures) = ModuleDiscovery.catalog(
      category: position.rawValue,
      directories: directory.map { [$0] } ?? ModuleDiscovery.searchDirectories(),
      make: { package in
        PedalModuleChoice(
          package: package,
          name: try LoadedModule.name(
            manifest: package.manifest, directory: package.directory, category: position.rawValue))
      },
      orderKey: \.familyOrderKey)
    var failures = loadFailures
    choices = loaded
    // Initial instances use index-based identities, the form MIDI targets took before
    // instances had their own identity, so mappings saved against the default chain still apply.
    var initial: [PedalInstance] = []
    for (index, choice) in self.choices.prefix(position.initialInstances).enumerated() {
      do {
        initial.append(
          PedalInstance(
            id: "\(position.rawValue).\(index)", module: try choice.instantiate(), ordinal: 1,
            lifecycle: .chained))
      } catch { failures.append(error.localizedDescription) }
    }
    for instance in initial { connect(instance) }
    chain = initial
    editors = Self.defaultEditors(initial)
    if !failures.isEmpty { error = failures.joined(separator: " ") }
  }

  /// Chain instances in order, skipping any too wide for the units that remain.
  static func defaultEditors(_ chain: [PedalInstance]) -> PedalEditorLayout {
    var spans: [PedalEditorLayout.Span] = []
    var free = PedalEditorLayout.capacity
    for instance in chain where instance.module.boardWidth <= free {
      spans.append(.editor(instance.id, width: instance.module.boardWidth))
      free -= instance.module.boardWidth
    }
    return PedalEditorLayout(spans)
  }

  func instance(_ id: String) -> PedalInstance? {
    chain.first { $0.id == id } ?? drafts.first { $0.id == id }
  }

  /// Chain instances whose editors are not shown; they keep processing.
  var hiddenInstances: [PedalInstance] {
    let visible = Set(editors.visible)
    return chain.filter { !visible.contains($0.id) }
  }

  private func connect(_ instance: PedalInstance) {
    instance.module.timing = timing
    let id = instance.id
    instance.module.onParameterChange = { [weak self] parameter, value in
      try self?.parameterWillChange(id, parameter, value)
    }
    instance.module.onFileChange = { [weak self] in
      guard let self else { return }
      if self.chain.contains(where: { $0.id == id }) {
        try self.onChange?(self.audioDescriptions())
      } else if let index = self.drafts.firstIndex(where: { $0.id == id }) {
        self.drafts[index].lifecycle = .editedDraft
      }
      self.objectWillChange.send()
    }
  }

  /// Runs before an instance's control copy takes a value; throwing leaves the value unchanged.
  /// Chain instances forward to the audio side. Drafts stay local, except that switching one on
  /// adds it to the chain.
  private func parameterWillChange(_ id: String, _ parameter: UInt32, _ value: Float) throws {
    if let instance = chain.first(where: { $0.id == id }) {
      try onParameterChange?(id, parameter, value)
      if parameter == instance.module.bypassParameter {
        Task { @MainActor in self.objectWillChange.send() }
      }
      return
    }
    guard let index = drafts.firstIndex(where: { $0.id == id }) else { return }
    if parameter == drafts[index].module.bypassParameter {
      if value >= 0.5 { try activate(id, power: value) }
      return
    }
    drafts[index].lifecycle = .editedDraft
  }

  func descriptions(_ chain: [PedalInstance], power: (id: String, value: Float)? = nil)
    throws -> [[String: Any]]
  {
    try chain.map { instance in
      var description = try instance.module.audioDescription()
      description["slot"] = instance.id
      if instance.hostBypassed { description["bypassed"] = true }
      if let power, power.id == instance.id,
        var parameters = description["parameters"]
          as? [[String: Any]]
      {
        for index in parameters.indices
        where parameters[index]["id"] as? UInt32 == instance.module.bypassParameter {
          parameters[index]["value"] = power.value
        }
        description["parameters"] = parameters
      }
      return description
    }
  }

  /// Audio descriptions of the whole chain in processing order, including bypassed instances and
  /// instances whose editors are not shown. Drafts are never included.
  func audioDescriptions() throws -> [[String: Any]] { try descriptions(chain) }

  /// Installed modules grouped by plugin family, for adding to the chain.
  static func libraryOptions(_ choices: [PedalModuleChoice]) -> [SettingsOption] {
    choices.map { SettingsOption(id: $0.id, title: $0.name, group: $0.pluginFamily) }
  }

  var libraryOptions: [SettingsOption] { Self.libraryOptions(choices) }

  /// Options for an editor position: the board's existing instances, to show the same instance
  /// again; every installed module by plugin family, each creating a new draft; and "No editor".
  var editorOptions: [SettingsOption] {
    (chain + drafts).map {
      SettingsOption(
        id: $0.id, title: $0.title, group: Self.currentGroup)
    }
      + choices.map {
        SettingsOption(id: Self.libraryPrefix + $0.id, title: $0.name, group: $0.pluginFamily)
      }
      + [SettingsOption(id: "", title: "No editor")]
  }

  /// Commits a chain change once the audio side accepts it; otherwise nothing changes.
  @discardableResult
  private func commit(_ next: [PedalInstance], power: (id: String, value: Float)? = nil) -> Bool {
    do {
      try onChange?(descriptions(next, power: power))
      chain = next
      error = nil
      return true
    } catch {
      self.error = error.localizedDescription
      return false
    }
  }

  /// Every instance on the board takes part, so that titles stay distinct among drafts too.
  private func ordinal(for moduleID: String) -> Int {
    let used = Set((chain + drafts).filter { $0.module.id == moduleID }.map(\.ordinal))
    return (1...).first { !used.contains($0) }!
  }

  private func makeInstance(_ choice: PedalModuleChoice, lifecycle: PedalInstance.Lifecycle)
    throws -> PedalInstance
  {
    PedalInstance(
      id: "\(position.rawValue).\(UUID().uuidString)", module: try choice.instantiate(),
      ordinal: ordinal(for: choice.id), lifecycle: lifecycle)
  }

  /// Appends a new instance of an installed module to the chain and shows its editor.
  func add(_ choiceID: String) {
    guard let choice = choices.first(where: { $0.id == choiceID }) else { return }
    do {
      let instance = try makeInstance(choice, lifecycle: .chained)
      connect(instance)
      if commit(chain + [instance]) { showEditor(instance.id) }
    } catch { self.error = error.localizedDescription }
  }

  /// Shows a new instance of an installed module at a board unit, switched off and outside the
  /// processing chain until it is switched on.
  func addDraft(_ choiceID: String, in unit: Int) {
    guard let choice = choices.first(where: { $0.id == choiceID }) else { return }
    do {
      var instance = try makeInstance(choice, lifecycle: .untouchedDraft)
      if let bypass = instance.module.bypassParameter {
        try instance.module.setValue(bypass, 0)
      } else {
        instance.hostBypassed = true
      }
      connect(instance)
      drafts.append(instance)
      error = nil
      setEditors(editors.showing(instance.id, width: instance.module.boardWidth, at: unit))
    } catch { self.error = error.localizedDescription }
  }

  /// Appends a draft to the end of the chain, switched on. `power` is the value its bypass
  /// parameter is about to take; the audio side receives it with the rest of the chain.
  private func activate(_ id: String, power: Float? = nil) throws {
    guard let index = drafts.firstIndex(where: { $0.id == id }) else { return }
    var instance = drafts[index]
    instance.lifecycle = .chained
    instance.hostBypassed = false
    guard commit(chain + [instance], power: power.map { (id, $0) }) else {
      throw AmpError.message(error ?? "Cannot add \(instance.title) to the signal chain")
    }
    drafts.remove(at: index)
  }

  /// Inserts a copy of a chain instance, with its settings and bypass state, right after it.
  func duplicate(_ id: String) {
    guard let index = chain.firstIndex(where: { $0.id == id }),
      let choice = choices.first(where: { $0.id == chain[index].module.id })
    else { return }
    do {
      let source = chain[index]
      var copy = try makeInstance(choice, lifecycle: .chained)
      do {
        try copy.module.restoreState(try source.module.saveState())
      } catch {
        for parameter in source.module.parameters {
          try copy.module.setValue(parameter.id, try source.module.value(parameter.id))
        }
      }
      copy.hostBypassed = source.hostBypassed
      connect(copy)
      var next = chain
      next.insert(copy, at: index + 1)
      if commit(next) { showEditor(copy.id) }
    } catch { self.error = error.localizedDescription }
  }

  /// Removes an instance. When a chain instance's editor was shown, the nearest hidden neighbour
  /// in the chain that fits the free units takes its place; a removed draft leaves its units
  /// free.
  func remove(_ id: String) {
    if let index = drafts.firstIndex(where: { $0.id == id }) {
      drafts.remove(at: index)
      setEditors(editors.clearing(id))
      return
    }
    guard let index = chain.firstIndex(where: { $0.id == id }) else { return }
    var next = chain
    next.remove(at: index)
    guard commit(next) else { return }
    setEditors(Self.editors(editors, removing: id, at: index, chain: next))
  }

  static func editors(
    _ layout: PedalEditorLayout, removing id: String, at index: Int, chain: [PedalInstance]
  ) -> PedalEditorLayout {
    guard let span = layout.spans.firstIndex(where: { $0.id == id }) else { return layout }
    let unit = layout.units[span]
    let cleared = layout.clearing(id)
    let visible = Set(cleared.visible)
    let survivor = chain.indices
      .sorted { (abs($0 - index), $0) < (abs($1 - index), $1) }
      .map { chain[$0] }
      .first { !visible.contains($0.id) && $0.module.boardWidth <= cleared.freeUnits }
    guard let survivor else { return cleared }
    return cleared.showing(survivor.id, width: survivor.module.boardWidth, at: unit)
  }

  /// Moves a chain instance to a chain position; its identity and settings are unchanged.
  func move(_ id: String, to destination: Int) {
    guard let index = chain.firstIndex(where: { $0.id == id }),
      chain.indices.contains(destination), destination != index
    else { return }
    var next = chain
    next.insert(next.remove(at: index), at: destination)
    commit(next)
  }

  func move(_ id: String, by distance: Int) {
    guard let index = chain.firstIndex(where: { $0.id == id }) else { return }
    move(id, to: index + distance)
  }

  /// Switches an instance off or on. A chain instance keeps its position either way; switching a
  /// draft on appends it to the chain.
  func setBypassed(_ id: String, _ bypassed: Bool) {
    guard let instance = instance(id) else { return }
    let module = instance.module
    if let bypass = module.bypassParameter {
      do {
        try module.setValue(bypass, bypassed ? 0 : 1)
        error = nil
        objectWillChange.send()
      } catch { self.error = error.localizedDescription }
    } else if instance.isDraft {
      guard !bypassed else { return }
      try? activate(id)
    } else if let index = chain.firstIndex(where: { $0.id == id }) {
      var next = chain
      next[index].hostBypassed = bypassed
      commit(next)
    }
  }

  /// Shows an instance's editor, in the span at board `unit` when given, following
  /// `PedalEditorLayout.showing`. Changes the view only.
  func showEditor(_ id: String, in unit: Int? = nil) {
    guard let instance = instance(id) else { return }
    setEditors(editors.showing(id, width: instance.module.boardWidth, at: unit))
  }

  /// Chooses the editor shown in the span at a board unit: an existing instance, a new draft of
  /// an installed module (an ID with `libraryPrefix`), or none for an empty ID. Never changes the
  /// processing chain.
  func chooseEditor(_ id: String, in unit: Int) {
    if id.hasPrefix(Self.libraryPrefix) {
      addDraft(String(id.dropFirst(Self.libraryPrefix.count)), in: unit)
    } else if !id.isEmpty {
      showEditor(id, in: unit)
    } else {
      setEditors(editors.clearing(at: unit))
    }
  }

  /// Shows `layout`, discarding untouched drafts that it no longer shows.
  private func setEditors(_ layout: PedalEditorLayout) {
    editors = layout
    let visible = Set(layout.visible)
    drafts.removeAll { $0.lifecycle == .untouchedDraft && !visible.contains($0.id) }
  }

  /// A board built from a preset but not yet shown or processing.
  struct StagedBoard {
    let chain: [PedalInstance]
    let drafts: [PedalInstance]
    let editors: PedalEditorLayout
  }

  func snapshot() throws -> BoardSnapshot {
    func snapshot(_ instance: PedalInstance) throws -> PedalInstanceSnapshot {
      let lifecycle: PedalInstanceSnapshot.Lifecycle =
        switch instance.lifecycle {
        case .untouchedDraft: .untouchedDraft
        case .editedDraft: .editedDraft
        case .chained: .chained
        }
      return PedalInstanceSnapshot(
        id: instance.id, ordinal: instance.ordinal, lifecycle: lifecycle,
        hostBypassed: instance.hostBypassed, module: try instance.module.snapshot())
    }
    let layout = EditorLayoutSnapshot(kind: .pedals, slots: editors.spans.map(\.id))
    return BoardSnapshot(
      chain: try chain.map(snapshot), drafts: try drafts.map(snapshot), editors: layout)
  }

  /// Creates new instances for `board` with their saved identities and settings, without
  /// changing this board. Fails when the board is inconsistent or uses a module that is not
  /// installed; no instance is dropped or substituted. Editors that no longer fit, because a
  /// module now declares a wider board width, are hidden as `PedalEditorLayout.init` describes.
  func stage(_ board: BoardSnapshot) throws -> StagedBoard {
    var ids = Set<String>()
    var ordinals = Set<String>()
    func stage(_ saved: PedalInstanceSnapshot, chained: Bool) throws -> PedalInstance {
      guard saved.id.hasPrefix(position.rawValue + "."), saved.id.count <= 128,
        ids.insert(saved.id).inserted
      else { throw AmpError.message("The preset has an invalid \(position.title) effect identity") }
      guard saved.ordinal >= 1, saved.ordinal <= 1000,
        ordinals.insert("\(saved.module.id)#\(saved.ordinal)").inserted
      else { throw AmpError.message("The preset numbers \(position.title) effects inconsistently") }
      guard (saved.lifecycle == .chained) == chained else {
        throw AmpError.message("The preset mixes \(position.title) drafts into the chain")
      }
      guard let choice = choices.first(where: { $0.id == saved.module.id }) else {
        throw AmpError.message(
          "The preset uses \(position.title) module \(saved.module.id), which is not installed")
      }
      let module = try choice.instantiate()
      try module.apply(try module.stage(saved.module))
      let lifecycle: PedalInstance.Lifecycle =
        switch saved.lifecycle {
        case .untouchedDraft: .untouchedDraft
        case .editedDraft: .editedDraft
        case .chained: .chained
        }
      return PedalInstance(
        id: saved.id, module: module, ordinal: saved.ordinal, lifecycle: lifecycle,
        hostBypassed: saved.hostBypassed)
    }
    let chain = try board.chain.map { try stage($0, chained: true) }
    let drafts = try board.drafts.map { try stage($0, chained: false) }
    let instances = Dictionary(uniqueKeysWithValues: (chain + drafts).map { ($0.id, $0) })
    let slots = board.editors.slots
    let shown = slots.compactMap { $0 }
    guard slots.count <= PedalEditorLayout.capacity, Set(shown).count == shown.count,
      shown.allSatisfy({ instances[$0] != nil }),
      board.editors.kind == .pedals || (slots.count == 1 && slots[0] != nil)
    else { throw AmpError.message("The preset's \(position.title) editor layout is invalid") }
    let editors = PedalEditorLayout(
      slots.map { id in id.map { .editor($0, width: instances[$0]!.module.boardWidth) } ?? .free })
    return StagedBoard(chain: chain, drafts: drafts, editors: editors)
  }

  /// Shows and processes a staged board. The caller has already submitted its audio
  /// descriptions, so `onChange` is not called.
  func commit(_ board: StagedBoard) {
    for instance in board.chain + board.drafts { connect(instance) }
    chain = board.chain
    drafts = board.drafts
    editors = board.editors
    error = nil
  }
}
