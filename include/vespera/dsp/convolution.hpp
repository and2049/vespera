#ifndef VESPERA_DSP_CONVOLUTION_HPP
#define VESPERA_DSP_CONVOLUTION_HPP

#include <vespera/dsp/common.hpp>
#include <vespera/dsp/fft.hpp>
#include <vespera/dsp/utility.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

// Zero-latency partitioned convolution.
//
// The response is split into a direct-form head of `head_frames` taps followed by levels of
// uniformly partitioned overlap-save convolution whose partition size doubles from level to
// level. A level with partition size N begins 2N frames into the response, so the work for each
// completed input block of N frames can be spread evenly over the next N frames and still be
// ready when its first output frame is due. All work happens at fixed positions in the input
// stream, so the output is bit-identical however the host splits it into blocks.
//
// Threading: a Convolver belongs to one processing thread. PreparedResponse objects are built on
// another thread, are immutable afterwards, and may be shared by any number of convolvers.
// ResponseExchange hands them between one control thread and one audio thread.

namespace vespera::dsp {

inline constexpr std::size_t maximum_response_frames = std::size_t{1} << 19;
inline constexpr double maximum_convolver_fade_seconds = 10.0;

inline constexpr std::size_t convolution_head_alignment = 16;

struct ConvolutionLevel {
  std::size_t partition_frames = 0;
  std::size_t partitions = 0;

  friend bool operator==(const ConvolutionLevel &, const ConvolutionLevel &) = default;
};

struct ConvolutionLayout {
  static constexpr std::size_t max_levels = 16;
  static constexpr std::size_t largest_partition_limit = 8192;

  std::size_t head_frames = 0;
  std::size_t level_count = 0;
  std::array<ConvolutionLevel, max_levels> levels{};

  std::span<const ConvolutionLevel> active_levels() const noexcept {
    return {levels.data(), level_count};
  }
  std::size_t smallest_partition() const noexcept {
    return level_count == 0 ? 0 : levels[0].partition_frames;
  }
  std::size_t largest_partition() const noexcept {
    return level_count == 0 ? 0 : levels[level_count - 1].partition_frames;
  }
  std::size_t capacity_frames() const noexcept {
    std::size_t frames = head_frames;
    for (const ConvolutionLevel &level : active_levels()) {
      frames += level.partition_frames * level.partitions;
    }
    return frames;
  }

  friend bool operator==(const ConvolutionLayout &, const ConvolutionLayout &) = default;

  // Head of 2 * first taps, then levels of first, 2 * first, ... frames with two partitions each
  // until `largest`, which takes as many partitions as `frames` needs. A response no longer than
  // the head gets no levels.
  static ConvolutionLayout doubling(std::size_t frames, std::size_t first, std::size_t largest) {
    if (frames == 0 || frames > maximum_response_frames || first < convolution_head_alignment / 2 ||
        largest < first || largest > largest_partition_limit || !std::has_single_bit(first) ||
        !std::has_single_bit(largest)) {
      throw std::invalid_argument("Convolution layout parameters are out of range");
    }
    ConvolutionLayout layout;
    layout.head_frames = 2 * first;
    std::size_t start = layout.head_frames;
    std::size_t size = first;
    while (start < frames) {
      const std::size_t needed = (frames - start + size - 1) / size;
      const std::size_t partitions = size < largest ? std::min<std::size_t>(2, needed) : needed;
      layout.levels[layout.level_count++] = {size, partitions};
      start += partitions * size;
      size = size < largest ? size * 2 : size;
    }
    return layout;
  }

  static ConvolutionLayout direct(std::size_t frames) {
    if (frames == 0 || frames > maximum_response_frames) {
      throw std::invalid_argument("Convolution layout parameters are out of range");
    }
    ConvolutionLayout layout;
    layout.head_frames = (frames + convolution_head_alignment - 1) / convolution_head_alignment *
                         convolution_head_alignment;
    return layout;
  }

  // Estimated processing cost in nanoseconds per frame and channel with one response. The
  // per-operation costs were fitted to vespera_dsp_bench measurements of single-level layouts on
  // Apple silicon; only their ratios matter for plan().
  double estimated_cost() const noexcept {
    double cost = head_tap_cost * static_cast<double>(head_frames);
    for (const ConvolutionLevel &level : active_levels()) {
      const auto size = static_cast<double>(level.partition_frames);
      const auto partitions = static_cast<double>(level.partitions);
      cost += level_cost + partitions * (bin_cost * (size + 1.0) + partition_cost) / size;
    }
    return cost;
  }

  // The cheapest of the direct layout (for short responses) and every doubling layout with a
  // first partition of 16 to 256 frames, by estimated_cost(). Throws for 0 frames or more than
  // maximum_response_frames.
  static ConvolutionLayout plan(std::size_t frames) {
    ConvolutionLayout best = direct(frames);
    double best_cost = frames <= direct_limit ? best.estimated_cost() : HUGE_VAL;
    for (std::size_t first = 16; first <= 256; first *= 2) {
      if (frames <= 2 * first) {
        break;
      }
      for (std::size_t largest = first; largest <= largest_partition_limit; largest *= 2) {
        const ConvolutionLayout candidate = doubling(frames, first, largest);
        const double cost = candidate.estimated_cost();
        if (cost < best_cost) {
          best = candidate;
          best_cost = cost;
        }
        if (candidate.largest_partition() < largest) {
          break;
        }
      }
    }
    return best;
  }

  static constexpr std::size_t direct_limit = 4096;
  static constexpr double head_tap_cost = 0.18;
  static constexpr double level_cost = 20.0;
  static constexpr double bin_cost = 1.0;
  static constexpr double partition_cost = 10.0;
};

struct ConvolverConfig {
  double sample_rate = 48000.0;
  std::size_t channels = 2;
  // Longest response that will be installed; it fixes the partition layout and memory.
  std::size_t max_response_frames = 0;
  double fade_seconds = 0.02;
  // Overrides ConvolutionLayout::plan(); it must be the direct or a doubling layout for exactly
  // max_response_frames.
  std::optional<ConvolutionLayout> layout;
};

namespace detail {

inline void validate_convolver_config(const ConvolverConfig &config) {
  validate_sample_rate(config.sample_rate);
  if (config.channels != 1 && config.channels != 2) {
    throw std::invalid_argument("Convolver channel count must be 1 or 2");
  }
  if (config.max_response_frames == 0 || config.max_response_frames > maximum_response_frames) {
    throw std::invalid_argument("Convolver response capacity must be within [1, 524288] frames");
  }
  if (!std::isfinite(config.fade_seconds) || config.fade_seconds < 0.0 ||
      config.fade_seconds > maximum_convolver_fade_seconds) {
    throw std::invalid_argument("Convolver fade must be finite and within [0, 10] seconds");
  }
  if (config.layout) {
    const ConvolutionLayout &layout = *config.layout;
    if (layout.level_count > ConvolutionLayout::max_levels) {
      throw std::invalid_argument("Convolver layout has too many levels");
    }
    const std::size_t first = layout.smallest_partition();
    const std::size_t largest = layout.largest_partition();
    const bool valid =
        layout.level_count == 0
            ? layout == ConvolutionLayout::direct(config.max_response_frames)
            : first >= convolution_head_alignment / 2 && std::has_single_bit(first) &&
                  std::has_single_bit(largest) &&
                  largest <= ConvolutionLayout::largest_partition_limit &&
                  layout == ConvolutionLayout::doubling(config.max_response_frames, first, largest);
    if (!valid) {
      throw std::invalid_argument("Convolver layout does not fit max_response_frames");
    }
  }
}

inline ConvolutionLayout convolver_layout(const ConvolverConfig &config) {
  validate_convolver_config(config);
  return config.layout ? *config.layout : ConvolutionLayout::plan(config.max_response_frames);
}

}

// An impulse response transformed for one convolver layout and sample rate. Construction
// allocates and transforms the response, so build it off the audio thread. It is immutable
// afterwards and can be installed in any compatible convolver, including several at once.
//
// A one-channel response is applied to every convolver channel from a single copy of its
// spectra; a two-channel response needs a two-channel convolver.
class PreparedResponse {
public:
  // Throws std::invalid_argument, changing nothing, for an invalid config, a channel count other
  // than 1 or 2, a channel that is empty, longer than config.max_response_frames or that has a
  // non-finite sample.
  PreparedResponse(const ConvolverConfig &config, std::span<const std::span<const float>> channels)
      : layout_(detail::convolver_layout(config)), sample_rate_(config.sample_rate) {
    if (channels.empty() || channels.size() > 2) {
      throw std::invalid_argument("A response has one or two channels");
    }
    for (std::span<const float> channel : channels) {
      if (channel.empty() || channel.size() > config.max_response_frames) {
        throw std::invalid_argument("Response length must be within [1, max_response_frames]");
      }
      if (!std::all_of(channel.begin(), channel.end(), [](float v) { return std::isfinite(v); })) {
        throw std::invalid_argument("Response samples must be finite");
      }
      frames_ = std::max(frames_, channel.size());
    }
    for (std::span<const float> channel : channels) {
      channels_.push_back(transform(channel));
    }
  }

  std::size_t channels() const noexcept { return channels_.size(); }
  std::size_t frames() const noexcept { return frames_; }
  double sample_rate() const noexcept { return sample_rate_; }
  const ConvolutionLayout &layout() const noexcept { return layout_; }

private:
  friend class Convolver;

  struct Channel {
    // Head taps in reverse order, so that a dot product with the newest input window applies them.
    std::vector<float> head;
    // Per level and partition: bins real parts then bins imaginary parts, scaled by
    // 1 / (2 * partition_frames) for the unscaled inverse transform.
    std::vector<std::vector<float>> spectra;
  };

  Channel transform(std::span<const float> response) const {
    Channel channel;
    channel.head.assign(layout_.head_frames, 0.0f);
    for (std::size_t k = 0; k < std::min(layout_.head_frames, response.size()); ++k) {
      channel.head[layout_.head_frames - 1 - k] = response[k];
    }
    std::size_t start = layout_.head_frames;
    for (const ConvolutionLevel &level : layout_.active_levels()) {
      const std::size_t size = level.partition_frames;
      const std::size_t bins = size + 1;
      RealFft fft(2 * size);
      std::vector<float> block(2 * size);
      std::vector<float> spectra(level.partitions * 2 * bins);
      const float scale = 1.0f / static_cast<float>(2 * size);
      for (std::size_t p = 0; p < level.partitions; ++p) {
        std::fill(block.begin(), block.end(), 0.0f);
        const std::size_t first = start + p * size;
        for (std::size_t k = 0; k < size && first + k < response.size(); ++k) {
          block[k] = response[first + k] * scale;
        }
        float *re = spectra.data() + p * 2 * bins;
        fft.forward(block, {re, bins}, {re + bins, bins});
      }
      channel.spectra.push_back(std::move(spectra));
      start += level.partitions * size;
    }
    return channel;
  }

  ConvolutionLayout layout_;
  double sample_rate_ = 0.0;
  std::size_t frames_ = 0;
  std::vector<Channel> channels_;
};

// steady: one response is audible. priming: a new response has been requested and its
// partitioned outputs are being computed while the old one stays audible. fading: both are
// computed and the output crossfades linearly from the old to the new.
enum class ConvolverSwitch { steady, priming, fading };

// Zero-latency convolution of one or two channels with a PreparedResponse.
//
// Latency is exactly 0 frames: output frame n includes response tap 0 times input frame n.
// Construction allocates every buffer; process(), crossfade_to(), take_retired() and reset() do
// not allocate, lock or throw. Without a response the output is silence.
//
// Switching: crossfade_to() starts priming. The fade begins at the second multiple of
// layout().largest_partition() frames at or after the request position (at the request for a
// layout without levels) and lasts fade_frames() frames. A request made while priming or fading
// is queued and starts when the current fade ends; a later request replaces the queued one. The
// convolver never destroys a response. A response it no longer references becomes retired and
// is returned once by take_retired(); at most retired_capacity responses can be held or retired
// at once, and crossfade_to() refuses a new one beyond that.
//
// Non-finite input frames are treated as silence. Finite input whose convolution exceeds the
// float range produces non-finite output until that input has left the response length.
class Convolver {
public:
  static constexpr std::size_t retired_capacity = 8;
  static constexpr std::size_t latency_frames() noexcept { return 0; }

  explicit Convolver(const ConvolverConfig &config)
      : config_(config), layout_(detail::convolver_layout(config)),
        fade_frames_(
            static_cast<std::uint64_t>(std::llround(config.fade_seconds * config.sample_rate))) {
    const std::size_t largest = layout_.largest_partition();
    grid_ = layout_.smallest_partition();
    max_run_ = grid_ == 0 ? 256 : grid_;
    ring_size_ = std::bit_ceil(std::max(layout_.head_frames + max_run_, 4 * largest));
    rings_.assign(config_.channels, std::vector<float>(2 * ring_size_, 0.0f));
    path_.assign(2, std::vector<float>(max_run_, 0.0f));
    for (std::size_t i = 0; i < layout_.level_count; ++i) {
      levels_.emplace_back(i, layout_.levels[i], config_.channels);
    }
  }

  const ConvolverConfig &config() const noexcept { return config_; }
  const ConvolutionLayout &layout() const noexcept { return layout_; }
  std::size_t channels() const noexcept { return config_.channels; }
  std::uint64_t fade_frames() const noexcept { return fade_frames_; }
  ConvolverSwitch switch_state() const noexcept { return state_; }

  // The response heard before any pending switch completes.
  const PreparedResponse *response() const noexcept { return slots_[current_]; }

  // True for nullptr and for responses prepared for this layout and sample rate with one channel
  // or channels() channels.
  bool accepts(const PreparedResponse *response) const noexcept {
    return response == nullptr ||
           (response->layout_ == layout_ && response->sample_rate_ == config_.sample_rate &&
            (response->channels() == 1 || response->channels() == channels()));
  }

  // Installs a response immediately, completing any switch and clearing history. Use while
  // processing is quiescent. Throws std::invalid_argument for an incompatible response and
  // std::length_error when retired responses must be taken first; both leave state unchanged.
  void set_response(const PreparedResponse *response) {
    if (!accepts(response)) {
      throw std::invalid_argument("Response was prepared for a different convolver layout");
    }
    if (!can_hold(response)) {
      throw std::length_error("Take retired responses before installing another");
    }
    install(response);
  }

  // Real-time switch request; see the class comment. Returns false, changing nothing, for an
  // incompatible response or when retired responses must be taken first.
  [[nodiscard]] bool crossfade_to(const PreparedResponse *response) noexcept {
    if (!accepts(response) || !can_hold(response)) {
      return false;
    }
    if (state_ == ConvolverSwitch::steady) {
      if (response != slots_[current_]) {
        begin_switch(response);
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

  // Returns the oldest response this convolver no longer references, or nullptr. Each retired
  // response is returned exactly once; destroying it is the caller's job, off the audio thread.
  const PreparedResponse *take_retired() noexcept {
    if (retired_count_ == 0) {
      return nullptr;
    }
    const PreparedResponse *response = retired_[retired_first_];
    retired_first_ = (retired_first_ + 1) % retired_capacity;
    --retired_count_;
    return response;
  }

  // Clears all history and completes any switch at its final target. Allocation-free, but takes
  // time proportional to the convolver's memory.
  void reset() noexcept {
    const PreparedResponse *target =
        queued_ ? *queued_
                : (state_ == ConvolverSwitch::steady ? slots_[current_] : slots_[1 - current_]);
    install(target);
  }

  // inputs and outputs hold channels() pointers to `frames` samples. An output may be the same
  // pointer as its input; otherwise buffers must not overlap.
  void process(std::span<const float *const> inputs, std::span<float *const> outputs,
               std::size_t frames) noexcept {
    assert(inputs.size() >= channels() && outputs.size() >= channels());
    std::size_t offset = 0;
    while (offset < frames) {
      advance_switch();
      if (grid_ != 0 && position_ == next_grid_) {
        run_grid();
        next_grid_ += grid_;
      }
      std::uint64_t run = std::min<std::uint64_t>(frames - offset, max_run_);
      if (grid_ != 0) {
        run = std::min(run, next_grid_ - position_);
      }
      if (state_ == ConvolverSwitch::priming) {
        run = std::min(run, fade_start_ - position_);
      } else if (state_ == ConvolverSwitch::fading) {
        run = std::min(run, fade_end_ - position_);
      }
      const auto count = static_cast<std::size_t>(run);
      for (std::size_t c = 0; c < channels(); ++c) {
        process_run(c, inputs[c] + offset, outputs[c] + offset, count);
      }
      position_ += run;
      offset += count;
    }
    advance_switch();
  }

  // Single-channel convenience; requires channels() == 1 and output.size() >= input.size().
  void process(std::span<const float> input, std::span<float> output) noexcept {
    assert(channels() == 1 && output.size() >= input.size());
    const float *in = input.data();
    float *out = output.data();
    process(std::span<const float *const>(&in, 1), std::span<float *const>(&out, 1), input.size());
  }

private:
  enum class PassKind : std::uint8_t { gather, butterflies, split, multiply, merge, scatter };

  // Relative cost of one element of each pass, measured on Apple silicon, so that every grid
  // slot of a level does about the same amount of work. Butterfly passes over spans shorter than
  // 16 (including the fused first pass) have short inner loops and cost more per element.
  static constexpr std::size_t gather_weight = 6;
  static constexpr std::size_t short_butterfly_weight = 6;
  static constexpr std::size_t butterfly_weight = 2;
  static constexpr std::size_t split_weight = 8;
  static constexpr std::size_t multiply_weight = 2;
  static constexpr std::size_t merge_weight = 10;
  static constexpr std::size_t scatter_weight = 1;

  struct Pass {
    PassKind kind = PassKind::gather;
    std::uint8_t channel = 0;
    std::uint8_t slot = 0;
    std::size_t index = 0;
    std::size_t elements = 0;
    std::size_t weight = 1;
    const PreparedResponse *response = nullptr;
  };

  struct LevelChannel {
    std::vector<float> history;
    std::vector<float> work;
    std::vector<float> sum;
    std::array<std::vector<float>, 2> output;
  };

  struct Level {
    Level(std::size_t level_index, const ConvolutionLevel &level_shape, std::size_t channels)
        : index(level_index), shape(level_shape), plan(2 * level_shape.partition_frames),
          bins(level_shape.partition_frames + 1) {
      const std::size_t size = shape.partition_frames;
      for (std::size_t c = 0; c < channels; ++c) {
        LevelChannel channel;
        channel.history.assign(shape.partitions * 2 * bins, 0.0f);
        channel.work.assign(2 * size, 0.0f);
        channel.sum.assign(2 * bins, 0.0f);
        channel.output[0].assign(2 * size, 0.0f);
        channel.output[1].assign(2 * size, 0.0f);
        state.push_back(std::move(channel));
      }
      program.reserve(channels * (3 + plan.pass_count() + 2 * (3 + plan.pass_count())));
    }

    std::size_t index;
    ConvolutionLevel shape;
    detail::FftPlan plan;
    std::size_t bins;
    std::vector<LevelChannel> state;
    std::vector<Pass> program;
    std::uint64_t period_start = 0;
    std::size_t newest = 0;
    std::size_t readable = 0;
    std::size_t cost = 0;
    std::size_t done = 0;
    std::size_t pass = 0;
    std::size_t element = 0;
  };

  bool holds(const PreparedResponse *response) const noexcept {
    return response != nullptr &&
           (response == slots_[0] || response == slots_[1] || (queued_ && *queued_ == response));
  }

  std::size_t held_count() const noexcept {
    std::size_t count = 0;
    const PreparedResponse *seen[3] = {slots_[0], slots_[1], queued_.value_or(nullptr)};
    for (std::size_t i = 0; i < 3; ++i) {
      if (seen[i] != nullptr && std::find(seen, seen + i, seen[i]) == seen + i) {
        ++count;
      }
    }
    return count;
  }

  bool can_hold(const PreparedResponse *response) const noexcept {
    return response == nullptr || holds(response) ||
           retired_count_ + held_count() + 1 <= retired_capacity;
  }

  void release(const PreparedResponse *response) noexcept {
    if (response == nullptr || holds(response)) {
      return;
    }
    assert(retired_count_ < retired_capacity);
    retired_[(retired_first_ + retired_count_) % retired_capacity] = response;
    ++retired_count_;
  }

  void install(const PreparedResponse *response) noexcept {
    const PreparedResponse *previous[3] = {slots_[0], slots_[1], queued_.value_or(nullptr)};
    slots_ = {response, nullptr};
    current_ = 0;
    queued_.reset();
    state_ = ConvolverSwitch::steady;
    for (std::size_t i = 0; i < 3; ++i) {
      if (std::find(previous, previous + i, previous[i]) == previous + i) {
        release(previous[i]);
      }
    }
    clear_history();
  }

  void clear_history() noexcept {
    for (std::vector<float> &ring : rings_) {
      std::fill(ring.begin(), ring.end(), 0.0f);
    }
    for (Level &level : levels_) {
      for (LevelChannel &channel : level.state) {
        std::fill(channel.history.begin(), channel.history.end(), 0.0f);
        std::fill(channel.work.begin(), channel.work.end(), 0.0f);
        std::fill(channel.sum.begin(), channel.sum.end(), 0.0f);
        std::fill(channel.output[0].begin(), channel.output[0].end(), 0.0f);
        std::fill(channel.output[1].begin(), channel.output[1].end(), 0.0f);
      }
      level.program.clear();
      level.period_start = 0;
      level.newest = 0;
      level.readable = 0;
      level.cost = level.done = level.pass = level.element = 0;
    }
    position_ = 0;
    next_grid_ = 0;
  }

  void begin_switch(const PreparedResponse *response) noexcept {
    slots_[1 - current_] = response;
    const std::uint64_t largest = layout_.largest_partition();
    fade_start_ = largest == 0 ? position_ : ((position_ + largest - 1) / largest + 1) * largest;
    fade_end_ = fade_start_ + fade_frames_;
    state_ = ConvolverSwitch::priming;
    advance_switch();
  }

  void advance_switch() noexcept {
    for (;;) {
      if (state_ == ConvolverSwitch::priming && position_ >= fade_start_) {
        state_ = ConvolverSwitch::fading;
      } else if (state_ == ConvolverSwitch::fading && position_ >= fade_end_) {
        const PreparedResponse *outgoing = slots_[current_];
        slots_[current_] = nullptr;
        current_ = 1 - current_;
        state_ = ConvolverSwitch::steady;
        release(outgoing);
        if (queued_) {
          const PreparedResponse *next = *queued_;
          queued_.reset();
          if (next != slots_[current_]) {
            begin_switch(next);
          }
        }
      } else {
        return;
      }
    }
  }

  // Whether a slot's response is heard in the period [from, from + size).
  bool slot_audible(std::size_t slot, std::uint64_t from, std::uint64_t size) const noexcept {
    if (slots_[slot] == nullptr) {
      return false;
    }
    if (state_ == ConvolverSwitch::steady) {
      return slot == current_;
    }
    return slot == current_ ? from < fade_end_ : from + size > fade_start_;
  }

  void run_grid() noexcept {
    for (Level &level : levels_) {
      const std::uint64_t size = level.shape.partition_frames;
      if (position_ % size == 0) {
        start_period(level);
      }
      const std::uint64_t slots = size / grid_;
      const std::uint64_t slot = (position_ - level.period_start) / grid_;
      advance_level(level, static_cast<std::size_t>(level.cost * (slot + 1) / slots));
    }
  }

  void start_period(Level &level) noexcept {
    level.period_start = position_;
    level.readable = 1 - level.readable;
    level.newest = (level.newest + 1) % level.shape.partitions;
    level.program.clear();
    const std::size_t size = level.shape.partition_frames;
    const std::size_t half_bins = size / 2 + 1;
    for (std::size_t c = 0; c < channels(); ++c) {
      const auto channel = static_cast<std::uint8_t>(c);
      level.program.push_back({PassKind::gather, channel, 0, 0, size, gather_weight, nullptr});
      push_butterflies(level, channel, 0, nullptr);
      level.program.push_back({PassKind::split, channel, 0, 0, half_bins, split_weight, nullptr});
      for (std::uint8_t slot = 0; slot < 2; ++slot) {
        if (!slot_audible(slot, position_ + size, size)) {
          continue;
        }
        const PreparedResponse *response = slots_[slot];
        level.program.push_back({PassKind::multiply, channel, slot, 0,
                                 level.shape.partitions * level.bins, multiply_weight, response});
        level.program.push_back(
            {PassKind::merge, channel, slot, 0, half_bins, merge_weight, response});
        push_butterflies(level, channel, slot, response);
        level.program.push_back(
            {PassKind::scatter, channel, slot, 0, size / 2, scatter_weight, response});
      }
    }
    level.cost = 0;
    for (const Pass &pass : level.program) {
      level.cost += pass.elements * pass.weight;
    }
    level.done = level.pass = level.element = 0;
  }

  static void push_butterflies(Level &level, std::uint8_t channel, std::uint8_t slot,
                               const PreparedResponse *response) noexcept {
    for (std::size_t p = 0; p < level.plan.pass_count(); ++p) {
      const std::size_t elements = level.plan.pass_elements(p);
      const bool short_runs = p == 0 || (std::size_t{2} << p) < 16;
      const std::size_t weight = short_runs ? short_butterfly_weight : butterfly_weight;
      level.program.push_back(
          {PassKind::butterflies, channel, slot, p, elements, weight, response});
    }
  }

  void advance_level(Level &level, std::size_t target) noexcept {
    while (level.done < target && level.pass < level.program.size()) {
      const Pass &pass = level.program[level.pass];
      const std::size_t wanted = (target - level.done + pass.weight - 1) / pass.weight;
      const std::size_t count = std::min(pass.elements - level.element, wanted);
      run_pass(level, pass, level.element, level.element + count);
      level.element += count;
      level.done += count * pass.weight;
      if (level.element == pass.elements) {
        ++level.pass;
        level.element = 0;
      }
    }
  }

  void run_pass(Level &level, const Pass &pass, std::size_t begin, std::size_t end) noexcept {
    LevelChannel &channel = level.state[pass.channel];
    const std::size_t size = level.shape.partition_frames;
    const std::size_t bins = level.bins;
    float *re = channel.work.data();
    float *im = re + size;
    switch (pass.kind) {
    case PassKind::gather: {
      const std::size_t window =
          static_cast<std::size_t>((level.period_start + ring_size_ - 2 * size) & (ring_size_ - 1));
      level.plan.gather(rings_[pass.channel].data() + window, re, im, begin, end);
      break;
    }
    case PassKind::butterflies:
      level.plan.pass(pass.index, re, im, begin, end);
      break;
    case PassKind::split: {
      float *spectrum = channel.history.data() + level.newest * 2 * bins;
      level.plan.split(re, im, spectrum, spectrum + bins, begin, end);
      break;
    }
    case PassKind::multiply:
      multiply(level, channel, pass, begin, end);
      break;
    case PassKind::merge:
      level.plan.merge(channel.sum.data(), channel.sum.data() + bins, re, im, begin, end);
      break;
    case PassKind::scatter: {
      float *output = channel.output[pass.slot].data() + (1 - level.readable) * size;
      level.plan.scatter(re + size / 2, im + size / 2, output, begin, end);
      break;
    }
    }
  }

  static void multiply(Level &level, LevelChannel &channel, const Pass &pass, std::size_t begin,
                       std::size_t end) noexcept {
    const std::size_t bins = level.bins;
    const std::size_t partitions = level.shape.partitions;
    const PreparedResponse::Channel &response =
        pass.response
            ->channels_[std::min<std::size_t>(pass.channel, pass.response->channels() - 1)];
    const std::vector<float> &spectra = response.spectra[level.index];
    float *sum = channel.sum.data();
    std::size_t p = begin / bins;
    std::size_t k0 = begin % bins;
    std::size_t slot = (level.newest + partitions - p) % partitions;
    std::size_t remaining = end - begin;
    while (remaining > 0) {
      const std::size_t k1 = std::min(bins, k0 + remaining);
      const float *x = channel.history.data() + slot * 2 * bins;
      const float *h = spectra.data() + p * 2 * bins;
      if (p == 0) {
        multiply_bins(sum, sum + bins, x, x + bins, h, h + bins, k0, k1);
      } else {
        accumulate_bins(sum, sum + bins, x, x + bins, h, h + bins, k0, k1);
      }
      remaining -= k1 - k0;
      k0 = 0;
      ++p;
      slot = slot == 0 ? partitions - 1 : slot - 1;
    }
  }

  static void multiply_bins(float *__restrict sum_re, float *__restrict sum_im,
                            const float *__restrict x_re, const float *__restrict x_im,
                            const float *__restrict h_re, const float *__restrict h_im,
                            std::size_t begin, std::size_t end) noexcept {
    for (std::size_t k = begin; k < end; ++k) {
      sum_re[k] = x_re[k] * h_re[k] - x_im[k] * h_im[k];
      sum_im[k] = x_re[k] * h_im[k] + x_im[k] * h_re[k];
    }
  }

  static void accumulate_bins(float *__restrict sum_re, float *__restrict sum_im,
                              const float *__restrict x_re, const float *__restrict x_im,
                              const float *__restrict h_re, const float *__restrict h_im,
                              std::size_t begin, std::size_t end) noexcept {
    for (std::size_t k = begin; k < end; ++k) {
      sum_re[k] += x_re[k] * h_re[k] - x_im[k] * h_im[k];
      sum_im[k] += x_re[k] * h_im[k] + x_im[k] * h_re[k];
    }
  }

  void process_run(std::size_t c, const float *input, float *output, std::size_t count) noexcept {
    std::vector<float> &ring = rings_[c];
    const std::size_t mask = ring_size_ - 1;
    for (std::size_t i = 0; i < count; ++i) {
      const float sample = std::isfinite(input[i]) ? input[i] : 0.0f;
      const auto index = static_cast<std::size_t>((position_ + i) & mask);
      ring[index] = sample;
      ring[index + ring_size_] = sample;
    }
    switch (state_) {
    case ConvolverSwitch::steady:
    case ConvolverSwitch::priming:
      if (slots_[current_] == nullptr) {
        std::fill(output, output + count, 0.0f);
      } else {
        render(c, current_, output, count);
      }
      break;
    case ConvolverSwitch::fading: {
      float *from = path_[0].data();
      float *to = path_[1].data();
      render_or_silence(c, current_, from, count);
      render_or_silence(c, 1 - current_, to, count);
      const double length = static_cast<double>(fade_frames_);
      const std::uint64_t elapsed = position_ - fade_start_;
      for (std::size_t i = 0; i < count; ++i) {
        const double weight = static_cast<double>(elapsed + i + 1) / length;
        output[i] = crossfade(from[i], to[i], weight);
      }
      break;
    }
    }
  }

  void render_or_silence(std::size_t c, std::size_t slot, float *output,
                         std::size_t count) noexcept {
    if (slots_[slot] == nullptr) {
      std::fill(output, output + count, 0.0f);
    } else {
      render(c, slot, output, count);
    }
  }

  // Head plus every level's output for one response, summed in a fixed order.
  void render(std::size_t c, std::size_t slot, float *output, std::size_t count) noexcept {
    const PreparedResponse *response = slots_[slot];
    const PreparedResponse::Channel &taps =
        response->channels_[std::min<std::size_t>(c, response->channels() - 1)];
    const float *ring = rings_[c].data();
    const std::size_t head = layout_.head_frames;
    for (std::size_t i = 0; i < count; ++i) {
      const auto start =
          static_cast<std::size_t>((position_ + i + 1 + ring_size_ - head) & (ring_size_ - 1));
      output[i] = dot(taps.head.data(), ring + start, head);
    }
    for (const Level &level : levels_) {
      const std::size_t size = level.shape.partition_frames;
      const float *source = level.state[c].output[slot].data() + level.readable * size +
                            static_cast<std::size_t>(position_ - level.period_start);
      for (std::size_t i = 0; i < count; ++i) {
        output[i] += source[i];
      }
    }
  }

  // Interleaved partial sums combined pairwise: a fixed order, so the result does not depend on
  // how the compiler vectorizes the loop.
  static float dot(const float *__restrict taps, const float *__restrict window,
                   std::size_t count) noexcept {
    float lanes[convolution_head_alignment] = {};
    for (std::size_t i = 0; i < count; i += convolution_head_alignment) {
      for (std::size_t l = 0; l < convolution_head_alignment; ++l) {
        lanes[l] += taps[i + l] * window[i + l];
      }
    }
    for (std::size_t width = convolution_head_alignment / 2; width > 0; width /= 2) {
      for (std::size_t l = 0; l < width; ++l) {
        lanes[l] += lanes[l + width];
      }
    }
    return lanes[0];
  }

  ConvolverConfig config_;
  ConvolutionLayout layout_;
  std::uint64_t fade_frames_ = 0;
  std::size_t grid_ = 0;
  std::size_t ring_size_ = 0;
  std::size_t max_run_ = 0;
  std::vector<std::vector<float>> rings_;
  std::vector<std::vector<float>> path_;
  std::vector<Level> levels_;
  std::uint64_t position_ = 0;
  std::uint64_t next_grid_ = 0;

  ConvolverSwitch state_ = ConvolverSwitch::steady;
  std::array<const PreparedResponse *, 2> slots_{};
  std::size_t current_ = 0;
  std::optional<const PreparedResponse *> queued_;
  std::uint64_t fade_start_ = 0;
  std::uint64_t fade_end_ = 0;
  std::array<const PreparedResponse *, retired_capacity> retired_{};
  std::size_t retired_first_ = 0;
  std::size_t retired_count_ = 0;
};

// Owns prepared responses and hands them between one control thread and one audio thread
// without locks, and without allocating or freeing on the audio thread.
//
// Control thread: submit() a response built off the audio thread; collect() destroys responses
// the audio thread has retired. Audio thread: take() the newest submission, pass it to
// Convolver::crossfade_to() (or set_response() while quiescent), and retire() every pointer
// returned by Convolver::take_retired() or refused by crossfade_to(). A submission that the
// audio thread has not taken yet is replaced by the next one and destroyed by submit().
// Destroying the exchange destroys every response it owns, so the convolver must no longer
// reference them.
class ResponseExchange {
public:
  static constexpr std::size_t capacity = 8;

  ResponseExchange() = default;
  ResponseExchange(const ResponseExchange &) = delete;
  ResponseExchange &operator=(const ResponseExchange &) = delete;

  // Takes ownership and returns true, or returns false leaving `response` untouched when
  // `capacity` responses are owned even after collect(). A null response is not submitted.
  bool submit(std::unique_ptr<PreparedResponse> &response) {
    collect();
    if (!response) {
      return false;
    }
    const auto free_slot = std::find(owned_.begin(), owned_.end(), nullptr);
    if (free_slot == owned_.end()) {
      return false;
    }
    *free_slot = std::move(response);
    destroy(pending_.exchange(free_slot->get(), std::memory_order_acq_rel));
    return true;
  }

  // Destroys the responses retired so far and returns how many.
  std::size_t collect() {
    const std::size_t written = retired_written_.load(std::memory_order_acquire);
    std::size_t read = retired_read_;
    const std::size_t count = written - read;
    for (; read != written; ++read) {
      destroy(retired_[read % capacity].load(std::memory_order_relaxed));
    }
    retired_read_ = read;
    return count;
  }

  // Audio thread: the newest submission not taken before, or nullptr.
  const PreparedResponse *take() noexcept {
    return pending_.exchange(nullptr, std::memory_order_acq_rel);
  }

  // Audio thread: hands back a pointer obtained from take() that nothing references any more.
  void retire(const PreparedResponse *response) noexcept {
    if (response == nullptr) {
      return;
    }
    const std::size_t written = retired_written_.load(std::memory_order_relaxed);
    retired_[written % capacity].store(response, std::memory_order_relaxed);
    retired_written_.store(written + 1, std::memory_order_release);
  }

private:
  void destroy(const PreparedResponse *response) {
    if (response == nullptr) {
      return;
    }
    for (std::unique_ptr<PreparedResponse> &owned : owned_) {
      if (owned.get() == response) {
        owned.reset();
        return;
      }
    }
  }

  std::array<std::unique_ptr<PreparedResponse>, capacity> owned_{};
  std::atomic<const PreparedResponse *> pending_{nullptr};
  std::array<std::atomic<const PreparedResponse *>, capacity> retired_{};
  std::atomic<std::size_t> retired_written_{0};
  std::size_t retired_read_ = 0;
};

}

#endif
