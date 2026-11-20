// A small drive pedal on the module kit for vespera-module-check tests. Building it with one of
// FIXTURE_PASS_THROUGH it instead passes every sample through unchanged, non-finite input
// included, which conforms. Building it with one of the other FIXTURE_* definitions introduces
// exactly one conformance defect:
//   FIXTURE_ALLOCATES          allocates and frees memory inside process
//   FIXTURE_BLOCK_DEPENDENT    output depends on where blocks begin
//   FIXTURE_INEXACT_BYPASS     bypassed output is not delayed by the reported latency
//   FIXTURE_ACCEPTS_BAD_STATE  state load repairs invalid values instead of rejecting them
//   FIXTURE_EMITS_NAN          the maximum Drive setting divides by zero
//   FIXTURE_BAD_CAPABILITIES   declares a bypass parameter that does not range from 0 to 1
//   FIXTURE_METADATA_SKEW      parameter metadata gives Level a display skew of 0
//   FIXTURE_METADATA_UNDECLARED parameter metadata describes a parameter it does not declare
//   FIXTURE_METADATA_BYPASS    parameter metadata describes the bypass parameter as continuous
//   FIXTURE_METADATA_LABELS    parameter metadata names a Character value with an empty label
//   FIXTURE_BLOCKS_ON_WORK     process waits up to 50 ms for a requested table
//   FIXTURE_IGNORES_CANCEL     a table build keeps running for 200 ms after it is cancelled
//   FIXTURE_FREES_RESULT       frees tables it no longer uses itself instead of handing them back
//   FIXTURE_OWN_THREAD         builds tables on its own thread and adopts them at the next span
//   FIXTURE_LOGS_IN_PROCESS    logs from inside process
//   FIXTURE_STATUS_WAITS       status waits for process to return
// The Drive parameter selects a transfer table that kit::Derived rebuilds off the processing
// thread.
#include <vespera/kit/module.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#if defined(FIXTURE_FREES_RESULT) || defined(FIXTURE_LOGS_IN_PROCESS) ||                           \
    defined(FIXTURE_STATUS_WAITS) || defined(FIXTURE_FOREIGN_REQUEST) ||                           \
    defined(FIXTURE_BAD_STATUS)
#define FIXTURE_WRAPS_SERVICES 1
#endif

#ifndef FIXTURE_ID
#define FIXTURE_ID "test.module-check.drive"
#endif

namespace {
namespace kit = vespera::kit;

constexpr const char *characters[] = {"Warm", "Bright"};

// tanh(gain * x) over x in [-1, 1], with linear interpolation between 1025 points.
struct Table {
  std::array<float, 1025> points{};

  explicit Table(float drive) {
    const double gain = 1.0 + 9.0 * drive;
    for (size_t i = 0; i < points.size(); ++i) {
      points[i] = static_cast<float>(std::tanh(gain * (static_cast<double>(i) / 512.0 - 1.0)));
    }
  }

  float operator()(float x) const noexcept {
    const float position = (std::clamp(x, -1.0f, 1.0f) + 1.0f) * 512.0f;
    const auto index = std::min<size_t>(static_cast<size_t>(position), points.size() - 2);
    const float fraction = position - static_cast<float>(index);
    return points[index] + (points[index + 1] - points[index]) * fraction;
  }
};

using Clock = std::chrono::steady_clock;

[[maybe_unused]] void spin(std::chrono::milliseconds duration) {
  const auto until = Clock::now() + duration;
  while (Clock::now() < until) {
  }
}

#ifdef FIXTURE_BLOCKS_ON_WORK
std::atomic<bool> built{true};
#endif

#ifdef FIXTURE_WRAPS_SERVICES
// The host table of the most recently attached instance, for logging from process.
std::atomic<const vs_host_services *> attached_services{nullptr};
#endif

[[maybe_unused]] Table build(float drive, [[maybe_unused]] bool live,
                             [[maybe_unused]] kit::Job &job) {
#ifdef FIXTURE_IGNORES_CANCEL
  if (live) {
    spin(std::chrono::milliseconds(10));
    if (job.cancelled()) {
      spin(std::chrono::milliseconds(200));
    }
  }
#endif
  Table table(drive);
#ifdef FIXTURE_BLOCKS_ON_WORK
  built = true;
#endif
  return table;
}

struct Drive {
  static constexpr const char *id = FIXTURE_ID;
  static constexpr const char *name = "Drive";
  static constexpr const char *version = "1.0.0";
  static constexpr uint32_t layouts = VS_LAYOUT_MONO | VS_LAYOUT_STEREO;
#ifdef FIXTURE_INEXACT_BYPASS
  static constexpr auto bypass = kit::BypassPolicy::custom;
#endif
#ifdef FIXTURE_PASS_THROUGH
  static constexpr bool sanitise_input = false;
  static constexpr bool flush_subnormals = false;
#endif
  static constexpr auto parameters =
      kit::parameters(kit::toggle(1, "Enabled", true).bypass(),
                      kit::knob(2, "Drive", 0.25f).shows(0, 10).decimals(1),
                      kit::knob(3, "Level", 1).range(0, 2).decimals(2).display_unit("x"),
                      kit::choice(4, "Character", characters));

  struct Processor {
    static constexpr uint32_t latency_frames = 3;
    static constexpr uint64_t tail_frames = 0;
    float enabled;
    float drive;
    float level;
    std::array<kit::Frame, latency_frames> delay{};
    uint32_t position = 0;
#ifdef FIXTURE_OWN_THREAD
    std::unique_ptr<Table> table;
    std::unique_ptr<Table> next;
    std::thread builder;
    bool adopt_next_span = false;
#else
    kit::Derived<Table, float> table;
    bool waiting = false;
#endif

    Processor(const kit::Setup &setup, kit::Host &host)
        : enabled(setup.values[0]), drive(setup.values[1]), level(setup.values[2]),
#ifdef FIXTURE_OWN_THREAD
          table(std::make_unique<Table>(setup.values[1])) {
      (void)host;
    }
    ~Processor() {
      if (builder.joinable()) {
        builder.join();
      }
    }
#else
          table(
              host.derive<Table>(setup.values[1], [live = host.live()](float value, kit::Job &job) {
                return build(value, live, job);
              })) {
    }
#endif
    void reset() noexcept {
      delay = {};
      position = 0;
    }
    void set_parameter(size_t index, float value) noexcept {
      if (index < 3) {
        (index == 0 ? enabled : index == 1 ? drive : level) = value;
      }
      if (index != 1) {
        return;
      }
#ifdef FIXTURE_OWN_THREAD
      if (builder.joinable()) {
        builder.join();
        table = std::move(next);
      }
      builder = std::thread([this, value] { next = std::make_unique<Table>(value); });
      adopt_next_span = false;
#else
#ifdef FIXTURE_BLOCKS_ON_WORK
      built = false;
      waiting = true;
#endif
      table.request(value);
#endif
    }

    const Table &current() noexcept {
#ifdef FIXTURE_OWN_THREAD
      return *table;
#else
#ifdef FIXTURE_BLOCKS_ON_WORK
      if (waiting) {
        waiting = false;
        const auto until = Clock::now() + std::chrono::milliseconds(50);
        while (!built && Clock::now() < until) {
        }
      }
#endif
#ifdef FIXTURE_LOGS_IN_PROCESS
      if (const vs_host_services *services = attached_services.load(std::memory_order_relaxed)) {
        services->log(services->context, VS_LOG_DEBUG, "processing");
      }
#endif
      return *table.current();
#endif
    }

    float shape(float sample) noexcept {
#if defined(FIXTURE_EMITS_NAN)
      return level * sample / (1.0f - drive);
#elif defined(FIXTURE_PASS_THROUGH)
      return sample;
#else
      return level * current()(sample);
#endif
    }

    kit::Frame delayed(const kit::Frame &input) noexcept {
      const kit::Frame output = delay[position];
      delay[position] = input;
      position = (position + 1) % latency_frames;
      return output;
    }

#if defined(FIXTURE_BLOCK_DEPENDENT) || defined(FIXTURE_OWN_THREAD)
    void process(kit::Block &block) noexcept {
#ifdef FIXTURE_OWN_THREAD
      if (adopt_next_span && builder.joinable()) {
        builder.join();
        table = std::move(next);
      }
      adopt_next_span = builder.joinable();
      constexpr float first = 1.0f;
#else
      constexpr float first = 0.5f;
#endif
      for (uint32_t f = 0; f < block.frames; ++f) {
        kit::Frame frame{};
        for (uint32_t c = 0; c < block.channels; ++c) {
          frame[c] = shape(block.inputs[c][f]) * (f == 0 ? first : 1.0f);
        }
        const kit::Frame output = delayed(frame);
        for (uint32_t c = 0; c < block.channels; ++c) {
          block.outputs[c][f] = output[c];
        }
      }
    }
#else
    kit::Frame process_frame(const kit::Frame &input) noexcept {
#ifdef FIXTURE_ALLOCATES
      void *(*volatile allocate)(size_t) = std::malloc;
      void (*volatile deallocate)(void *) = std::free;
      deallocate(allocate(64));
#endif
      const kit::Frame output = delayed({shape(input[0]), shape(input[1])});
#ifdef FIXTURE_INEXACT_BYPASS
      return enabled >= 0.5f ? output : input;
#else
      return output;
#endif
    }
#endif
  };
};

using Module = kit::Module<Drive>;

#ifdef FIXTURE_ACCEPTS_BAD_STATE
vs_result lenient_load(void *instance, uint32_t schema, const uint8_t *buffer, uint64_t bytes) {
  constexpr size_t count = Drive::parameters.size();
  if (!buffer || bytes != count * sizeof(float)) {
    return Module::state_api.load(instance, schema, buffer, bytes);
  }
  std::array<uint8_t, count * sizeof(float)> repaired{};
  for (size_t i = 0; i < count; ++i) {
    const vs_parameter_descriptor &d = Module::api.parameters[i];
    float value = 0.0f;
    std::memcpy(&value, buffer + i * sizeof(float), sizeof(float));
    value = std::isfinite(value) ? std::clamp(value, d.minimum, d.maximum) : d.default_value;
    std::memcpy(repaired.data() + i * sizeof(float), &value, sizeof(float));
  }
  return Module::state_api.load(instance, schema, repaired.data(), repaired.size());
}

const vs_module_state_api state_api = [] {
  vs_module_state_api api = Module::state_api;
  api.load = lenient_load;
  return api;
}();
#else
const vs_module_state_api &state_api = Module::state_api;
#endif

#if defined(FIXTURE_BAD_CAPABILITIES)
const vs_module_api module_api = [] {
  vs_module_api api = Module::api;
  api.bypass_parameter = 3;
  return api;
}();
#elif defined(FIXTURE_WRAPS_SERVICES)
vs_result locked_process(void *instance, const vs_process_block *block);
void wrapped_destroy(void *instance);
const vs_module_api module_api = [] {
  vs_module_api api = Module::api;
  api.process = locked_process;
  api.destroy = wrapped_destroy;
  return api;
}();
#else
const vs_module_api &module_api = Module::api;
#endif

#ifdef FIXTURE_METADATA_LABELS
constexpr const char *blank_characters[] = {"Warm", ""};
#endif

const auto metadata_entries = [] {
  auto entries = Drive::parameters.metadata;
#if defined(FIXTURE_METADATA_SKEW)
  entries[2].skew = 0;
#elif defined(FIXTURE_METADATA_UNDECLARED)
  entries[3].parameter_id = 9;
#elif defined(FIXTURE_METADATA_BYPASS)
  entries[0].kind = VS_PARAMETER_CONTINUOUS;
  entries[0].steps = 0;
#elif defined(FIXTURE_METADATA_LABELS)
  entries[3].labels = blank_characters;
#endif
  return entries;
}();

const vs_module_metadata_api metadata_api = [] {
  vs_module_metadata_api api = Module::metadata_api;
  api.parameters = metadata_entries.data();
  return api;
}();

#ifdef FIXTURE_WRAPS_SERVICES
// Stands between the kit and the host's services to misuse them.
struct Wrapped {
  const vs_host_services *host = nullptr;
  vs_host_services table{};
  vs_work_desc desc{};
};
std::map<void *, std::unique_ptr<Wrapped>> wrappers;

void wrapped_destroy(void *instance) {
  Module::api.destroy(instance);
  wrappers.erase(instance);
}

Wrapped &wrapped(void *context) { return *static_cast<Wrapped *>(context); }

vs_work *wrapped_open(void *context, const vs_work_desc *desc) {
  Wrapped &self = wrapped(context);
  self.desc = *desc;
  auto *work = self.host->open_work(self.host->context, desc);
#ifdef FIXTURE_FOREIGN_REQUEST
  if (work) {
    std::thread foreign([&] { self.host->request(self.host->context, work, 0); });
    foreign.join();
  }
#endif
  return work;
}
vs_result wrapped_request(void *context, vs_work *work, uint64_t token) {
  return wrapped(context).host->request(wrapped(context).host->context, work, token);
}
int wrapped_adopt(void *context, vs_work *work, void **current, uint64_t *token) {
  return wrapped(context).host->adopt(wrapped(context).host->context, work, current, token);
}
int wrapped_retire(void *context, vs_work *work, void *result) {
  Wrapped &self = wrapped(context);
#ifdef FIXTURE_FREES_RESULT
  (void)work;
  if (result) {
    self.desc.dispose(self.desc.context, result);
  }
  return 1;
#else
  return self.host->retire(self.host->context, work, result);
#endif
}
void wrapped_close(void *context, vs_work *work, void *const *held, uint32_t count) {
  wrapped(context).host->close_work(wrapped(context).host->context, work, held, count);
}
void wrapped_notify(void *context, uint32_t what) {
  wrapped(context).host->notify(wrapped(context).host->context, what);
}
void wrapped_log(void *context, uint32_t level, const char *message) {
  wrapped(context).host->log(wrapped(context).host->context, level, message);
}

std::mutex processing;

vs_result attach(void *instance, const vs_host_services *services) {
  if (!services) {
    return VS_ERROR_ARGUMENT;
  }
  auto *self = new Wrapped{services, *services, {}};
  wrappers[instance].reset(self);
  self->table.context = self;
  self->table.open_work = wrapped_open;
  self->table.request = wrapped_request;
  self->table.adopt = wrapped_adopt;
  self->table.retire = wrapped_retire;
  self->table.close_work = wrapped_close;
  self->table.notify = wrapped_notify;
  self->table.log = wrapped_log;
  return Module::services_api.attach(instance, &self->table);
}

vs_result status(void *instance, vs_module_status *status) {
#ifdef FIXTURE_STATUS_WAITS
  std::lock_guard lock(processing);
#endif
  const auto result = Module::services_api.status(instance, status);
#ifdef FIXTURE_BAD_STATUS
  if (result == VS_OK) {
    std::memset(status->message, 'x', sizeof(status->message));
  }
#endif
  return result;
}

const vs_module_services_api services_api{sizeof(vs_module_services_api),
                                          VS_MODULE_SERVICES_ABI_VERSION, attach, status};
#else
const vs_module_services_api &services_api = Module::services_api;
#endif

#ifdef FIXTURE_WRAPS_SERVICES
vs_result locked_process(void *instance, const vs_process_block *block) {
#ifdef FIXTURE_STATUS_WAITS
  std::lock_guard lock(processing);
#endif
  const auto found = wrappers.find(instance);
  attached_services.store(found == wrappers.end() ? nullptr : found->second->host,
                          std::memory_order_relaxed);
  return Module::api.process(instance, block);
}
#endif
}

extern "C" VS_MODULE_EXPORT const vs_module_api *vespera_module_entry(uint32_t version) {
  return version == VS_MODULE_ABI_VERSION ? &module_api : nullptr;
}

extern "C" VS_MODULE_EXPORT const vs_module_state_api *
vespera_module_state_entry(uint32_t version) {
  return version == VS_MODULE_STATE_ABI_VERSION ? &state_api : nullptr;
}

extern "C" VS_MODULE_EXPORT const vs_module_parameter_api *
vespera_module_parameter_entry(uint32_t version) {
  return version == VS_MODULE_PARAMETER_ABI_VERSION ? &Module::parameter_api : nullptr;
}

extern "C" VS_MODULE_EXPORT const vs_module_metadata_api *
vespera_module_metadata_entry(uint32_t version) {
  return version == VS_MODULE_METADATA_ABI_VERSION ? &metadata_api : nullptr;
}

extern "C" VS_MODULE_EXPORT const vs_module_services_api *
vespera_module_services_entry(uint32_t version) {
  return version == VS_MODULE_SERVICES_ABI_VERSION ? &services_api : nullptr;
}
