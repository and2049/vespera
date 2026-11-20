import CoreMIDI
import Foundation
import LayoutRenderer
import ModuleSDK
import SwiftUI

struct ModuleParameter: Identifiable {
  let id: UInt32
  let name: String
  let unit: String
  let minimum: Float
  let maximum: Float
  /// The module's description of how the parameter's values are shown and which it takes.
  let display: VesperaParameterDisplay

  /// The fraction of the range one step covers: one value of a toggle, stepped or enumerated
  /// parameter, otherwise one 7-bit MIDI increment.
  var positionStep: Float { display.steps >= 2 ? 1 / Float(display.steps - 1) : 1 / 127 }

  /// The value at a position in the range, moved to the nearest value the parameter takes.
  func value(atPosition position: Float) -> Float {
    display.nearestValue(minimum + min(1, max(0, position)) * (maximum - minimum))
  }

  func text(atPosition position: Float) -> String {
    display.text(forValue: value(atPosition: position))
  }
}

struct MIDIMapping: Identifiable, Codable, Equatable {
  enum Kind: String, Codable, CaseIterable {
    case absolute = "CC Absolute"
    case relative = "CC Relative"
    case toggle = "CC Toggle"
    case gate = "CC Gate"
    case value = "CC Value"
    case pair = "CC Dec/Inc"
    case noteToggle = "Note Toggle"
    case noteGate = "Note Gate"
    case noteValue = "Note Value"
    case notePair = "Note Dec/Inc"
    case programValue = "PC Value"

    var usesPair: Bool { self == .pair || self == .notePair }
    var usesValue: Bool { self == .value || self == .noteValue || self == .programValue }
  }

  var id = UUID()
  var enabled = true
  var kind = Kind.absolute
  var target = ""
  var channel = 0
  var number = 0
  var secondNumber = 1
  var value = 1.0

  /// The new position for a message, or nil when the message does not apply. `step` is the
  /// position change of one relative or decrement/increment message.
  func apply(status: UInt8, number: UInt8, data: UInt8, current: Float, step: Float = 1 / 127)
    -> Float?
  {
    guard enabled, channel == 0 || channel == Int(status & 15) + 1 else { return nil }
    let message = status & 0xf0
    let note = kind == .noteToggle || kind == .noteGate || kind == .noteValue || kind == .notePair
    guard
      note
        ? (message == 0x90 || message == 0x80) : message == (kind == .programValue ? 0xc0 : 0xb0),
      Int(number) == self.number || (kind.usesPair && Int(number) == secondNumber)
    else { return nil }
    let pressed = message != 0x80 && data > 0
    switch kind {
    case .absolute: return Float(data) / 127
    case .relative:
      let delta = data < 64 ? Int(data) : Int(data) - 128
      return min(1, max(0, current + Float(delta) * step))
    case .toggle, .noteToggle: return pressed ? (current >= 0.5 ? 0 : 1) : nil
    case .gate, .noteGate: return pressed ? 1 : 0
    case .value, .noteValue: return pressed ? Float(value) : nil
    case .programValue: return Float(value)
    case .pair, .notePair:
      return pressed
        ? min(1, max(0, current + (Int(number) == self.number ? -step : step))) : nil
    }
  }

  /// `mappings` with each target of a Pre or Post FX instance (`<instance>/<module>/<parameter>`)
  /// whose module is installed as a Special FX module retargeted to that module, and how many
  /// changed. Parameter IDs are kept: the module binary is the same.
  static func relocated(_ mappings: [MIDIMapping], special: Set<String>) -> (
    mappings: [MIDIMapping], moved: Int
  ) {
    var moved = 0
    let result = mappings.map { mapping in
      guard let key = MIDITargetKey(storage: mapping.target),
        key.slot.hasPrefix("pre-fx.") || key.slot.hasPrefix("post-fx."),
        special.contains(key.moduleID)
      else { return mapping }
      var relocated = mapping
      relocated.target =
        MIDITargetKey(slot: "specialFX", moduleID: key.moduleID, parameterID: key.parameterID)
        .storage
      moved += 1
      return relocated
    }
    return (result, moved)
  }
}

final class MIDIMappingInput: @unchecked Sendable {
  struct Message {
    let status: UInt8
    let number: UInt8
    let value: UInt8
  }
  private let lock = NSLock()
  private var messages: [Message] = []
  private var overflowed = false
  private var status: UInt8 = 0
  private var bytes: [UInt8] = []
  private var client = MIDIClientRef()
  private var port = MIDIPortRef()
  private var source = MIDIEndpointRef()

  deinit {
    if port != 0 { MIDIPortDispose(port) }
    if client != 0 { MIDIClientDispose(client) }
  }

  func connect(_ identifier: Int32) throws {
    disconnect()
    guard identifier != 0 else { return }
    var object = MIDIObjectRef()
    var type = MIDIObjectType.other
    guard MIDIObjectFindByUniqueID(identifier, &object, &type) == noErr, type == .source else {
      throw AmpError.message("MIDI source is unavailable.")
    }
    if client == 0 {
      guard MIDIClientCreateWithBlock("vespera mappings" as CFString, &client, nil) == noErr else {
        throw AmpError.message("Could not create MIDI mapping client.")
      }
    }
    if port == 0 {
      guard
        MIDIInputPortCreateWithBlock(
          client, "Mappings" as CFString, &port,
          { [weak self] packets, context in
            self?.receive(packets, source: MIDIEndpointRef(UInt(bitPattern: context)))
          }) == noErr
      else { throw AmpError.message("Could not create MIDI mapping input.") }
    }
    lock.lock()
    source = object
    lock.unlock()
    if MIDIPortConnectSource(port, object, UnsafeMutableRawPointer(bitPattern: UInt(object)))
      != noErr
    {
      disconnect()
      throw AmpError.message("Could not connect MIDI mappings.")
    }
  }

  func disconnect() {
    lock.lock()
    let previous = source
    source = 0
    messages.removeAll(keepingCapacity: true)
    overflowed = false
    bytes.removeAll(keepingCapacity: true)
    status = 0
    lock.unlock()
    if previous != 0 { MIDIPortDisconnectSource(port, previous) }
  }

  func drain() throws -> [Message] {
    lock.lock()
    defer { lock.unlock() }
    if overflowed {
      messages.removeAll(keepingCapacity: true)
      bytes.removeAll(keepingCapacity: true)
      status = 0
      overflowed = false
      throw AmpError.message("MIDI mapping input overflowed; pending mappings were discarded.")
    }
    let result = messages
    messages.removeAll(keepingCapacity: true)
    return result
  }

  private func receive(_ packets: UnsafePointer<MIDIPacketList>, source: MIDIEndpointRef) {
    lock.lock()
    defer { lock.unlock() }
    guard source != 0, source == self.source else { return }
    do {
      var packet = UnsafeRawPointer(packets).advanced(
        by: MemoryLayout<MIDIPacketList>.offset(of: \.packet)!
      ).assumingMemoryBound(to: MIDIPacket.self)
      for _ in 0..<packets.pointee.numPackets {
        do {
          let data = UnsafeRawPointer(packet).advanced(
            by: MemoryLayout<MIDIPacket>.offset(of: \.data)!
          ).assumingMemoryBound(to: UInt8.self)
          for byte in UnsafeBufferPointer(start: data, count: Int(packet.pointee.length)) {
            if byte >= 0xf8 { continue }
            if byte & 0x80 != 0 {
              status = byte < 0xf0 ? byte : 0
              bytes.removeAll(keepingCapacity: true)
            } else if status != 0 {
              bytes.append(byte)
              let count = status & 0xe0 == 0xc0 ? 1 : 2
              if bytes.count == count {
                if messages.count < 256 && !overflowed {
                  messages.append(
                    Message(status: status, number: bytes[0], value: count == 2 ? bytes[1] : 0))
                } else {
                  overflowed = true
                }
                bytes.removeAll(keepingCapacity: true)
              }
            }
          }
        }
        packet = UnsafePointer(MIDIPacketNext(packet))
      }
    }
  }
}

/// A mapping target's identity: the bridge slot, the module in it and the parameter. Mappings
/// persist `storage` — "slot/module/parameter" — and that string form is load-bearing for saves
/// written before this type existed.
struct MIDITargetKey: Equatable {
  let slot: String
  let moduleID: String
  let parameterID: UInt32

  init(slot: String, moduleID: String, parameterID: UInt32) {
    self.slot = slot
    self.moduleID = moduleID
    self.parameterID = parameterID
  }

  init?(storage: String) {
    let parts = storage.split(separator: "/", omittingEmptySubsequences: false)
    guard parts.count == 3, let parameterID = UInt32(parts[2]) else { return nil }
    self.init(slot: String(parts[0]), moduleID: String(parts[1]), parameterID: parameterID)
  }

  var storage: String { "\(slot)/\(moduleID)/\(parameterID)" }
}

struct MIDITarget: Identifiable {
  let id: String
  let title: String
  let module: LoadedModule
  let parameter: ModuleParameter
}

struct MIDIControls: View {
  @ObservedObject var audio: AudioController
  var dismiss: () -> Void = {}
  @State private var expanded: String?
  @State private var page = Page.mappings

  private enum Page: String {
    case input = "Input"
    case mappings = "Mappings"

    var title: String { self == .mappings ? "MIDI Mappings" : "MIDI Input" }
    var width: CGFloat { self == .input ? 340 : 940 }
    var option: SettingsOption { SettingsOption(id: rawValue, title: rawValue) }
  }

  private let metrics = UISize.compact.metrics

  var body: some View {
    SettingsPanel(title: page.title) {
      SettingsSegmentedControl(
        title: "MIDI settings page",
        options: [Page.input, .mappings].map(\.option),
        selection: Binding(
          get: { page.rawValue },
          set: {
            page = Page(rawValue: $0) ?? .mappings
            expanded = nil
          })
      ).fixedSize()
      SettingsIconButton(systemName: "xmark", label: "Close MIDI settings", action: dismiss)
    } content: {
      if page == .input {
        SettingsField(label: "Source") {
          SettingsDropdown(
            title: "MIDI source",
            options: [
              SettingsOption(
                id: "0",
                title: audio.midiSources.isEmpty ? "No MIDI sources available" : "No input")
            ]
              + audio.midiSources.map { SettingsOption(id: String($0.id), title: $0.name) },
            selection: Binding(
              get: { String(audio.midiSource) },
              set: { audio.updateMIDI(source: Int32($0) ?? 0, channel: audio.midiChannel) }),
            expanded: $expanded
          )
          .disabled(audio.midiSources.isEmpty)
        }
        SettingsField(label: "Channel") {
          SettingsDropdown(
            title: "MIDI channel", options: channels,
            selection: Binding(
              get: { String(audio.midiChannel) },
              set: { audio.updateMIDI(source: audio.midiSource, channel: UInt32($0) ?? 0) }),
            expanded: $expanded)
        }
        HStack {
          Button("Refresh", action: audio.refreshMIDI)
          Spacer()
          Button("Panic", action: audio.panicMIDI)
        }.buttonStyle(SettingsButtonStyle())
      } else {
        VStack(alignment: .leading, spacing: Theme.menuInset) {
          HStack(spacing: metrics.label_gap) {
            Color.clear.frame(width: Column.enabled, height: 1)
            SettingsLabel("Type").frame(width: Column.type, alignment: .leading)
            SettingsLabel("Parameter").frame(maxWidth: .infinity, alignment: .leading)
            SettingsLabel("Channel").frame(width: Column.channel, alignment: .leading)
            SettingsLabel("Note/CC/PC").frame(width: Column.number, alignment: .leading)
            SettingsLabel("Note/CC/PC").frame(width: Column.number, alignment: .leading)
            SettingsLabel("Value %").frame(width: Column.value, alignment: .leading)
            Color.clear.frame(width: Column.delete, height: 1)
          }
          ScrollView {
            VStack(spacing: Theme.menuInset) {
              ForEach($audio.midiMappings) { $mapping in
                row($mapping)
              }
              HStack(spacing: metrics.label_gap) {
                Color.clear.frame(width: Column.enabled, height: 1)
                Button {
                  var mapping = MIDIMapping()
                  mapping.target = audio.midiTargets.first?.id ?? ""
                  audio.midiMappings.append(mapping)
                } label: {
                  Text("+ New Mapping").frame(maxWidth: .infinity)
                }.buttonStyle(SettingsButtonStyle()).frame(width: Column.type)
                SettingsEmptyCell()
                SettingsEmptyCell().frame(width: Column.channel)
                SettingsEmptyCell().frame(width: Column.number)
                SettingsEmptyCell().frame(width: Column.number)
                SettingsEmptyCell().frame(width: Column.value)
                Color.clear.frame(width: Column.delete, height: 1)
              }
            }
          }.frame(height: 240)
        }
      }
    }
    .uiSize(.compact)
    .frame(width: page.width)
    .settingsDropdownOverlay()
    .onAppear(perform: audio.refreshMIDI)
  }

  private enum Column {
    static let enabled: CGFloat = 28
    static let type: CGFloat = 142
    static let channel: CGFloat = 82
    static let number: CGFloat = 86
    static let value: CGFloat = 64
    static let delete: CGFloat = 28
  }

  private var channels: [SettingsOption] {
    [SettingsOption(id: "0", title: "All channels")]
      + (1...16).map { SettingsOption(id: String($0), title: "Ch \($0)") }
  }

  private func row(_ mapping: Binding<MIDIMapping>) -> some View {
    HStack(spacing: metrics.label_gap) {
      Toggle("Mapping enabled", isOn: mapping.enabled)
        .toggleStyle(SettingsCheckboxStyle(labelHidden: true))
        .accessibilityLabel("Mapping enabled")
        .frame(width: Column.enabled)
      SettingsDropdown(
        title: "Type \(mapping.wrappedValue.id)",
        options: MIDIMapping.Kind.allCases.map {
          SettingsOption(id: $0.rawValue, title: $0.rawValue)
        },
        selection: Binding(
          get: { mapping.wrappedValue.kind.rawValue },
          set: { mapping.wrappedValue.kind = MIDIMapping.Kind(rawValue: $0) ?? .absolute }),
        expanded: $expanded
      ).frame(width: Column.type)
      SettingsDropdown(
        title: "Parameter \(mapping.wrappedValue.id)",
        options: targetOptions(mapping.wrappedValue), selection: mapping.target,
        expanded: $expanded)
      SettingsDropdown(
        title: "Channel \(mapping.wrappedValue.id)",
        options: channels.map { SettingsOption(id: $0.id, title: $0.id == "0" ? "All" : $0.id) },
        selection: Binding(
          get: { String(mapping.wrappedValue.channel) },
          set: { mapping.wrappedValue.channel = Int($0) ?? 0 }), expanded: $expanded
      ).frame(width: Column.channel)
      SettingsNumberField(label: "Note, CC or PC number", value: mapping.number, range: 0...127)
        .frame(width: Column.number)
      Group {
        if mapping.wrappedValue.kind.usesPair {
          SettingsNumberField(
            label: "Increment number", value: mapping.secondNumber, range: 0...127)
        } else {
          SettingsEmptyCell()
        }
      }.frame(width: Column.number)
      Group {
        if mapping.wrappedValue.kind.usesValue {
          SettingsDecimalField(
            label: "Value",
            value: Binding(
              get: { mapping.wrappedValue.value * 100 },
              set: { mapping.wrappedValue.value = $0 / 100 }), range: 0...100
          )
          .help(valueHelp(mapping.wrappedValue))
        } else {
          SettingsEmptyCell()
        }
      }.frame(width: Column.value)
      SettingsIconButton(systemName: "xmark", label: "Delete mapping") {
        audio.midiMappings.removeAll { $0.id == mapping.wrappedValue.id }
      }.frame(width: Column.delete)
    }
  }

  /// What a value mapping sets its parameter to, in the parameter's own units.
  private func valueHelp(_ mapping: MIDIMapping) -> String {
    guard let target = audio.midiTargets.first(where: { $0.id == mapping.target }),
      mapping.value.isFinite
    else { return "" }
    return
      "Sets \(target.parameter.name) to \(target.parameter.text(atPosition: Float(mapping.value)))"
  }

  private func targetOptions(_ mapping: MIDIMapping) -> [SettingsOption] {
    let options = audio.midiTargets.map { SettingsOption(id: $0.id, title: $0.title) }
    return options.contains(where: { $0.id == mapping.target })
      ? options : [SettingsOption(id: mapping.target, title: "Unavailable parameter")] + options
  }
}
