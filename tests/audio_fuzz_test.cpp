#include "audio_fuzz_case.hpp"

#include <charconv>
#include <random>
#include <string_view>

int main(int argc, char **argv) {
  if (argc < 2 || argc > 3) {
    std::fprintf(stderr, "usage: vespera_audio_fuzz_test GAIN_MODULE [SEED]\n");
    return 2;
  }
  uint32_t seed = 0x5eed1234;
  if (argc == 3) {
    const std::string_view text(argv[2]);
    const auto result = std::from_chars(text.data(), text.data() + text.size(), seed);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
      return 2;
    }
  }
  std::printf("synthetic audio fuzz seed=%u\n", seed);
  std::fflush(stdout);
  std::mt19937 random(seed);
  std::array<uint8_t, 4096> data{};
  std::array<uint32_t, 5> outcomes{};
  for (uint32_t iteration = 0; iteration < 2048; ++iteration) {
    for (auto &byte : data) {
      byte = static_cast<uint8_t>(random());
    }
    data[3] = static_cast<uint8_t>(iteration % 8);
    ++outcomes[static_cast<size_t>(audio_fuzz::run(data, argv[1]))];
  }
  std::printf("cases=2048 accepted=%u rejected=%u overflow_recovery=%u\n", outcomes[VS_OK],
              outcomes[VS_ERROR_ARGUMENT], outcomes[VS_ERROR_PROCESS]);
  vespera::Graph graph;
  for (size_t n = 0; n < vespera::max_preset_nodes; ++n) {
    graph.add_module(argv[1]);
  }
  graph.prepare({48000.0, 1, 1});
  float sample = 0.5f;
  const float *input = &sample;
  float *output = &sample;
  audio_fuzz::require(graph.process({&input, &output, 1, 1, 0, VS_PROCESS_OFFLINE}) == VS_OK,
                      "128-node processing failed");
  audio_fuzz::require(sample == 0.5f && graph.capture_preset().nodes.size() == 128,
                      "128-node processing/capture mismatch");
  return 0;
}
