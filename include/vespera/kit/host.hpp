#ifndef VESPERA_KIT_HOST_HPP
#define VESPERA_KIT_HOST_HPP

#include <vespera/file_content.hpp>
#include <vespera/kit/audio.hpp>
#include <vespera/kit/files.hpp>
#include <vespera/kit/media.hpp>
#include <vespera/module.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

/*
 * Host services for kit processors: background work whose results reach process() without
 * locking or allocating, status, logging and the package's resources.
 *
 * A processor constructed from (const kit::Setup &, kit::Host &) may create kit::Derived works
 * while it is constructed. kit::Module adopts their newest results at the start of each block and,
 * when the host runs work inline (offline rendering, or no host services at all), also at the
 * frame of the parameter change that requested them, so offline output does not depend on block
 * sizes.
 *
 * Without host services, work runs inline only during preparation or explicitly offline
 * processing. Processing a work-using module without services otherwise fails before any work
 * can run. Live hosts provide services.
 */

namespace vespera::kit {

enum class Status : uint32_t {
  ready = VS_STATUS_READY,
  busy = VS_STATUS_BUSY,
  warning = VS_STATUS_WARNING,
  error = VS_STATUS_ERROR
};

enum class Level : uint32_t {
  debug = VS_LOG_DEBUG,
  info = VS_LOG_INFO,
  warning = VS_LOG_WARNING,
  error = VS_LOG_ERROR
};

template <class T, class Input> class Derived;
template <class T> class FileWork;
class Host;

namespace detail {

inline thread_local const Job *active_job = nullptr;

class JobScope {
public:
  explicit JobScope(const Job &job) noexcept : previous_(std::exchange(active_job, &job)) {}
  ~JobScope() { active_job = previous_; }
  JobScope(const JobScope &) = delete;
  JobScope &operator=(const JobScope &) = delete;

private:
  const Job *previous_;
};

inline constexpr double fade_seconds = 0.02;
inline constexpr size_t max_works = 8;

// At most 255 bytes of text, cut at a character boundary so that it stays UTF-8.
inline std::string_view status_text(std::string_view text) noexcept {
  if (text.size() <= 255) {
    return text;
  }
  size_t length = 255;
  while (length > 0 && (static_cast<unsigned char>(text[length]) & 0xc0) == 0x80) {
    --length;
  }
  return text.substr(0, length);
}

// Runs every work synchronously on the requesting thread, for instances without host services
// and for works a host declined to open.
struct InlineWork {
  vs_work_desc desc{};
  void *ready = nullptr;
  uint64_t token = 0;
  uint64_t latest = 0;
  bool running = false;
  bool again = false;
};

inline int inline_cancelled(const vs_work_run *run) {
  return static_cast<const InlineWork *>(run->host)->again ? 1 : 0;
}
inline void inline_progress(const vs_work_run *, float) {}

inline vs_work *inline_open(void *, const vs_work_desc *desc) {
  if (!desc || desc->struct_size < sizeof(vs_work_desc) || !desc->run || !desc->dispose) {
    return nullptr;
  }
  auto *work = new (std::nothrow) InlineWork;
  if (work) {
    work->desc = *desc;
  }
  return reinterpret_cast<vs_work *>(work);
}

inline vs_result inline_request(void *, vs_work *handle, uint64_t token) {
  auto &work = *reinterpret_cast<InlineWork *>(handle);
  work.latest = token;
  if (work.running) {
    work.again = true;
    return VS_OK;
  }
  work.running = true;
  do {
    work.again = false;
    const vs_work_run run{sizeof(vs_work_run), work.latest, &work, inline_cancelled,
                          inline_progress};
    void *result = nullptr;
    if (work.desc.run(work.desc.context, &run, &result) == VS_OK && result) {
      if (work.ready) {
        work.desc.dispose(work.desc.context, work.ready);
      }
      work.ready = result;
      work.token = run.token;
    }
  } while (work.again);
  work.running = false;
  return VS_OK;
}

inline int inline_adopt(void *, vs_work *handle, void **current, uint64_t *token) {
  auto &work = *reinterpret_cast<InlineWork *>(handle);
  if (!work.ready) {
    return 0;
  }
  void *previous = *current;
  *current = std::exchange(work.ready, nullptr);
  *token = work.token;
  if (previous) {
    work.desc.dispose(work.desc.context, previous);
  }
  return 1;
}

inline int inline_retire(void *, vs_work *handle, void *result) {
  auto &work = *reinterpret_cast<InlineWork *>(handle);
  if (result) {
    work.desc.dispose(work.desc.context, result);
  }
  return 1;
}

inline void inline_close(void *, vs_work *handle, void *const *held, uint32_t count) {
  auto *work = reinterpret_cast<InlineWork *>(handle);
  if (work->ready) {
    work->desc.dispose(work->desc.context, work->ready);
  }
  for (uint32_t i = 0; held && i < count; ++i) {
    if (held[i]) {
      work->desc.dispose(work->desc.context, held[i]);
    }
  }
  delete work;
}

inline void inline_notify(void *, uint32_t) {}
inline void inline_log(void *, uint32_t, const char *) {}

inline constexpr vs_host_services inline_services{sizeof(vs_host_services),
                                                  VS_MODULE_SERVICES_ABI_VERSION,
                                                  VS_HOST_WORK_INLINE,
                                                  nullptr,
                                                  nullptr,
                                                  inline_open,
                                                  inline_request,
                                                  inline_adopt,
                                                  inline_retire,
                                                  inline_close,
                                                  inline_notify,
                                                  inline_log};

class Link;

// What kit::Module needs from a work, independent of its result type.
class WorkCore {
public:
  WorkCore(const WorkCore &) = delete;
  WorkCore &operator=(const WorkCore &) = delete;
  virtual ~WorkCore() = default;

  // Adopts the newest result. A fade from the previous result must finish before a live host's
  // next result is adopted; with inline work the newest result is adopted at once, so adoption
  // points depend only on request frames.
  void adopt_point(bool inline_work) noexcept {
    if (previous_ && fade_position_ >= fade_length_ && hand_back(previous_)) {
      previous_ = nullptr;
    }
    if (previous_ && !inline_work) {
      return;
    }
    void *incoming = nullptr;
    uint64_t token = 0;
    if (!services_->adopt(services_->context, work_, &incoming, &token)) {
      return;
    }
    if (previous_) {
      hand_back(previous_);
    }
    previous_ = current_;
    current_ = incoming;
    fade_position_ = 0;
  }

  void advance(uint32_t frames) noexcept {
    fade_position_ = std::min(fade_length_, fade_position_ + frames);
  }

  uint32_t fade_length() const noexcept { return fade_length_; }
  uint32_t fade_position() const noexcept { return fade_position_; }
  bool busy() const noexcept {
    return requested_.load(std::memory_order_acquire) != completed_.load(std::memory_order_acquire);
  }
  uint32_t file_slot() const noexcept { return file_slot_; }

protected:
  WorkCore(Link &link, uint32_t fade_length) noexcept;

  // Opens the work with the host's services, or runs it inline when there are none or the host
  // declines.
  void open(const vs_work_desc &desc);
  // Hands back everything the processor holds and closes the work. Called by the result type's
  // destructor, while the result type's dispose function is still usable.
  void close() noexcept;

  bool hand_back(void *result) noexcept {
    return result == first_ || services_->retire(services_->context, work_, result) != 0;
  }

  Link &link_;
  const vs_host_services *services_ = &inline_services;
  vs_work *work_ = nullptr;
  // The result built while preparing, owned by the processor rather than the host.
  void *first_ = nullptr;
  void *current_ = nullptr;
  void *previous_ = nullptr;
  uint32_t fade_length_;
  uint32_t fade_position_;
  std::atomic<uint64_t> requested_{0};
  std::atomic<uint64_t> completed_{0};
  uint32_t file_slot_ = 0;
};

// One instance's connection to the host: its services, its status and its works.
class Link {
public:
  vs_result attach(const vs_host_services *services) noexcept {
    if (!services || services->struct_size < sizeof(vs_host_services) ||
        services->abi_version != VS_MODULE_SERVICES_ABI_VERSION || !services->open_work ||
        !services->request || !services->adopt || !services->retire || !services->close_work ||
        !services->notify || !services->log) {
      return VS_ERROR_ARGUMENT;
    }
    try {
      resources_ = services->resource_directory
                       ? std::filesystem::path(services->resource_directory)
                       : std::filesystem::path();
    } catch (...) {
      return VS_ERROR_PROCESS;
    }
    host_ = services;
    mode_.store((services->flags & VS_HOST_WORK_INLINE) != 0 ? WorkMode::hosted_inline
                                                             : WorkMode::hosted,
                std::memory_order_relaxed);
    return VS_OK;
  }

  // How this instance's works run. `declined` is entered off-thread when the host refuses a
  // work during WorkCore::open and left when the last work is removed, so the mode is atomic
  // with relaxed access.
  enum class WorkMode : uint8_t {
    // No services attached: works run inline on module threads.
    detached,
    // Works run through the host's background workers.
    hosted,
    // The host declared VS_HOST_WORK_INLINE: works run inline through its own services.
    hosted_inline,
    // The host refused a work: every work runs inline on module threads.
    declined
  };

  const vs_host_services *host() const noexcept { return host_; }
  const vs_host_services &services() const noexcept { return host_ ? *host_ : inline_services; }
  WorkMode work_mode() const noexcept {
    if (!host_) {
      return WorkMode::detached;
    }
    return mode_.load(std::memory_order_relaxed);
  }
  bool inline_work() const noexcept {
    switch (work_mode()) {
    case WorkMode::detached:
    case WorkMode::hosted_inline:
    case WorkMode::declined:
      return true;
    case WorkMode::hosted:
      return false;
    }
    return true;
  }
  bool needs_offline() const noexcept {
    const WorkMode mode = work_mode();
    return count_ != 0 && (mode == WorkMode::detached || mode == WorkMode::declined);
  }
  void use_fallback() noexcept { mode_.store(WorkMode::declined, std::memory_order_relaxed); }
  const std::filesystem::path &resources() const noexcept { return resources_; }
  void set_file(uint32_t slot, File file) { files_[slot] = std::move(file); }
  void declare_files(std::vector<FileSlotMetadata> slots) { slots_ = std::move(slots); }
  const FileSlotMetadata &slot(uint32_t id) const {
    const auto it =
        std::find_if(slots_.begin(), slots_.end(), [id](const auto &s) { return s.id == id; });
    if (it == slots_.end()) {
      throw std::invalid_argument("undeclared file slot");
    }
    return *it;
  }
  const File &file(uint32_t slot) const {
    const auto it = files_.find(slot);
    return it == files_.end() ? empty_file_ : it->second;
  }

  bool has_works() const noexcept { return count_ != 0; }
  void add(WorkCore *work) {
    if (count_ == max_works) {
      throw std::length_error("a processor may use at most 8 works");
    }
    works_[count_++] = work;
  }
  void remove(WorkCore *work) noexcept {
    const auto end = works_.begin() + static_cast<std::ptrdiff_t>(count_);
    const auto it = std::find(works_.begin(), end, work);
    if (it != end) {
      std::rotate(it, it + 1, end);
      --count_;
      if (count_ == 0) {
        // The last work is gone, so a refusal no longer binds the next one.
        mode_.store(host_ && (host_->flags & VS_HOST_WORK_INLINE) != 0 ? WorkMode::hosted_inline
                                                                       : WorkMode::hosted,
                    std::memory_order_relaxed);
      }
    }
  }
  void adopt_all() noexcept {
    const bool inline_mode = inline_work();
    for (size_t i = 0; i < count_; ++i) {
      works_[i]->adopt_point(inline_mode);
    }
  }
  void advance(uint32_t frames) noexcept {
    for (size_t i = 0; i < count_; ++i) {
      works_[i]->advance(frames);
    }
  }

  void report(Status kind, std::string_view message, float progress, uint32_t file_slot = 0) {
    if (file_slot) {
      slot(file_slot);
    }
    {
      std::lock_guard lock(mutex_);
      reported_ = kind;
      message_.assign(status_text(message));
      progress_ = progress;
      reported_file_slot_ = file_slot;
    }
    notify(VS_NOTIFY_STATUS);
  }

  // Records the error of a work's last build, or clears it with an empty message.
  void work_error(const WorkCore *work, std::string_view message) {
    message = status_text(message);
    {
      std::lock_guard lock(mutex_);
      const auto it = errors_.find(work);
      if (message.empty()) {
        if (it == errors_.end()) {
          return;
        }
        errors_.erase(it);
      } else if (it == errors_.end()) {
        errors_.emplace(work, std::string(message));
      } else if (it->second != message) {
        it->second.assign(message);
      } else {
        return;
      }
    }
    notify(VS_NOTIFY_STATUS);
  }

  void forget(const WorkCore *work) noexcept {
    std::lock_guard lock(mutex_);
    errors_.erase(work);
  }

  vs_result status(vs_module_status *status) const noexcept {
    if (!status || status->struct_size < offsetof(vs_module_status, message) + 1) {
      return VS_ERROR_ARGUMENT;
    }
    std::lock_guard lock(mutex_);
    Status kind = reported_;
    const std::string *message = &message_;
    float progress = progress_;
    uint32_t slot = reported_file_slot_;
    for (const auto &[work, error] : errors_) {
      kind = Status::error;
      message = &error;
      progress = -1.0f;
      slot = work->file_slot();
      break;
    }
    if (kind == Status::ready) {
      for (size_t i = 0; i < count_; ++i) {
        if (works_[i]->busy()) {
          kind = Status::busy;
          slot = works_[i]->file_slot();
          break;
        }
      }
    }
    status->kind = static_cast<uint32_t>(kind);
    status->progress = progress;
    if (status->struct_size >= offsetof(vs_module_status, message)) {
      status->file_slot = slot;
    }
    const size_t room = std::min(sizeof(status->message),
                                 status->struct_size - offsetof(vs_module_status, message));
    std::string_view text = *message;
    if (work_mode() == WorkMode::declined && host_ && !(host_->flags & VS_HOST_WORK_INLINE)) {
      status->kind = VS_STATUS_ERROR;
      status->progress = -1.0f;
      text = "Background work is unavailable; offline processing only";
    }
    const size_t length = std::min(text.size(), room - 1);
    std::memcpy(status->message, text.data(), length);
    status->message[length] = '\0';
    return VS_OK;
  }

  void notify(uint32_t what) const noexcept {
    if (host_) {
      host_->notify(host_->context, what);
    }
  }

  void log(Level level, std::string_view message) const noexcept {
    if (!host_) {
      return;
    }
    try {
      const std::string text(message);
      host_->log(host_->context, static_cast<uint32_t>(level), text.c_str());
    } catch (...) {
    }
  }

private:
  const vs_host_services *host_ = nullptr;
  std::atomic<WorkMode> mode_{WorkMode::detached};
  std::filesystem::path resources_;
  std::array<WorkCore *, max_works> works_{};
  size_t count_ = 0;
  mutable std::mutex mutex_;
  Status reported_ = Status::ready;
  std::string message_;
  float progress_ = -1.0f;
  uint32_t reported_file_slot_ = 0;
  std::map<const WorkCore *, std::string> errors_;
  std::map<uint32_t, File> files_;
  File empty_file_;
  std::vector<FileSlotMetadata> slots_;
};

inline WorkCore::WorkCore(Link &link, uint32_t fade_length) noexcept
    : link_(link), fade_length_(fade_length), fade_position_(fade_length) {}

inline void WorkCore::open(const vs_work_desc &desc) {
  link_.add(this);
  services_ = &link_.services();
  work_ = services_->open_work(services_->context, &desc);
  if (!work_ && services_ != &inline_services) {
    link_.log(Level::warning, "the host declined background work; it runs inline instead");
    services_ = &inline_services;
    link_.use_fallback();
    work_ = services_->open_work(services_->context, &desc);
  }
  if (!work_) {
    link_.remove(this);
    throw std::bad_alloc();
  }
}

inline void WorkCore::close() noexcept {
  if (!work_) {
    return;
  }
  link_.remove(this);
  void *held[2] = {};
  uint32_t count = 0;
  for (void *result : {current_, previous_}) {
    if (result && result != first_) {
      held[count++] = result;
    }
  }
  services_->close_work(services_->context, work_, held, count);
  link_.forget(this);
  work_ = nullptr;
}

// The newest input written by the processing thread and read by work runs, without locks: one
// slot is being written, one holds the newest complete value and one is being read.
template <class Input> class Latest {
  static_assert(std::is_trivially_copyable_v<Input>, "work inputs must be trivially copyable");

public:
  explicit Latest(const Input &initial) noexcept : slots_{initial, initial, initial} {}

  void write(const Input &value) noexcept {
    slots_[write_] = value;
    write_ = middle_.exchange(write_ | fresh, std::memory_order_acq_rel) & index;
  }

  const Input &read() noexcept {
    if (middle_.load(std::memory_order_acquire) & fresh) {
      read_ = middle_.exchange(read_, std::memory_order_acq_rel) & index;
    }
    return slots_[read_];
  }

private:
  static constexpr uint8_t index = 3;
  static constexpr uint8_t fresh = 4;
  std::array<Input, 3> slots_;
  std::atomic<uint8_t> middle_{1};
  uint8_t write_ = 0;
  uint8_t read_ = 2;
};

template <class T, class Input> class DerivedCore final : public WorkCore {
public:
  using Build = std::function<T(const Input &, Job &)>;

  DerivedCore(Link &link, uint32_t fade_length, const Input &initial, Build build)
      : WorkCore(link, fade_length), latest_(initial), build_(std::move(build)) {
    Job job;
    first_value_ = std::make_unique<T>(build_(initial, job));
    first_ = first_value_.get();
    current_ = first_;
    open({sizeof(vs_work_desc), this, run, dispose});
  }

  ~DerivedCore() override { close(); }

  T *current() const noexcept { return static_cast<T *>(current_); }
  T *previous() const noexcept {
    return fade_position_ < fade_length_ ? static_cast<T *>(previous_) : nullptr;
  }

  void request(const Input &input) noexcept {
    latest_.write(input);
    const auto token = requested_.fetch_add(1, std::memory_order_acq_rel) + 1;
    services_->request(services_->context, work_, token);
    link_.notify(VS_NOTIFY_STATUS);
  }

private:
  static vs_result run(void *context, const vs_work_run *run, void **result) {
    auto &self = *static_cast<DerivedCore *>(context);
    try {
      Job job(run);
      JobScope scope(job);
      auto value = std::make_unique<T>(self.build_(self.latest_.read(), job));
      self.link_.work_error(&self, {});
      self.complete(run->token);
      *result = value.release();
      return VS_OK;
    } catch (const std::exception &error) {
      if (!run->cancelled(run)) {
        self.report(error.what());
      }
    } catch (...) {
      if (!run->cancelled(run)) {
        self.report("the build failed");
      }
    }
    self.complete(run->token);
    return VS_ERROR_PROCESS;
  }

  void complete(uint64_t token) noexcept {
    completed_.store(token, std::memory_order_release);
    link_.notify(VS_NOTIFY_STATUS);
  }

  void report(const char *message) noexcept {
    try {
      link_.work_error(this, *message ? message : "the build failed");
    } catch (...) {
    }
  }

  static void dispose(void *, void *result) { delete static_cast<T *>(result); }

  Latest<Input> latest_;
  Build build_;
  std::unique_ptr<T> first_value_;
};

template <class T> class FileCore final : public WorkCore {
public:
  using Build = std::function<T(const File &, Job &)>;
  FileCore(Link &link, uint32_t fade_length, uint32_t slot, Build build, bool optional)
      : WorkCore(link, fade_length), file_(link.file(slot)), build_(std::move(build)) {
    file_slot_ = slot;
    open({sizeof(vs_work_desc), this, run, dispose});
    try {
      if (file_.state == VS_FILE_PRESENT) {
        requested_.store(1, std::memory_order_release);
        services_->request(services_->context, work_, 1);
      } else if (file_.state == VS_FILE_MISSING || !optional) {
        link_.work_error(this, file_.state == VS_FILE_MISSING ? "File is missing; Locate it to load"
                                                              : "Choose a file to load");
      }
    } catch (...) {
      close();
      throw;
    }
  }
  ~FileCore() override { close(); }
  T *current() const noexcept { return static_cast<T *>(current_); }
  T *previous() const noexcept {
    return fade_position_ < fade_length_ ? static_cast<T *>(previous_) : nullptr;
  }

private:
  static vs_result run(void *context, const vs_work_run *run, void **result) {
    auto &self = *static_cast<FileCore *>(context);
    vs_result status = VS_ERROR_PROCESS;
    try {
      Job job(run);
      JobScope scope(job);
      auto value = std::make_unique<T>(self.build_(self.file_, job));
      if (!job.cancelled()) {
        self.link_.work_error(&self, {});
        *result = value.release();
        status = VS_OK;
      }
    } catch (const std::exception &error) {
      if (!run->cancelled(run)) {
        self.report(error.what());
      }
    } catch (...) {
      if (!run->cancelled(run)) {
        self.report("File build failed");
      }
    }
    self.completed_.store(run->token, std::memory_order_release);
    self.link_.notify(VS_NOTIFY_STATUS);
    return status;
  }
  void report(const char *message) noexcept {
    try {
      link_.work_error(this, message && *message ? message : "File build failed");
    } catch (...) {
    }
  }
  static void dispose(void *, void *result) { delete static_cast<T *>(result); }
  File file_;
  Build build_;
};

}

// A value derived from processor inputs, such as a table computed from parameter values, rebuilt
// off the processing thread whenever a newer input is requested. The first value is built while
// the processor is constructed, so current() is never null.
template <class T, class Input> class Derived {
public:
  Derived(Derived &&) noexcept = default;
  Derived &operator=(Derived &&) noexcept = default;

  // The adopted value, stable within a span.
  T *current() const noexcept { return core_->current(); }
  // The value being faded out for 20 ms after a new one was adopted, or null.
  T *previous() const noexcept { return core_->previous(); }
  // How far the fade from previous() to current() has progressed, from 0 to 1; 1 without a fade.
  float fade() const noexcept {
    return core_->previous() ? static_cast<float>(core_->fade_position()) /
                                   static_cast<float>(core_->fade_length())
                             : 1.0f;
  }
  // Asks for a value built from input. Real-time safe with a live host; requests that arrive
  // before a build starts are merged and only the newest input is built. Offline, the build runs
  // here and the value is adopted at this frame.
  void request(const Input &input) noexcept { core_->request(input); }

private:
  friend class Host;
  template <class W, class Render> friend void crossfade(W &, Block &, Render &&) noexcept;
  explicit Derived(std::unique_ptr<detail::DerivedCore<T, Input>> core) : core_(std::move(core)) {}
  const detail::WorkCore &core() const noexcept { return *core_; }
  std::unique_ptr<detail::DerivedCore<T, Input>> core_;
};

enum class Pending { silence, dry };

template <class T> class FileWork {
public:
  FileWork(FileWork &&) noexcept = default;
  FileWork &operator=(FileWork &&) noexcept = default;
  T *current() const noexcept { return core_->current(); }
  T *previous() const noexcept { return core_->previous(); }
  float fade() const noexcept {
    return current() ? static_cast<float>(core_->fade_position()) /
                           static_cast<float>(core_->fade_length())
                     : 0.0f;
  }
  template <class Reset> void reset(Reset reset) noexcept {
    if (auto *value = current()) {
      reset(*value);
    }
    if (auto *value = previous()) {
      reset(*value);
    }
  }

private:
  friend class Host;
  template <class W, class Render> friend void crossfade(W &, Block &, Render &&) noexcept;
  template <class U, class Render>
  friend void crossfade(FileWork<U> &, Block &, Pending, Render &&) noexcept;
  explicit FileWork(std::unique_ptr<detail::FileCore<T>> core) : core_(std::move(core)) {}
  const detail::WorkCore &core() const noexcept { return *core_; }
  std::unique_ptr<detail::FileCore<T>> core_;
};

// What a processor constructed from (const Setup &, Host &) can use. Valid while the processor
// exists; works can be created only while it is being constructed.
class Host {
public:
  Host(detail::Link &link, const Setup &setup) noexcept
      : link_(link), fade_frames_(static_cast<uint32_t>(
                         std::max(1.0, std::round(setup.sample_rate * detail::fade_seconds)))) {}
  Host(const Host &) = delete;
  Host &operator=(const Host &) = delete;

  // The package's resources directory, or an empty path when the host did not name one.
  const std::filesystem::path &resources() const noexcept { return link_.resources(); }
  // False when work runs on the requesting thread: offline hosts and hosts without services.
  bool live() const noexcept { return !link_.inline_work(); }
  const File &file_reference(uint32_t slot) const {
    link_.slot(slot);
    return link_.file(slot);
  }
  void timing_changed() const noexcept { link_.notify(VS_NOTIFY_TIMING); }
  template <class T, class Build> FileWork<T> file(uint32_t slot, Build build) {
    return FileWork<T>(std::make_unique<detail::FileCore<T>>(
        link_, std::max(1u, fade_frames_ / 2), slot, std::move(build), link_.slot(slot).optional));
  }

  // Builds the first value now with build(initial, job) and every later one on a host thread.
  // build returns a T or throws; a throw while preparing fails the preparation, a throw later
  // keeps the previous value and reports the message as an error status until a build succeeds.
  template <class T, class Input, class Build>
  Derived<T, Input> derive(Input initial, Build build) {
    std::function<T(const Input &, Job &)> function;
    if constexpr (std::is_invocable_r_v<T, Build &, const Input &, Job &>) {
      function = std::move(build);
    } else {
      static_assert(std::is_invocable_r_v<T, Build &, const Input &>,
                    "build must be callable as T(const Input &, kit::Job &) or T(const Input &)");
      function = [build = std::move(build)](const Input &input, Job &) mutable {
        return build(input);
      };
    }
    return Derived<T, Input>(std::make_unique<detail::DerivedCore<T, Input>>(
        link_, fade_frames_, initial, std::move(function)));
  }

  // The module's own status, reported to the host. Not from the processing thread.
  void report(Status kind, std::string_view message = {}, float progress = -1.0f,
              uint32_t file_slot = 0) {
    link_.report(kind, message, progress, file_slot);
  }
  // A line in the host's log. Not from the processing thread.
  void log(Level level, std::string_view message) const noexcept { link_.log(level, message); }

private:
  detail::Link &link_;
  uint32_t fade_frames_;
};

// Renders block with work's current value and, while it fades in, mixes in the previous value's
// rendering, falling linearly over the fade. render(T &value, Block &span) writes every output
// sample of span from its inputs; it is called for spans of at most 256 frames during a fade.
template <class W, class Render> void crossfade(W &work, Block &block, Render &&render) noexcept {
  auto *previous = work.previous();
  if (!previous) {
    render(*work.current(), block);
    return;
  }
  constexpr uint32_t chunk = 256;
  std::array<std::array<float, chunk>, max_channels> faded;
  const detail::WorkCore &core = work.core();
  uint32_t position = core.fade_position();
  uint32_t start = 0;
  while (start < block.frames) {
    Block span = block;
    span.frames = std::min(chunk, block.frames - start);
    for (uint32_t c = 0; c < block.channels; ++c) {
      span.inputs[c] = block.inputs[c] + start;
      span.outputs[c] = block.outputs[c] + start;
    }
    if (position >= core.fade_length()) {
      span.frames = block.frames - start;
      render(*work.current(), span);
      return;
    }
    span.frames = std::min(span.frames, core.fade_length() - position);
    render(*work.current(), span);
    Block other = span;
    for (uint32_t c = 0; c < block.channels; ++c) {
      other.outputs[c] = faded[c].data();
    }
    render(*previous, other);
    for (uint32_t i = 0; i < span.frames; ++i) {
      const float weight =
          std::min(1.0f, static_cast<float>(position + i) / static_cast<float>(core.fade_length()));
      for (uint32_t c = 0; c < block.channels; ++c) {
        span.outputs[c][i] = faded[c][i] + (span.outputs[c][i] - faded[c][i]) * weight;
      }
    }
    position += span.frames;
    start += span.frames;
  }
}

template <class T, class Render>
void crossfade(FileWork<T> &work, Block &block, Pending pending, Render &&render) noexcept {
  if (!work.current()) {
    for (uint32_t c = 0; c < block.channels; ++c) {
      if (pending == Pending::dry) {
        std::copy_n(block.inputs[c], block.frames, block.outputs[c]);
      } else {
        std::fill_n(block.outputs[c], block.frames, 0.0f);
      }
    }
    return;
  }
  if (work.previous() || work.core().fade_position() >= work.core().fade_length()) {
    crossfade<FileWork<T>, Render>(work, block, std::forward<Render>(render));
    return;
  }
  render(*work.current(), block);
  const auto &core = work.core();
  for (uint32_t i = 0; i < block.frames && i < core.fade_length() - core.fade_position(); ++i) {
    const float mix =
        static_cast<float>(core.fade_position() + i) / static_cast<float>(core.fade_length());
    for (uint32_t c = 0; c < block.channels; ++c) {
      const float dry = pending == Pending::dry ? block.inputs[c][i] : 0.0f;
      block.outputs[c][i] = dry + (block.outputs[c][i] - dry) * mix;
    }
  }
}

template <class T, class Render>
void crossfade(FileWork<T> &work, Block &block, Render &&render) noexcept {
  crossfade(work, block, Pending::silence, std::forward<Render>(render));
}

namespace detail {

// Entries released by every user are kept this long, so a rebuild or reopen finds them.
inline constexpr std::chrono::seconds shared_retention{30};

inline std::chrono::steady_clock::time_point (*shared_clock)() = std::chrono::steady_clock::now;

template <class T> class SharedCache {
  struct State : std::enable_shared_from_this<State> {
    struct Entry {
      std::shared_ptr<const T> value;
      std::weak_ptr<const T> user;
      std::chrono::steady_clock::time_point released{};
      bool building = false;
    };
    struct Lease {
      std::shared_ptr<State> owner;
      std::string key;
      std::shared_ptr<const T> value;
      Lease(std::shared_ptr<State> state, const std::string &name, std::shared_ptr<const T> data)
          : owner(std::move(state)), key(name), value(std::move(data)) {}
      ~Lease() {
        value.reset();
        owner->released(key);
      }
    };

    std::shared_ptr<const T> get(const std::string &key, const std::function<T()> &make) {
      std::vector<std::shared_ptr<const T>> retired;
      std::unique_lock lock(mutex_);
      sweep(retired);
      for (;;) {
        if (closed_) {
          throw std::runtime_error("the shared cache is closed");
        }
        auto it = entries_.find(key);
        if (it == entries_.end()) {
          break;
        }
        Entry &entry = it->second;
        if (entry.building) {
          if (active_job) {
            built_.wait_for(lock, std::chrono::milliseconds(10));
            if (active_job->cancelled()) {
              throw std::runtime_error("background work was cancelled");
            }
          } else {
            built_.wait(lock);
          }
          continue;
        }
        if (auto user = entry.user.lock()) {
          return user;
        }
        return hand_out(it->first, entry);
      }
      entries_[key].building = true;
      lock.unlock();
      std::shared_ptr<const T> value;
      try {
        value = std::make_shared<T>(make());
      } catch (...) {
        lock.lock();
        entries_.erase(key);
        built_.notify_all();
        throw;
      }
      lock.lock();
      if (closed_) {
        lock.unlock();
        throw std::runtime_error("the shared cache is closed");
      }
      Entry &entry = entries_[key];
      entry.building = false;
      entry.value = std::move(value);
      built_.notify_all();
      return hand_out(key, entry);
    }

    std::shared_ptr<const T> find(const std::string &key) {
      std::vector<std::shared_ptr<const T>> retired;
      std::unique_lock lock(mutex_);
      sweep(retired);
      const auto it = entries_.find(key);
      if (closed_ || it == entries_.end() || it->second.building) {
        return {};
      }
      if (auto user = it->second.user.lock()) {
        return user;
      }
      return hand_out(it->first, it->second);
    }

    std::shared_ptr<const T> hand_out(const std::string &key, Entry &entry) {
      auto lease = std::make_shared<Lease>(this->shared_from_this(), key, entry.value);
      std::shared_ptr<const T> user(lease, lease->value.get());
      entry.user = user;
      return user;
    }

    void released(const std::string &key) noexcept {
      std::shared_ptr<const T> retired;
      std::lock_guard lock(mutex_);
      if (closed_) {
        return;
      }
      if (auto it = entries_.find(key); it != entries_.end() && it->second.user.expired()) {
        it->second.released = shared_clock();
      }
      size_t idle = 0;
      auto oldest = entries_.end();
      for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (!it->second.building && it->second.user.expired()) {
          ++idle;
          if (oldest == entries_.end() || it->second.released < oldest->second.released) {
            oldest = it;
          }
        }
      }
      if (idle > 8 && oldest != entries_.end()) {
        retired = std::move(oldest->second.value);
        entries_.erase(oldest);
      }
    }

    void sweep(std::vector<std::shared_ptr<const T>> &retired) {
      const auto now = shared_clock();
      for (auto it = entries_.begin(); it != entries_.end();) {
        const Entry &entry = it->second;
        if (!entry.building && entry.user.expired() && now - entry.released >= shared_retention) {
          retired.push_back(std::move(it->second.value));
          it = entries_.erase(it);
        } else {
          ++it;
        }
      }
    }

    void shutdown() noexcept {
      std::map<std::string, Entry> retired;
      {
        std::lock_guard lock(mutex_);
        closed_ = true;
        retired.swap(entries_);
        built_.notify_all();
      }
    }

    std::mutex mutex_;
    std::condition_variable built_;
    std::map<std::string, Entry> entries_;
    bool closed_ = false;
  };

public:
  ~SharedCache() { state_->shutdown(); }
  std::shared_ptr<const T> get(const std::string &key, const std::function<T()> &make) {
    if (key.size() > 1024) {
      throw std::invalid_argument("shared cache keys are limited to 1024 bytes");
    }
    const auto state = state_;
    return state->get(key, make);
  }
  std::shared_ptr<const T> find(const std::string &key) {
    if (key.size() > 1024) {
      throw std::invalid_argument("shared cache keys are limited to 1024 bytes");
    }
    const auto state = state_;
    return state->find(key);
  }

private:
  std::shared_ptr<State> state_ = std::make_shared<State>();
};

template <class T> SharedCache<T> &shared_cache() {
  static SharedCache<T> cache;
  return cache;
}

}

// Immutable data shared by every instance of the module in the process, such as a decoded file,
// built once per key by make() and kept for 30 s after its last user lets go. A caller asking
// for a key that is being built waits for that build. Call it from work builds or while
// preparing, never from the processing thread.
template <class T, class Make> std::shared_ptr<const T> shared(const std::string &key, Make make) {
  return detail::shared_cache<T>().get(key, std::function<T()>(std::move(make)));
}

template <class T> std::shared_ptr<const T> cached(const std::string &key) {
  return detail::shared_cache<T>().find(key);
}

}

#endif
