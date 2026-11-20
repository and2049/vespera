#include <cstring>
#include <vespera/module.h>

#ifndef MIDI_ID
#define MIDI_ID "test.midi"
#endif
// Additive builds add the held note to their input instead of replacing it, so several MIDI
// modules in one chain each leave an audible trace.
#ifndef MIDI_ADDITIVE
#define MIDI_ADDITIVE 0
#endif

namespace {
struct State {
  float note = 0, gain = 1;
};
vs_result create(void **instance) {
  *instance = new State;
  return VS_OK;
}
void destroy(void *instance) { delete static_cast<State *>(instance); }
vs_result prepare(void *, const vs_prepare_info *, vs_module_timing *timing) {
  timing->latency_frames = 0;
  timing->tail_frames = 0;
  return VS_OK;
}
void reset(void *instance) { static_cast<State *>(instance)->note = 0; }
void release(void *) {}
vs_result process(void *instance, const vs_process_block *block) {
  auto &state = *static_cast<State *>(instance);
  if (block->flags & (VS_PROCESS_DISCONTINUITY | VS_PROCESS_MIDI_PANIC)) {
    state.note = 0;
  }
  uint32_t midi = 0, parameter = 0;
  for (uint32_t frame = 0; frame < block->frames; ++frame) {
    while (parameter < block->event_count && block->events[parameter].frame_offset == frame) {
      state.gain = block->events[parameter++].value;
    }
    while (midi < block->midi_event_count) {
      vs_midi_event event{};
      std::memcpy(&event,
                  static_cast<const uint8_t *>(block->midi_events) + midi * block->midi_event_size,
                  sizeof(event));
      if (event.frame_offset != frame) {
        break;
      }
      ++midi;
      state.note = (event.status & 0xf0) == 0x90 && event.data2 ? event.data1 / 128.0f : 0;
    }
    for (uint32_t c = 0; c < block->channels; ++c) {
      block->outputs[c][frame] =
          (MIDI_ADDITIVE ? block->inputs[c][frame] : 0.0f) + state.note * state.gain;
    }
  }
  return VS_OK;
}
const vs_parameter_descriptor parameters[]{{1, "Gain", "", 0, 2, 1}};
const vs_module_api api{sizeof(api),
                        VS_MODULE_ABI_VERSION,
                        MIDI_ID,
                        "MIDI Probe",
                        "1",
                        VS_LAYOUT_MONO | VS_LAYOUT_STEREO,
                        parameters,
                        1,
                        create,
                        destroy,
                        prepare,
                        release,
                        reset,
                        process,
                        VS_MODULE_MIDI_INPUT,
                        0};
}
extern "C" VS_MODULE_EXPORT const vs_module_api *vespera_module_entry(uint32_t version) {
  return version == VS_MODULE_ABI_VERSION ? &api : nullptr;
}
