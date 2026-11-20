struct GlobalControlSettings: Codable, Equatable {
  var inputGain = 0.0
  var gateEnabled = false
  var gateThreshold = -80.0
  var transpose = 0.0
  var stereoInput = false
  var doublerEnabled = false
  var doublerTime = 6.0
  var outputGain = 0.0
}
