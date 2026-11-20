// Host instance cache: opaque storage, key and signature matching, eviction and disposal.
#include <vespera/instance_cache.hpp>

#include <cstdio>
#include <memory>
#include <source_location>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using vespera::InstanceCache;

void check(bool condition, std::source_location location = std::source_location::current()) {
  if (!condition) {
    throw std::runtime_error("instance cache check failed at line " +
                             std::to_string(location.line()));
  }
}

struct Disposal {
  int instance;
  int payload;
  size_t idle_seen;
};

std::vector<Disposal> disposals;
InstanceCache *observed = nullptr;

int instances[8];
int payloads[8];

int index_of(void *pointer, int *table) {
  return static_cast<int>(static_cast<int *>(pointer) - table);
}

// Reads the cache from inside the dispose function, which would deadlock if the cache held its own
// mutex while disposing.
void dispose(void *instance, void *payload) {
  disposals.push_back({index_of(instance, instances), payload ? index_of(payload, payloads) : -1,
                       observed ? observed->idle() : 0});
}

void release(const vs_host_instance_cache &cache, const char *key, const char *signature, int i,
             vs_cached_instance_dispose_fn function = dispose) {
  cache.release(cache.context, key, signature, &instances[i], &payloads[i], function);
}

int acquire(const vs_host_instance_cache &cache, const char *key, const char *signature) {
  void *payload = nullptr;
  void *instance = cache.acquire(cache.context, key, signature, &payload);
  if (!instance) {
    check(payload == nullptr);
    return -1;
  }
  check(index_of(payload, payloads) == index_of(instance, instances));
  return index_of(instance, instances);
}

void matching() {
  disposals.clear();
  auto cache = std::make_shared<InstanceCache>();
  const auto client = cache->client(nullptr);
  check(client->struct_size == sizeof(vs_host_instance_cache));
  check(acquire(*client, "k", "s") == -1);
  release(*client, "k", "x", 0);
  release(*client, "k", "s", 1);
  release(*client, "k", "x", 2);
  release(*client, "other", "s", 3);
  check(cache->idle() == 4);
  check(acquire(*client, "k", "s") == 1);
  check(acquire(*client, "k", "s") == 2);
  check(acquire(*client, "k", "s") == 0);
  check(acquire(*client, "k", "s") == -1);
  void *payload = &payloads[0];
  check(client->acquire(client->context, nullptr, "s", &payload) == nullptr && payload == nullptr);
  check(client->acquire(client->context, "other", "s", nullptr) == nullptr);
  release(*client, "k", "s", 4, nullptr);
  check(cache->idle() == 1);
  const auto counts = cache->statistics();
  check(counts.hits == 3 && counts.signature_hits == 1 && counts.misses == 2 &&
        counts.stored == 4 && counts.evicted == 0);
  check(disposals.empty());
}

void eviction_and_disposal() {
  disposals.clear();
  auto cache = std::make_shared<InstanceCache>(2);
  observed = cache.get();
  auto owner = std::make_shared<int>(7);
  const std::weak_ptr<int> watched = owner;
  auto client = cache->client(owner);
  owner.reset();
  check(!watched.expired());
  release(*client, "k", "s", 0);
  release(*client, "k", "s", 1);
  release(*client, "j", "s", 2);
  release(*client, "k", "s", 3);
  check(disposals.size() == 1 && disposals[0].instance == 0 && disposals[0].payload == 0);
  check(disposals[0].idle_seen == 3);
  check(cache->statistics().evicted == 1);
  client.reset();
  check(!watched.expired());
  cache->trim(1);
  check(disposals.size() == 2 && disposals[1].instance == 1 && cache->idle() == 2);
  check(!watched.expired());
  auto reused = cache->client(nullptr);
  check(acquire(*reused, "k", "other") == 3);
  check(!watched.expired());
  observed = nullptr;
  cache.reset();
  release(*reused, "k", "s", 3);
  check(disposals.size() == 2 && !watched.expired());
  reused.reset();
  check(disposals.size() == 4 && watched.expired());
}

// The cache disposes what it still holds when its last owner and client go away, then releases the
// owners that kept the disposing code loaded.
void destruction() {
  disposals.clear();
  auto owner = std::make_shared<int>(1);
  const std::weak_ptr<int> watched = owner;
  {
    auto cache = std::make_shared<InstanceCache>();
    auto client = cache->client(std::move(owner));
    release(*client, "k", "s", 5);
    release(*client, "j", "s", 6);
  }
  check(disposals.size() == 2 && watched.expired());
}
}

int main() {
  try {
    matching();
    eviction_and_disposal();
    destruction();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
  return 0;
}
