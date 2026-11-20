#include "../../dsp/bench/wav.hpp"

#include <cstdlib>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  using namespace vespera::dsp::bench;
  WavAudio audio;
  try {
    audio = parse_wav({data, size}, {65536, 16384});
  } catch (const WavError &) {
    return 0;
  }
  const auto native = encode_wav(audio);
  if (encode_wav(parse_wav(native)) != native) {
    std::abort();
  }
  audio.format = SampleFormat::float32;
  const auto encoded = encode_wav(audio);
  const auto decoded = parse_wav(encoded);
  if (audio.samples != decoded.samples || audio.channels != decoded.channels ||
      audio.sample_rate != decoded.sample_rate) {
    std::abort();
  }
  return 0;
}
