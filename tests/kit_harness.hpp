#ifndef VESPERA_TESTS_KIT_HARNESS_HPP
#define VESPERA_TESTS_KIT_HARNESS_HPP

#include <vespera/module.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace kit_test {

inline int failures = 0;
inline const char *current_test = "";

#define CHECK(condition)                                                                           \
  do {                                                                                             \
    if (!(condition)) {                                                                            \
      std::fprintf(stderr, "%s:%d: [%s] check failed: %s\n", __FILE__, __LINE__,                   \
                   ::kit_test::current_test, #condition);                                          \
      ++::kit_test::failures;                                                                      \
    }                                                                                              \
  } while (0)

using Audio = std::vector<std::vector<float>>;

struct Event {
  uint64_t frame;
  uint32_t id;
  float value;
};

inline bool same_bits(float a, float b) {
  return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b);
}

inline bool same_bits(const Audio &a, const Audio &b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t c = 0; c < a.size(); ++c) {
    if (a[c].size() != b[c].size() ||
        !std::equal(a[c].begin(), a[c].end(), b[c].begin(),
                    [](float x, float y) { return same_bits(x, y); })) {
      return false;
    }
  }
  return true;
}

class Instance {
public:
  explicit Instance(const vs_module_api &api) : api_(api) {
    if (api_.create(&instance_) != VS_OK) {
      instance_ = nullptr;
    }
  }
  ~Instance() {
    if (instance_) {
      api_.release(instance_);
      api_.destroy(instance_);
    }
  }
  Instance(const Instance &) = delete;
  Instance &operator=(const Instance &) = delete;

  void *get() const { return instance_; }

  vs_result prepare(double rate = 48000, uint32_t max_frames = 512, uint32_t channels = 2,
                    vs_module_timing *timing = nullptr) {
    vs_prepare_info info{sizeof(info), rate, max_frames, channels};
    vs_module_timing local{sizeof(local), 0, 0};
    return api_.prepare(instance_, &info, timing ? timing : &local);
  }

  vs_result process(const Audio &input, Audio &output, uint64_t begin, uint32_t frames,
                    const std::vector<vs_parameter_event> &events = {}, uint32_t flags = 0) {
    std::vector<const float *> inputs;
    std::vector<float *> outputs;
    for (size_t c = 0; c < input.size(); ++c) {
      inputs.push_back(input[c].data() + begin);
      outputs.push_back(output[c].data() + begin);
    }
    const vs_process_block block{sizeof(block),
                                 inputs.data(),
                                 outputs.data(),
                                 static_cast<uint32_t>(input.size()),
                                 frames,
                                 begin,
                                 flags,
                                 events.empty() ? nullptr : events.data(),
                                 static_cast<uint32_t>(events.size()),
                                 nullptr,
                                 nullptr,
                                 0,
                                 0};
    return api_.process(instance_, &block);
  }

  // Processes input in consecutive blocks of partition frames, delivering each event in the
  // block that contains its frame. Returns the output, or an empty result if a block failed.
  Audio render(const Audio &input, const std::vector<Event> &events, uint32_t partition) {
    Audio output(input.size(), std::vector<float>(input[0].size()));
    const uint64_t total = input[0].size();
    size_t next = 0;
    for (uint64_t begin = 0; begin < total; begin += partition) {
      const auto frames = static_cast<uint32_t>(std::min<uint64_t>(partition, total - begin));
      std::vector<vs_parameter_event> block_events;
      while (next < events.size() && events[next].frame < begin + frames) {
        block_events.push_back({events[next].id, static_cast<uint32_t>(events[next].frame - begin),
                                events[next].value});
        ++next;
      }
      if (process(input, output, begin, frames, block_events) != VS_OK) {
        return {};
      }
    }
    return output;
  }

private:
  const vs_module_api &api_;
  void *instance_ = nullptr;
};

inline Audio signal(uint32_t channels, uint32_t frames, uint32_t seed = 1) {
  Audio audio(channels, std::vector<float>(frames));
  uint32_t state = seed * 2654435761u + 1;
  for (auto &channel : audio) {
    for (float &sample : channel) {
      state = state * 1664525u + 1013904223u;
      sample = static_cast<float>(state >> 8) / static_cast<float>(1u << 24) - 0.5f;
    }
  }
  return audio;
}

}

#endif
