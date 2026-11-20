import Foundation

/// Moves Pre and Post FX instances of modules now installed as Special FX into a preset's Special
/// FX section. Driven by content: an instance whose module ID is installed as a Special FX module
/// has moved, whatever the preset's schema or vendor.
enum SpecialFXRelocation {
  /// An installed Special FX module as relocation needs it.
  struct Module {
    let id: String
    let name: String
    let placement: SpecialFXPlacement
  }

  struct Report: Equatable {
    var notices: [String] = []
    /// The relocated rig cannot reproduce the saved one: an instance was not kept, or a module
    /// that was in a chain now processes elsewhere.
    var lossy = false
  }

  private struct Candidate {
    let preFX: Bool
    let instance: PedalInstanceSnapshot
    /// Its index in the board's chain, nil for a draft.
    let position: Int?
    let chainCount: Int

    var board: String { preFX ? "Pre FX" : "Post FX" }

    func title(_ name: String) -> String {
      instance.ordinal > 1 ? "\(name) \(instance.ordinal)" : name
    }
  }

  static func relocate(_ rig: RigSnapshot, special: [Module]) -> (rig: RigSnapshot, report: Report)
  {
    var rig = rig
    var report = Report()
    var active = Set(rig.specialFX.activeModules)
    var removed = Set<String>()
    for module in special {
      var chained: [Candidate] = []
      var drafts: [Candidate] = []
      for preFX in [true, false] {
        let board = preFX ? rig.preFX : rig.postFX
        for (index, instance) in board.chain.enumerated() where instance.module.id == module.id {
          chained.append(
            Candidate(
              preFX: preFX, instance: instance, position: index, chainCount: board.chain.count))
        }
      }
      for preFX in [true, false] {
        for instance in (preFX ? rig.preFX : rig.postFX).drafts
        where instance.module.id == module.id {
          removed.insert(instance.id)
          if instance.lifecycle == .editedDraft {
            drafts.append(Candidate(preFX: preFX, instance: instance, position: nil, chainCount: 0))
          }
        }
      }
      removed.formUnion(chained.map(\.instance.id))
      let candidates = chained + drafts
      guard let winner = candidates.first else { continue }
      report.notices.append("\(module.name) moved from \(winner.board) to Special FX")
      var lost = Array(candidates.dropFirst())
      let existing = rig.specialFX.modules.firstIndex { $0.id == module.id }
      if winner.position != nil {
        if let existing {
          rig.specialFX.modules[existing] = winner.instance.module
          report.notices.append(
            "the saved Special FX settings of \(module.name) were replaced by \(winner.title(module.name)) "
              + "from \(winner.board); Special FX holds one \(module.name)")
          report.lossy = true
        } else {
          rig.specialFX.modules.append(winner.instance.module)
        }
        if winner.instance.hostBypassed {
          active.remove(module.id)
        } else {
          active.insert(module.id)
        }
        if let moved = positionChange(winner, module) {
          report.notices.append(moved)
          report.lossy = true
        }
      } else if existing == nil {
        rig.specialFX.modules.append(winner.instance.module)
      } else {
        lost.insert(winner, at: 0)
      }
      for candidate in lost {
        report.notices.append(
          "\(candidate.title(module.name)) in \(candidate.board) was not kept; Special FX holds one "
            + module.name)
        report.lossy = true
      }
    }
    guard !removed.isEmpty else { return (rig, report) }
    for board in [\RigSnapshot.preFX, \RigSnapshot.postFX] {
      rig[keyPath: board].chain.removeAll { removed.contains($0.id) }
      rig[keyPath: board].drafts.removeAll { removed.contains($0.id) }
      var editors = rig[keyPath: board].editors
      editors.slots = editors.slots.map { $0.flatMap { removed.contains($0) ? nil : $0 } }
      if editors.kind == .rack && editors.slots.allSatisfy({ $0 == nil }) {
        editors = EditorLayoutSnapshot(kind: .pedals, slots: [])
      }
      rig[keyPath: board].editors = editors
    }
    rig.specialFX.active = active.sorted()
    return (rig, report)
  }

  /// A notice when the chained `candidate` ran elsewhere than `module` now runs.
  private static func positionChange(_ candidate: Candidate, _ module: Module) -> String? {
    guard let position = candidate.position else { return nil }
    let first = position == 0
    let last = position == candidate.chainCount - 1
    let unchanged =
      switch module.placement {
      case .beforePreFX: candidate.preFX && first
      case .beforeAmp: candidate.preFX && last
      case .afterEQ: !candidate.preFX && first
      case .afterPostFX: !candidate.preFX && last
      case .beforeCabinet, .afterCabinet: false
      }
    guard !unchanged else { return nil }
    return
      "\(candidate.title(module.name)) ran at position \(position + 1) of \(candidate.chainCount) "
      + "in the \(candidate.board) chain and now runs \(module.placement.phrase)"
  }
}
