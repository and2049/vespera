#ifndef VESPERA_MIDI_QUEUE_HPP
#define VESPERA_MIDI_QUEUE_HPP

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <vespera/runtime.hpp>

namespace vespera {
inline uint64_t midi_clock_ns() noexcept {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

// Producers serialize outside the audio callback. The consumer never locks or allocates.
// A full queue requests a panic independently of queue capacity, so lost releases cannot stick.
// A panic discards only the events pushed before it was requested; later events are delivered
// with the panic, which modules apply at the start of the block.
class MidiQueue {
public:
  static constexpr uint32_t capacity = 128;
  uint64_t generation() const noexcept { return generation_.load(std::memory_order_acquire); }
  uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }
  // Events pushed and not yet delivered or discarded, including those deferred to a later block.
  uint32_t outstanding() const noexcept { return outstanding_.load(std::memory_order_acquire); }

  bool push(uint64_t generation, NodeId node, vs_midi_event message, uint64_t timestamp_ns = 0) {
    if (!vs_midi_event_valid(&message) || node == invalid_node_id) {
      return false;
    }
    std::lock_guard lock(producer_);
    if (generation != this->generation()) {
      return false;
    }
    const uint64_t tail = tail_.load(std::memory_order_relaxed);
    if (outstanding_.fetch_add(1, std::memory_order_acq_rel) >= capacity) {
      outstanding_.fetch_sub(1, std::memory_order_release);
      dropped_.fetch_add(1, std::memory_order_relaxed);
      panic();
      return false;
    }
    ring_[tail % capacity] = {generation, node, message, timestamp_ns, tail};
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  // Any thread, including a producer holding the producer lock. Repeated panics keep the latest
  // boundary. An event pushed concurrently with the panic may fall on either side of it.
  void panic() noexcept {
    const uint64_t tail = tail_.load(std::memory_order_acquire);
    uint64_t current = panic_request_.load(std::memory_order_acquire);
    uint64_t next = 0;
    do {
      const uint64_t boundary = std::max(current >> panic_count_bits, tail);
      next = (boundary << panic_count_bits) | ((current + 1) & panic_count_mask);
    } while (!panic_request_.compare_exchange_weak(current, next, std::memory_order_acq_rel,
                                                   std::memory_order_acquire));
  }

  // Consumer-only; old producer tokens stay invalid even if node IDs are reused.
  void reset() noexcept {
    generation_.fetch_add(1, std::memory_order_acq_rel);
    outstanding_.fetch_sub(pending_count_, std::memory_order_release);
    pending_count_ = 0;
    seen_panic_request_ = panic_request_.load(std::memory_order_acquire);
  }

  std::span<const Event> collect(uint64_t start_ns, uint32_t frames, double sample_rate,
                                 bool &panic_requested) noexcept {
    // The tail is read before the panic request, so a visible event pushed after a panic always
    // comes with that panic. The request is one word, so its flag and boundary always agree.
    const auto tail = tail_.load(std::memory_order_acquire);
    const auto panic_request = panic_request_.load(std::memory_order_acquire);
    panic_requested = panic_request != seen_panic_request_;
    seen_panic_request_ = panic_request;
    const auto boundary = panic_request >> panic_count_bits;
    const auto generation = this->generation();
    while (head_ != tail) {
      const auto request = ring_[head_++ % capacity];
      if (request.generation == generation) {
        pending_[pending_count_++] = request;
      } else {
        outstanding_.fetch_sub(1, std::memory_order_release);
      }
    }
    uint32_t count = 0, retained = 0;
    for (uint32_t i = 0; i < pending_count_; ++i) {
      const auto &request = pending_[i];
      if (request.sequence < boundary) {
        outstanding_.fetch_sub(1, std::memory_order_release);
        continue;
      }
      const double offset =
          request.timestamp_ns <= start_ns
              ? 0
              : static_cast<double>(request.timestamp_ns - start_ns) * sample_rate / 1e9;
      if (offset >= frames) {
        pending_[retained++] = request;
      } else {
        auto &event = events_[count++];
        event = {request.node, {}, Event::Kind::midi, request.message};
        event.midi.frame_offset = static_cast<uint32_t>(offset);
        outstanding_.fetch_sub(1, std::memory_order_release);
      }
    }
    pending_count_ = retained;
    for (uint32_t i = 1; i < count; ++i) {
      const auto value = events_[i];
      uint32_t j = i;
      while (j && events_[j - 1].frame_offset() > value.frame_offset()) {
        events_[j] = events_[j - 1];
        --j;
      }
      events_[j] = value;
    }
    return {events_.data(), count};
  }

private:
  struct Request {
    uint64_t generation = 0;
    NodeId node = invalid_node_id;
    vs_midi_event message{};
    uint64_t timestamp_ns = 0;
    // Position in the queue's whole push history; 64 bits do not wrap in practice.
    uint64_t sequence = 0;
  };
  // A panic request packs the boundary (a push sequence) above a request count that changes with
  // every panic, so a panic that leaves the boundary unchanged is still seen. The count would
  // have to wrap exactly between two collects to go unseen; 48 boundary bits do not wrap in
  // practice.
  static constexpr unsigned panic_count_bits = 16;
  static constexpr uint64_t panic_count_mask = (uint64_t{1} << panic_count_bits) - 1;
  std::mutex producer_;
  std::atomic<uint64_t> generation_{1}, tail_{0}, dropped_{0}, panic_request_{0};
  std::atomic<uint32_t> outstanding_{0};
  uint64_t seen_panic_request_ = 0;
  uint64_t head_ = 0;
  uint32_t pending_count_ = 0;
  std::array<Request, capacity> ring_{}, pending_{};
  std::array<Event, capacity> events_{};
};
}
#endif
