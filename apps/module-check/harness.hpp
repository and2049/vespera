#ifndef VESPERA_MODULE_CHECK_HARNESS_HPP
#define VESPERA_MODULE_CHECK_HARNESS_HPP

#include "module_check.hpp"

#include <vespera/files.hpp>
#include <vespera/host_services.hpp>
#include <vespera/module.h>
#include <vespera/parameter_display.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace vespera::module_check {

class CheckError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

struct Descriptor {
  uint32_t id = 0;
  std::string name;
  std::string unit;
  float minimum = 0.0f;
  float maximum = 0.0f;
  float default_value = 0.0f;
};

// A module binary opened through the public C ABI. The library stays loaded for the life of the
// process, as hosts keep module libraries loaded.
class Binary {
public:
  explicit Binary(const std::filesystem::path &path);

  std::filesystem::path path;
  // Why the binary cannot be driven at all; empty when it can.
  std::string unusable;
  bool entry_exported = false;
  const vs_module_api *api = nullptr;
  bool state_exported = false;
  bool parameters_exported = false;
  const vs_module_state_api *state = nullptr;
  const vs_module_parameter_api *parameters = nullptr;
  std::string state_error;
  std::string parameters_error;
  // The parameter metadata extension, validated as the host validates it. displays describes every
  // declared parameter, by its metadata when the binary exports valid metadata.
  bool metadata_exported = false;
  const vs_module_metadata_api *metadata = nullptr;
  std::string metadata_error;
  bool services_exported = false;
  const vs_module_services_api *services = nullptr;
  std::string services_error;
  const vs_module_files_api *files = nullptr;
  std::vector<FileSlotMetadata> file_slots;
  std::string files_error;
  std::vector<ParameterDisplay> displays;
  std::vector<Descriptor> descriptors;
  // The declared bypass parameter, when the declaration is consistent.
  std::optional<uint32_t> bypass;
  // For a module whose manifest declares routes: a parameter value that keeps it on a route where
  // it implements its own bypass, applied to every generated setting, or no_serial_route when the
  // host realises bypass in every route.
  struct RouteSetting {
    uint32_t parameter = 0;
    float value = 0.0f;
  };
  std::optional<RouteSetting> serial_route;
  bool no_serial_route = false;
  // Whether the bypass parameter is the module's own: false when the host realises it in every
  // route.
  bool own_bypass() const { return bypass.has_value() && !no_serial_route; }
  std::vector<uint32_t> channel_counts;

  const Descriptor *find(uint32_t id) const;
  const ParameterDisplay *display(uint32_t id) const;
};

using Audio = std::vector<std::vector<float>>;
using Settings = std::vector<std::pair<uint32_t, float>>;

// A parameter change at an absolute frame of a render.
struct Automation {
  uint64_t frame = 0;
  uint32_t id = 0;
  float value = 0.0f;
};

class Random {
public:
  explicit Random(uint64_t seed) : state_(seed) {}
  uint64_t next();
  double uniform();
  uint32_t below(uint32_t limit);

private:
  uint64_t state_;
};

// Allocation and free events observed inside process() calls since the process started.
uint64_t process_allocations();

// The host services the harness attaches to an instance: the host's own implementation, auditing
// every service call against the calls the harness declares, seen through a proxy table that lets
// checks observe and steer work. Work runs only when a check runs it (manual) or inside request
// (synchronous); allocations made by work running inline inside process are not counted as the
// module's.
class ServicesHost {
public:
  ServicesHost(const std::string &module, HostWorkMode mode);
  ~ServicesHost();
  ServicesHost(const ServicesHost &) = delete;
  ServicesHost &operator=(const ServicesHost &) = delete;

  const vs_host_services *table() const { return &table_; }
  HostServices &services() const { return *services_; }
  HostServices::Client &client() const { return *client_; }
  HostServices::Statistics statistics() const { return services_->statistics(); }
  // Runs and disposals of a work that started after the module closed it.
  uint64_t calls_after_close() const { return after_close_.load(); }
  // Asks again for a running work, which marks the run cancelled, and returns false when no run
  // is in progress.
  bool cancel_running();
  void cancel_next_run() { cancel_next_.store(true, std::memory_order_release); }
  // The median of the seconds runs took to return after cancel_running() cancelled them, which a
  // single descheduled run does not move; zero when none was cancelled.
  double typical_cancelled_return() const;
  // Holds the processing thread inside its next adopt or retire until release_hold(), so that
  // checks can call into the instance while process is known to be in progress.
  void hold_processing();
  bool wait_processing_held(std::chrono::steady_clock::time_point deadline);
  void release_hold();

private:
  struct Work {
    ServicesHost *host = nullptr;
    vs_work_desc desc{};
    vs_work *handle = nullptr;
    std::atomic<bool> closed{false};
  };

  static vs_work *open_work(void *context, const vs_work_desc *desc);
  static vs_result request(void *context, vs_work *work, uint64_t token);
  static int adopt(void *context, vs_work *work, void **current, uint64_t *token);
  static int retire(void *context, vs_work *work, void *result);
  static void close_work(void *context, vs_work *work, void *const *held, uint32_t count);
  static void notify(void *context, uint32_t what);
  static void log(void *context, uint32_t level, const char *message);
  static vs_result run(void *context, const vs_work_run *run, void **result);
  static void dispose(void *context, void *result);
  void hold_point();

  std::shared_ptr<HostServices> services_;
  std::shared_ptr<HostServices::Client> client_;
  const vs_host_services *inner_ = nullptr;
  vs_host_services table_{};
  std::deque<Work> works_;
  std::atomic<uint64_t> after_close_{0};
  std::atomic<bool> cancel_next_{false};
  std::mutex mutex_;
  std::condition_variable changed_;
  Work *running_ = nullptr;
  std::optional<std::chrono::steady_clock::time_point> cancelled_at_;
  std::vector<double> cancelled_returns_;
  std::atomic<bool> hold_{false};
  bool held_ = false;
};

class Instance {
public:
  // Modules exporting the services extension get a ServicesHost in the given mode.
  explicit Instance(const Binary &binary, HostWorkMode mode = HostWorkMode::synchronous);
  ~Instance();
  Instance(const Instance &) = delete;
  Instance &operator=(const Instance &) = delete;

  void *handle() const { return handle_; }
  bool prepared() const { return prepared_; }
  vs_result try_prepare(const vs_prepare_info *info, vs_module_timing *timing);
  vs_module_timing prepare(double rate, uint32_t channels, uint32_t max_frames);
  void release();
  void reset();
  // Applies values through the parameter value extension, or as events at the first rendered
  // frame when the module has none.
  void configure(const Settings &settings);
  std::optional<float> value(uint32_t id) const;
  vs_result process(const vs_process_block &block);
  // Renders input in consecutive blocks of the given sizes. Automation frames are absolute within
  // this render and sorted; flags apply to the first block only.
  // after_block, when given, runs after every block.
  Audio render(const Audio &input, const std::vector<uint32_t> &blocks,
               const std::vector<Automation> &automation = {}, uint32_t first_flags = 0,
               const std::function<void()> &after_block = {});
  // The attached services, or null for a module without the extension.
  ServicesHost *services() const { return services_.get(); }
  // The module's status through the services extension, called on this thread.
  std::optional<vs_module_status> status() const;

private:
  // Declares a call on the instance to its services, when it has them.
  class Call {
  public:
    Call(const ServicesHost *host, HostServices::Client::Call call) {
      if (host) {
        scope_.emplace(host->client(), call);
      }
    }

  private:
    std::optional<HostServices::Client::Scope> scope_;
  };

  const Binary &binary_;
  std::unique_ptr<ServicesHost> services_;
  void *handle_ = nullptr;
  bool prepared_ = false;
  uint32_t channels_ = 0;
  uint64_t position_ = 0;
  std::vector<Automation> pending_;
};

std::vector<uint32_t> fixed_blocks(size_t frames, uint32_t size);
std::vector<uint32_t> random_blocks(size_t frames, uint32_t maximum, Random &random);
// Blocks of at most size frames, additionally split so that every automation frame starts a block.
std::vector<uint32_t> split_blocks(size_t frames, uint32_t size,
                                   const std::vector<Automation> &automation);

// Program material: a gliding sine, a second partial, noise and sparse impulses, below full scale.
Audio test_signal(uint32_t channels, size_t frames, double rate, uint64_t seed);
Audio silence(uint32_t channels, size_t frames);

struct Difference {
  bool identical = true;
  double maximum = 0.0;
  size_t frame = 0;
  uint32_t channel = 0;
};

// Compares frames from `from` onwards; non-finite mismatches count as an infinite difference.
Difference compare(const Audio &a, const Audio &b, size_t from = 0);
bool within(const Difference &difference, float tolerance);
std::string describe(const Difference &difference);
bool finite(const Audio &audio, size_t *frame = nullptr);

Settings random_settings(const Binary &binary, Random &random);
Settings endpoint_settings(const Binary &binary, bool maximum);
std::string result_name(vs_result result);
std::string number(double value);
std::string range_text(float minimum, float maximum);

}

#endif
