import Testing

@testable import VesperaUI

private func device(
  _ id: String, inputs: Int, outputs: Int, pairable: Bool = true,
  granularity: Int = 1
) -> AudioDeviceInfo {
  AudioDeviceInfo(
    id: id, backend: "coreaudio", name: id, pairable: pairable,
    inputs: (0..<inputs).map { AudioChannelInfo(id: UInt32($0), name: "") },
    outputs: (0..<outputs).map { AudioChannelInfo(id: UInt32($0), name: "") },
    rates: [44100, 48000], currentRate: 48000, minimumBuffer: 16,
    maximumBuffer: 1024, preferredBuffer: 128, granularity: granularity)
}

@Test func audioDevicePairAndChannelValidation() throws {
  let devices = [device("mic", inputs: 1, outputs: 0), device("speakers", inputs: 0, outputs: 2)]
  var config = AudioConfiguration()
  config.inputDevice = "mic"
  config.outputDevice = "speakers"
  try config.validate(devices: devices)
  config.inputChannels = [0, 1]
  #expect(throws: (any Error).self) { try config.validate(devices: devices) }
  config.inputChannels = [0]
  config.outputChannels = [0, 0]
  #expect(throws: (any Error).self) { try config.validate(devices: devices) }
  config.outputChannels = [1]
  try config.validate(devices: devices)
  config.sampleRate = 96000
  #expect(throws: (any Error).self) { try config.validate(devices: devices) }
  config.sampleRate = 48000
  #expect(throws: (any Error).self) {
    try config.validate(devices: [
      devices[0], device("speakers", inputs: 0, outputs: 2, pairable: false),
    ])
  }
  #expect(throws: (any Error).self) { try config.validate(devices: [devices[0]]) }
}

@Test func audioBufferConstraints() {
  let powers = device("powers", inputs: 2, outputs: 2, granularity: -1)
  #expect(powers.supports(buffer: 128))
  #expect(!powers.supports(buffer: 127))
  #expect(!powers.supports(buffer: 0))
  #expect(!powers.supports(buffer: 2048))
  let stepped = device("stepped", inputs: 2, outputs: 2, granularity: 32)
  #expect(stepped.supports(buffer: 48))
  #expect(!stepped.supports(buffer: 64))
}
