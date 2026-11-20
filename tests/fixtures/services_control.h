#ifndef VESPERA_TESTS_SERVICES_CONTROL_H
#define VESPERA_TESTS_SERVICES_CONTROL_H

#include <vespera/module.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

// Control block of the services fixture (fixtures/services_module.cpp), which tests rewrite and
// read through the exported symbol before and after a host uses the module.
//
// The module has parameter 1 Level (0-1, default 0.5) and the state and parameter value
// extensions. While prepared with services it opens one work and requests it with token
// round(Level * 1000); every block adopts the newest result and outputs that token / 1000 as a
// constant, or silence while nothing was adopted. Without services it computes the level itself
// in prepare.
struct ServicesControl {
  vs_module_services_api api{};
  // Makes the entry point decline the host's services ABI.
  bool decline = false;
  vs_result attach_result = VS_OK;
  // Leaves the work open at release instead of closing it.
  bool leave_open = false;
  // Runs wait until they are cancelled.
  std::atomic<bool> hold{false};
  std::atomic<int> held{0};
  std::mutex prepare_mutex;
  std::condition_variable prepare_changed;
  bool hold_prepare = false;
  bool prepare_entered = false;
  std::atomic<uint32_t> latency{0};
  std::atomic<bool> timing_on_prepare{false};
  std::mutex calls_mutex;
  // The status the module reports.
  uint32_t status_kind = VS_STATUS_READY;
  char status_message[256] = {};
  bool terminate_message = true;
  vs_result status_result = VS_OK;
  // Calls in order: c create, a attach, s state load, v set_value, p prepare, r release, d destroy.
  char calls[64] = {};
  std::atomic<int> runs{0};
  std::atomic<int> disposed{0};
  std::atomic<int> created{0};
  std::atomic<int> destroyed{0};
  const vs_host_services *attached = nullptr;
  void (*restore)() = nullptr;
};

#define SERVICES_CONTROL_SYMBOL "vespera_services_control"
using services_control_fn = ServicesControl *(*)();

#endif
