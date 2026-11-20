#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <source_location>
#include <stdexcept>
#include <string>
#include <vector>
#include <vespera/kit/module.hpp>
#include <vespera/tempo.hpp>

namespace {
void check(bool ok, std::source_location where = std::source_location::current()) {
  if (!ok) {
    throw std::runtime_error("Tempo check failed at " + std::to_string(where.line()));
  }
}

struct Gain {
  static constexpr const char *id = "test.untimed";
  static constexpr const char *name = "Untimed";
  static constexpr const char *version = "1";
  static constexpr uint32_t layouts = VS_LAYOUT_MONO;
  static constexpr auto parameters =
      vespera::kit::parameters(vespera::kit::knob(1, "Gain").range(0, 2).initial(1));
  struct Processor {
    static constexpr uint32_t latency_frames = 0;
    static constexpr uint64_t tail_frames = 0;
    explicit Processor(const vespera::kit::Setup &) {}
    void reset() noexcept {}
    vespera::kit::Frame process_frame(const vespera::kit::Frame &input) noexcept { return input; }
  };
};
}

int main() {
  try {
    for (double rate : {44100., 48000., 96000.}) {
      vespera::TempoClock clock;
      clock.prepare(rate, {137, true, -12});
      std::vector<uint64_t> onsets;
      bool sounding = false;
      const auto total = static_cast<uint64_t>(rate * 20);
      uint64_t frame = 0;
      while (frame < total) {
        const auto count = std::min<uint64_t>(total - frame, 1 + frame % 511);
        auto context = clock.context();
        check(std::abs(context.beat_position - static_cast<double>(frame) * 137 / (60 * rate)) <
              1e-10);
        for (uint64_t f = 0; f < count; ++f) {
          float value = clock.sample();
          check(std::isfinite(value) && std::abs(value) < 0.26f);
          const bool active = std::abs(value) > 1e-7f;
          if (active && !sounding &&
              (onsets.empty() || frame + f - onsets.back() > static_cast<uint64_t>(rate * .1))) {
            onsets.push_back(frame + f);
          }
          sounding = active;
        }
        frame += count;
      }
      check(onsets.size() == 46);
      for (size_t i = 0; i < onsets.size(); ++i) {
        const double expected = static_cast<double>(i) * rate * 60 / 137;
        check(std::abs(static_cast<double>(onsets[i]) - expected) <= 2);
      }
      const double before = clock.beat_position();
      check(clock.set_settings({73, false, -18}));
      check(clock.beat_position() == before);
      for (int i = 0; i < 1000; ++i) {
        clock.sample();
      }
      check(std::abs(clock.beat_position() - before - 1000 * 73 / (60 * rate)) < 1e-10);
      check(clock.sample() == 0);
      check(!clock.set_settings({std::numeric_limits<double>::quiet_NaN(), true, 0}));
      check(!clock.set_settings({39, false, -18}));
      check(!clock.set_settings({241, false, -18}));
      check(!clock.set_settings({120, true, 1}));
    }

    vespera::TempoClock clock;
    clock.prepare(48000, {120, false, -6}, .25);
    check(clock.set_settings({120, true, -6}));
    for (int i = 0; i <= 18000; ++i) {
      check(clock.sample() == 0);
    }
    check(clock.sample() != 0);
    const auto beat = clock.beat_position();
    clock.prepare(96000, {120, false, -6}, beat);
    check(clock.beat_position() == beat);

    vespera::TapTempo tap;
    check(!tap.push(0));
    check(tap.push(.25) == 240);
    check(tap.push(1.75) == 40);
    check(!tap.push(1.8));
    check(!tap.push(std::numeric_limits<double>::infinity()));
    check(std::abs(tap.push(2.3).value() - 120) < 1e-10);
    check(!tap.push(1));

    // A module that does not declare host tempo never reads the tempo pointer, whatever the
    // block's size.
    using Module = vespera::kit::Module<Gain>;
    check(Module::api.capabilities == 0);
    void *instance = nullptr;
    check(Module::api.create(&instance) == VS_OK);
    vs_prepare_info info{sizeof(info), 48000, 16, 1};
    vs_module_timing timing{sizeof(timing), 0, 0};
    check(Module::api.prepare(instance, &info, &timing) == VS_OK);
    float input[16]{.5f}, output[16]{};
    const float *in[] = {input};
    float *out[] = {output};
    vs_process_block block{};
    block.inputs = in;
    block.outputs = out;
    block.channels = 1;
    block.frames = 16;
    block.tempo = reinterpret_cast<const vs_tempo_context *>(1);
    for (uint32_t size : {uint32_t(VS_PROCESS_BLOCK_V1_SIZE), uint32_t(sizeof(block))}) {
      block.struct_size = size;
      output[0] = 0;
      check(Module::api.process(instance, &block) == VS_OK);
      check(output[0] == .5f);
    }
    Module::api.destroy(instance);
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
