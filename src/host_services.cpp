#include <vespera/host_services.hpp>
#include <vespera/parameter_display.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string_view>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <pthread.h>
#include <sys/qos.h>
#else
#include <pthread.h>
#include <semaphore.h>
#endif

namespace vespera {
namespace {

// Large enough for two full graphs of 128 nodes with 8 works each, so a bounded queue entry per
// work always fits and request never fails because of load.
constexpr size_t max_works = 4096;
constexpr size_t max_log_bytes = 1024;
constexpr uint32_t log_lines_per_second = 20;
constexpr uint32_t known_notices = VS_NOTIFY_STATUS | VS_NOTIFY_TIMING;
static_assert(std::atomic<void *>::is_always_lock_free);
static_assert(std::atomic<size_t>::is_always_lock_free);
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::atomic<uint32_t>::is_always_lock_free);

thread_local HostServices::Client::Scope *current_scope = nullptr;

// Copies at most max_log_bytes of message, replacing bytes that are not well-formed UTF-8.
std::string sanitise(const char *message) {
  const std::string_view text(message, strnlen(message, max_log_bytes));
  std::string result;
  result.reserve(text.size());
  size_t i = 0;
  while (i < text.size()) {
    const display_detail::Utf8Sequence sequence = display_detail::decode_utf8(text, i);
    if (sequence.valid && (sequence.code_point >= 0x20 || sequence.code_point == '\t')) {
      result.append(text.substr(i, sequence.length));
      i += sequence.length;
    } else {
      result.push_back('?');
      i += sequence.valid ? sequence.length : 1;
    }
  }
  return result;
}

}

// Explicit states of one work. A request moves idle to queued; a worker moves queued to running
// and back to idle when the run returns. A request during a run moves it to superseded, which
// marks the run cancelled and queues the newer request when it returns. close_work moves any
// state to closing and, once no run is in progress, to closed. A closed work whose queue entries
// have all been consumed may be reused.
enum class WorkPhase : uint32_t {
  free,
  idle,
  queued,
  running,
  superseded,
  closing,
  joined,
  closed
};
static_assert(std::atomic<WorkPhase>::is_always_lock_free);

struct HostServices::Work {
  struct Slot {
    void *result = nullptr;
    uint64_t token = 0;
    std::atomic<bool> busy{false};
  };

  std::atomic<WorkPhase> phase{WorkPhase::free};
  vs_work_desc desc{};
  Client *client = nullptr;
  std::atomic<uint64_t> token{0};
  // A delivered result is written to a free slot and published through ready. Three slots
  // suffice: one may be in ready and one being read by adopt.
  std::array<Slot, 3> slots;
  std::atomic<Slot *> ready{nullptr};
  // A result handed back by adopt or retire, waiting to be disposed.
  std::atomic<void *> handed_back{nullptr};
  // Queue entries naming this work, and disposals of its results in progress.
  std::atomic<uint32_t> references{0};
  std::atomic<uint32_t> disposing{0};
  std::atomic<float> progress{-1.0f};

  bool reusable() const noexcept {
    const WorkPhase state = phase.load(std::memory_order_acquire);
    return state == WorkPhase::free ||
           (state == WorkPhase::closed && references.load(std::memory_order_acquire) == 0 &&
            disposing.load(std::memory_order_acquire) == 0);
  }
};

struct HostServices::Counters {
  std::atomic<uint64_t> opened{0}, closed{0}, left_open{0}, requested{0}, coalesced{0}, runs{0},
      failed_runs{0}, cancelled_runs{0}, produced{0}, delivered{0}, superseded{0}, adopted{0},
      handed_back{0}, disposed{0}, log_in_process{0}, open_outside_prepare{0},
      request_outside_call{0}, adopt_outside_call{0}, close_outside_release{0};
};

namespace {

void count(std::atomic<uint64_t> &counter) noexcept {
  counter.fetch_add(1, std::memory_order_relaxed);
}

}

// Wakes a sleeping thread. Signalling neither locks nor allocates, so the processing thread may
// signal.
class HostServices::Signal {
public:
  Signal() {
#if defined(__APPLE__)
    if (semaphore_create(mach_task_self(), &semaphore_, SYNC_POLICY_FIFO, 0) != KERN_SUCCESS) {
      throw std::runtime_error("cannot create a work semaphore");
    }
#else
    if (sem_init(&semaphore_, 0, 0) != 0) {
      throw std::runtime_error("cannot create a work semaphore");
    }
#endif
  }
  ~Signal() {
#if defined(__APPLE__)
    semaphore_destroy(mach_task_self(), semaphore_);
#else
    sem_destroy(&semaphore_);
#endif
  }
  Signal(const Signal &) = delete;
  Signal &operator=(const Signal &) = delete;

  void post() noexcept {
#if defined(__APPLE__)
    semaphore_signal(semaphore_);
#else
    sem_post(&semaphore_);
#endif
  }

  void wait() noexcept {
#if defined(__APPLE__)
    while (semaphore_wait(semaphore_) == KERN_ABORTED) {
    }
#else
    while (sem_wait(&semaphore_) != 0) {
    }
#endif
  }

private:
#if defined(__APPLE__)
  semaphore_t semaphore_{};
#else
  sem_t semaphore_{};
#endif
};

// Bounded multi-producer, multi-consumer queue of works (Vyukov's sequence-numbered ring). push
// and pop neither lock nor allocate; a producer never waits for a consumer.
class HostServices::Queue {
public:
  explicit Queue(size_t capacity) : cells_(new Cell[capacity]), mask_(capacity - 1) {
    for (size_t i = 0; i < capacity; ++i) {
      cells_[i].sequence.store(i, std::memory_order_relaxed);
    }
  }

  bool push(Work *work) noexcept {
    size_t position = enqueue_.load(std::memory_order_relaxed);
    Cell *cell = nullptr;
    for (;;) {
      cell = &cells_[position & mask_];
      const size_t sequence = cell->sequence.load(std::memory_order_acquire);
      const auto difference =
          static_cast<std::ptrdiff_t>(sequence) - static_cast<std::ptrdiff_t>(position);
      if (difference == 0) {
        if (enqueue_.compare_exchange_weak(position, position + 1, std::memory_order_relaxed)) {
          break;
        }
      } else if (difference < 0) {
        return false;
      } else {
        position = enqueue_.load(std::memory_order_relaxed);
      }
    }
    cell->value = work;
    cell->sequence.store(position + 1, std::memory_order_release);
    return true;
  }

  Work *pop() noexcept {
    size_t position = dequeue_.load(std::memory_order_relaxed);
    Cell *cell = nullptr;
    for (;;) {
      cell = &cells_[position & mask_];
      const size_t sequence = cell->sequence.load(std::memory_order_acquire);
      const auto difference =
          static_cast<std::ptrdiff_t>(sequence) - static_cast<std::ptrdiff_t>(position + 1);
      if (difference == 0) {
        if (dequeue_.compare_exchange_weak(position, position + 1, std::memory_order_relaxed)) {
          break;
        }
      } else if (difference < 0) {
        return nullptr;
      } else {
        position = dequeue_.load(std::memory_order_relaxed);
      }
    }
    Work *work = cell->value;
    cell->sequence.store(position + mask_ + 1, std::memory_order_release);
    return work;
  }

private:
  struct Cell {
    std::atomic<size_t> sequence{0};
    Work *value = nullptr;
  };
  std::unique_ptr<Cell[]> cells_;
  size_t mask_;
  alignas(64) std::atomic<size_t> enqueue_{0};
  alignas(64) std::atomic<size_t> dequeue_{0};
};

HostServices::HostServices(Options options)
    : options_(options), runs_(std::make_unique<Queue>(max_works * 2)),
      disposals_(std::make_unique<Queue>(max_works * 2)), run_signal_(std::make_unique<Signal>()),
      dispose_signal_(std::make_unique<Signal>()), counters_(std::make_unique<Counters>()) {
  works_.reserve(max_works);
}

std::shared_ptr<HostServices> HostServices::create(Options options) {
  if (options.mode == HostWorkMode::threaded && options.workers == 0) {
    throw std::invalid_argument("threaded host services need at least one worker");
  }
  std::shared_ptr<HostServices> services(new HostServices(options));
  services->start();
  return services;
}

std::shared_ptr<HostServices> HostServices::shared() {
  static std::mutex mutex;
  static std::weak_ptr<HostServices> instance;
  std::lock_guard lock(mutex);
  auto services = instance.lock();
  if (!services) {
    services = create(Options{});
    instance = services;
  }
  return services;
}

void HostServices::start() {
  if (options_.mode != HostWorkMode::threaded) {
    return;
  }
  try {
    for (uint32_t i = 0; i < options_.workers; ++i) {
      threads_.emplace_back([this] { worker_loop(); });
    }
    threads_.emplace_back([this] { disposer_loop(); });
  } catch (...) {
    stopping_.store(true, std::memory_order_release);
    for (size_t i = 0; i < threads_.size(); ++i) {
      run_signal_->post();
      dispose_signal_->post();
    }
    for (auto &thread : threads_) {
      thread.join();
    }
    throw;
  }
}

HostServices::~HostServices() {
  stopping_.store(true, std::memory_order_release);
  for (size_t i = 0; i < threads_.size(); ++i) {
    run_signal_->post();
  }
  dispose_signal_->post();
  for (auto &thread : threads_) {
    thread.join();
  }
}

namespace {

void name_thread([[maybe_unused]] const char *name) noexcept {
#if defined(__APPLE__)
  pthread_setname_np(name);
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
#endif
}

}

void HostServices::worker_loop() {
  name_thread("vespera.module-work");
  for (;;) {
    run_signal_->wait();
    if (stopping_.load(std::memory_order_acquire)) {
      return;
    }
    while (Work *work = runs_->pop()) {
      execute(*work);
      work->references.fetch_sub(1, std::memory_order_acq_rel);
    }
  }
}

void HostServices::disposer_loop() {
  name_thread("vespera.module-dispose");
  for (;;) {
    dispose_signal_->wait();
    while (Work *work = disposals_->pop()) {
      dispose_handed_back(*work);
      work->references.fetch_sub(1, std::memory_order_acq_rel);
      notify_waiters();
    }
    if (stopping_.load(std::memory_order_acquire)) {
      return;
    }
  }
}

void HostServices::notify_waiters() {
  std::lock_guard lock(mutex_);
  changed_.notify_all();
}

void HostServices::push_run(Work &work) noexcept {
  work.references.fetch_add(1, std::memory_order_acq_rel);
  runs_->push(&work);
  if (options_.mode == HostWorkMode::threaded) {
    run_signal_->post();
  }
}

void HostServices::push_dispose(Work &work) noexcept {
  work.references.fetch_add(1, std::memory_order_acq_rel);
  disposals_->push(&work);
  if (options_.mode == HostWorkMode::threaded) {
    dispose_signal_->post();
  }
}

void HostServices::dispose(Work &work, void *result) noexcept {
  if (result) {
    work.desc.dispose(work.desc.context, result);
    count(counters_->disposed);
  }
}

void HostServices::dispose_handed_back(Work &work) noexcept {
  static int disposing_marker;
  void *result = work.handed_back.load(std::memory_order_acquire);
  if (!result || result == &disposing_marker ||
      !work.handed_back.compare_exchange_strong(result, &disposing_marker,
                                                std::memory_order_acq_rel)) {
    return;
  }
  work.disposing.fetch_add(1, std::memory_order_acq_rel);
  dispose(work, result);
  work.disposing.fetch_sub(1, std::memory_order_acq_rel);
  work.handed_back.store(nullptr, std::memory_order_release);
}

void HostServices::hand_back(Work &work, void *result) noexcept {
  work.handed_back.store(result, std::memory_order_release);
  count(counters_->handed_back);
  if (options_.mode == HostWorkMode::synchronous) {
    dispose_handed_back(work);
  } else {
    push_dispose(work);
  }
}

void HostServices::deliver(Work &work, void *result, uint64_t token) noexcept {
  Work::Slot *slot = &work.slots[0];
  for (auto &candidate : work.slots) {
    if (!candidate.busy.load(std::memory_order_acquire)) {
      slot = &candidate;
      break;
    }
  }
  slot->result = result;
  slot->token = token;
  slot->busy.store(true, std::memory_order_relaxed);
  Work::Slot *previous = work.ready.exchange(slot, std::memory_order_acq_rel);
  count(counters_->delivered);
  if (previous) {
    void *superseded = previous->result;
    previous->busy.store(false, std::memory_order_release);
    count(counters_->superseded);
    dispose(work, superseded);
  }
}

int HostServices::run_cancelled(const vs_work_run *run) {
  if (!run || !run->host) {
    return 1;
  }
  return static_cast<const Work *>(run->host)->phase.load(std::memory_order_acquire) !=
                 WorkPhase::running
             ? 1
             : 0;
}

void HostServices::report_progress(const vs_work_run *run, float fraction) {
  if (run && run->host && std::isfinite(fraction)) {
    static_cast<Work *>(run->host)->progress.store(std::clamp(fraction, 0.0f, 1.0f),
                                                   std::memory_order_relaxed);
    static_cast<Work *>(run->host)->client->notices_.fetch_or(VS_NOTIFY_STATUS,
                                                              std::memory_order_relaxed);
  }
}

bool HostServices::execute(Work &work) noexcept {
  bool ran = false;
  for (;;) {
    WorkPhase expected = WorkPhase::queued;
    if (!work.phase.compare_exchange_strong(expected, WorkPhase::running,
                                            std::memory_order_acq_rel)) {
      return ran;
    }
    ran = true;
    running_.fetch_add(1, std::memory_order_acq_rel);
    notify_waiters();
    Client &client = *work.client;
    const uint64_t token = work.token.load(std::memory_order_acquire);
    const vs_work_run run{sizeof(vs_work_run), token, &work, run_cancelled, report_progress};
    void *result = nullptr;
    vs_result status = VS_ERROR_PROCESS;
    {
      Client::Scope scope(client, Client::Call::run);
      count(counters_->runs);
      try {
        status = work.desc.run(work.desc.context, &run, &result);
      } catch (...) {
        status = VS_ERROR_PROCESS;
        result = nullptr;
      }
    }
    const bool cancelled = work.phase.load(std::memory_order_acquire) != WorkPhase::running;
    if (cancelled) {
      count(counters_->cancelled_runs);
    }
    if (status == VS_OK) {
      if (result) {
        count(counters_->produced);
      }
      if (cancelled) {
        dispose(work, result);
      } else {
        deliver(work, result, token);
      }
    } else {
      count(counters_->failed_runs);
      if (!cancelled) {
        try {
          emit({VS_LOG_WARNING, client.module(),
                "background work failed (" + std::to_string(status) + ")"});
        } catch (...) {
        }
      }
    }
    work.progress.store(-1.0f, std::memory_order_relaxed);
    running_.fetch_sub(1, std::memory_order_acq_rel);

    bool again = false;
    WorkPhase phase = WorkPhase::running;
    for (;;) {
      if (phase == WorkPhase::running) {
        if (work.phase.compare_exchange_weak(phase, WorkPhase::idle, std::memory_order_acq_rel)) {
          break;
        }
      } else if (phase == WorkPhase::superseded) {
        if (work.phase.compare_exchange_weak(phase, WorkPhase::queued, std::memory_order_acq_rel)) {
          again = true;
          break;
        }
      } else {
        work.phase.store(WorkPhase::joined, std::memory_order_release);
        break;
      }
    }
    notify_waiters();
    if (!again) {
      return ran;
    }
    if (options_.mode != HostWorkMode::synchronous) {
      push_run(work);
      return ran;
    }
  }
}

std::shared_ptr<HostServices::Client> HostServices::client(std::shared_ptr<const void> owner,
                                                           std::string module,
                                                           std::string resource_directory) {
  std::shared_ptr<Client> client(new Client);
  client->services_ = shared_from_this();
  client->owner_ = std::move(owner);
  client->module_ = std::move(module);
  client->resource_directory_ = std::move(resource_directory);
  client->descriptor_ = {
      sizeof(vs_host_services),
      VS_MODULE_SERVICES_ABI_VERSION,
      options_.mode == HostWorkMode::synchronous ? uint32_t{VS_HOST_WORK_INLINE} : 0u,
      client.get(),
      client->resource_directory_.empty() ? nullptr : client->resource_directory_.c_str(),
      open_work,
      request,
      adopt,
      retire,
      close_work,
      notify,
      log};
  return client;
}

namespace {

HostServices::Client &client_of(void *context) {
  return *static_cast<HostServices::Client *>(context);
}

}

vs_work *HostServices::open_work(void *context, const vs_work_desc *desc) {
  Client &client = client_of(context);
  HostServices &self = *client.services_;
  if (!client.inside(Client::Call::prepare)) {
    if (self.options_.audit) {
      count(self.counters_->open_outside_prepare);
    }
    return nullptr;
  }
  if (!desc || desc->struct_size < sizeof(vs_work_desc) || !desc->run || !desc->dispose) {
    return nullptr;
  }
  std::lock_guard lock(self.mutex_);
  if (client.work_count_ >= Client::max_works) {
    return nullptr;
  }
  Work *work = nullptr;
  for (const auto &candidate : self.works_) {
    if (candidate->reusable()) {
      work = candidate.get();
      break;
    }
  }
  if (!work) {
    if (self.works_.size() >= max_works) {
      return nullptr;
    }
    try {
      self.works_.push_back(std::make_unique<Work>());
    } catch (...) {
      return nullptr;
    }
    work = self.works_.back().get();
  }
  work->desc = vs_work_desc{sizeof(vs_work_desc), desc->context, desc->run, desc->dispose};
  work->client = &client;
  work->token.store(0, std::memory_order_relaxed);
  for (auto &slot : work->slots) {
    slot.result = nullptr;
    slot.busy.store(false, std::memory_order_relaxed);
  }
  work->ready.store(nullptr, std::memory_order_relaxed);
  work->handed_back.store(nullptr, std::memory_order_relaxed);
  work->progress.store(-1.0f, std::memory_order_relaxed);
  work->phase.store(WorkPhase::idle, std::memory_order_release);
  client.works_[client.work_count_++] = work;
  count(self.counters_->opened);
  return reinterpret_cast<vs_work *>(work);
}

vs_result HostServices::request(void *context, vs_work *handle, uint64_t token) {
  Client &client = client_of(context);
  HostServices &self = *client.services_;
  Work *work = reinterpret_cast<Work *>(handle);
  if (!work || work->client != &client) {
    return VS_ERROR_ARGUMENT;
  }
  if (self.options_.audit && !client.inside_any()) {
    count(self.counters_->request_outside_call);
  }
  WorkPhase phase = work->phase.load(std::memory_order_acquire);
  if (phase == WorkPhase::free || phase == WorkPhase::closing || phase == WorkPhase::joined ||
      phase == WorkPhase::closed) {
    return VS_ERROR_STATE;
  }
  count(self.counters_->requested);
  work->token.store(token, std::memory_order_release);
  for (;;) {
    switch (phase) {
    case WorkPhase::idle:
      if (work->phase.compare_exchange_weak(phase, WorkPhase::queued, std::memory_order_acq_rel)) {
        if (self.options_.mode == HostWorkMode::synchronous) {
          self.execute(*work);
        } else {
          self.push_run(*work);
        }
        return VS_OK;
      }
      break;
    case WorkPhase::running:
      if (work->phase.compare_exchange_weak(phase, WorkPhase::superseded,
                                            std::memory_order_acq_rel)) {
        return VS_OK;
      }
      break;
    case WorkPhase::queued:
    case WorkPhase::superseded:
      count(self.counters_->coalesced);
      return VS_OK;
    default:
      return VS_ERROR_STATE;
    }
  }
}

namespace {

bool closed_phase(WorkPhase phase) noexcept {
  return phase == WorkPhase::free || phase == WorkPhase::closing || phase == WorkPhase::joined ||
         phase == WorkPhase::closed;
}

}

bool HostServices::Client::may_adopt() const noexcept {
  return inside(Call::process) || inside(Call::prepare) || inside(Call::release) ||
         inside(Call::control);
}

int HostServices::adopt(void *context, vs_work *handle, void **current, uint64_t *token) {
  Client &client = client_of(context);
  HostServices &self = *client.services_;
  Work *work = reinterpret_cast<Work *>(handle);
  if (!work || work->client != &client || !current || !token ||
      closed_phase(work->phase.load(std::memory_order_acquire))) {
    return 0;
  }
  if (self.options_.audit && !client.may_adopt()) {
    count(self.counters_->adopt_outside_call);
  }
  if (!work->ready.load(std::memory_order_acquire) ||
      (*current && work->handed_back.load(std::memory_order_acquire))) {
    return 0;
  }
  Work::Slot *slot = work->ready.exchange(nullptr, std::memory_order_acq_rel);
  if (!slot) {
    return 0;
  }
  void *previous = *current;
  *current = slot->result;
  *token = slot->token;
  slot->busy.store(false, std::memory_order_release);
  count(self.counters_->adopted);
  if (previous) {
    self.hand_back(*work, previous);
  }
  return 1;
}

int HostServices::retire(void *context, vs_work *handle, void *result) {
  Client &client = client_of(context);
  HostServices &self = *client.services_;
  Work *work = reinterpret_cast<Work *>(handle);
  if (!work || work->client != &client ||
      closed_phase(work->phase.load(std::memory_order_acquire))) {
    return 0;
  }
  if (self.options_.audit && !client.may_adopt()) {
    count(self.counters_->adopt_outside_call);
  }
  if (!result) {
    return 1;
  }
  if (work->handed_back.load(std::memory_order_acquire)) {
    return 0;
  }
  self.hand_back(*work, result);
  return 1;
}

void HostServices::close(Work &work, void *const *held, uint32_t held_count) noexcept {
  WorkPhase previous = work.phase.load(std::memory_order_acquire);
  if (closed_phase(previous)) {
    return;
  }
  previous = work.phase.exchange(WorkPhase::closing, std::memory_order_acq_rel);
  if (previous == WorkPhase::running || previous == WorkPhase::superseded) {
    std::unique_lock lock(mutex_);
    changed_.wait(lock,
                  [&] { return work.phase.load(std::memory_order_acquire) == WorkPhase::joined; });
  } else {
    work.phase.store(WorkPhase::joined, std::memory_order_release);
  }
  if (Work::Slot *slot = work.ready.exchange(nullptr, std::memory_order_acq_rel)) {
    void *result = slot->result;
    slot->busy.store(false, std::memory_order_release);
    dispose(work, result);
  }
  dispose_handed_back(work);
  {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [&] {
      return work.disposing.load(std::memory_order_acquire) == 0 &&
             work.handed_back.load(std::memory_order_acquire) == nullptr;
    });
  }
  for (uint32_t i = 0; held && i < held_count; ++i) {
    dispose(work, held[i]);
  }
  std::lock_guard lock(mutex_);
  Client &client = *work.client;
  auto end = client.works_.begin() + static_cast<std::ptrdiff_t>(client.work_count_);
  auto it = std::find(client.works_.begin(), end, &work);
  if (it != end) {
    std::rotate(it, it + 1, end);
    client.works_[--client.work_count_] = nullptr;
  }
  count(counters_->closed);
  work.phase.store(WorkPhase::closed, std::memory_order_release);
}

void HostServices::close_work(void *context, vs_work *handle, void *const *held,
                              uint32_t held_count) {
  Client &client = client_of(context);
  HostServices &self = *client.services_;
  Work *work = reinterpret_cast<Work *>(handle);
  if (!work || work->client != &client) {
    return;
  }
  if (self.options_.audit && !client.inside(Client::Call::release) &&
      !client.inside(Client::Call::prepare)) {
    count(self.counters_->close_outside_release);
  }
  self.close(*work, held, held_count);
}

void HostServices::notify(void *context, uint32_t what) {
  client_of(context).notices_.fetch_or(what & known_notices, std::memory_order_acq_rel);
}

void HostServices::log(void *context, uint32_t level, const char *message) {
  Client &client = client_of(context);
  HostServices &self = *client.services_;
  if (!message) {
    return;
  }
  if (self.options_.audit && client.inside(Client::Call::process)) {
    count(self.counters_->log_in_process);
    return;
  }
  try {
    ModuleLogLine line{std::min<uint32_t>(level, VS_LOG_ERROR), client.module_, sanitise(message)};
    uint32_t suppressed = 0;
    {
      std::lock_guard lock(self.mutex_);
      const auto now = std::chrono::steady_clock::now();
      if (now - client.log_window_ >= std::chrono::seconds(1)) {
        client.log_window_ = now;
        client.log_lines_ = 0;
        suppressed = std::exchange(client.log_suppressed_, 0);
      }
      if (client.log_lines_ >= log_lines_per_second) {
        ++client.log_suppressed_;
        return;
      }
      ++client.log_lines_;
    }
    if (suppressed > 0) {
      self.emit({VS_LOG_WARNING, client.module_,
                 std::to_string(suppressed) + " log lines suppressed in the previous second"});
    }
    self.emit(line);
  } catch (...) {
  }
}

void HostServices::emit(const ModuleLogLine &line) {
  LogSink sink;
  {
    std::lock_guard lock(mutex_);
    sink = sink_;
  }
  if (sink) {
    sink(line);
    return;
  }
  const char *enabled = std::getenv("VESPERA_MODULE_LOG");
  if (enabled && std::string_view(enabled) == "1") {
    static constexpr const char *levels[] = {"debug", "info", "warning", "error"};
    std::fprintf(stderr, "%s: %s: %s\n", line.module.c_str(), levels[line.level],
                 line.message.c_str());
  }
}

void HostServices::set_log_sink(LogSink sink) {
  std::lock_guard lock(mutex_);
  sink_ = std::move(sink);
}

bool HostServices::run_one() {
  if (options_.mode != HostWorkMode::manual) {
    return false;
  }
  while (Work *work = runs_->pop()) {
    const bool ran = execute(*work);
    work->references.fetch_sub(1, std::memory_order_acq_rel);
    if (ran) {
      return true;
    }
  }
  return false;
}

size_t HostServices::run_pending() {
  size_t runs = 0;
  while (run_one()) {
    ++runs;
  }
  if (options_.mode == HostWorkMode::manual) {
    while (Work *work = disposals_->pop()) {
      dispose_handed_back(*work);
      work->references.fetch_sub(1, std::memory_order_acq_rel);
    }
  }
  return runs;
}

bool HostServices::wait_idle(std::span<const Client *const> clients,
                             std::chrono::steady_clock::time_point deadline) {
  if (options_.mode == HostWorkMode::synchronous) {
    return true;
  }
  std::unique_lock lock(mutex_);
  return changed_.wait_until(lock, deadline, [&] {
    for (const Client *client : clients) {
      for (size_t i = 0; client && i < client->work_count_; ++i) {
        const WorkPhase phase = client->works_[i]->phase.load(std::memory_order_acquire);
        if (phase == WorkPhase::queued || phase == WorkPhase::running ||
            phase == WorkPhase::superseded) {
          return false;
        }
      }
    }
    return true;
  });
}

bool HostServices::wait_running(std::chrono::steady_clock::time_point deadline) {
  std::unique_lock lock(mutex_);
  return changed_.wait_until(lock, deadline,
                             [&] { return running_.load(std::memory_order_acquire) > 0; });
}

HostServices::Statistics HostServices::statistics() const {
  const Counters &c = *counters_;
  auto read = [](const std::atomic<uint64_t> &value) {
    return value.load(std::memory_order_acquire);
  };
  return {read(c.opened),
          read(c.closed),
          read(c.left_open),
          read(c.requested),
          read(c.coalesced),
          read(c.runs),
          read(c.failed_runs),
          read(c.cancelled_runs),
          read(c.produced),
          read(c.delivered),
          read(c.superseded),
          read(c.adopted),
          read(c.handed_back),
          read(c.disposed),
          read(c.log_in_process),
          read(c.open_outside_prepare),
          read(c.request_outside_call),
          read(c.adopt_outside_call),
          read(c.close_outside_release)};
}

HostServices::Client::Scope::Scope(Client &client, Call call) noexcept
    : client_(&client), call_(call), previous_(current_scope) {
  current_scope = this;
}

HostServices::Client::Scope::~Scope() { current_scope = previous_; }

bool HostServices::Client::inside(Call call) const noexcept {
  for (const Scope *scope = current_scope; scope; scope = scope->previous_) {
    if (scope->client_ == this && scope->call_ == call) {
      return true;
    }
  }
  return false;
}

bool HostServices::Client::inside_any() const noexcept {
  for (const Scope *scope = current_scope; scope; scope = scope->previous_) {
    if (scope->client_ == this) {
      return true;
    }
  }
  return false;
}

HostServices::Client::~Client() { close_all(); }

size_t HostServices::Client::open_works() const {
  std::lock_guard lock(services_->mutex_);
  return work_count_;
}

float HostServices::Client::progress() const {
  std::lock_guard lock(services_->mutex_);
  float progress = -1.0f;
  for (size_t i = 0; i < work_count_; ++i) {
    progress = std::max(progress, works_[i]->progress.load(std::memory_order_relaxed));
  }
  return progress;
}

void HostServices::Client::close_all() noexcept {
  for (;;) {
    Work *work = nullptr;
    {
      std::lock_guard lock(services_->mutex_);
      if (work_count_ == 0) {
        return;
      }
      work = works_[work_count_ - 1];
    }
    count(services_->counters_->left_open);
    services_->close(*work, nullptr, 0);
  }
}

}
