import Darwin
import Foundation
import ModuleSDK

/// Every request the audio bridge accepts, named by the `op` field its JSON carries.
enum BridgeOp: String {
  case devices
  case status
  case start
  case stop
  case parameter
  case rig
  case preset
  case preFX
  case postFX
  case specialFX
  case midi
  case midiDevices
  case midiPanic
  case tap
  case tempo
  case globals
  case cancelCandidate
  case renderGraph
  case record
  case reamp
}

final class AudioBridge {
  private let library: UnsafeMutableRawPointer
  private let api: UnsafePointer<vs_app_audio_api>
  private let instance: UnsafeMutableRawPointer

  init(path: URL? = nil) throws {
    let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
      .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
    let bundled = Bundle.main.executableURL?.deletingLastPathComponent()
      .appendingPathComponent("libvespera_app_audio.dylib")
    let url =
      path ?? bundled.flatMap { FileManager.default.fileExists(atPath: $0.path) ? $0 : nil }
      ?? root.appendingPathComponent("build/debug/libvespera_app_audio.dylib")
    guard let library = dlopen(url.path, RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE) else {
      throw AmpError.message(
        "Build the audio backend with cmake --build --preset debug. \(String(cString: dlerror()))")
    }
    guard let symbol = dlsym(library, "vespera_app_audio_entry") else {
      dlclose(library)
      throw AmpError.message("Audio backend entry point is missing.")
    }
    let entry = unsafeBitCast(symbol, to: vs_app_audio_entry_fn.self)
    guard let api = entry(1), api.pointee.struct_size >= MemoryLayout<vs_app_audio_api>.size,
      api.pointee.version == 1, api.pointee.destroy != nil, api.pointee.request != nil,
      let create = api.pointee.create, let instance = create()
    else {
      dlclose(library)
      throw AmpError.message("Could not initialize the audio backend.")
    }
    self.library = library
    self.api = api
    self.instance = instance
  }

  deinit {
    api.pointee.destroy(instance)
    dlclose(library)
  }

  func request(_ object: [String: Any]) throws -> Data {
    let input = try JSONSerialization.data(withJSONObject: object)
    let text = String(decoding: input, as: UTF8.self)
    guard let reply = text.withCString({ api.pointee.request(instance, $0) }) else {
      throw AmpError.message("Audio backend returned no response.")
    }
    let data = Data(String(cString: reply).utf8)
    let envelope = try JSONSerialization.jsonObject(with: data) as? [String: Any]
    guard envelope?["ok"] as? Bool == true else {
      throw AmpError.message(envelope?["error"] as? String ?? "Audio request failed.")
    }
    return try JSONSerialization.data(
      withJSONObject: envelope?["value"] ?? NSNull(), options: [.fragmentsAllowed])
  }

  func request(_ op: BridgeOp, _ fields: [String: Any] = [:]) throws -> Data {
    var object = fields
    object["op"] = op.rawValue
    return try request(object)
  }

  static func object<T: Encodable>(_ value: T) throws -> Any {
    try JSONSerialization.jsonObject(with: JSONEncoder().encode(value))
  }
}
