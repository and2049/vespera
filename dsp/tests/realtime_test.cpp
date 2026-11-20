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
#include <cstdlib>
#include <memory>
#include <new>
#include <span>
#include <vector>

namespace {
std::atomic<bool> processing{false};
std::atomic<unsigned> allocations{0};
std::atomic<unsigned> deallocations{0};
}

void *operator new(std::size_t size) {
  if (processing.load(std::memory_order_relaxed)) {
    allocations.fetch_add(1, std::memory_order_relaxed);
  }
  if (void *memory = std::malloc(size == 0 ? 1 : size)) {
    return memory;
  }
  throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return operator new(size); }
void operator delete(void *memory) noexcept {
  if (memory && processing.load(std::memory_order_relaxed)) {
    deallocations.fetch_add(1, std::memory_order_relaxed);
  }
  std::free(memory);
}
void operator delete[](void *memory) noexcept { operator delete(memory); }
void operator delete(void *memory, std::size_t) noexcept { operator delete(memory); }
void operator delete[](void *memory, std::size_t) noexcept { operator delete(memory); }

namespace {

// Builds responses before processing starts, then runs every real-time convolver and exchange
// operation inside the measured region: block processing of several sizes, switches started,
// queued and replaced mid-fade, reset, and retirement through the exchange.
bool convolution_is_allocation_free() {
  using namespace vespera::dsp;
  ConvolverConfig config;
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
  Convolver convolver(config);
  convolver.set_response(responses[0].get());
  const std::vector<float> source(4096, 0.25f);
  std::vector<float> left(4096);
  std::vector<float> right(4096);
  std::array<float *, 2> outputs{left.data(), right.data()};
  std::array<const float *, 2> inputs{source.data(), source.data()};
  const std::size_t blocks[] = {1, 64, 4096, 17, 512};
  bool accepted = true;
  processing.store(true);
  for (std::size_t i = 0; i < 200; ++i) {
    if (i % 25 == 3) {
      accepted = accepted && convolver.crossfade_to(responses[1].get());
      accepted = accepted && convolver.crossfade_to(responses[2].get());
      accepted = accepted && convolver.crossfade_to(responses[(i / 25) % 3].get());
    }
    if (i == 150) {
      convolver.reset();
    }
    if (const PreparedResponse *taken = exchange.take()) {
      exchange.retire(taken);
    }
    convolver.process(inputs, outputs, blocks[i % 5]);
    while (const PreparedResponse *old = convolver.take_retired()) {
      (void)old;
    }
  }
  processing.store(false);
  return accepted && std::isfinite(left[0]) && std::isfinite(right[0]);
}

// Builds the history buffer and allpasses before processing starts, then runs push, multi-tap
// linear and Lagrange reads, delay modulation, real-time coefficient and gain moves (including
// rejections), block processing of several sizes and resets inside the measured region.
bool history_and_allpass_are_allocation_free() {
  using namespace vespera::dsp;
  HistoryBuffer history({48000.0, 2048});
  FirstOrderAllpass first_order(0.5);
  SchroederAllpass schroeder({48000.0, 128, 0.6});
  Lfo lfo({48000.0, 2.0, LfoWaveform::sine, 0.0});
  const std::size_t blocks[] = {1, 64, 517, 32};
  double sum = 0.0;
  bool accepted = true;
  processing.store(true);
  for (std::size_t i = 0; i < 400; ++i) {
    const std::size_t block = blocks[i % 4];
    for (std::size_t j = 0; j < block; ++j) {
      const float lfo_sample = lfo.process();
      const double modulation = 512.0 + 256.0 * static_cast<double>(lfo_sample);
      history.push(lfo_sample);
      const double taps =
          static_cast<double>(history.read(modulation)) +
          static_cast<double>(history.read(1.5 * modulation, Interpolation::lagrange3));
      history.advance();
      accepted = accepted && first_order.try_set_coefficient(0.5 * static_cast<double>(lfo_sample));
      accepted = accepted && schroeder.try_set_gain(0.6 * static_cast<double>(lfo_sample));
      accepted = accepted && !first_order.try_set_coefficient(1.5);
      accepted = accepted && !schroeder.try_set_gain(-1.0);
      sum += first_order.process(schroeder.process(static_cast<float>(taps)));
    }
    if (i % 128 == 7) {
      history.reset();
      first_order.reset();
      schroeder.reset();
    }
  }
  processing.store(false);
  return accepted && std::isfinite(sum);
}

}

int main() {
  using namespace vespera::dsp;
  if (!convolution_is_allocation_free()) {
    return 1;
  }
  if (!history_and_allpass_are_allocation_free()) {
    return 1;
  }
  FractionalDelay delay({48000, 1024, 1.5});
  EnvelopeFollower envelope({48000, 0.01, 0.1});
  Lfo lfo({48000, 2.0, LfoWaveform::triangle, 0.0});
  LinearSmoother linear;
  ExponentialSmoother exponential;
  StateVariableFilter svf(48000, 1000, 0.7071067811865476);
  LinkwitzRileyCrossover crossover(48000, 800);
  linear.configure(48000, 0.01);
  exponential.configure(48000, 0.01);
  double sum = 0.0;
  processing.store(true);
  for (unsigned i = 0; i < 100000; ++i) {
    linear.set_target(i % 2 == 0 ? 1.0 : 0.0);
    exponential.set_target(i % 2 == 0 ? 1.0 : 0.0);
    if (!delay.try_set_delay_samples(static_cast<double>(i % 1024) + 0.5)) {
      std::abort();
    }
    if (!svf.try_set_controls(i % 2 == 0 ? 100.0 : 5000.0, i % 16 == 0 ? 0.5 : 4.0)) {
      std::abort();
    }
    const StateVariableFilter::Output filtered = svf.process(lfo.process());
    const LinkwitzRileyCrossover::Output split = crossover.process(filtered.band);
    sum += envelope.process(delay.process(static_cast<float>(filtered.low + split.low))) *
           linear.process() * exponential.process();
    if (i % 1024 == 0) {
      delay.reset();
      envelope.reset();
      lfo.reset();
      linear.reset();
      exponential.reset();
      svf.reset();
      crossover.reset();
    }
  }
  processing.store(false);
  return allocations.load() == 0 && deallocations.load() == 0 && std::isfinite(sum) && sum > 0.0
             ? 0
             : 1;
}
