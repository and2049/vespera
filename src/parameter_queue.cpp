#include <vespera/parameter_queue.hpp>

#include <algorithm>

namespace vespera {

const char *to_string(ParameterResult result) noexcept {
  switch (result) {
  case ParameterResult::accepted:
    return "accepted";
  case ParameterResult::not_running:
    return "not_running";
  case ParameterResult::stale_token:
    return "stale_token";
  case ParameterResult::unknown_node:
    return "unknown_node";
  case ParameterResult::unknown_parameter:
    return "unknown_parameter";
  case ParameterResult::invalid_value:
    return "invalid_value";
  case ParameterResult::queue_full:
    return "queue_full";
  }
  return "unknown";
}

void ParameterQueue::clear(uint64_t graph_generation) noexcept {
  const uint64_t tail = tail_.load(std::memory_order_relaxed);
  const uint64_t head = head_.load(std::memory_order_relaxed);
  const uint64_t discarded = (tail - head) + pending_count_;
  if (discarded != 0) {
    stale_.fetch_add(discarded, std::memory_order_relaxed);
  }
  head_.store(tail, std::memory_order_relaxed);
  pending_count_ = 0;
  next_sequence_ = 0;
  outstanding_.store(0, std::memory_order_relaxed);
  graph_generation_.store(graph_generation, std::memory_order_relaxed);
  consumer_graph_ = graph_generation;
  consumer_timeline_ = timeline_generation_.load(std::memory_order_relaxed) + 1;
  timeline_generation_.store(consumer_timeline_, std::memory_order_release);
}

void ParameterQueue::reset_counters() noexcept {
  accepted_.store(0, std::memory_order_relaxed);
  rejected_.store(0, std::memory_order_relaxed);
  queue_full_.store(0, std::memory_order_relaxed);
  applied_.store(0, std::memory_order_relaxed);
  stale_.store(0, std::memory_order_relaxed);
  late_.store(0, std::memory_order_relaxed);
}

ParameterToken ParameterQueue::token() const noexcept {
  return {graph_generation_.load(std::memory_order_acquire),
          timeline_generation_.load(std::memory_order_acquire)};
}

ParameterQueue::PushResult ParameterQueue::push(ParameterToken token, NodeId node,
                                                uint32_t parameter_id, float value,
                                                std::optional<uint64_t> frame_position) noexcept {
  if (token.graph_generation == 0 || token != this->token()) {
    rejected_.fetch_add(1, std::memory_order_relaxed);
    return PushResult::stale_token;
  }
  const uint64_t tail = tail_.load(std::memory_order_relaxed);
  if (outstanding_.load(std::memory_order_acquire) >= capacity ||
      tail - head_.load(std::memory_order_acquire) >= capacity) {
    rejected_.fetch_add(1, std::memory_order_relaxed);
    queue_full_.fetch_add(1, std::memory_order_relaxed);
    return PushResult::queue_full;
  }
  outstanding_.fetch_add(1, std::memory_order_relaxed);
  Request &slot = ring_[tail & (capacity - 1)];
  slot.token = token;
  slot.node = node;
  slot.parameter_id = parameter_id;
  slot.value = value;
  slot.immediate = !frame_position.has_value();
  slot.frame_position = frame_position.value_or(0);
  tail_.store(tail + 1, std::memory_order_release);
  accepted_.fetch_add(1, std::memory_order_relaxed);
  return PushResult::accepted;
}

void ParameterQueue::count_rejected() noexcept {
  rejected_.fetch_add(1, std::memory_order_relaxed);
}

void ParameterQueue::release(uint32_t count) noexcept {
  if (count != 0) {
    outstanding_.fetch_sub(count, std::memory_order_release);
  }
}

void ParameterQueue::invalidate_timeline() noexcept {
  consumer_timeline_ = timeline_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
  const uint32_t dropped = pending_count_;
  pending_count_ = 0;
  if (dropped != 0) {
    stale_.fetch_add(dropped, std::memory_order_relaxed);
  }
  release(dropped);
}

void ParameterQueue::activate_graph(uint64_t graph_generation) noexcept {
  invalidate_timeline();
  consumer_graph_ = graph_generation;
  graph_generation_.store(graph_generation, std::memory_order_release);
}

void ParameterQueue::drain() noexcept {
  const uint64_t tail = tail_.load(std::memory_order_acquire);
  uint64_t head = head_.load(std::memory_order_relaxed);
  uint32_t stale = 0;
  for (; head != tail; ++head) {
    const Request &request = ring_[head & (capacity - 1)];
    if (request.token.graph_generation != consumer_graph_ ||
        request.token.timeline_generation != consumer_timeline_) {
      ++stale;
      continue;
    }
    // The outstanding-slot reservation bounds ring and pending occupancy together.
    pending_[pending_count_++] = Pending{request, next_sequence_++};
  }
  head_.store(head, std::memory_order_release);
  if (stale != 0) {
    stale_.fetch_add(stale, std::memory_order_relaxed);
  }
  release(stale);
}

std::span<const Event> ParameterQueue::collect(uint64_t frame_position, uint32_t frames) noexcept {
  if (frames == 0) {
    return {};
  }
  drain();
  uint32_t due = 0;
  uint32_t kept = 0;
  uint32_t late = 0;
  for (uint32_t index = 0; index < pending_count_; ++index) {
    const Pending &pending = pending_[index];
    const Request &request = pending.request;
    uint32_t offset = 0;
    if (!request.immediate) {
      if (request.frame_position < frame_position) {
        ++late;
      } else if (request.frame_position - frame_position < frames) {
        offset = static_cast<uint32_t>(request.frame_position - frame_position);
      } else {
        pending_[kept++] = pending;
        continue;
      }
    }
    due_[due++] =
        Due{offset, pending.sequence,
            Event{request.node, vs_parameter_event{request.parameter_id, offset, request.value}}};
  }
  pending_count_ = kept;
  std::sort(due_.begin(), due_.begin() + due, [](const Due &a, const Due &b) {
    return a.offset != b.offset ? a.offset < b.offset : a.sequence < b.sequence;
  });
  for (uint32_t index = 0; index < due; ++index) {
    events_[index] = due_[index].event;
  }
  if (late != 0) {
    late_.fetch_add(late, std::memory_order_relaxed);
  }
  release(due);
  return {events_.data(), due};
}

void ParameterQueue::count_applied(uint64_t events) noexcept {
  if (events != 0) {
    applied_.fetch_add(events, std::memory_order_relaxed);
  }
}

ParameterTelemetry ParameterQueue::telemetry() const noexcept {
  ParameterTelemetry result;
  result.accepted = accepted_.load(std::memory_order_relaxed);
  result.rejected = rejected_.load(std::memory_order_relaxed);
  result.queue_full = queue_full_.load(std::memory_order_relaxed);
  result.applied = applied_.load(std::memory_order_relaxed);
  result.stale = stale_.load(std::memory_order_relaxed);
  result.late = late_.load(std::memory_order_relaxed);
  return result;
}

}
