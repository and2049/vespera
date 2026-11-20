#ifndef VESPERA_TESTS_SHARED_CONTROL_H
#define VESPERA_TESTS_SHARED_CONTROL_H

#include <vespera/module.h>

#include <cstdint>

// Per-library counters of the shared-engine fixture, read and reset by tests through the
// exported shared_fixture_stats symbol.
struct SharedFixtureStats {
  int prepares = 0;
  int engine_prepares = 0;
  int live = 0;
  int resets = 0;
  int applies = 0;
  uint64_t target_events = 0;
  uint64_t midi_events = 0;
  bool fail_prepare = false;
  // Number of upcoming engine_apply calls that apply their first assignment, then fail.
  int fail_apply = 0;
  const vs_host_instance_cache *last_cache = nullptr;
  // Pooled engine instances: created, taken from the host cache (from any library, or from another
  // library only), and disposed by this library's dispose function.
  int pool_created = 0;
  int reused = 0;
  int reused_foreign = 0;
  int disposed = 0;
  uint32_t last_flags = 0;
  uint32_t last_target_count = 0;
  float last_model = 0;
  char last_signature[256] = {};
  // Variants built with SHARED_SERVICES: works the leader opened in engine_prepare and closed in
  // release, and results adopted while processing.
  int works_opened = 0;
  int works_closed = 0;
  int adopted = 0;
};

#endif
