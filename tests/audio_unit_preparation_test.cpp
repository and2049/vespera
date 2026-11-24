// Real-time preparation of a hosted plugin whose startup work runs on the main thread: off the
// main thread a fresh instance's preparation lasts as long as its playback would, so that work
// overlaps rendering before frame zero, and values the work reverts are given again.
#include "fake_audio_unit.hpp"

#include <vespera/instance_cache.hpp>
#include <vespera/sdk/audio_unit.hpp>

#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <functional>
#include <iterator>
#include <source_location>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using vespera::sdk::MainThread;

void check(bool condition, std::source_location location = std::source_location::current()) {
  if (!condition) {
    throw std::runtime_error("preparation check failed at line " + std::to_string(location.line()));
  }
}

struct Configuration {
  static constexpr uint32_t type = fake_au::type, subtype = fake_au::subtype,
                            manufacturer = fake_au::manufacturer;
  static constexpr const char *id = "test.fake-au.prepared";
  static constexpr const char *name = "Fake Audio Unit";
  static constexpr double preparation_seconds = .05;
  static constexpr bool preparation_in_real_time = true;
  static constexpr const char *shared_engine = "test.fake-au/1";
  static constexpr uint32_t shared_stage = 1;
  static constexpr auto parameters =
      std::to_array<vs_parameter_descriptor>({{1, "Level", "", 0, 1, 1}});
  static constexpr auto mapping = std::to_array<vespera::sdk::AudioUnitParameter>(
      {{fake_au::active_parameter, "Active", 0, 1}});
  static constexpr auto settings = std::array<vespera::sdk::AudioUnitSetting, 0>{};
};
using Module = vespera::sdk::AudioUnitModule<Configuration>;

// Writes values the plugin represents differently: a fractional index it truncates and a value
// beyond the range it clamps.
struct Represented : Configuration {
  static constexpr const char *id = "test.fake-au.represented";
  static constexpr auto settings = std::to_array<vespera::sdk::AudioUnitSetting>(
      {{{fake_au::mode_parameter, "Mode", 0, 3}, 1.5f},
       {{fake_au::gain_parameter, "Gain", 0, 1}, 1.25f}});
};

// Paces only the first part of a longer preparation.
struct PartlyPaced : Configuration {
  static constexpr const char *id = "test.fake-au.partly-paced";
  static constexpr double preparation_seconds = .2;
  static constexpr double preparation_paced_seconds = .05;
};

struct CountingPolicy {
  static inline std::atomic<int> configured{0}, verified{0};
  std::string variant() const { return "counting"; }
  bool configure(AudioUnit, uint32_t) const { return ++configured, true; }
  bool verify(AudioUnit, uint32_t) const { return ++verified, true; }
};

struct Policed : Configuration {
  static constexpr const char *id = "test.fake-au.policed";
  static constexpr CountingPolicy instance_policy{};
};

struct Unpaced : Configuration {
  static constexpr bool preparation_in_real_time = false;
};

struct Fresh {
  bool silent_until_startup = false;
  bool startup_reverts = false;
  bool always_reverts = false;
  bool reverts_first_write = false;
  bool drifting = false;
  bool drift_to_nan = false;
  bool unreadable_after_startup = false;
};

void behave(const Fresh &fresh) {
  auto &behaviour = fake_au::behaviour();
  behaviour.silent_until_startup = fresh.silent_until_startup;
  behaviour.startup_reverts = fresh.startup_reverts;
  behaviour.always_reverts = fresh.always_reverts;
  behaviour.reverts_first_write = fresh.reverts_first_write;
  behaviour.drifting = fresh.drifting;
  behaviour.drift_to_nan = fresh.drift_to_nan;
  behaviour.unreadable_after_startup = fresh.unreadable_after_startup;
}

// What `work` writes to standard error.
std::string standard_error_of(const std::function<void()> &work) {
  char path[] = "/tmp/vespera-preparation-stderr-XXXXXX";
  const int file = mkstemp(path);
  check(file >= 0);
  std::fflush(stderr);
  const int saved = dup(STDERR_FILENO);
  dup2(file, STDERR_FILENO);
  std::exception_ptr failure;
  try {
    work();
  } catch (...) {
    failure = std::current_exception();
  }
  std::fflush(stderr);
  dup2(saved, STDERR_FILENO);
  close(saved);
  close(file);
  std::ifstream in(path);
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::remove(path);
  if (failure) {
    std::rethrow_exception(failure);
  }
  return text;
}

// Runs `work` on a worker while the main thread services its run loop, as the app's does.
void on_worker(const std::function<void()> &work) {
  std::atomic<bool> done = false;
  std::exception_ptr failure;
  std::thread worker([&] {
    try {
      work();
    } catch (...) {
      failure = std::current_exception();
    }
    done = true;
  });
  const auto deadline = Clock::now() + 10s;
  while (!done && Clock::now() < deadline) {
    MainThread::wait(.002);
  }
  worker.join();
  if (failure) {
    std::rethrow_exception(failure);
  }
}

void settle() {
  const auto deadline = Clock::now() + 5s;
  while (fake_au::open_instances() != 0 && Clock::now() < deadline) {
    MainThread::wait(.005);
  }
  check(fake_au::open_instances() == 0);
}

// Prepares through the shared engine, which offers the cache, or without one as a plain module,
// which gives the instance the module's parameter values.
template <class Wrapper = Module>
vs_result prepare(void *instance, const vs_host_instance_cache *cache = nullptr) {
  vs_prepare_info info{sizeof(info), 48000, 128, 1};
  vs_module_timing timing{sizeof(timing), 0, 0};
  if (!cache) {
    return Wrapper::api.prepare(instance, &info, &timing);
  }
  return Wrapper::shared_api()->engine_prepare(instance, &info, cache, "fixture", 0, nullptr, 0,
                                               sizeof(vs_shared_target), nullptr, 0, &timing);
}

template <class Wrapper = Module> float first_output(void *instance, uint32_t flags = 0) {
  std::array<float, 128> in{}, out{};
  in.fill(.5f);
  const float *inputs[] = {in.data()};
  float *outputs[] = {out.data()};
  vs_process_block block{sizeof(block), inputs, outputs, 1,       128, 0, 0,
                         nullptr,       0,      nullptr, nullptr, 0,   0};
  block.flags = flags;
  check(Wrapper::api.process(instance, &block) == VS_OK);
  return out[0];
}

void startup_work_completes_before_frame_zero() {
  behave({.silent_until_startup = true});
  for (int round = 0; round < 10; ++round) {
    on_worker([] {
      void *instance = nullptr;
      check(Module::api.create(&instance) == VS_OK);
      const auto begun = Clock::now();
      check(prepare(instance) == VS_OK);
      check(Clock::now() - begun >= 45ms);
      check(first_output(instance) == .5f);
      Module::api.destroy(instance);
    });
  }
  settle();
}

void only_the_first_fresh_block_preserves_preparation() {
  struct Preserved : Configuration {
    enum { preserve_preparation_on_start = true };
  };
  using Wrapper = vespera::sdk::AudioUnitModule<Preserved>;
  check(std::string(Module::shared_api()->engine) != Wrapper::shared_api()->engine);
  behave({});
  void *instance = nullptr;
  check(Wrapper::api.create(&instance) == VS_OK);
  check(prepare<Wrapper>(instance) == VS_OK);
  int before = fake_au::counters().resets;
  first_output<Wrapper>(instance, VS_PROCESS_DISCONTINUITY);
  check(fake_au::counters().resets == before);
  first_output<Wrapper>(instance, VS_PROCESS_DISCONTINUITY);
  check(fake_au::counters().resets == before + 1);
  Wrapper::api.reset(instance);
  check(fake_au::counters().resets == before + 2);
  Wrapper::api.destroy(instance);
  settle();
}

void reverted_values_are_given_again() {
  behave({.startup_reverts = true});
  for (int round = 0; round < 10; ++round) {
    on_worker([] {
      void *instance = nullptr;
      check(Module::api.create(&instance) == VS_OK);
      check(Module::parameter_api.set_value(instance, 1, .25f) == VS_OK);
      check(prepare(instance) == VS_OK);
      check(first_output(instance) == .125f);
      Module::api.destroy(instance);
    });
  }
  settle();
}

void persistent_reverts_fail_preparation() {
  behave({.always_reverts = true});
  const auto message = standard_error_of([] {
    on_worker([] {
      void *instance = nullptr;
      check(Module::api.create(&instance) == VS_OK);
      check(Module::parameter_api.set_value(instance, 1, .25f) == VS_OK);
      check(prepare(instance) == VS_ERROR_UNSUPPORTED);
      Module::api.destroy(instance);
    });
  });
  check(message.find("kept reverting parameter 0 (given 0.25, holds 1; 1 differ)") !=
        std::string::npos);
  settle();
}

// A revert that lands while the wrapper is still writing values is compared with what the wrapper
// wrote, not with what the plugin held afterwards.
void revert_during_setup_is_given_again() {
  behave({.reverts_first_write = true});
  for (int round = 0; round < 3; ++round) {
    on_worker([] {
      void *instance = nullptr;
      check(Module::api.create(&instance) == VS_OK);
      check(Module::parameter_api.set_value(instance, 1, .25f) == VS_OK);
      check(prepare(instance) == VS_OK);
      check(first_output(instance) == .125f);
      Module::api.destroy(instance);
    });
  }
  settle();
}

void unwritten_parameters_may_change() {
  for (const bool to_nan : {false, true}) {
    behave({.drifting = true, .drift_to_nan = to_nan});
    on_worker([] {
      void *instance = nullptr;
      check(Module::api.create(&instance) == VS_OK);
      const auto begun = Clock::now();
      check(prepare(instance) == VS_OK);
      check(Clock::now() - begun < 90ms);
      check(first_output(instance) == .5f);
      Module::api.destroy(instance);
    });
  }
  settle();
}

void represented_values_hold() {
  behave({});
  using Wrapper = vespera::sdk::AudioUnitModule<Represented>;
  on_worker([] {
    void *instance = nullptr;
    check(Wrapper::api.create(&instance) == VS_OK);
    const auto begun = Clock::now();
    check(prepare<Wrapper>(instance) == VS_OK);
    check(Clock::now() - begun < 90ms);
    check(first_output<Wrapper>(instance) == .5f);
    Wrapper::api.destroy(instance);
  });
  settle();
}

void unreadable_parameters_are_reported() {
  behave({.unreadable_after_startup = true});
  const auto message = standard_error_of([] {
    on_worker([] {
      void *instance = nullptr;
      check(Module::api.create(&instance) == VS_OK);
      check(prepare(instance) == VS_ERROR_UNSUPPORTED);
      Module::api.destroy(instance);
    });
  });
  check(message.find("could not read parameter 0") != std::string::npos);
  check(message.find("kept reverting") == std::string::npos);
  settle();
}

// The unpaced remainder renders at full speed without shortening the preparation.
void only_the_paced_part_takes_playback_time() {
  behave({.silent_until_startup = true});
  using Wrapper = vespera::sdk::AudioUnitModule<PartlyPaced>;
  for (int round = 0; round < 5; ++round) {
    on_worker([] {
      void *instance = nullptr;
      check(Wrapper::api.create(&instance) == VS_OK);
      const auto begun = Clock::now();
      check(prepare<Wrapper>(instance) == VS_OK);
      const auto took = Clock::now() - begun;
      check(took >= 45ms && took < 150ms);
      check(first_output<Wrapper>(instance) == .5f);
      Wrapper::api.destroy(instance);
    });
  }
  settle();
}

// A cached instance keeps the configuration its cache key names, so it is only verified.
void reused_instances_are_verified_not_configured() {
  behave({});
  using Wrapper = vespera::sdk::AudioUnitModule<Policed>;
  auto cache = std::make_shared<vespera::InstanceCache>();
  auto client = cache->client(nullptr);
  void *instance = nullptr;
  check(Wrapper::api.create(&instance) == VS_OK);
  on_worker([&] {
    for (int round = 0; round < 3; ++round) {
      check(prepare<Wrapper>(instance, client.get()) == VS_OK);
      check(first_output<Wrapper>(instance) == .5f);
      Wrapper::api.release(instance);
    }
  });
  check(CountingPolicy::configured == 1 && CountingPolicy::verified == 2);
  check(cache->statistics().misses == 1);
  Wrapper::api.destroy(instance);
  client.reset();
  cache.reset();
  settle();
}

// Members of one engine share the leader's preparation, so modules that pace differently, or
// were built before pacing was part of identity, never share an engine.
void pacing_is_part_of_engine_identity() {
  const std::string paced = Module::shared_api()->engine;
  const std::string partly = vespera::sdk::AudioUnitModule<PartlyPaced>::shared_api()->engine;
  const std::string unpaced = vespera::sdk::AudioUnitModule<Unpaced>::shared_api()->engine;
  check(paced != partly && paced != unpaced && partly != unpaced);
  check(paced.ends_with("|sdk-engine:2"));
}

// On the main thread the startup work cannot run during preparation, so pacing would only delay
// it; a cached instance has already run its startup work.
void only_fresh_instances_off_main_are_paced() {
  behave({});
  void *instance = nullptr;
  check(Module::api.create(&instance) == VS_OK);
  auto begun = Clock::now();
  check(prepare(instance) == VS_OK);
  check(Clock::now() - begun < 40ms);
  Module::api.release(instance);

  auto cache = std::make_shared<vespera::InstanceCache>();
  auto client = cache->client(nullptr);
  on_worker([&] {
    begun = Clock::now();
    check(prepare(instance, client.get()) == VS_OK);
    check(Clock::now() - begun >= 45ms);
    Module::api.release(instance);
    begun = Clock::now();
    check(prepare(instance, client.get()) == VS_OK);
    check(Clock::now() - begun < 40ms);
    check(first_output(instance) == .5f);
    Module::api.release(instance);
  });
  check(cache->statistics().misses == 1);
  Module::api.destroy(instance);
  client.reset();
  cache.reset();
  settle();
}

}

int main(int argc, char **argv) {
  fake_au::register_component();
  const std::pair<const char *, void (*)()> tests[] = {
      {"only_the_first_fresh_block_preserves_preparation",
       only_the_first_fresh_block_preserves_preparation},
      {"startup_work_completes_before_frame_zero", startup_work_completes_before_frame_zero},
      {"reverted_values_are_given_again", reverted_values_are_given_again},
      {"persistent_reverts_fail_preparation", persistent_reverts_fail_preparation},
      {"revert_during_setup_is_given_again", revert_during_setup_is_given_again},
      {"unwritten_parameters_may_change", unwritten_parameters_may_change},
      {"represented_values_hold", represented_values_hold},
      {"unreadable_parameters_are_reported", unreadable_parameters_are_reported},
      {"only_the_paced_part_takes_playback_time", only_the_paced_part_takes_playback_time},
      {"pacing_is_part_of_engine_identity", pacing_is_part_of_engine_identity},
      {"reused_instances_are_verified_not_configured",
       reused_instances_are_verified_not_configured},
      {"only_fresh_instances_off_main_are_paced", only_fresh_instances_off_main_are_paced},
  };
  for (const auto &[name, test] : tests) {
    if (argc > 1 && std::string(argv[1]) != name) {
      continue;
    }
    try {
      test();
    } catch (const std::exception &error) {
      std::fprintf(stderr, "%s: %s\n", name, error.what());
      return 1;
    }
  }
  std::puts("audio unit preparation tests passed");
  return 0;
}
