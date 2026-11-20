#ifndef VESPERA_PARAMETER_QUEUE_HPP
#define VESPERA_PARAMETER_QUEUE_HPP

#include <vespera/runtime.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <span>

namespace vespera {

// Identifies the prepared graph and continuous timeline a control-thread request was issued
// against. Zero values never match an active timeline.
struct ParameterToken {
  uint64_t graph_generation = 0;
  uint64_t timeline_generation = 0;

  friend bool operator==(const ParameterToken &, const ParameterToken &) = default;
};

enum class ParameterResult {
  accepted,
  not_running,
  stale_token,
  unknown_node,
  unknown_parameter,
  invalid_value,
  queue_full
};

const char *to_string(ParameterResult result) noexcept;

// Counters are individually race-free but sampled independently, so a snapshot is approximate.
// Accepted requests may later be counted as stale when the timeline is invalidated before
// delivery. Applied counts events delivered to a block whose processing succeeded.
struct ParameterTelemetry {
  uint64_t accepted = 0;
  uint64_t rejected = 0;
  uint64_t queue_full = 0;
  uint64_t applied = 0;
  uint64_t stale = 0;
  uint64_t late = 0;
};

// Single-producer/single-consumer transport of parameter changes from one control thread to
// one audio thread. Every accepted request occupies one of `capacity` slots from publication
// until it is delivered to a block or discarded, whether it is still in transit or waiting for
// a future frame. Requests are rejected when full rather than overwritten or coalesced.
//
// Delivered events are ordered by frame offset and then by acceptance order. Future events are
// held aside so they never delay immediate or earlier events. Absolute positions already in the
// past are delivered at offset zero and counted as late; a position equal to the end of the
// block remains pending for the next block.
//
// Threading: push() and token() belong to the producer; collect(), invalidate_timeline(),
// activate_graph() and count_applied() belong to the consumer; clear() and reset_counters() require
// that neither is running concurrently. telemetry() may be called from any thread.
class ParameterQueue {
public:
  static constexpr uint32_t capacity = 256;
  static_assert(capacity <= Graph::max_events_per_block);
  static_assert((capacity & (capacity - 1)) == 0);

  enum class PushResult { accepted, stale_token, queue_full };

  ParameterQueue() noexcept = default;
  ParameterQueue(const ParameterQueue &) = delete;
  ParameterQueue &operator=(const ParameterQueue &) = delete;

  // Discards every outstanding request, binds the queue to `graph_generation`, and begins a new
  // timeline so that all previously issued tokens become stale. Requires quiescence.
  void clear(uint64_t graph_generation) noexcept;
  void reset_counters() noexcept;

  ParameterToken token() const noexcept;

  // Producer. `frame_position` empty means the next processed block, at offset zero.
  // Accepted means queued for delivery, not applied: it may still be discarded as stale.
  PushResult push(ParameterToken token, NodeId node, uint32_t parameter_id, float value,
                  std::optional<uint64_t> frame_position) noexcept;
  // Producer. Records a request rejected before reaching push().
  void count_rejected() noexcept;

  // Consumer. Starts a new timeline and drops every pending request of the previous one;
  // requests still in transit are dropped when the consumer next observes them.
  void invalidate_timeline() noexcept;

  // Consumer. Rebinds to a prepared graph without resetting the ring or counters. Pending and
  // in-transit requests for the previous graph/timeline become stale. A racing producer may
  // publish a stale request, which collect() discards rather than delivering to the new graph.
  void activate_graph(uint64_t graph_generation) noexcept;

  // Consumer. Returns events due within [frame_position, frame_position + frames), valid until
  // the next consumer call. Their slots are released before returning. Only requests published
  // before the call begins are considered, so concurrent pushes cannot extend its work beyond
  // `capacity` requests.
  // A zero-frame block leaves all requests pending.
  std::span<const Event> collect(uint64_t frame_position, uint32_t frames) noexcept;
  void count_applied(uint64_t events) noexcept;

  ParameterTelemetry telemetry() const noexcept;
  uint32_t outstanding() const noexcept { return outstanding_.load(std::memory_order_acquire); }

private:
  struct Request {
    ParameterToken token;
    NodeId node = 0;
    uint32_t parameter_id = 0;
    float value = 0.0f;
    bool immediate = true;
    uint64_t frame_position = 0;
  };

  struct Pending {
    Request request;
    uint64_t sequence = 0;
  };

  struct Due {
    uint32_t offset = 0;
    uint64_t sequence = 0;
    Event event;
  };

  static_assert(std::atomic<uint64_t>::is_always_lock_free);
  static_assert(std::atomic<uint32_t>::is_always_lock_free);

  void release(uint32_t count) noexcept;
  void drain() noexcept;

  // Producer-owned.
  alignas(64) std::atomic<uint64_t> tail_{0};
  std::atomic<uint64_t> accepted_{0};
  std::atomic<uint64_t> rejected_{0};
  std::atomic<uint64_t> queue_full_{0};

  // Consumer-owned.
  alignas(64) std::atomic<uint64_t> head_{0};
  std::atomic<uint64_t> applied_{0};
  std::atomic<uint64_t> stale_{0};
  std::atomic<uint64_t> late_{0};
  uint64_t consumer_timeline_ = 0;
  uint64_t consumer_graph_ = 0;
  uint64_t next_sequence_ = 0;
  uint32_t pending_count_ = 0;

  // Shared.
  alignas(64) std::atomic<uint32_t> outstanding_{0};
  std::atomic<uint64_t> graph_generation_{0};
  std::atomic<uint64_t> timeline_generation_{0};

  alignas(64) std::array<Request, capacity> ring_{};
  std::array<Pending, capacity> pending_{};
  std::array<Due, capacity> due_{};
  std::array<Event, capacity> events_{};
};

}

#endif
