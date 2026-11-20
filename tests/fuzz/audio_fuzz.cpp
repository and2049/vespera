#include "../audio_fuzz_case.hpp"

#include <dlfcn.h>

extern "C" int LLVMFuzzerInitialize(int *, char ***) {
  // Coverage counters in the module must remain mapped for the entire fuzzing process.
  if (dlopen(VESPERA_GAIN_PATH, RTLD_NOW | RTLD_LOCAL) == nullptr) {
    std::fprintf(stderr, "cannot load fuzz module: %s\n", dlerror());
    std::abort();
  }
  return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  audio_fuzz::run({data, size}, VESPERA_GAIN_PATH);
  return 0;
}
