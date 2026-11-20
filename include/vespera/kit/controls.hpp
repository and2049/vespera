#ifndef VESPERA_KIT_CONTROLS_HPP
#define VESPERA_KIT_CONTROLS_HPP

#include <vespera/dsp/smoothing.hpp>
#include <vespera/kit/parameters.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

namespace vespera::kit {

inline constexpr uint32_t default_coefficient_cadence = 32;

// Linear smoothing for every parameter of a table, each over its declared smoothing time, plus a
// coefficient refresh schedule that depends only on the number of frames advanced, never on how
// the host partitions blocks. Belongs to one processor and its processing thread.
template <size_t N> class Controls {
public:
  Controls(const Table<N> &table, double sample_rate, std::span<const float> values,
           uint32_t cadence = default_coefficient_cadence)
      : cadence_(cadence) {
    configure(table, sample_rate, values);
  }

  // Starts from values held at double precision, for offline use with settings that binary32
  // cannot represent exactly.
  Controls(const Table<N> &table, double sample_rate, std::span<const double> values,
           uint32_t cadence = default_coefficient_cadence)
      : cadence_(cadence) {
    configure(table, sample_rate, values);
  }

  // Movement of this control no longer requests a coefficient refresh, for controls the
  // processor reads directly every frame.
  void exclude(size_t index) noexcept { tracked_[index] = false; }

  void set(size_t index, double value) noexcept { smoothers_[index].set_target(value); }

  // Settles every control at its target and requests a refresh on the next frame.
  void reset() noexcept {
    for (auto &smoother : smoothers_) {
      smoother.reset(smoother.target());
    }
    countdown_ = 0;
    dirty_ = true;
  }

  // Advances every control by one frame. Returns true when the processor should recompute
  // coefficients from the current values now: on the first frame of each cadence period in
  // which a tracked control moved since the previous refresh.
  bool advance() noexcept {
    for (size_t i = 0; i < N; ++i) {
      dirty_ = dirty_ || (tracked_[i] && smoothers_[i].active());
      smoothers_[i].process();
    }
    bool refresh = false;
    if (countdown_ == 0) {
      refresh = dirty_;
      dirty_ = false;
      countdown_ = cadence_;
    }
    --countdown_;
    return refresh;
  }

  double operator[](size_t index) const noexcept { return smoothers_[index].current(); }
  double target(size_t index) const noexcept { return smoothers_[index].target(); }
  bool active(size_t index) const noexcept { return smoothers_[index].active(); }

private:
  template <class Value>
  void configure(const Table<N> &table, double sample_rate, std::span<const Value> values) {
    if (values.size() != N) {
      throw std::invalid_argument("Control values must match the parameter table");
    }
    if (cadence_ == 0) {
      throw std::invalid_argument("Coefficient cadence must be positive");
    }
    for (size_t i = 0; i < N; ++i) {
      smoothers_[i].configure(sample_rate, table[i].smoothing_seconds);
      smoothers_[i].reset(values[i]);
      tracked_[i] = true;
    }
  }

  std::array<dsp::LinearSmoother, N> smoothers_{};
  std::array<bool, N> tracked_{};
  uint32_t cadence_;
  uint32_t countdown_ = 0;
  bool dirty_ = false;
};

}

#endif
