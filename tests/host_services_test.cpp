// Host services: work coalescing, cancellation, the real-time hand-off into process, disposal,
// the three execution modes, owner lifetime, notices, logging and call auditing.
#include <vespera/host_services.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <new>
#include <set>
#include <source_location>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> counting{false};
thread_local bool counted_thread = false;
std::atomic<uint64_t> allocations{0};

}

void *operator new(std::size_t size) {
  if (counted_thread && counting.load(std::memory_order_relaxed)) {
    allocations.fetch_add(1, std::memory_order_relaxed);
  }
  if (void *memory = std::malloc(size == 0 ? 1 : size)) {
    return memory;
  }
  throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return operator new(size); }
void operator delete(void *memory) noexcept {
  if (memory && counted_thread && counting.load(std::memory_order_relaxed)) {
    allocations.fetch_add(1, std::memory_order_relaxed);
  }
  std::free(memory);
}
void operator delete[](void *memory) noexcept { operator delete(memory); }
void operator delete(void *memory, std::size_t) noexcept { operator delete(memory); }
void operator delete[](void *memory, std::size_t) noexcept { operator delete(memory); }

namespace {

using vespera::HostServices;
using vespera::HostWorkMode;
using Call = HostServices::Client::Call;
using Scope = HostServices::Client::Scope;
using Clock = std::chrono::steady_clock;

int failures = 0;

void check(bool condition, std::source_location location = std::source_location::current()) {
  if (!condition) {
    std::fprintf(stderr, "host services check failed at line %u\n",
                 static_cast<unsigned>(location.line()));
    ++failures;
  }
}

Clock::time_point soon(int milliseconds = 5000) {
  return Clock::now() + std::chrono::milliseconds(milliseconds);
}

// A result: the token it was built for.
struct Result {
  uint64_t token;
};

// The work's context. Runs build a Result for their token. A run may be held until released, to
// make it observable while running.
struct Builder {
  std::mutex mutex;
  std::condition_variable changed;
  bool hold = false;
  bool held = false;
  bool honour_cancel = true;
  bool fail = false;
  bool null_result = false;
  std::vector<uint64_t> runs;
  std::atomic<uint64_t> disposed{0};
  std::atomic<uint64_t> disposed_elsewhere{0};
  std::thread::id forbidden_disposal_thread{};
  std::atomic<bool> returned{false};

  static vs_result run(void *context, const vs_work_run *run, void **result) {
    auto &self = *static_cast<Builder *>(context);
    std::unique_lock lock(self.mutex);
    self.runs.push_back(run->token);
    self.returned = false;
    if (self.hold) {
      self.held = true;
      self.changed.notify_all();
      const auto deadline = soon();
      while (self.hold && !(self.honour_cancel && run->cancelled(run)) && Clock::now() < deadline) {
        self.changed.wait_for(lock, std::chrono::milliseconds(1));
      }
      self.held = false;
    }
    run->progress(run, 1.0f);
    self.returned = true;
    if (self.fail) {
      return VS_ERROR_PROCESS;
    }
    *result = self.null_result ? nullptr : new Result{run->token};
    return VS_OK;
  }

  static void dispose(void *context, void *result) {
    auto &self = *static_cast<Builder *>(context);
    if (std::this_thread::get_id() == self.forbidden_disposal_thread) {
      self.disposed_elsewhere.fetch_add(1);
    }
    delete static_cast<Result *>(result);
    self.disposed.fetch_add(1);
  }

  void release_hold() {
    std::lock_guard lock(mutex);
    hold = false;
    changed.notify_all();
  }

  bool wait_held() {
    std::unique_lock lock(mutex);
    return changed.wait_until(lock, soon(), [&] { return held; });
  }

  vs_work_desc desc() { return {sizeof(vs_work_desc), this, run, dispose}; }
};

struct Instance {
  std::shared_ptr<HostServices> services;
  std::shared_ptr<HostServices::Client> client;
  const vs_host_services *table;

  explicit Instance(std::shared_ptr<HostServices> s, std::shared_ptr<const void> owner = nullptr)
      : services(std::move(s)), client(services->client(std::move(owner), "test.module", "")),
        table(client->descriptor()) {}

  vs_work *open(Builder &builder) {
    Scope scope(*client, Call::prepare);
    const vs_work_desc desc = builder.desc();
    return table->open_work(table->context, &desc);
  }
  vs_result request(vs_work *work, uint64_t token) {
    Scope scope(*client, Call::process);
    return table->request(table->context, work, token);
  }
  int adopt(vs_work *work, void **current, uint64_t *token) {
    Scope scope(*client, Call::process);
    return table->adopt(table->context, work, current, token);
  }
  int retire(vs_work *work, void *result) {
    Scope scope(*client, Call::process);
    return table->retire(table->context, work, result);
  }
  void close(vs_work *work, void *current) {
    Scope scope(*client, Call::release);
    void *const held[] = {current};
    table->close_work(table->context, work, held, 1);
  }
};

uint64_t token_of(void *result) { return static_cast<Result *>(result)->token; }

HostServices::Options mode(HostWorkMode m, bool audit = false) {
  HostServices::Options options;
  options.mode = m;
  options.audit = audit;
  return options;
}

void balanced(const HostServices &services, const Builder &builder) {
  const auto stats = services.statistics();
  check(stats.produced == stats.disposed);
  check(builder.disposed.load() == stats.disposed);
  check(stats.opened == stats.closed);
}

void test_descriptor() {
  auto services = HostServices::create(mode(HostWorkMode::manual));
  auto client = services->client(nullptr, "test.module", "/tmp/resources");
  const vs_host_services &table = *client->descriptor();
  check(table.struct_size == sizeof(vs_host_services));
  check(table.abi_version == VS_MODULE_SERVICES_ABI_VERSION);
  check(table.flags == 0);
  check(std::string(table.resource_directory) == "/tmp/resources");
  check(table.open_work && table.request && table.adopt && table.retire && table.close_work &&
        table.notify && table.log);
  auto inline_services = HostServices::create(mode(HostWorkMode::synchronous));
  auto inline_client = inline_services->client(nullptr, "test.module", "");
  check(inline_client->descriptor()->flags == VS_HOST_WORK_INLINE);
  check(inline_client->descriptor()->resource_directory == nullptr);
}

void test_open_rules() {
  auto services = HostServices::create(mode(HostWorkMode::manual, true));
  Instance instance(services);
  Builder builder;
  const vs_work_desc desc = builder.desc();
  check(instance.table->open_work(instance.table->context, &desc) == nullptr);
  check(services->statistics().open_outside_prepare == 1);
  {
    Scope scope(*instance.client, Call::process);
    check(instance.table->open_work(instance.table->context, &desc) == nullptr);
  }
  Scope scope(*instance.client, Call::prepare);
  vs_work_desc broken = desc;
  broken.dispose = nullptr;
  check(instance.table->open_work(instance.table->context, &broken) == nullptr);
  broken = desc;
  broken.struct_size = sizeof(vs_work_desc) - 1;
  check(instance.table->open_work(instance.table->context, &broken) == nullptr);
  std::vector<vs_work *> works;
  for (int i = 0; i < 8; ++i) {
    works.push_back(instance.table->open_work(instance.table->context, &desc));
    check(works.back() != nullptr);
  }
  check(instance.table->open_work(instance.table->context, &desc) == nullptr);
  check(instance.client->open_works() == 8);
  for (vs_work *work : works) {
    instance.table->close_work(instance.table->context, work, nullptr, 0);
  }
  check(instance.client->open_works() == 0);
  check(instance.table->request(instance.table->context, works[0], 1) == VS_ERROR_STATE);
}

void test_inline() {
  auto services = HostServices::create(mode(HostWorkMode::synchronous));
  Builder builder;
  {
    Instance instance(services);
    vs_work *work = instance.open(builder);
    void *current = nullptr;
    uint64_t token = 0;
    check(instance.adopt(work, &current, &token) == 0);
    check(instance.request(work, 1) == VS_OK);
    check(builder.runs.size() == 1);
    check(instance.adopt(work, &current, &token) == 1 && token == 1 && token_of(current) == 1);
    check(instance.request(work, 2) == VS_OK);
    check(instance.request(work, 3) == VS_OK);
    check(builder.runs == std::vector<uint64_t>({1, 2, 3}));
    check(services->statistics().superseded == 1);
    check(instance.adopt(work, &current, &token) == 1 && token == 3);
    check(builder.disposed == 2);
    void *other = nullptr;
    check(instance.request(work, 4) == VS_OK);
    check(instance.adopt(work, &other, &token) == 1);
    check(instance.retire(work, current) == 1);
    check(builder.disposed == 3);
    instance.close(work, other);
    check(builder.disposed == 4);
    check(services->wait_idle({}, Clock::now()));
  }
  balanced(*services, builder);
}

void test_manual_coalescing() {
  auto services = HostServices::create(mode(HostWorkMode::manual));
  Builder builder;
  {
    Instance instance(services);
    vs_work *work = instance.open(builder);
    check(instance.request(work, 1) == VS_OK);
    check(instance.request(work, 2) == VS_OK);
    check(instance.request(work, 3) == VS_OK);
    check(builder.runs.empty());
    const HostServices::Client *clients[] = {instance.client.get()};
    check(!services->wait_idle(clients, Clock::now()));
    check(services->run_pending() == 1);
    check(services->wait_idle(clients, Clock::now()));
    check(builder.runs == std::vector<uint64_t>({3}));
    check(services->statistics().coalesced == 2);
    void *current = nullptr;
    uint64_t token = 0;
    check(instance.adopt(work, &current, &token) == 1 && token == 3);
    check(instance.adopt(work, &current, &token) == 0);
    check(instance.request(work, 4) == VS_OK);
    services->run_pending();
    check(instance.adopt(work, &current, &token) == 1 && token == 4);
    // The handed-back result waits for run_pending; until it is disposed no further result that
    // would hand back another can be adopted.
    check(builder.disposed == 0);
    check(instance.request(work, 5) == VS_OK);
    check(services->run_one());
    check(!services->run_one());
    check(instance.adopt(work, &current, &token) == 0);
    check(instance.retire(work, current) == 0);
    services->run_pending();
    check(builder.disposed == 1);
    check(instance.adopt(work, &current, &token) == 1 && token == 5);
    instance.close(work, current);
    services->run_pending();
  }
  balanced(*services, builder);
}

void test_failures_deliver_nothing() {
  auto services = HostServices::create(mode(HostWorkMode::synchronous));
  std::vector<vespera::ModuleLogLine> lines;
  services->set_log_sink([&](const vespera::ModuleLogLine &line) { lines.push_back(line); });
  Builder builder;
  builder.fail = true;
  {
    Instance instance(services);
    vs_work *work = instance.open(builder);
    check(instance.request(work, 1) == VS_OK);
    void *current = nullptr;
    uint64_t token = 0;
    check(instance.adopt(work, &current, &token) == 0);
    check(services->statistics().failed_runs == 1);
    check(lines.size() == 1 && lines[0].level == VS_LOG_WARNING &&
          lines[0].module == "test.module");
    instance.close(work, nullptr);
  }
  balanced(*services, builder);
}

void test_successful_null_clears_current() {
  auto services = HostServices::create(mode(HostWorkMode::manual));
  Builder builder;
  Instance instance(services);
  vs_work *work = instance.open(builder);
  void *current = nullptr;
  uint64_t token = 0;
  instance.request(work, 1);
  services->run_pending();
  check(instance.adopt(work, &current, &token) == 1 && current != nullptr);
  builder.null_result = true;
  instance.request(work, 2);
  services->run_pending();
  check(instance.adopt(work, &current, &token) == 1 && current == nullptr && token == 2);
  services->run_pending();
  instance.close(work, current);
  balanced(*services, builder);
}

void test_cancel_running() {
  auto services = HostServices::create(mode(HostWorkMode::manual));
  Builder builder;
  {
    Instance instance(services);
    vs_work *work = instance.open(builder);
    builder.hold = true;
    check(instance.request(work, 1) == VS_OK);
    std::thread runner([&] { services->run_one(); });
    check(builder.wait_held());
    check(services->wait_running(soon()));
    check(instance.request(work, 2) == VS_OK);
    runner.join();
    check(builder.returned);
    check(services->statistics().cancelled_runs == 1);
    builder.hold = false;
    check(services->run_pending() == 1);
    check(builder.runs == std::vector<uint64_t>({1, 2}));
    void *current = nullptr;
    uint64_t token = 0;
    check(instance.adopt(work, &current, &token) == 1 && token == 2);
    check(services->statistics().superseded == 0);
    instance.close(work, current);
    services->run_pending();
  }
  balanced(*services, builder);
}

void test_close_while_running() {
  auto services = HostServices::create(mode(HostWorkMode::manual));
  Builder builder;
  {
    Instance instance(services);
    vs_work *work = instance.open(builder);
    builder.hold = true;
    check(instance.request(work, 7) == VS_OK);
    std::thread runner([&] { services->run_one(); });
    check(builder.wait_held());
    const auto start = Clock::now();
    instance.close(work, nullptr);
    const auto took = Clock::now() - start;
    check(builder.returned);
    // Only a hang detector: the run polls cancellation every millisecond, but scheduling under
    // load can delay its return.
    check(took < std::chrono::seconds(2));
    runner.join();
    check(services->statistics().cancelled_runs == 1);
    check(instance.request(work, 8) == VS_ERROR_STATE);
    check(services->run_pending() == 0);
    check(builder.runs.size() == 1);
  }
  balanced(*services, builder);
}

void test_close_with_queued_work() {
  auto services = HostServices::create(mode(HostWorkMode::manual));
  Builder builder;
  std::weak_ptr<int> owner;
  {
    auto library = std::make_shared<int>(1);
    owner = library;
    Instance instance(services, std::move(library));
    vs_work *work = instance.open(builder);
    check(instance.request(work, 1) == VS_OK);
    check(!owner.expired());
    instance.client.reset();
    check(owner.expired());
    check(services->statistics().left_open == 1);
    check(services->run_pending() == 0);
    check(builder.runs.empty());
  }
  // The work's slot is reused once its stale queue entry is consumed.
  {
    Instance instance(services);
    vs_work *work = instance.open(builder);
    check(work != nullptr);
    check(instance.request(work, 2) == VS_OK);
    check(services->run_pending() == 1);
    instance.close(work, nullptr);
  }
  balanced(*services, builder);
}

void test_owner_kept_until_closed() {
  auto services = HostServices::create(mode(HostWorkMode::threaded));
  Builder builder;
  std::weak_ptr<int> owner;
  {
    auto library = std::make_shared<int>(1);
    owner = library;
    Instance instance(services, std::move(library));
    vs_work *work = instance.open(builder);
    builder.hold = true;
    check(instance.request(work, 1) == VS_OK);
    check(builder.wait_held());
    check(!owner.expired());
    // Destroying the client closes the running work: the run is cancelled and waited for
    // before the owner is released.
    instance.client.reset();
    check(builder.returned);
    check(owner.expired());
  }
  balanced(*services, builder);
}

void test_threaded_disposal_off_the_processing_thread() {
  auto services = HostServices::create(mode(HostWorkMode::threaded));
  Builder builder;
  builder.forbidden_disposal_thread = std::this_thread::get_id();
  {
    Instance instance(services);
    vs_work *work = instance.open(builder);
    const HostServices::Client *clients[] = {instance.client.get()};
    void *current = nullptr;
    uint64_t token = 0;
    for (uint64_t t = 1; t <= 50; ++t) {
      check(instance.request(work, t) == VS_OK);
      check(services->wait_idle(clients, soon()));
      const auto deadline = soon();
      while (instance.adopt(work, &current, &token) == 0 && Clock::now() < deadline) {
        std::this_thread::yield();
      }
      check(token == t);
    }
    check(builder.disposed_elsewhere == 0);
    instance.close(work, current);
  }
  check(builder.disposed == 50);
  balanced(*services, builder);
}

// The processing-thread services must not allocate or free.
void test_realtime_calls_do_not_allocate() {
  static_assert(std::atomic<void *>::is_always_lock_free);
  static_assert(std::atomic<uint64_t>::is_always_lock_free);
  static_assert(std::atomic<uint32_t>::is_always_lock_free);
  auto services = HostServices::create(mode(HostWorkMode::threaded));
  Builder builder;
  {
    Instance instance(services);
    vs_work *work = instance.open(builder);
    const HostServices::Client *clients[] = {instance.client.get()};
    void *current = nullptr;
    uint64_t token = 0;
    const vs_host_services &table = *instance.table;
    for (uint64_t t = 1; t <= 200; ++t) {
      counted_thread = true;
      counting = true;
      table.request(table.context, work, t);
      table.request(table.context, work, t);
      table.notify(table.context, VS_NOTIFY_STATUS);
      counting = false;
      counted_thread = false;
      services->wait_idle(clients, soon());
      counted_thread = true;
      counting = true;
      for (int attempt = 0;
           attempt < 1000000 && table.adopt(table.context, work, &current, &token) == 0;
           ++attempt) {
      }
      void *spare = nullptr;
      table.retire(table.context, work, spare);
      counting = false;
      counted_thread = false;
    }
    check(allocations == 0);
    check(instance.client->take_notices() == VS_NOTIFY_STATUS);
    check(instance.client->take_notices() == 0);
    instance.close(work, current);
  }
  balanced(*services, builder);
}

// A processing thread requests and adopts every block while two workers build, for many blocks,
// then closes. Adopted tokens never decrease (a request racing the start of a run may run its
// token twice) and every result is disposed exactly once.
void test_stress() {
  auto services = HostServices::create(mode(HostWorkMode::threaded));
  struct Stress {
    static vs_result run(void *, const vs_work_run *run, void **result) {
      volatile uint64_t spin = 0;
      for (uint64_t i = 0; i < (run->token % 7) * 200 && !run->cancelled(run); ++i) {
        spin = spin + i;
      }
      *result = new Result{run->token};
      return VS_OK;
    }
    static void dispose(void *context, void *result) {
      static_cast<std::atomic<uint64_t> *>(context)->fetch_add(1);
      delete static_cast<Result *>(result);
    }
  };
  std::atomic<uint64_t> disposed{0};
  const vs_work_desc desc{sizeof(vs_work_desc), &disposed, Stress::run, Stress::dispose};
  constexpr int instances = 6;
  std::vector<std::unique_ptr<Instance>> clients;
  std::vector<vs_work *> works;
  for (int i = 0; i < instances; ++i) {
    clients.push_back(std::make_unique<Instance>(services));
    Scope scope(*clients.back()->client, Call::prepare);
    works.push_back(clients.back()->table->open_work(clients.back()->table->context, &desc));
    check(works.back() != nullptr);
  }
  std::atomic<bool> monotonic{true};
  std::vector<void *> currents(instances, nullptr);
  std::thread audio([&] {
    std::vector<uint64_t> last(instances, 0);
    for (uint64_t block = 1; block <= 20000; ++block) {
      for (int i = 0; i < instances; ++i) {
        Scope scope(*clients[static_cast<size_t>(i)]->client, Call::process);
        const vs_host_services &table = *clients[static_cast<size_t>(i)]->table;
        uint64_t token = 0;
        if (table.adopt(table.context, works[static_cast<size_t>(i)],
                        &currents[static_cast<size_t>(i)], &token)) {
          if (token < last[static_cast<size_t>(i)]) {
            monotonic = false;
          }
          last[static_cast<size_t>(i)] = token;
        }
        if (block % (static_cast<uint64_t>(i) + 2) == 0) {
          table.request(table.context, works[static_cast<size_t>(i)], block);
        }
      }
    }
  });
  audio.join();
  for (int i = 0; i < instances; ++i) {
    clients[static_cast<size_t>(i)]->close(works[static_cast<size_t>(i)],
                                           currents[static_cast<size_t>(i)]);
  }
  clients.clear();
  check(monotonic);
  const auto stats = services->statistics();
  check(stats.produced == stats.disposed);
  check(disposed == stats.disposed);
  check(stats.runs > 0 && stats.adopted > 0);
  check(stats.opened == stats.closed && stats.left_open == 0);
}

void test_wait_idle_threaded() {
  auto services = HostServices::create(mode(HostWorkMode::threaded));
  Builder builder;
  {
    Instance instance(services);
    vs_work *work = instance.open(builder);
    const HostServices::Client *clients[] = {instance.client.get()};
    builder.hold = true;
    builder.honour_cancel = false;
    check(instance.request(work, 1) == VS_OK);
    check(builder.wait_held());
    check(!services->wait_idle(clients, Clock::now() + std::chrono::milliseconds(20)));
    builder.release_hold();
    check(services->wait_idle(clients, soon()));
    void *current = nullptr;
    uint64_t token = 0;
    check(instance.adopt(work, &current, &token) == 1 && token == 1);
    instance.close(work, current);
  }
  balanced(*services, builder);
}

void test_shared_instance() {
  auto first = HostServices::shared();
  auto second = HostServices::shared();
  check(first == second && first->mode() == HostWorkMode::threaded);
}

void test_retirement_stays_occupied_during_disposal() {
  struct Slow {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool finish = false;
    static vs_result run(void *, const vs_work_run *run, void **result) {
      *result = new Result{run->token};
      return VS_OK;
    }
    static void dispose(void *context, void *result) {
      auto &self = *static_cast<Slow *>(context);
      std::unique_lock lock(self.mutex);
      self.entered = true;
      self.changed.notify_all();
      self.changed.wait_until(lock, soon(), [&] { return self.finish; });
      delete static_cast<Result *>(result);
    }
  } slow;
  auto services = HostServices::create(mode(HostWorkMode::threaded));
  Instance instance(services);
  const vs_work_desc desc{sizeof(desc), &slow, Slow::run, Slow::dispose};
  vs_work *work;
  {
    Scope scope(*instance.client, Call::prepare);
    work = instance.table->open_work(instance.table->context, &desc);
  }
  const HostServices::Client *clients[] = {instance.client.get()};
  void *first = nullptr;
  uint64_t token = 0;
  check(instance.request(work, 1) == VS_OK);
  check(services->wait_idle(clients, soon()));
  check(instance.adopt(work, &first, &token) == 1);
  check(instance.retire(work, first) == 1);
  {
    std::unique_lock lock(slow.mutex);
    check(slow.changed.wait_until(lock, soon(), [&] { return slow.entered; }));
  }
  check(instance.request(work, 2) == VS_OK);
  check(services->wait_idle(clients, soon()));
  void *second = nullptr;
  check(instance.adopt(work, &second, &token) == 1);
  check(instance.retire(work, second) == 0);
  {
    std::lock_guard lock(slow.mutex);
    slow.finish = true;
    slow.changed.notify_all();
  }
  instance.close(work, second);
  check(services->statistics().disposed == 2);
}

void test_audit() {
  auto services = HostServices::create(mode(HostWorkMode::manual, true));
  std::vector<vespera::ModuleLogLine> lines;
  services->set_log_sink([&](const vespera::ModuleLogLine &line) { lines.push_back(line); });
  Builder builder;
  Instance instance(services);
  vs_work *work = instance.open(builder);
  const vs_host_services &table = *instance.table;
  table.request(table.context, work, 1);
  check(services->statistics().request_outside_call == 1);
  void *current = nullptr;
  uint64_t token = 0;
  {
    Scope scope(*instance.client, Call::run);
    table.adopt(table.context, work, &current, &token);
  }
  check(services->statistics().adopt_outside_call == 1);
  {
    Scope scope(*instance.client, Call::process);
    table.log(table.context, VS_LOG_INFO, "from process");
  }
  check(services->statistics().log_in_process == 1);
  check(lines.empty());
  {
    Scope scope(*instance.client, Call::control);
    table.log(table.context, VS_LOG_INFO, "from control");
  }
  check(lines.size() == 1);
  table.close_work(table.context, work, nullptr, 0);
  check(services->statistics().close_outside_release == 1);
  services->run_pending();
}

void test_log() {
  auto services = HostServices::create(mode(HostWorkMode::manual));
  std::vector<vespera::ModuleLogLine> lines;
  services->set_log_sink([&](const vespera::ModuleLogLine &line) { lines.push_back(line); });
  Instance instance(services);
  const vs_host_services &table = *instance.table;
  table.log(table.context, 9, "bad \xff byte\nnext");
  check(lines.size() == 1 && lines[0].level == VS_LOG_ERROR &&
        lines[0].message == "bad ? byte?next");
  std::string long_message(5000, 'x');
  table.log(table.context, VS_LOG_DEBUG, long_message.c_str());
  check(lines.size() == 2 && lines[1].message.size() == 1024);
  table.log(table.context, VS_LOG_INFO, nullptr);
  for (int i = 0; i < 40; ++i) {
    table.log(table.context, VS_LOG_INFO, "repeated");
  }
  check(lines.size() == 20);
}

}

int main() {
  test_descriptor();
  test_open_rules();
  test_inline();
  test_manual_coalescing();
  test_failures_deliver_nothing();
  test_successful_null_clears_current();
  test_cancel_running();
  test_close_while_running();
  test_close_with_queued_work();
  test_owner_kept_until_closed();
  test_threaded_disposal_off_the_processing_thread();
  test_realtime_calls_do_not_allocate();
  test_stress();
  test_wait_idle_threaded();
  test_shared_instance();
  test_retirement_stays_occupied_during_disposal();
  test_audit();
  test_log();
  if (failures != 0) {
    std::fprintf(stderr, "%d host services checks failed\n", failures);
    return 1;
  }
  std::puts("host services tests passed");
  return 0;
}
