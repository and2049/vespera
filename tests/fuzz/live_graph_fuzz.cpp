#include "../audio_fuzz_case.hpp"
#include "../scripted_audio_backend.h"

#include <vespera/audio_session.hpp>

#include <dlfcn.h>

namespace {

float output(float input, const std::vector<float> &gains) {
  for (float gain : gains) {
    input *= gain;
  }
  return input;
}

}

extern "C" int LLVMFuzzerInitialize(int *, char ***) {
  // The fuzzer retains pointers to coverage counters exported by the dynamically loaded module.
  audio_fuzz::require(dlopen(VESPERA_GAIN_PATH, RTLD_NOW | RTLD_LOCAL) != nullptr,
                      "cannot load gain module");
  return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  using audio_fuzz::require;
  audio_fuzz::Bytes bytes({data, size});
  scripted_ae::reset();
  vespera::AudioSession session;
  vespera::StreamConfig config;
  config.backend_id = scripted_ae::backend_id;
  config.device_id = scripted_ae::device_id;
  config.frames_per_block = 64;
  config.graph_channels = 1 + bytes.byte() % 2;
  config.output_mode = vespera::OutputMode::monitor;
  const float initial = static_cast<float>(bytes.byte()) / 64.0f;
  session.graph().add_module(VESPERA_GAIN_PATH);
  session.set_parameter_value(0, 1, initial);
  session.prepare(config);
  session.start();
  std::vector<float> current{initial};
  const uint32_t count = 1 + bytes.byte() % 8;
  std::array<float, 64> input{}, left{}, right{};
  for (uint32_t transition = 0; transition < count; ++transition) {
    auto candidate = std::make_unique<vespera::Graph>();
    std::vector<float> next;
    const uint32_t nodes = bytes.byte() % 3;
    for (uint32_t n = 0; n < nodes; ++n) {
      const auto id = candidate->add_module(VESPERA_GAIN_PATH);
      const float gain = static_cast<float>(bytes.byte()) / 64.0f;
      candidate->set_parameter_value(id, 1, gain);
      next.push_back(gain);
    }
    const uint32_t fade = 1 + bytes.word() % 512;
    const uint32_t action = bytes.byte() % 4;
    session.request_graph_replacement(std::move(candidate), fade);
    require(session.parameter_token() == vespera::ParameterToken{},
            "token exposed during transition");
    uint32_t elapsed = 0;
    while (elapsed < fade && action != 1) {
      const uint32_t frames = 1 + bytes.byte() % 64;
      for (uint32_t f = 0; f < frames; ++f) {
        input[f] = (static_cast<float>(bytes.byte()) - 128.0f) / 128.0f;
      }
      require(scripted_ae::deliver(input.data(), left.data(), right.data(), frames) ==
                  AE_PROCESS_OK,
              "transition processing failed");
      for (uint32_t f = 0; f < frames; ++f) {
        const float weight =
            std::min(1.0f, static_cast<float>(elapsed + f + 1) / static_cast<float>(fade));
        const float expected =
            output(input[f], current) * (1.0f - weight) + output(input[f], next) * weight;
        require(std::fabs(left[f] - expected) <= 1e-5f && left[f] == right[f],
                "transition output differs from reference");
      }
      elapsed += frames;
      if (action == 2 || action == 3) {
        break;
      }
      require(session.poll().state == vespera::SessionState::running, "unexpected stream failure");
    }
    if (action == 3) {
      scripted_ae::script().faults = AE_FAULT_DEVICE;
      require(scripted_ae::deliver(input.data(), left.data(), right.data(), 64) ==
                  AE_PROCESS_SILENCE,
              "device fault did not suppress callbacks");
      for (uint32_t f = 0; f < 64; ++f) {
        require(left[f] == 0.0f && right[f] == 0.0f, "device fault did not silence output");
      }
      require(session.poll().state == vespera::SessionState::recovery_required,
              "device fault did not require recovery");
      require(session.close() == vespera::CloseResult::closed, "faulted transition close failed");
      return 0;
    }
    if (action == 1 || action == 2) {
      session.stop();
      require(session.state() == vespera::SessionState::prepared,
              "stop did not quiesce transition");
      current.clear();
      for (const auto &node : session.nodes()) {
        current.push_back(session.parameter_value(node.instance_id, 1));
      }
      session.start();
    } else {
      session.poll();
      current = std::move(next);
    }
    require(session.graph_transition_state() == vespera::GraphTransitionState::idle,
            "transition was not settled on the control thread");
    input.fill(0.25f);
    require(scripted_ae::deliver(input.data(), left.data(), right.data(), 64) == AE_PROCESS_OK,
            "processing after transition failed");
    for (uint32_t f = 0; f < 64; ++f) {
      require(left[f] == output(0.25f, current) && right[f] == left[f],
              "settled graph differs from its authoritative values");
    }
  }
  require(session.close() == vespera::CloseResult::closed, "transition session close failed");
  return 0;
}
