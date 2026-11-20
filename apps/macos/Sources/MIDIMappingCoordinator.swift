import Foundation

/// Runs MIDI mappings against the installed modules: the parameters they expose as targets,
/// what each arriving message does to them, and the gates held while a pedal or note is down.
/// Failures are reported through the owner's status line.
@MainActor
final class MIDIMappingCoordinator {
  private let amps: AmpCatalog
  private let cabinets: CabinetCatalog
  private let preFX: PedalCatalog
  private let postFX: PedalCatalog
  private let specialFX: SpecialFXCatalog
  private let report: (Error) -> Void
  /// Targets a mapping keeps pressed — their parameter held above its minimum — while the pedal
  /// or note that engaged them stays down.
  private var heldGates: [UUID: MIDITarget] = [:]

  init(
    amps: AmpCatalog, cabinets: CabinetCatalog, preFX: PedalCatalog, postFX: PedalCatalog,
    specialFX: SpecialFXCatalog, report: @escaping (Error) -> Void
  ) {
    self.amps = amps
    self.cabinets = cabinets
    self.preFX = preFX
    self.postFX = postFX
    self.specialFX = specialFX
    self.report = report
  }

  var targets: [MIDITarget] {
    var modules: [(slot: String, title: String, name: String, module: LoadedModule)] = []
    if let module = amps.activeAmp { modules.append(("amp", "Amp", module.name, module)) }
    if let module = cabinets.activeCabinet {
      modules.append(("cabinet", "Cabinet", module.name, module))
    }
    // Every installed Special FX module is a target, processing or not, so a mapping can switch
    // one into use through its own controls.
    for module in specialFX.modules {
      modules.append(("specialFX", "Special FX", module.name, module))
    }
    if let module = amps.activeAmp?.equalizer { modules.append(("eq", "EQ", module.name, module)) }
    // Chain instances are addressed by their persistent identity, so reordering or showing
    // other editors never retargets a mapping.
    for catalog in [preFX, postFX] {
      for instance in catalog.chain {
        modules.append((instance.id, catalog.position.title, instance.title, instance.module))
      }
    }
    return modules.flatMap { slot, title, name, module in
      module.parameters.map { parameter in
        let key = MIDITargetKey(slot: slot, moduleID: module.id, parameterID: parameter.id)
        return MIDITarget(
          id: key.storage, title: "\(title) · \(name) · \(parameter.name)", module: module,
          parameter: parameter)
      }
    }
  }

  /// Applies `messages` to `mappings` on MIDI `channel` (all channels for 0), releasing any
  /// held gate whose target is no longer installed where the mapping points.
  func apply(
    _ messages: [MIDIMappingInput.Message], mappings: [MIDIMapping], channel: UInt32
  ) {
    let targets = Dictionary(uniqueKeysWithValues: self.targets.map { ($0.id, $0) })
    for (id, held) in heldGates where targets[held.id]?.module !== held.module {
      releaseGate(id)
    }
    guard !messages.isEmpty else { return }
    for message in messages where channel == 0 || UInt32(message.status & 15) + 1 == channel {
      for mapping in mappings {
        guard let target = targets[mapping.target], mapping.value.isFinite,
          (0...1).contains(mapping.value), (0...16).contains(mapping.channel),
          (0...127).contains(mapping.number), (0...127).contains(mapping.secondNumber)
        else { continue }
        let parameter = target.parameter
        let span = parameter.maximum - parameter.minimum
        guard span > 0 else { continue }
        do {
          let current = (try target.module.value(parameter.id) - parameter.minimum) / span
          if let value = mapping.apply(
            status: message.status, number: message.number, data: message.value,
            current: current, step: parameter.positionStep)
          {
            try target.module.setValue(parameter.id, parameter.value(atPosition: value))
            if mapping.kind == .gate || mapping.kind == .noteGate {
              heldGates[mapping.id] = value > 0 ? target : nil
            }
          }
        } catch { report(error) }
      }
    }
  }

  func releaseGates() {
    for id in Array(heldGates.keys) { releaseGate(id) }
  }

  /// Returns a gate's parameter to its minimum, ending a held pedal or note.
  func releaseGate(_ id: UUID) {
    if let target = heldGates.removeValue(forKey: id) {
      do { try target.module.setValue(target.parameter.id, target.parameter.minimum) } catch {
        report(error)
      }
    }
  }

  /// Drops every held gate without touching the parameters, for a rig load that replaced the
  /// modules they pointed at.
  func clearGates() { heldGates.removeAll() }
}
