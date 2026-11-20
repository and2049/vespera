#include "bridge_dispatch_control.hpp"
#include <vespera/module.h>
#include <vespera/sdk/main_thread.hpp>

#include <algorithm>
#include <new>

namespace {
using vespera::sdk::MainThread;
BridgeDispatchControl control;

vs_result create(void **instance) {
  *instance = new (std::nothrow) bool(false);
  if (!*instance) {
    return VS_ERROR_PROCESS;
  }
  ++control.live;
  return VS_OK;
}

void destroy(void *instance) {
  delete static_cast<bool *>(instance);
  --control.live;
}

vs_result prepare(void *instance, const vs_prepare_info *, vs_module_timing *timing) {
  ++control.entered;
  auto &acquired = *static_cast<bool *>(instance);
  MainThread::call(
      [&] {
        acquired = true;
        ++control.called;
        if (control.on_main) {
          control.on_main(control.context);
          MainThread::wait(0.050);
        }
      },
      1.0);
  if (!acquired) {
    ++control.timed_out;
    return VS_ERROR_PROCESS;
  }
  timing->latency_frames = 0;
  timing->tail_frames = 0;
  return control.fail ? VS_ERROR_PROCESS : VS_OK;
}

void release(void *instance) {
  if (*static_cast<bool *>(instance)) {
    MainThread::post([] { ++control.released; });
  }
}

void reset(void *) {}

vs_result process(void *, const vs_process_block *block) {
  for (uint32_t c = 0; c < block->channels; ++c) {
    std::fill_n(block->outputs[c], block->frames, .5f);
  }
  return VS_OK;
}

const vs_module_api api{sizeof(api),
                        VS_MODULE_ABI_VERSION,
                        "test.bridge-dispatch",
                        "Bridge Dispatch Fixture",
                        "1",
                        VS_LAYOUT_MONO | VS_LAYOUT_STEREO,
                        nullptr,
                        0,
                        create,
                        destroy,
                        prepare,
                        release,
                        reset,
                        process,
                        0,
                        0};
}

extern "C" VS_MODULE_EXPORT const vs_module_api *vespera_module_entry(uint32_t version) {
  return version == VS_MODULE_ABI_VERSION ? &api : nullptr;
}

extern "C" VS_MODULE_EXPORT BridgeDispatchControl *vespera_bridge_dispatch_control() {
  return &control;
}
