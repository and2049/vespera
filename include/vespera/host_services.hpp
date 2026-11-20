#ifndef VESPERA_HOST_SERVICES_HPP
#define VESPERA_HOST_SERVICES_HPP

#include <vespera/module.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace vespera {

// How work requested through vs_host_services runs.
enum class HostWorkMode {
  // On shared worker threads; request, adopt and retire are real-time safe.
  threaded,
  // Inside request, on the requesting thread (VS_HOST_WORK_INLINE), so offline output does not
  // depend on timing.
  synchronous,
  // Only when run_one or run_pending is called, so tests decide exactly when work runs.
  manual
};

struct ModuleLogLine {
  uint32_t level = VS_LOG_INFO;
  std::string module;
  std::string message;
};

// Implements vs_host_services for every instance a host attaches it to. Each instance gets its
// own Client, which keeps the instance's owner (typically its loaded library) alive while any of
// its work exists, so no module code can run from an unloaded library. Must be owned by a
// std::shared_ptr; clients keep it alive.
//
// Per work, the hand-off into process uses one atomic mailbox for the newest ready result and
// one atomic cell for a result handed back, so adopt and retire never lock, allocate or free.
// Handed-back and superseded results are disposed on a worker or disposer thread (threaded), on
// the calling thread (synchronous), or in run_pending (manual).
class HostServices : public std::enable_shared_from_this<HostServices> {
public:
  struct Options {
    HostWorkMode mode = HostWorkMode::threaded;
    // Worker threads in threaded mode; one more thread disposes handed-back results.
    uint32_t workers = 2;
    // Counts service calls made outside the calls the contract allows, using the call scopes the
    // host declares with Client::Scope. Meant for conformance testing.
    bool audit = false;
  };

  struct Statistics {
    uint64_t opened = 0;
    uint64_t closed = 0;
    // Works the host closed because the module left them open at release.
    uint64_t left_open = 0;
    uint64_t requested = 0;
    // Requests merged into one that had not run yet.
    uint64_t coalesced = 0;
    uint64_t runs = 0;
    uint64_t failed_runs = 0;
    // Runs that returned after a newer request or close flagged them cancelled.
    uint64_t cancelled_runs = 0;
    // Results returned by runs, each of which must be disposed exactly once.
    uint64_t produced = 0;
    uint64_t delivered = 0;
    // Delivered results replaced by a newer one before they were adopted.
    uint64_t superseded = 0;
    uint64_t adopted = 0;
    // Results handed back by adopt or retire.
    uint64_t handed_back = 0;
    uint64_t disposed = 0;
    // Service calls outside the allowed calls; counted only with Options::audit.
    uint64_t log_in_process = 0;
    uint64_t open_outside_prepare = 0;
    uint64_t request_outside_call = 0;
    uint64_t adopt_outside_call = 0;
    uint64_t close_outside_release = 0;
  };

  using LogSink = std::function<void(const ModuleLogLine &)>;

  class Client;

  static std::shared_ptr<HostServices> create(Options options);
  // The process-wide threaded instance, created when first needed and kept while anyone holds it.
  static std::shared_ptr<HostServices> shared();

  ~HostServices();
  HostServices(const HostServices &) = delete;
  HostServices &operator=(const HostServices &) = delete;

  HostWorkMode mode() const noexcept { return options_.mode; }

  // A table for one instance. module names the instance in logs; resource_directory may be empty.
  std::shared_ptr<Client> client(std::shared_ptr<const void> owner, std::string module,
                                 std::string resource_directory);

  // Manual mode: runs the oldest queued request on the calling thread and returns true, or
  // returns false when none is queued.
  bool run_one();
  // Manual mode: runs queued requests until none is left and disposes handed-back results;
  // returns the number of runs.
  size_t run_pending();
  // Waits until no work of the given clients is queued or running, or until deadline. True when
  // idle. Synchronous mode is always idle.
  bool wait_idle(std::span<const Client *const> clients,
                 std::chrono::steady_clock::time_point deadline);
  // Waits until a run of any client is in progress, or until deadline.
  bool wait_running(std::chrono::steady_clock::time_point deadline);

  // Receives every accepted module log line, on the thread that logged it. Without a sink, lines
  // go to stderr when the environment sets VESPERA_MODULE_LOG=1.
  void set_log_sink(LogSink sink);
  Statistics statistics() const;

private:
  struct Work;
  class Signal;
  class Queue;

  explicit HostServices(Options options);
  void start();
  void worker_loop();
  void disposer_loop();
  bool execute(Work &work) noexcept;
  void deliver(Work &work, void *result, uint64_t token) noexcept;
  void dispose(Work &work, void *result) noexcept;
  void hand_back(Work &work, void *result) noexcept;
  void close(Work &work, void *const *held, uint32_t held_count) noexcept;
  void dispose_handed_back(Work &work) noexcept;
  void push_run(Work &work) noexcept;
  void push_dispose(Work &work) noexcept;
  void notify_waiters();
  void emit(const ModuleLogLine &line);

  static int run_cancelled(const vs_work_run *run);
  static void report_progress(const vs_work_run *run, float fraction);
  static vs_work *open_work(void *context, const vs_work_desc *desc);
  static vs_result request(void *context, vs_work *work, uint64_t token);
  static int adopt(void *context, vs_work *work, void **current, uint64_t *token);
  static int retire(void *context, vs_work *work, void *result);
  static void close_work(void *context, vs_work *work, void *const *held, uint32_t held_count);
  static void notify(void *context, uint32_t what);
  static void log(void *context, uint32_t level, const char *message);

  Options options_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::vector<std::unique_ptr<Work>> works_;
  std::unique_ptr<Queue> runs_;
  std::unique_ptr<Queue> disposals_;
  std::unique_ptr<Signal> run_signal_;
  std::unique_ptr<Signal> dispose_signal_;
  std::vector<std::thread> threads_;
  std::atomic<bool> stopping_{false};
  std::atomic<uint32_t> running_{0};
  LogSink sink_;
  struct Counters;
  std::unique_ptr<Counters> counters_;
};

// The services one instance sees. The host keeps it until the instance is destroyed.
class HostServices::Client {
public:
  // The calls a host makes on the instance. The host declares them around each call so that
  // open_work can require prepare and, with auditing, service calls can be checked.
  // run marks a work run, which the host itself declares.
  enum class Call { attach, prepare, process, release, control, run };

  class Scope {
  public:
    Scope(Client &client, Call call) noexcept;
    ~Scope();
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;

  private:
    friend class Client;
    friend class HostServices;
    const Client *client_;
    Call call_;
    Scope *previous_;
  };

  ~Client();
  Client(const Client &) = delete;
  Client &operator=(const Client &) = delete;

  const vs_host_services *descriptor() const noexcept { return &descriptor_; }
  const std::string &module() const noexcept { return module_; }
  // Reads and clears the VS_NOTIFY_* bits.
  uint32_t take_notices() noexcept { return notices_.exchange(0, std::memory_order_acq_rel); }
  size_t open_works() const;
  // Progress a running work last reported, or a negative value.
  float progress() const;
  // Closes every work the module left open, disposing what the host holds.
  void close_all() noexcept;

private:
  friend class HostServices;
  Client() = default;
  bool inside(Call call) const noexcept;
  bool inside_any() const noexcept;
  bool may_adopt() const noexcept;

  vs_host_services descriptor_{};
  std::shared_ptr<HostServices> services_;
  std::shared_ptr<const void> owner_;
  std::string module_;
  std::string resource_directory_;
  std::atomic<uint32_t> notices_{0};
  static constexpr size_t max_works = 8;
  std::array<Work *, max_works> works_{};
  size_t work_count_ = 0;
  std::chrono::steady_clock::time_point log_window_{};
  uint32_t log_lines_ = 0;
  uint32_t log_suppressed_ = 0;
};

}

#endif
