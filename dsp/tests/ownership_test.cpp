#include "support.hpp"

#include <vespera/dsp/convolution.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace {

using namespace test_support;
using vespera::dsp::ConvolutionLayout;
using vespera::dsp::Convolver;
using vespera::dsp::ConvolverConfig;
using vespera::dsp::ConvolverSwitch;
using vespera::dsp::PreparedResponse;

using Signal = std::vector<float>;

// Independent reference model of the convolver's documented response-ownership policy. It shares
// no code with Convolver: states, the two response slots, the queue and the retirement ring are
// re-derived from the documented rules (steady -> priming -> fading, one queued request, fade
// start at the second multiple of the largest partition at or after the request, reset and
// set_response installing their target and releasing everything else, at most
// retired_capacity responses held or retired). Installments release the previous responses in
// slot order, and which slot is audible flips with every completed fade, so the model tracks
// that parity to predict the retirement ring's FIFO order. Each released distinct pointer
// enters the FIFO exactly once, so comparing take_retired() results step by step proves
// exact-once retirement for every generated trace.
class OwnershipModel {
public:
  explicit OwnershipModel(const Convolver &convolver)
      : largest_(convolver.layout().largest_partition()), fade_frames_(convolver.fade_frames()) {}

  ConvolverSwitch state() const noexcept { return state_; }
  const PreparedResponse *audible() const noexcept { return slots_[current_]; }
  std::size_t pending() const noexcept { return retired_.size(); }
  const std::map<const PreparedResponse *, std::size_t> &released_counts() const noexcept {
    return released_;
  }

  bool holds(const PreparedResponse *response) const noexcept {
    return response != nullptr &&
           (response == slots_[0] || response == slots_[1] || (queued_ && *queued_ == response));
  }

  std::size_t held() const noexcept {
    const PreparedResponse *seen[3] = {slots_[0], slots_[1], queued_.value_or(nullptr)};
    std::size_t count = 0;
    for (std::size_t i = 0; i < 3; ++i) {
      if (seen[i] != nullptr && std::find(seen, seen + i, seen[i]) == seen + i) {
        ++count;
      }
    }
    return count;
  }

  bool can_hold(const PreparedResponse *response) const noexcept {
    return response == nullptr || holds(response) ||
           retired_.size() + held() + 1 <= Convolver::retired_capacity;
  }

  // The compatibility of a response is a property of its layout, which the model does not know;
  // incompatible requests are refused without touching any state.
  bool crossfade_to(const PreparedResponse *response, bool compatible) {
    if (!compatible || !can_hold(response)) {
      return false;
    }
    if (state_ == ConvolverSwitch::steady) {
      if (response != slots_[current_]) {
        begin_switch(response, position_);
        settle();
      }
      return true;
    }
    const PreparedResponse *incoming = slots_[1 - current_];
    if (!queued_) {
      if (response != incoming) {
        queued_ = response;
      }
      return true;
    }
    const PreparedResponse *replaced = *queued_;
    if (response == incoming) {
      queued_.reset();
    } else {
      queued_ = response;
    }
    release(replaced);
    return true;
  }

  enum class InstallResult { installed, invalid, full };

  InstallResult set_response(const PreparedResponse *response, bool compatible) {
    if (!compatible) {
      return InstallResult::invalid;
    }
    if (!can_hold(response)) {
      return InstallResult::full;
    }
    install(response);
    return InstallResult::installed;
  }

  void reset() {
    install(queued_
                ? *queued_
                : (state_ == ConvolverSwitch::steady ? slots_[current_] : slots_[1 - current_]));
  }

  void process(std::size_t frames) {
    position_ += frames;
    settle();
  }

  const PreparedResponse *pop_retired() {
    if (retired_.empty()) {
      return nullptr;
    }
    const PreparedResponse *response = retired_.front();
    retired_.erase(retired_.begin());
    return response;
  }

private:
  void begin_switch(const PreparedResponse *response, std::uint64_t at) noexcept {
    slots_[1 - current_] = response;
    fade_start_ = largest_ == 0 ? at : ((at + largest_ - 1) / largest_ + 1) * largest_;
    fade_end_ = fade_start_ + fade_frames_;
    state_ = ConvolverSwitch::priming;
  }

  // Applies every fade transition whose boundary lies at or before the current position. A fade
  // that completes hands its end position to a queued switch, because the convolver reaches that
  // boundary exactly when it starts the next fade. The outgoing slot is cleared and the audible
  // slot flips before the outgoing response is released, and the queue is still engaged then, so
  // a queued switch back to the outgoing response keeps it alive.
  void settle() noexcept {
    for (;;) {
      if (state_ == ConvolverSwitch::priming && position_ >= fade_start_) {
        state_ = ConvolverSwitch::fading;
      } else if (state_ == ConvolverSwitch::fading && fade_end_ <= position_) {
        const std::uint64_t completed_end = fade_end_;
        const PreparedResponse *outgoing = slots_[current_];
        slots_[current_] = nullptr;
        current_ = 1 - current_;
        state_ = ConvolverSwitch::steady;
        release(outgoing);
        if (queued_) {
          const PreparedResponse *next = *queued_;
          queued_.reset();
          if (next != slots_[current_]) {
            begin_switch(next, completed_end);
          }
        }
      } else {
        return;
      }
    }
  }

  void install(const PreparedResponse *target) noexcept {
    const PreparedResponse *previous[3] = {slots_[0], slots_[1], queued_.value_or(nullptr)};
    slots_ = {target, nullptr};
    current_ = 0;
    queued_.reset();
    state_ = ConvolverSwitch::steady;
    for (std::size_t i = 0; i < 3; ++i) {
      if (std::find(previous, previous + i, previous[i]) == previous + i) {
        release(previous[i]);
      }
    }
    position_ = 0;
  }

  void release(const PreparedResponse *response) noexcept {
    if (response == nullptr || holds(response)) {
      return;
    }
    retired_.push_back(response);
    ++released_[response];
  }

  std::size_t largest_ = 0;
  std::uint64_t fade_frames_ = 0;
  std::uint64_t position_ = 0;
  ConvolverSwitch state_ = ConvolverSwitch::steady;
  std::array<const PreparedResponse *, 2> slots_{};
  std::size_t current_ = 0;
  std::optional<const PreparedResponse *> queued_;
  std::uint64_t fade_start_ = 0;
  std::uint64_t fade_end_ = 0;
  std::vector<const PreparedResponse *> retired_;
  std::map<const PreparedResponse *, std::size_t> released_;
};

std::unique_ptr<PreparedResponse> prepare_response(const ConvolverConfig &config,
                                                   std::size_t frames, std::uint32_t seed) {
  const std::vector<double> values = noise(frames, seed);
  const Signal response(values.begin(), values.end());
  const std::span<const float> channel(response);
  return std::make_unique<PreparedResponse>(config,
                                            std::span<const std::span<const float>>(&channel, 1));
}

struct TraceSpec {
  const char *label;
  std::size_t capacity = 3000;
  double fade_seconds = 0.002;
  std::size_t pool_size = 5;
  std::size_t operations = 160;
  std::uint32_t seed = 1;
  // Percentage of steps that drain the retirement ring; low values keep the ring full so requests
  // are refused by capacity.
  std::uint32_t drain_percent = 60;
};

// One deterministic operation trace: seeded blocks of process, crossfade and set requests over a
// fixed response pool (plus null and one incompatible response), retirements taken one by one or
// drained, and resets. After every step the convolver must agree with the model's response,
// switch state, refusals and retirement FIFO.
void run_trace(const TraceSpec &spec) {
  ConvolverConfig config;
  config.sample_rate = 48000.0;
  config.channels = 1;
  config.max_response_frames = spec.capacity;
  config.fade_seconds = spec.fade_seconds;

  ConvolverConfig other = config;
  other.max_response_frames = spec.capacity + 2000;
  const auto incompatible = prepare_response(other, 500, 909u);

  std::vector<std::unique_ptr<PreparedResponse>> pool;
  for (std::size_t i = 0; i < spec.pool_size; ++i) {
    const std::size_t frames = std::max<std::size_t>(1, std::min(spec.capacity, 64 + 37 * i));
    pool.push_back(prepare_response(config, frames, 100u + 13 * static_cast<std::uint32_t>(i)));
  }

  Convolver convolver(config);
  OwnershipModel model(convolver);
  std::map<const PreparedResponse *, std::size_t> returned;

  const std::size_t block_sizes[] = {1, 16, 64, 512};
  const Signal input(512, 0.25f);
  Signal output(512);
  std::uint32_t rng = spec.seed;
  const auto next_random = [&rng] {
    rng = rng * 1664525u + 1013904223u;
    return rng >> 8;
  };

  const auto request = [&](std::uint32_t choice) -> const PreparedResponse * {
    return choice < spec.pool_size ? pool[choice].get() : nullptr;
  };
  const auto verify_state = [&](std::size_t at) {
    if (convolver.response() != model.audible() || convolver.switch_state() != model.state()) {
      std::fprintf(stderr, "%s: %s: state mismatch at step %zu: response %d, state %d\n",
                   current_test, spec.label, at, convolver.response() == model.audible(),
                   convolver.switch_state() == model.state());
      ++failures;
    }
    if (model.pending() + model.held() > Convolver::retired_capacity) {
      std::fprintf(stderr, "%s: %s: model exceeds retirement capacity at step %zu\n", current_test,
                   spec.label, at);
      ++failures;
    }
  };
  const auto take_one = [&](std::size_t at) {
    const PreparedResponse *expected = model.pop_retired();
    const PreparedResponse *actual = convolver.take_retired();
    if (actual != expected) {
      std::fprintf(stderr, "%s: %s: retirement mismatch at step %zu\n", current_test, spec.label,
                   at);
      ++failures;
    }
    if (actual != nullptr) {
      ++returned[actual];
    }
  };

  for (std::size_t step = 0; step < spec.operations; ++step) {
    const std::uint32_t roll = next_random() % 100;
    if (roll < 40) {
      const std::size_t block = block_sizes[next_random() % std::size(block_sizes)];
      convolver.process(std::span<const float>(input).first(block),
                        std::span<float>(output).first(block));
      model.process(block);
    } else if (roll < 68) {
      const std::uint32_t choice = next_random();
      const PreparedResponse *target = request(choice % (spec.pool_size + 1));
      const bool predicted = model.crossfade_to(target, true);
      if (convolver.crossfade_to(target) != predicted) {
        std::fprintf(stderr, "%s: %s: crossfade refusal mismatch at step %zu\n", current_test,
                     spec.label, step);
        ++failures;
      }
    } else if (roll < 76) {
      const std::uint32_t choice = next_random();
      const PreparedResponse *target = request(choice % (spec.pool_size + 1));
      const auto predicted = model.set_response(target, true);
      bool failed_invalid = false;
      bool failed_full = false;
      try {
        convolver.set_response(target);
      } catch (const std::invalid_argument &) {
        failed_invalid = true;
      } catch (const std::length_error &) {
        failed_full = true;
      }
      if ((predicted == OwnershipModel::InstallResult::invalid) != failed_invalid ||
          (predicted == OwnershipModel::InstallResult::full) != failed_full) {
        std::fprintf(stderr, "%s: %s: set_response outcome mismatch at step %zu\n", current_test,
                     spec.label, step);
        ++failures;
      }
    } else if (roll < 82) {
      convolver.reset();
      model.reset();
    } else if (roll < 88) {
      const bool predicted = model.crossfade_to(incompatible.get(), false);
      if (convolver.crossfade_to(incompatible.get()) != predicted) {
        std::fprintf(stderr, "%s: %s: incompatible crossfade mismatch at step %zu\n", current_test,
                     spec.label, step);
        ++failures;
      }
    } else if (roll < 94) {
      const auto predicted = model.set_response(incompatible.get(), false);
      bool failed_invalid = false;
      try {
        convolver.set_response(incompatible.get());
      } catch (const std::invalid_argument &) {
        failed_invalid = true;
      } catch (...) {
      }
      if (predicted != OwnershipModel::InstallResult::invalid || !failed_invalid) {
        std::fprintf(stderr, "%s: %s: incompatible set_response mismatch at step %zu\n",
                     current_test, spec.label, step);
        ++failures;
      }
    } else {
      take_one(step);
    }
    verify_state(step);
    if (next_random() % 100 < spec.drain_percent) {
      for (;;) {
        const PreparedResponse *expected = model.pop_retired();
        const PreparedResponse *actual = convolver.take_retired();
        if (actual != expected) {
          std::fprintf(stderr, "%s: %s: drained retirement mismatch at step %zu\n", current_test,
                       spec.label, step);
          ++failures;
          break;
        }
        if (actual == nullptr) {
          break;
        }
        ++returned[actual];
      }
    }
  }

  convolver.reset();
  model.reset();
  verify_state(spec.operations);
  for (;;) {
    const PreparedResponse *expected = model.pop_retired();
    const PreparedResponse *actual = convolver.take_retired();
    if (actual != expected) {
      std::fprintf(stderr, "%s: %s: final retirement mismatch\n", current_test, spec.label);
      ++failures;
      break;
    }
    if (actual == nullptr) {
      break;
    }
    ++returned[actual];
  }
  if (returned != model.released_counts()) {
    std::fprintf(stderr, "%s: %s: retirement counts differ from release events\n", current_test,
                 spec.label);
    ++failures;
  }
}

void test_ownership_traces() {
  const TraceSpec specs[] = {
      {"direct zero fade", 40, 0.0, 4, 160, 1, 60},
      {"direct fading", 40, 0.005, 4, 160, 2, 60},
      {"partitioned short fade", 3000, 0.002, 5, 200, 3, 60},
      {"partitioned long fade", 3000, 0.01, 5, 200, 4, 60},
      {"near capacity", 3000, 0.0005, 6, 260, 5, 12},
      {"near capacity long fade", 3000, 0.01, 6, 260, 6, 10},
  };
  // Three seeds per fixture family: 18 traces of at most 260 operations each, a fixed and
  // documented scope rather than an exhaustive search of operation sequences.
  for (const TraceSpec &spec : specs) {
    for (std::uint32_t variant = 0; variant < 3; ++variant) {
      TraceSpec seeded = spec;
      seeded.seed += 101u * variant;
      seeded.label = spec.label;
      run_trace(seeded);
    }
  }
  current_test = "ownership traces";
}

void test_backpressure_refusal_sequence() {
  const ConvolverConfig config = [] {
    ConvolverConfig config;
    config.sample_rate = 48000.0;
    config.channels = 1;
    config.max_response_frames = 64;
    config.fade_seconds = 0.0;
    return config;
  }();
  const Signal impulse{1};
  std::vector<std::unique_ptr<PreparedResponse>> pool;
  for (std::size_t i = 0; i < Convolver::retired_capacity + 2; ++i) {
    const std::span<const float> channel(impulse);
    pool.push_back(std::make_unique<PreparedResponse>(
        config, std::span<const std::span<const float>>(&channel, 1)));
  }
  Convolver convolver(config);
  convolver.set_response(pool[0].get());
  std::size_t accepted = 1;
  while (accepted < pool.size() && convolver.crossfade_to(pool[accepted].get())) {
    ++accepted;
  }
  CHECK(accepted == Convolver::retired_capacity);
  CHECK(!convolver.crossfade_to(pool[accepted].get()));
  bool length_error = false;
  try {
    convolver.set_response(pool[accepted].get());
  } catch (const std::length_error &) {
    length_error = true;
  }
  CHECK(length_error);
  CHECK(convolver.response() == pool[accepted - 1].get());
  CHECK(convolver.switch_state() == ConvolverSwitch::steady);
  CHECK(convolver.take_retired() == pool[0].get());
  CHECK(convolver.crossfade_to(pool[accepted].get()));
  CHECK(convolver.take_retired() == pool[1].get());
  for (std::size_t i = 2; i < accepted; ++i) {
    CHECK(convolver.take_retired() == pool[i].get());
  }
  CHECK(convolver.take_retired() == nullptr);
}

}

int main() {
  run("ownership traces", test_ownership_traces);
  run("backpressure refusal sequence", test_backpressure_refusal_sequence);
  return finish("ownership");
}
