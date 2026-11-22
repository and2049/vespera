#ifndef VESPERA_SDK_PLUGIN_LIFECYCLE_HPP
#define VESPERA_SDK_PLUGIN_LIFECYCLE_HPP

#include <mutex>
#include <string>

#if defined(__APPLE__) || defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace vespera::sdk {

/*
 * Process-wide lock serializing hosted plugin lifecycle calls: creating, configuring,
 * initializing, uninitializing and disposing instances. Some plugins crash when these overlap on
 * different threads, and they can be issued by independently built module libraries, by host
 * code and by host UI catalogs, none of which share a C++ static.
 *
 * Each library holds an exclusive flock(2) on a per-process lock file in the user's private
 * temporary directory while it holds the lock. flock conflicts between separate open file
 * descriptions even within one process, so separately loaded libraries exclude each other; the
 * kernel releases it when the process exits. A library-local recursive mutex serializes threads
 * within the library and makes the lock reentrant on one thread inside one library. Code holding
 * the lock must not call into another library that takes it on the same thread.
 *
 * Acquisition can fail, for example when the lock file cannot be opened. The lock is then not
 * held and converts to false; callers must not make lifecycle calls. Preparation reports an
 * error, and release abandons the instance rather than disposing it unserialized.
 *
 * Constructed with std::try_to_lock it never waits: acquisition() is busy while another thread
 * or library holds the lock, so a thread that must stay responsive can retry later.
 */
class PluginLifecycleLock {
public:
  enum class Acquisition { held, busy, failed };

  PluginLifecycleLock() {
    State &s = state();
    s.mutex.lock();
    finish(s, s.depth > 0 ? Attempt::held : s.acquire_file(true));
  }
  explicit PluginLifecycleLock(std::try_to_lock_t) {
    State &s = state();
    if (!s.mutex.try_lock()) {
      acquisition_ = Acquisition::busy;
      return;
    }
    finish(s, s.depth > 0 ? Attempt::held : s.acquire_file(false));
  }
  ~PluginLifecycleLock() {
    if (acquisition_ != Acquisition::held) {
      return;
    }
    State &s = state();
    if (--s.depth == 0) {
      s.release_file();
    }
    s.mutex.unlock();
  }
  PluginLifecycleLock(const PluginLifecycleLock &) = delete;
  PluginLifecycleLock &operator=(const PluginLifecycleLock &) = delete;

  explicit operator bool() const noexcept { return acquisition_ == Acquisition::held; }
  Acquisition acquisition() const noexcept { return acquisition_; }

private:
  enum class Attempt { held, busy, failed };

  struct State {
    std::recursive_mutex mutex;
    int depth = 0;
    int descriptor = -1;

    static std::string directory() {
#if defined(VESPERA_PLUGIN_LIFECYCLE_DIRECTORY)
      return VESPERA_PLUGIN_LIFECYCLE_DIRECTORY;
#elif defined(__APPLE__)
      char path[1024];
      const size_t length = confstr(_CS_DARWIN_USER_TEMP_DIR, path, sizeof(path));
      return length > 0 && length <= sizeof(path) ? std::string(path) : std::string();
#else
      return "/tmp/";
#endif
    }

    Attempt acquire_file(bool wait) noexcept {
#if defined(__APPLE__) || defined(__linux__)
      try {
        if (descriptor < 0) {
          const std::string base = directory();
          if (base.empty()) {
            return Attempt::failed;
          }
          const std::string path = base + (base.back() == '/' ? "" : "/") +
                                   "vespera-plugin-lifecycle-" + std::to_string(getpid()) + ".lock";
          descriptor = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
          if (descriptor < 0) {
            return Attempt::failed;
          }
        }
      } catch (...) {
        return Attempt::failed;
      }
      while (flock(descriptor, LOCK_EX | (wait ? 0 : LOCK_NB)) != 0) {
        if (errno == EWOULDBLOCK && !wait) {
          return Attempt::busy;
        }
        if (errno != EINTR) {
          return Attempt::failed;
        }
      }
      return Attempt::held;
#else
      static_cast<void>(wait);
      return Attempt::failed;
#endif
    }
    void release_file() {
#if defined(__APPLE__) || defined(__linux__)
      flock(descriptor, LOCK_UN);
#endif
    }
  };
  // Function-local and never destroyed, so it outlives static destructors that dispose plugins.
  static State &state() {
    static State *instance = new State;
    return *instance;
  }

  void finish(State &s, Attempt attempt) noexcept {
    if (attempt != Attempt::held) {
      s.mutex.unlock();
      acquisition_ = attempt == Attempt::busy ? Acquisition::busy : Acquisition::failed;
      return;
    }
    ++s.depth;
    acquisition_ = Acquisition::held;
  }

  Acquisition acquisition_ = Acquisition::failed;
};

}

#endif
