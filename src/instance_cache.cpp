#include <vespera/instance_cache.hpp>

#include <algorithm>
#include <map>
#include <string_view>

namespace vespera {

InstanceCache::InstanceCache(size_t capacity_per_key) : capacity_per_key_(capacity_per_key) {}

InstanceCache::~InstanceCache() { trim(0); }

std::shared_ptr<const vs_host_instance_cache>
InstanceCache::client(std::shared_ptr<const void> owner) {
  auto client = std::make_shared<Client>();
  client->descriptor = {sizeof(vs_host_instance_cache), client.get(), acquire, release};
  client->cache = shared_from_this();
  client->owner = std::move(owner);
  return {client, &client->descriptor};
}

void *InstanceCache::acquire(void *context, const char *key, const char *signature,
                             void **payload) {
  auto &self = *static_cast<Client *>(context)->cache;
  if (payload) {
    *payload = nullptr;
  }
  if (!key || !signature || !payload) {
    return nullptr;
  }
  std::lock_guard lock(self.mutex_);
  auto best = self.entries_.end();
  for (auto it = self.entries_.begin(); it != self.entries_.end(); ++it) {
    if (it->key != key) {
      continue;
    }
    const bool same = it->signature == signature;
    const bool best_same = best != self.entries_.end() && best->signature == signature;
    if (best == self.entries_.end() || (same && !best_same) ||
        (same == best_same && it->sequence > best->sequence)) {
      best = it;
    }
  }
  if (best == self.entries_.end()) {
    ++self.statistics_.misses;
    return nullptr;
  }
  ++self.statistics_.hits;
  if (best->signature == signature) {
    ++self.statistics_.signature_hits;
  }
  void *instance = best->instance;
  *payload = best->payload;
  self.entries_.erase(best);
  return instance;
}

void InstanceCache::release(void *context, const char *key, const char *signature, void *instance,
                            void *payload, vs_cached_instance_dispose_fn dispose_instance) {
  const auto &client = *static_cast<Client *>(context);
  auto &self = *client.cache;
  if (!instance || !dispose_instance) {
    return;
  }
  const std::string_view stored_key(key ? key : "");
  std::vector<Entry> evicted;
  bool stored = false;
  {
    std::lock_guard lock(self.mutex_);
    try {
      evicted.reserve(self.entries_.size() + 1);
      self.entries_.push_back(Entry{std::string(stored_key), signature ? signature : "", instance,
                                    payload, dispose_instance, client.owner, self.sequence_ + 1});
      ++self.sequence_;
      stored = true;
      ++self.statistics_.stored;
      auto count =
          static_cast<size_t>(std::count_if(self.entries_.begin(), self.entries_.end(),
                                            [&](const Entry &e) { return e.key == stored_key; }));
      while (count-- > self.capacity_per_key_) {
        auto oldest = self.entries_.end();
        for (auto it = self.entries_.begin(); it != self.entries_.end(); ++it) {
          if (it->key == stored_key &&
              (oldest == self.entries_.end() || it->sequence < oldest->sequence)) {
            oldest = it;
          }
        }
        evicted.push_back(std::move(*oldest));
        self.entries_.erase(oldest);
      }
    } catch (...) {
    }
    self.statistics_.evicted += evicted.size() + (stored ? 0 : 1);
  }
  if (!stored) {
    dispose_instance(instance, payload);
    return;
  }
  dispose(evicted);
}

void InstanceCache::dispose(std::vector<Entry> &entries) noexcept {
  for (auto &entry : entries) {
    entry.dispose(entry.instance, entry.payload);
    entry.instance = nullptr;
  }
  entries.clear();
}

void InstanceCache::trim(size_t keep_per_key) {
  std::vector<Entry> evicted;
  {
    std::lock_guard lock(mutex_);
    std::sort(entries_.begin(), entries_.end(),
              [](const Entry &a, const Entry &b) { return a.sequence > b.sequence; });
    std::vector<Entry> kept;
    std::map<std::string, size_t> seen;
    for (auto &e : entries_) {
      (seen[e.key]++ < keep_per_key ? kept : evicted).push_back(std::move(e));
    }
    entries_ = std::move(kept);
    statistics_.evicted += evicted.size();
  }
  dispose(evicted);
}

size_t InstanceCache::idle() const {
  std::lock_guard lock(mutex_);
  return entries_.size();
}

InstanceCache::Statistics InstanceCache::statistics() const {
  std::lock_guard lock(mutex_);
  return statistics_;
}

}
