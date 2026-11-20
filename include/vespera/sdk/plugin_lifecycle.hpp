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
 */
class PluginLifecycleLock {
public:
  PluginLifecycleLock() {
    State &s = state();
    s.mutex.lock();
    bool held = s.depth > 0;
    try {
      held = held || s.acquire_file();
    } catch (...) {
    }
    if (!held) {
      s.mutex.unlock();
      return;
    }
    ++s.depth;
    acquired_ = true;
  }
  ~PluginLifecycleLock() {
    if (!acquired_) {
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

  explicit operator bool() const noexcept { return acquired_; }

private:
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

    bool acquire_file() {
#if defined(__APPLE__) || defined(__linux__)
      if (descriptor < 0) {
        const std::string base = directory();
        if (base.empty()) {
          return false;
        }
        const std::string path = base + (base.back() == '/' ? "" : "/") +
                                 "vespera-plugin-lifecycle-" + std::to_string(getpid()) + ".lock";
        descriptor = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (descriptor < 0) {
          return false;
        }
      }
      while (flock(descriptor, LOCK_EX) != 0) {
        if (errno != EINTR) {
          return false;
        }
      }
      return true;
#else
      return false;
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

  bool acquired_ = false;
};

}

#endif
