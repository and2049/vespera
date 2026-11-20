import CoreAudio
import Foundation

/// The CoreAudio queries and observations Audio Settings relies on: which devices the system
/// defaults to, the buffer sizes a pair of devices supports, and a listener for the system's
/// device list changing.
final class AudioDeviceStore {
  private var listener: AudioObjectPropertyListenerBlock?

  /// Calls `onDevicesChanged` on the main actor whenever the system's device list changes.
  init(onDevicesChanged: @escaping @MainActor () -> Void) {
    let listener: AudioObjectPropertyListenerBlock = { _, _ in
      MainActor.assumeIsolated { onDevicesChanged() }
    }
    var address = AudioObjectPropertyAddress(
      mSelector: kAudioHardwarePropertyDevices,
      mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
    if AudioObjectAddPropertyListenerBlock(
      AudioObjectID(kAudioObjectSystemObject), &address, .main, listener) == noErr
    {
      self.listener = listener
    }
  }

  /// Stops watching the device list.
  func stop() {
    guard let listener else { return }
    var address = AudioObjectPropertyAddress(
      mSelector: kAudioHardwarePropertyDevices,
      mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
    AudioObjectRemovePropertyListenerBlock(
      AudioObjectID(kAudioObjectSystemObject), &address, .main, listener)
    self.listener = nil
  }

  /// The system's default input or output device, by its stable UID.
  static func defaultDevice(input: Bool) -> String? {
    var address = AudioObjectPropertyAddress(
      mSelector: input
        ? kAudioHardwarePropertyDefaultInputDevice : kAudioHardwarePropertyDefaultOutputDevice,
      mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
    var device = AudioDeviceID(0)
    var size = UInt32(MemoryLayout<AudioDeviceID>.size)
    guard
      AudioObjectGetPropertyData(
        AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size, &device) == noErr
    else { return nil }
    address.mSelector = kAudioDevicePropertyDeviceUID
    var uid: Unmanaged<CFString>?
    size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
    guard AudioObjectGetPropertyData(device, &address, 0, nil, &size, &uid) == noErr else {
      return nil
    }
    return uid?.takeRetainedValue() as String?
  }

  /// The power-of-two buffer sizes both devices support, including each one's preferred size.
  static func bufferSizes(input: AudioDeviceInfo?, output: AudioDeviceInfo?) -> [UInt32] {
    guard let input, let output else { return [] }
    let candidates: [UInt32] = [
      16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192,
      input.preferredBuffer, output.preferredBuffer,
    ]
    return Set(candidates).filter { input.supports(buffer: $0) && output.supports(buffer: $0) }
      .sorted()
  }
}
