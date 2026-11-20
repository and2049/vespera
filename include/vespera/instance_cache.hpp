#ifndef VESPERA_INSTANCE_CACHE_HPP
#define VESPERA_INSTANCE_CACHE_HPP

#include <vespera/module.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace vespera {

// Idle module-created instances kept for reuse by later engine preparations, implementing
// vs_host_instance_cache. Instances and payloads are opaque: the cache only matches keys and
// signatures, keeps at most capacity_per_key idle instances per key, newest first, and disposes
// the instances it evicts, trims or still holds when destroyed through the dispose function each
// was released with. It calls dispose functions outside its own mutex, so they may take their own
// locks, such as the plugin lifecycle lock.
//
// Modules reach the cache through client descriptors. Every instance released through a client
// keeps the client's owner, typically the loaded library containing the dispose function, alive
// until the instance is reused or disposed. Clients keep the cache alive, which must therefore be
// owned by a std::shared_ptr.
class InstanceCache : public std::enable_shared_from_this<InstanceCache> {
public:
  struct Statistics {
    uint64_t hits = 0;
    uint64_t signature_hits = 0;
    uint64_t misses = 0;
    uint64_t stored = 0;
    uint64_t evicted = 0;
  };

  explicit InstanceCache(size_t capacity_per_key = 4);
  ~InstanceCache();
  InstanceCache(const InstanceCache &) = delete;
  InstanceCache &operator=(const InstanceCache &) = delete;

  // A descriptor that stays valid while the returned pointer is held.
  std::shared_ptr<const vs_host_instance_cache> client(std::shared_ptr<const void> owner);
  // Disposes the oldest idle instances until at most keep_per_key remain for every key.
  void trim(size_t keep_per_key);
  size_t idle() const;
  Statistics statistics() const;

private:
  struct Client {
    vs_host_instance_cache descriptor;
    std::shared_ptr<InstanceCache> cache;
    std::shared_ptr<const void> owner;
  };
  struct Entry {
    std::string key;
    std::string signature;
    void *instance = nullptr;
    void *payload = nullptr;
    vs_cached_instance_dispose_fn dispose = nullptr;
    std::shared_ptr<const void> owner;
    uint64_t sequence = 0;
  };

  static void *acquire(void *context, const char *key, const char *signature, void **payload);
  static void release(void *context, const char *key, const char *signature, void *instance,
                      void *payload, vs_cached_instance_dispose_fn dispose);
  static void dispose(std::vector<Entry> &entries) noexcept;

  mutable std::mutex mutex_;
  size_t capacity_per_key_;
  uint64_t sequence_ = 0;
  std::vector<Entry> entries_;
  Statistics statistics_;
};

}

#endif
