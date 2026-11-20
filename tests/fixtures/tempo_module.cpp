#include <new>
#include <vespera/module.h>

namespace {
struct State {
  double rate = 48000;
};
vs_result create(void **instance) {
  *instance = new State;
  return VS_OK;
}
void destroy(void *instance) { delete static_cast<State *>(instance); }
vs_result prepare(void *instance, const vs_prepare_info *info, vs_module_timing *timing) {
  static_cast<State *>(instance)->rate = info->sample_rate;
  timing->latency_frames = 0;
  timing->tail_frames = 0;
  return VS_OK;
}
void reset(void *) {}
vs_result process(void *instance, const vs_process_block *block) {
  if (block->struct_size < sizeof(*block) || !block->tempo ||
      !(block->tempo->flags & VS_TEMPO_VALID)) {
    return VS_ERROR_ARGUMENT;
  }
  const auto &tempo = *block->tempo;
  const double rate = static_cast<State *>(instance)->rate;
  for (uint32_t c = 0; c < block->channels; ++c) {
    for (uint32_t f = 0; f < block->frames; ++f) {
      block->outputs[c][f] =
          block->inputs[c][f] +
          static_cast<float>(tempo.bpm / 1000 +
                             (tempo.beat_position + f * tempo.bpm / (60 * rate)) / 100);
    }
  }
  return VS_OK;
}
const vs_module_api api{sizeof(api),
                        VS_MODULE_ABI_VERSION,
                        "test.tempo",
                        "Tempo Probe",
                        "1",
                        VS_LAYOUT_MONO | VS_LAYOUT_STEREO,
                        nullptr,
                        0,
                        create,
                        destroy,
                        prepare,
                        reset,
                        reset,
                        process,
                        VS_MODULE_HOST_TEMPO,
                        0};
}
extern "C" VS_MODULE_EXPORT const vs_module_api *vespera_module_entry(uint32_t version) {
  return version == VS_MODULE_ABI_VERSION ? &api : nullptr;
}
