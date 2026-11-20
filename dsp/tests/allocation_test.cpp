#include <vespera/dsp/allpass.hpp>
#include <vespera/dsp/convolution.hpp>
#include <vespera/dsp/crossover.hpp>
#include <vespera/dsp/delay.hpp>
#include <vespera/dsp/dynamics.hpp>
#include <vespera/dsp/history.hpp>
#include <vespera/dsp/oscillator.hpp>
#include <vespera/dsp/smoothing.hpp>
#include <vespera/dsp/state_variable_filter.hpp>

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <span>
#include <vector>

#if defined(__APPLE__)
#include <malloc/malloc.h>
#endif

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) ||                         \
    __has_feature(memory_sanitizer) || __has_feature(undefined_behavior_sanitizer)
#define VESPERA_ALLOCATION_TEST_SANITIZED 1
#endif
#endif

namespace {
std::atomic<bool> armed{false};
std::atomic<std::uint64_t> c_allocations{0};
std::atomic<std::uint64_t> c_frees{0};
std::atomic<std::uint64_t> new_calls{0};
std::atomic<std::uint64_t> delete_calls{0};
}

// Counting operator new/delete, as realtime_test does: every C++ allocation made while armed is
// counted.
void *operator new(std::size_t size) {
  if (armed.load(std::memory_order_relaxed)) {
    new_calls.fetch_add(1, std::memory_order_relaxed);
  }
  if (void *memory = std::malloc(size == 0 ? 1 : size)) {
    return memory;
  }
  throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return operator new(size); }
void operator delete(void *memory) noexcept {
  if (memory && armed.load(std::memory_order_relaxed)) {
    delete_calls.fetch_add(1, std::memory_order_relaxed);
  }
  std::free(memory);
}
void operator delete[](void *memory) noexcept { operator delete(memory); }
void operator delete(void *memory, std::size_t) noexcept { operator delete(memory); }
void operator delete[](void *memory, std::size_t) noexcept { operator delete(memory); }

#if defined(__APPLE__) && !defined(VESPERA_ALLOCATION_TEST_SANITIZED)

// Complements the operator-new counters with C-allocator visibility: the definitions in this
// executable take precedence for every call compiled into it, which covers all header-only
// vespera::dsp code, and forward to the default malloc zone so the process behaves normally.
// Sanitizer builds replace the C allocator themselves and are skipped to avoid mixing allocators.
extern "C" {

void *malloc(std::size_t size) {
  if (armed.load(std::memory_order_relaxed)) {
    c_allocations.fetch_add(1, std::memory_order_relaxed);
  }
  return malloc_zone_malloc(malloc_default_zone(), size == 0 ? 1 : size);
}

void free(void *memory) {
  if (memory == nullptr) {
    return;
  }
  if (armed.load(std::memory_order_relaxed)) {
    c_frees.fetch_add(1, std::memory_order_relaxed);
  }
  malloc_zone_free(malloc_default_zone(), memory);
}

void *calloc(std::size_t count, std::size_t size) {
  if (armed.load(std::memory_order_relaxed)) {
    c_allocations.fetch_add(1, std::memory_order_relaxed);
  }
  return malloc_zone_calloc(malloc_default_zone(), count == 0 ? 1 : count, size == 0 ? 1 : size);
}

void *realloc(void *memory, std::size_t size) {
  if (armed.load(std::memory_order_relaxed)) {
    c_allocations.fetch_add(1, std::memory_order_relaxed);
  }
  return malloc_zone_realloc(malloc_default_zone(), memory, size == 0 ? 1 : size);
}
}
#endif

namespace {

using vespera::dsp::Convolver;
using vespera::dsp::ConvolverConfig;
using vespera::dsp::ConvolverSwitch;
using vespera::dsp::PreparedResponse;
using vespera::dsp::ResponseExchange;

// Everything the processing loop touches is built before the counters arm: two convolvers (one
// switched through the exchange, one driven directly), the time-domain real-time primitives, and
// preallocated I/O buffers. The armed region then covers process blocks of several sizes, switch
// requests started, queued and replaced mid-fade, reset, retirement through both paths, and the
// exchange's audio-thread operations.
bool real_time_paths_allocate_nothing() {
  ConvolverConfig config;
  config.channels = 2;
  config.max_response_frames = 9600;
  config.fade_seconds = 0.005;
  std::vector<float> first(9600, 0.0f);
  std::vector<float> second(4000, 0.0f);
  for (std::size_t i = 0; i < first.size(); ++i) {
    first[i] = static_cast<float>(i % 7) * 0.001f;
  }
  for (std::size_t i = 0; i < second.size(); ++i) {
    second[i] = static_cast<float>(i % 5) * -0.002f;
  }
  const std::span<const float> first_span(first);
  const std::span<const float> second_span(second);
  const std::array<std::span<const float>, 2> pair{first_span, second_span};
  ResponseExchange exchange;
  std::vector<std::unique_ptr<PreparedResponse>> responses;
  responses.push_back(std::make_unique<PreparedResponse>(
      config, std::span<const std::span<const float>>(&first_span, 1)));
  responses.push_back(std::make_unique<PreparedResponse>(config, pair));
  responses.push_back(std::make_unique<PreparedResponse>(
      config, std::span<const std::span<const float>>(&second_span, 1)));
  std::unique_ptr<PreparedResponse> pending = std::make_unique<PreparedResponse>(
      config, std::span<const std::span<const float>>(&first_span, 1));
  if (!exchange.submit(pending)) {
    return false;
  }
  Convolver exchanged(config);
  exchanged.set_response(responses[0].get());
  Convolver direct(config);
  direct.set_response(responses[1].get());

  vespera::dsp::FractionalDelay delay({48000, 2048, 1.5});
  vespera::dsp::EnvelopeFollower envelope({48000, 0.01, 0.1});
  vespera::dsp::Lfo lfo({48000, 2.0, vespera::dsp::LfoWaveform::triangle, 0.0});
  vespera::dsp::LinearSmoother linear;
  vespera::dsp::ExponentialSmoother exponential;
  linear.configure(48000, 0.01);
  exponential.configure(48000, 0.01);
  linear.set_target(0.5);
  exponential.set_target(0.25);
  vespera::dsp::HistoryBuffer history({48000, 2048});
  vespera::dsp::FirstOrderAllpass first_order(0.5);
  vespera::dsp::SchroederAllpass schroeder({48000, 113, 0.6});
  vespera::dsp::StateVariableFilter svf(48000, 1000, 0.7071067811865476);
  vespera::dsp::LinkwitzRileyCrossover crossover(48000, 800);

  const std::vector<float> source(4096, 0.25f);
  std::vector<float> left(4096);
  std::vector<float> right(4096);
  std::vector<float> scalar(1024, 0.5f);
  const std::size_t blocks[] = {1, 64, 4096, 17, 512};
  bool accepted = true;
  double sum = 0.0;

  armed.store(true);
  for (std::size_t i = 0; i < 240; ++i) {
    if (const PreparedResponse *taken = exchange.take()) {
      accepted = accepted && exchanged.crossfade_to(taken);
    }
    if (i % 25 == 3) {
      accepted = accepted && direct.crossfade_to(responses[1].get());
      accepted = accepted && direct.crossfade_to(responses[2].get());
      accepted = accepted && direct.crossfade_to(responses[(i / 25) % 3].get());
    }
    if (i == 150) {
      exchanged.reset();
      direct.reset();
      svf.reset();
      crossover.reset();
    }
    const std::size_t count = blocks[i % 5] % scalar.size();
    std::array<const float *, 2> inputs{source.data(), source.data()};
    std::array<float *, 2> outputs{left.data(), right.data()};
    exchanged.process(inputs, outputs, count);
    direct.process(inputs, outputs, count);
    while (const PreparedResponse *old = exchanged.take_retired()) {
      exchange.retire(old);
    }
    while (const PreparedResponse *old = direct.take_retired()) {
      (void)old;
    }
    for (std::size_t n = 0; n < count; ++n) {
      accepted =
          accepted && delay.try_set_delay_samples(static_cast<double>((i * 31 + n) % 2048) + 0.5);
      accepted =
          accepted && svf.try_set_controls(n % 2 == 0 ? 120.0 : 9000.0, n % 3 == 0 ? 0.3 : 12.0);
      accepted = accepted && !svf.try_set_cutoff(-1.0) && !svf.try_set_q(0.0);
      const auto filtered = svf.process(lfo.process());
      const auto split = crossover.process(filtered.band);
      sum += envelope.process(delay.process(static_cast<float>(filtered.low + split.high))) *
             linear.process() * exponential.process();
      const double modulation = static_cast<double>((i * 17 + n) % 1900) + 0.25;
      history.push(source[n]);
      const float taps = history.read(modulation) +
                         history.read(modulation + 7.5, vespera::dsp::Interpolation::lagrange3);
      history.advance();
      accepted = accepted && first_order.try_set_coefficient(0.9 * std::sin(modulation)) &&
                 schroeder.try_set_gain(0.6 * std::cos(modulation)) &&
                 !first_order.try_set_coefficient(1.0) && !schroeder.try_set_gain(-1.0);
      sum += first_order.process(schroeder.process(taps));
    }
    if (i == 200) {
      history.reset();
      first_order.reset();
      schroeder.reset();
    }
    if (i % 1024 == 1023) {
      linear.set_target(i % 2048 == 1023 ? 1.0 : 0.0);
      exponential.set_target(i % 2048 == 1023 ? 0.75 : 0.1);
    }
  }
  armed.store(false);
  exchange.collect();
  return accepted && std::isfinite(sum) && std::isfinite(left[0]) && std::isfinite(right[0]) &&
         sum != 0.0;
}

}

int main() {
  const bool processed = real_time_paths_allocate_nothing();
  const std::uint64_t new_total = new_calls.load();
  const std::uint64_t delete_total = delete_calls.load();
  const std::uint64_t c_allocation_total = c_allocations.load();
  const std::uint64_t c_free_total = c_frees.load();
  if (!processed) {
    std::fputs("allocation: processing did not run\n", stderr);
    return 1;
  }
  bool failed = false;
  if (new_total != 0 || delete_total != 0) {
    std::fprintf(stderr, "allocation: %llu C++ allocation(s), %llu deallocation(s) while armed\n",
                 static_cast<unsigned long long>(new_total),
                 static_cast<unsigned long long>(delete_total));
    failed = true;
  }
  if (c_allocation_total != 0 || c_free_total != 0) {
    std::fprintf(stderr, "allocation: %llu C allocation(s), %llu C free(s) while armed\n",
                 static_cast<unsigned long long>(c_allocation_total),
                 static_cast<unsigned long long>(c_free_total));
    failed = true;
  }
#if !defined(__APPLE__) || defined(VESPERA_ALLOCATION_TEST_SANITIZED)
  std::fputs("allocation: C-allocator interposition not active on this build\n", stderr);
#endif
  if (!failed) {
    std::puts("all DSP allocation tests passed");
  }
  return failed ? 1 : 0;
}
