#include <vespera/parameter_queue.hpp>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <thread>
#include <utility>
#include <vector>

namespace {

std::atomic<bool> counting_allocations{false};
std::atomic<uint64_t> allocation_count{0};

}

void *operator new(std::size_t size) {
  if (counting_allocations.load(std::memory_order_relaxed)) {
    allocation_count.fetch_add(1, std::memory_order_relaxed);
  }
  if (void *memory = std::malloc(size == 0 ? 1 : size)) {
    return memory;
  }
  throw std::bad_alloc();
}

void *operator new[](std::size_t size) { return operator new(size); }
void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete[](void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void *memory, std::size_t) noexcept { std::free(memory); }

namespace {

using vespera::Event;
using vespera::ParameterQueue;
using vespera::ParameterToken;
using Push = ParameterQueue::PushResult;

int failures = 0;
const char *current_test = "";

#define CHECK(condition)                                                                           \
  do {                                                                                             \
    if (!(condition)) {                                                                            \
      std::fprintf(stderr, "%s:%d: %s: CHECK(%s) failed\n", __FILE__, __LINE__, current_test,      \
                   #condition);                                                                    \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (false)

std::unique_ptr<ParameterQueue> make_queue(uint64_t graph_generation = 7) {
  auto queue = std::make_unique<ParameterQueue>();
  queue->clear(graph_generation);
  return queue;
}

Push push(ParameterQueue &queue, float value, std::optional<uint64_t> frame = std::nullopt,
          vespera::NodeId node = 0, uint32_t parameter = 1) {
  return queue.push(queue.token(), node, parameter, value, frame);
}

bool event_is(const Event &event, vespera::NodeId node, uint32_t parameter, uint32_t offset,
              float value) {
  return event.node == node && event.parameter.parameter_id == parameter &&
         event.parameter.frame_offset == offset && event.parameter.value == value;
}

void test_tokens() {
  ParameterQueue unbound;
  CHECK(unbound.token() == ParameterToken{});
  CHECK(unbound.push({}, 0, 1, 1.0f, std::nullopt) == Push::stale_token);

  auto queue = make_queue(7);
  const ParameterToken first = queue->token();
  CHECK(first.graph_generation == 7);
  CHECK(first.timeline_generation != 0);
  CHECK(queue->push({7, first.timeline_generation + 1}, 0, 1, 1.0f, std::nullopt) ==
        Push::stale_token);
  CHECK(queue->push({8, first.timeline_generation}, 0, 1, 1.0f, std::nullopt) == Push::stale_token);
  CHECK(queue->push(first, 0, 1, 1.0f, std::nullopt) == Push::accepted);

  queue->clear(7);
  CHECK(queue->token().timeline_generation > first.timeline_generation);
  CHECK(queue->push(first, 0, 1, 1.0f, std::nullopt) == Push::stale_token);
  CHECK(queue->outstanding() == 0);
  const auto telemetry = queue->telemetry();
  CHECK(telemetry.accepted == 1);
  CHECK(telemetry.rejected == 3);
  CHECK(telemetry.stale == 1);
  CHECK(queue->collect(0, 64).empty());
}

void test_immediate_and_offsets() {
  auto queue = make_queue();
  CHECK(push(*queue, 2.0f, 1000 + 40) == Push::accepted);
  CHECK(push(*queue, 1.0f) == Push::accepted);
  CHECK(push(*queue, 3.0f, 1000 + 10, 4, 9) == Push::accepted);
  const auto events = queue->collect(1000, 64);
  CHECK(events.size() == 3);
  if (events.size() == 3) {
    CHECK(event_is(events[0], 0, 1, 0, 1.0f));
    CHECK(event_is(events[1], 4, 9, 10, 3.0f));
    CHECK(event_is(events[2], 0, 1, 40, 2.0f));
  }
  CHECK(queue->outstanding() == 0);
  CHECK(queue->collect(1064, 64).empty());
}

void test_empty_block_preserves_requests() {
  auto queue = make_queue();
  CHECK(push(*queue, 1.0f) == Push::accepted);
  CHECK(push(*queue, 2.0f, 5) == Push::accepted);
  CHECK(queue->collect(10, 0).empty());
  CHECK(queue->outstanding() == 2);
  CHECK(queue->telemetry().late == 0);
  const auto events = queue->collect(10, 1);
  CHECK(events.size() == 2);
  CHECK(queue->telemetry().late == 1);
}

void test_future_does_not_block_later_requests() {
  auto queue = make_queue();
  CHECK(push(*queue, 4.0f, 5000) == Push::accepted);
  CHECK(push(*queue, 1.0f) == Push::accepted);
  CHECK(push(*queue, 2.0f, 70) == Push::accepted);
  auto events = queue->collect(0, 64);
  CHECK(events.size() == 1 && event_is(events[0], 0, 1, 0, 1.0f));
  CHECK(push(*queue, 3.0f) == Push::accepted);
  events = queue->collect(64, 64);
  CHECK(events.size() == 2);
  if (events.size() == 2) {
    CHECK(event_is(events[0], 0, 1, 0, 3.0f));
    CHECK(event_is(events[1], 0, 1, 6, 2.0f));
  }
  CHECK(queue->outstanding() == 1);
  events = queue->collect(4990, 64);
  CHECK(events.size() == 1 && event_is(events[0], 0, 1, 10, 4.0f));
  CHECK(queue->telemetry().late == 0);
}

void test_equal_positions_keep_acceptance_order() {
  auto queue = make_queue();
  for (int i = 0; i < 200; ++i) {
    CHECK(push(*queue, static_cast<float>(i),
               i % 2 == 0 ? std::optional<uint64_t>(5) : std::optional<uint64_t>()) ==
          Push::accepted);
  }
  const auto events = queue->collect(0, 16);
  CHECK(events.size() == 200);
  float previous_immediate = -1.0f;
  float previous_scheduled = -1.0f;
  for (size_t i = 0; i < events.size(); ++i) {
    const auto &parameter = events[i].parameter;
    if (i < 100) {
      CHECK(parameter.frame_offset == 0 && parameter.value > previous_immediate);
      previous_immediate = parameter.value;
    } else {
      CHECK(parameter.frame_offset == 5 && parameter.value > previous_scheduled);
      previous_scheduled = parameter.value;
    }
  }
}

void test_late_and_block_end() {
  auto queue = make_queue();
  CHECK(push(*queue, 1.0f, 64) == Push::accepted);
  CHECK(push(*queue, 2.0f, 10) == Push::accepted);
  CHECK(push(*queue, 3.0f) == Push::accepted);
  auto events = queue->collect(128, 64);
  CHECK(events.size() == 3);
  if (events.size() == 3) {
    CHECK(event_is(events[0], 0, 1, 0, 1.0f));
    CHECK(event_is(events[1], 0, 1, 0, 2.0f));
    CHECK(event_is(events[2], 0, 1, 0, 3.0f));
  }
  CHECK(queue->telemetry().late == 2);

  CHECK(push(*queue, 4.0f, 256) == Push::accepted);
  CHECK(push(*queue, 5.0f, 255) == Push::accepted);
  events = queue->collect(192, 64);
  CHECK(events.size() == 1 && event_is(events[0], 0, 1, 63, 5.0f));
  events = queue->collect(256, 1);
  CHECK(events.size() == 1 && event_is(events[0], 0, 1, 0, 4.0f));
  CHECK(queue->telemetry().late == 2);

  CHECK(push(*queue, 6.0f, std::numeric_limits<uint64_t>::max()) == Push::accepted);
  CHECK(queue->collect(std::numeric_limits<uint64_t>::max() - 3, 3).empty());
  events = queue->collect(std::numeric_limits<uint64_t>::max() - 1, 2);
  CHECK(events.size() == 1 && events[0].parameter.frame_offset == 1);
}

void test_capacity_counts_pending_and_transit() {
  auto queue = make_queue();
  for (uint32_t i = 0; i < 200; ++i) {
    CHECK(push(*queue, 1.0f, 1000 + i) == Push::accepted);
  }
  CHECK(queue->collect(0, 64).empty());
  CHECK(queue->outstanding() == 200);
  for (uint32_t i = 0; i < 56; ++i) {
    CHECK(push(*queue, 2.0f, 2000) == Push::accepted);
  }
  CHECK(queue->outstanding() == ParameterQueue::capacity);
  CHECK(push(*queue, 3.0f) == Push::queue_full);
  CHECK(queue->collect(64, 64).empty());
  CHECK(push(*queue, 3.0f) == Push::queue_full);
  auto telemetry = queue->telemetry();
  CHECK(telemetry.queue_full == 2);
  CHECK(telemetry.rejected == 2);

  auto events = queue->collect(1000, 10);
  CHECK(events.size() == 10);
  CHECK(queue->outstanding() == ParameterQueue::capacity - 10);
  for (int i = 0; i < 10; ++i) {
    CHECK(push(*queue, 3.0f) == Push::accepted);
  }
  CHECK(push(*queue, 3.0f) == Push::queue_full);
  events = queue->collect(1010, 1024);
  CHECK(events.size() == ParameterQueue::capacity);
  CHECK(queue->outstanding() == 0);
}

void test_invalidation_drops_pending_and_transit() {
  auto queue = make_queue();
  const ParameterToken old = queue->token();
  CHECK(push(*queue, 1.0f, 500) == Push::accepted);
  CHECK(queue->collect(0, 64).empty());
  CHECK(push(*queue, 2.0f) == Push::accepted);
  queue->invalidate_timeline();
  CHECK(queue->token().timeline_generation != old.timeline_generation);
  CHECK(queue->outstanding() == 1);
  CHECK(queue->push(old, 0, 1, 3.0f, std::nullopt) == Push::stale_token);
  CHECK(push(*queue, 4.0f) == Push::accepted);
  const auto events = queue->collect(1000, 64);
  CHECK(events.size() == 1 && event_is(events[0], 0, 1, 0, 4.0f));
  CHECK(queue->outstanding() == 0);
  const auto telemetry = queue->telemetry();
  CHECK(telemetry.stale == 2);
  CHECK(telemetry.accepted == 3);
}

void test_no_allocations() {
  auto queue = make_queue();
  counting_allocations.store(true);
  allocation_count.store(0);
  for (uint32_t i = 0; i < ParameterQueue::capacity; ++i) {
    push(*queue, static_cast<float>(i % 7),
         i % 3 == 0 ? std::optional<uint64_t>(300 - i) : std::optional<uint64_t>());
  }
  auto events = queue->collect(0, 128);
  events = queue->collect(128, 128);
  queue->invalidate_timeline();
  queue->activate_graph(9);
  queue->count_applied(events.size());
  counting_allocations.store(false);
  CHECK(allocation_count.load() == 0);
}

void test_graph_activation() {
  auto queue = make_queue(7);
  const auto old = queue->token();
  CHECK(push(*queue, 1.0f, 500, 7) == Push::accepted);
  CHECK(queue->collect(0, 64).empty());
  CHECK(push(*queue, 2.0f, std::nullopt, 7) == Push::accepted);
  queue->activate_graph(8);
  const auto next = queue->token();
  CHECK(next.graph_generation == 8);
  CHECK(next.timeline_generation != old.timeline_generation);
  CHECK(queue->outstanding() == 1);
  CHECK(queue->push(old, 7, 1, 3.0f, std::nullopt) == Push::stale_token);
  CHECK(push(*queue, 4.0f, std::nullopt, 8) == Push::accepted);
  const auto events = queue->collect(64, 64);
  CHECK(events.size() == 1 && event_is(events[0], 8, 1, 0, 4.0f));
  queue->count_applied(events.size());
  CHECK(queue->outstanding() == 0);
  CHECK(queue->telemetry().accepted == 3);
  CHECK(queue->telemetry().stale == 2);
  CHECK(queue->telemetry().applied == 1);
  queue->activate_graph(8);
  CHECK(queue->push(next, 8, 1, 5.0f, std::nullopt) == Push::stale_token);
}

void test_concurrent_graph_activation() {
  auto queue = make_queue(1);
  std::atomic<bool> done{false};
  uint64_t accepted = 0;
  uint64_t delivered = 0;
  uint64_t wrong_graph = 0;
  uint64_t capacity_errors = 0;
  std::thread producer([&] {
    for (uint32_t i = 0; i < 100000; ++i) {
      const auto token = queue->token();
      if (queue->push(token, token.graph_generation, 1, 1.0f, std::nullopt) == Push::accepted) {
        ++accepted;
      }
      if (i % 256 == 0) {
        std::this_thread::yield();
      }
    }
    done.store(true, std::memory_order_release);
  });
  uint64_t generation = 1;
  for (uint64_t block = 0;; ++block) {
    const bool finished = done.load(std::memory_order_acquire);
    if (block % 31 == 0) {
      queue->activate_graph(++generation);
    }
    for (const auto &event : queue->collect(block * 64, 64)) {
      wrong_graph += event.node != generation;
      ++delivered;
    }
    capacity_errors += queue->outstanding() > ParameterQueue::capacity;
    if (finished && queue->outstanding() == 0) {
      break;
    }
  }
  producer.join();
  CHECK(accepted > 0);
  CHECK(wrong_graph == 0);
  CHECK(capacity_errors == 0);
  CHECK(delivered + queue->telemetry().stale == accepted);
  CHECK(queue->telemetry().accepted == accepted);
}

// One producer and one consumer, exercising ring wraparound, the shared capacity limit,
// timeline invalidation racing with publication, and FIFO order across many blocks.
void test_two_thread_stress() {
  auto queue = make_queue(3);
  constexpr uint64_t attempts = 400000;
  constexpr uint32_t frames = 64;
  std::atomic<bool> producer_done{false};
  std::atomic<uint64_t> published_position{0};
  uint64_t accepted = 0;
  uint64_t full = 0;
  uint64_t delivered = 0;
  uint64_t order_errors = 0;
  uint64_t bound_errors = 0;

  std::thread producer([&] {
    uint32_t value = 0;
    for (uint64_t i = 0; i < attempts; ++i) {
      std::optional<uint64_t> frame;
      vespera::NodeId node = 0;
      if (i % 3 == 0) {
        node = 1;
        frame = published_position.load(std::memory_order_relaxed) + (i * 37) % 1500;
      }
      const ParameterToken token = queue->token();
      switch (queue->push(token, node, 1, static_cast<float>(value), frame)) {
      case Push::accepted:
        ++accepted;
        value = (value + 1) % (1u << 24);
        break;
      case Push::queue_full:
        ++full;
        std::this_thread::yield();
        break;
      case Push::stale_token:
        break;
      }
    }
    producer_done.store(true, std::memory_order_release);
  });

  uint64_t position = 0;
  uint64_t blocks = 0;
  float previous_immediate = -1.0f;
  while (true) {
    const bool done = producer_done.load(std::memory_order_acquire);
    if (++blocks % 251 == 0) {
      queue->invalidate_timeline();
      previous_immediate = -1.0f;
      position += 1000;
    }
    if (queue->outstanding() > ParameterQueue::capacity) {
      ++bound_errors;
    }
    const auto events = queue->collect(position, frames);
    uint32_t previous_offset = 0;
    for (const Event &event : events) {
      if (event.parameter.frame_offset < previous_offset ||
          event.parameter.frame_offset >= frames) {
        ++order_errors;
      }
      previous_offset = event.parameter.frame_offset;
      if (event.node == 0) {
        if (event.parameter.value <= previous_immediate &&
            previous_immediate - event.parameter.value < 1000000.0f) {
          ++order_errors;
        }
        previous_immediate = event.parameter.value;
      }
    }
    delivered += events.size();
    queue->count_applied(events.size());
    position += frames;
    published_position.store(position, std::memory_order_relaxed);
    if (done && queue->outstanding() == 0) {
      break;
    }
  }
  producer.join();

  const auto telemetry = queue->telemetry();
  CHECK(order_errors == 0);
  CHECK(bound_errors == 0);
  CHECK(accepted > attempts / 4);
  CHECK(accepted / ParameterQueue::capacity > 2);
  CHECK(telemetry.accepted == accepted);
  CHECK(telemetry.queue_full == full);
  CHECK(telemetry.applied == delivered);
  CHECK(delivered + telemetry.stale == accepted);
  CHECK(telemetry.stale > 0);
}

}

int main() {
  const std::pair<const char *, std::function<void()>> tests[] = {
      {"tokens", test_tokens},
      {"immediate_and_offsets", test_immediate_and_offsets},
      {"future_does_not_block_later_requests", test_future_does_not_block_later_requests},
      {"equal_positions_keep_acceptance_order", test_equal_positions_keep_acceptance_order},
      {"late_and_block_end", test_late_and_block_end},
      {"empty_block_preserves_requests", test_empty_block_preserves_requests},
      {"capacity_counts_pending_and_transit", test_capacity_counts_pending_and_transit},
      {"invalidation_drops_pending_and_transit", test_invalidation_drops_pending_and_transit},
      {"no_allocations", test_no_allocations},
      {"graph_activation", test_graph_activation},
      {"concurrent_graph_activation", test_concurrent_graph_activation},
      {"two_thread_stress", test_two_thread_stress},
  };
  for (const auto &[name, test] : tests) {
    current_test = name;
    test();
  }
  if (failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("all parameter queue tests passed\n");
  return 0;
}
