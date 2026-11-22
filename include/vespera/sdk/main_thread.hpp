#ifndef VESPERA_SDK_MAIN_THREAD_HPP
#define VESPERA_SDK_MAIN_THREAD_HPP

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <pthread.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>

namespace vespera::sdk {

/*
 * Runs work through a common-mode source on the process main run loop. Unlike dispatching to
 * the main queue, this also runs when a main-queue callback is pumping a nested run loop.
 *
 * call(job, timeout) runs job inline on the main thread. Elsewhere it queues the job and waits.
 * Each call moves through explicit states:
 *   queued    -> running    main started the job; it may use anything the caller's stack owns
 *   running   -> completed  call returns done (rethrowing the job's exception, if any)
 *   queued    -> abandoned  the timeout passed before main started it: call returns not_started
 *                           and the queued entry later returns without invoking the job
 * The timeout bounds only the wait for main to start the job. Once running, call waits for the
 * job to finish however long that takes, because the job may reference the caller's stack and a
 * synchronous main-thread call cannot be cancelled. A caller needing a hard bound must keep its
 * jobs bounded.
 *
 * post(job) moves job into a heap copy, queues it and returns. Within an image, jobs run in
 * queue order except for inline calls on the main thread. Separate images have independent
 * sources, so neither a different image's job nor an inline call drains earlier posts.
 * Posted jobs must own all their data by value: they routinely outlive the object that posted
 * them. post_after(seconds, job) is post through a one-shot main run-loop timer instead, for
 * retrying later without occupying the main thread meanwhile; it is unordered with posts.
 *
 * alive(timeout) is call with an empty job. wait(seconds) sleeps off the main thread and runs the
 * main run loop in its default mode on it, so main-thread jobs keep running.
 *
 * Never call, alive or wait while holding PluginLifecycleLock, and never run the main run loop
 * while holding it: a job on main that takes the lock would deadlock with the waiter.
 *
 * First use pins the image containing the caller (one never-released, no-delete dlopen
 * reference), because a queued or abandoned job runs this image's code after the caller may have
 * unloaded it. The class has hidden visibility so every image pins itself rather than sharing
 * another image's coalesced definition. If pinning fails, call returns not_started and post
 * returns false without queuing.
 */
class __attribute__((visibility("hidden"))) MainThread {
public:
  enum class Result { done, not_started };

  static bool on_main() noexcept { return pthread_main_np() != 0; }

  template <class Job> static Result call(Job &&job, double timeout_seconds) {
    if (on_main()) {
      job();
      return Result::done;
    }
    if (!pin()) {
      return Result::not_started;
    }
    auto invoke = [&job] { job(); };
    auto shared = std::make_shared<Call>();
    shared->invoke = [](void *target) { (*static_cast<decltype(invoke) *>(target))(); };
    shared->job = &invoke;
    auto *context = new std::shared_ptr<Call>(shared);
    if (!enqueue(run_call, context)) {
      delete context;
      return Result::not_started;
    }
    std::unique_lock lock(shared->mutex);
    const auto completed = [&] { return shared->state == State::completed; };
    if (!shared->changed.wait_until(lock, deadline(timeout_seconds), completed)) {
      if (shared->state == State::queued) {
        shared->job = nullptr;
        shared->state = State::abandoned;
        return Result::not_started;
      }
      shared->changed.wait(lock, completed);
    }
    if (shared->error) {
      std::rethrow_exception(shared->error);
    }
    return Result::done;
  }

  template <class Job> static bool post(Job &&job) noexcept {
    using Stored = std::decay_t<Job>;
    if (!pin()) {
      return false;
    }
    Stored *stored = nullptr;
    try {
      stored = new Stored(std::forward<Job>(job));
    } catch (...) {
      return false;
    }
    if (!enqueue(
            [](void *context) {
              auto *posted = static_cast<Stored *>(context);
              try {
                (*posted)();
              } catch (...) {
              }
              delete posted;
            },
            stored)) {
      delete stored;
      return false;
    }
    return true;
  }

  template <class Job> static bool post_after(double seconds, Job &&job) noexcept {
    using Stored = std::decay_t<Job>;
    if (!pin()) {
      return false;
    }
    Stored *stored = nullptr;
    try {
      stored = new Stored(std::forward<Job>(job));
    } catch (...) {
      return false;
    }
    CFRunLoopTimerContext context{};
    context.info = stored;
    auto *timer = CFRunLoopTimerCreate(
        nullptr, CFAbsoluteTimeGetCurrent() + std::max(seconds, 0.0), 0, 0, 0,
        [](CFRunLoopTimerRef, void *info) {
          auto *posted = static_cast<Stored *>(info);
          try {
            (*posted)();
          } catch (...) {
          }
          delete posted;
        },
        &context);
    if (!timer) {
      delete stored;
      return false;
    }
    CFRunLoopAddTimer(CFRunLoopGetMain(), timer, kCFRunLoopCommonModes);
    CFRelease(timer);
    return true;
  }

  static bool alive(double timeout_seconds) {
    return call([] {}, timeout_seconds) == Result::done;
  }

  static void wait(double seconds) {
    const auto end = deadline(seconds);
    if (!on_main()) {
      std::this_thread::sleep_until(end);
      return;
    }
    for (;;) {
      const std::chrono::duration<double> remaining = end - std::chrono::steady_clock::now();
      if (remaining.count() <= 0) {
        return;
      }
      if (CFRunLoopRunInMode(kCFRunLoopDefaultMode, remaining.count(), false) ==
          kCFRunLoopRunFinished) {
        std::this_thread::sleep_for(
            std::min<std::chrono::duration<double>>(remaining, std::chrono::milliseconds(1)));
      }
    }
  }

  // Pins the image containing this copy of the class. Idempotent; true once pinned.
  static bool pin() noexcept {
    static const bool pinned = [] {
      Dl_info info{};
      return dladdr(reinterpret_cast<const void *>(&MainThread::pin), &info) && info.dli_fname &&
             dlopen(info.dli_fname, RTLD_LAZY | RTLD_NOLOAD | RTLD_NODELETE) != nullptr;
    }();
    return pinned;
  }

private:
  struct JobEntry {
    void (*run)(void *);
    void *context;
  };
  struct Queue {
    std::mutex mutex;
    std::deque<JobEntry> jobs;
    CFRunLoopSourceRef source = nullptr;
    bool performing = false;
    Queue() {
      CFRunLoopSourceContext context{};
      context.info = this;
      context.perform = [](void *info) {
        auto &queue = *static_cast<Queue *>(info);
        JobEntry job;
        {
          std::lock_guard lock(queue.mutex);
          if (queue.jobs.empty()) {
            return;
          }
          job = queue.jobs.front();
          queue.jobs.pop_front();
          queue.performing = true;
        }
        job.run(job.context);
        std::lock_guard lock(queue.mutex);
        queue.performing = false;
        if (!queue.jobs.empty()) {
          CFRunLoopSourceSignal(queue.source);
          CFRunLoopWakeUp(CFRunLoopGetMain());
        }
      };
      source = CFRunLoopSourceCreate(nullptr, 0, &context);
      if (source) {
        CFRunLoopAddSource(CFRunLoopGetMain(), source, kCFRunLoopCommonModes);
      }
    }
  };

  static bool enqueue(void (*run)(void *), void *context) noexcept {
    try {
      static auto *const queue = new Queue;
      if (!queue->source) {
        return false;
      }
      std::lock_guard lock(queue->mutex);
      queue->jobs.push_back({run, context});
      if (!queue->performing) {
        CFRunLoopSourceSignal(queue->source);
        CFRunLoopWakeUp(CFRunLoopGetMain());
      }
      return true;
    } catch (...) {
      return false;
    }
  }

  enum class State { queued, running, completed, abandoned };
  struct Call {
    std::mutex mutex;
    std::condition_variable changed;
    State state = State::queued;
    void (*invoke)(void *) = nullptr;
    void *job = nullptr;
    std::exception_ptr error;
  };

  static std::chrono::steady_clock::time_point deadline(double seconds) {
    const double bounded = seconds > 0 ? std::min(seconds, 86400.0) : 0.0;
    return std::chrono::steady_clock::now() +
           std::chrono::duration_cast<std::chrono::steady_clock::duration>(
               std::chrono::duration<double>(bounded));
  }

  static void run_call(void *context) noexcept {
    auto *owner = static_cast<std::shared_ptr<Call> *>(context);
    const std::shared_ptr<Call> shared = std::move(*owner);
    delete owner;
    {
      std::lock_guard lock(shared->mutex);
      if (shared->state != State::queued) {
        return;
      }
      shared->state = State::running;
    }
    try {
      shared->invoke(shared->job);
    } catch (...) {
      shared->error = std::current_exception();
    }
    {
      std::lock_guard lock(shared->mutex);
      shared->state = State::completed;
    }
    shared->changed.notify_all();
  }
};

}

#endif
#endif
