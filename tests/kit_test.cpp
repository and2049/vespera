#include "kit_harness.hpp"

#include <vespera/kit/controls.hpp>
#include <vespera/kit/module.hpp>
#include <vespera/parameter_display.hpp>

#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace kit = vespera::kit;
using kit_test::Audio;
using kit_test::Event;
using kit_test::Instance;
using kit_test::same_bits;
using kit_test::signal;

constexpr float nan = std::numeric_limits<float>::quiet_NaN();
constexpr float infinity = std::numeric_limits<float>::infinity();

// A one-pole lowpass whose cutoff follows a skewed, smoothed control refreshed at the coefficient
// cadence, scaled by a smoothed level read every frame and offset by a choice.
struct SmoothParameters {
  static constexpr auto parameters = kit::parameters(
      kit::toggle(1, "Enabled", true).bypass(), kit::knob(2, "Level").smoothed(0.005),
      kit::knob(3, "Tone").maps(100, 5000).skew(0.5).smoothed(0.003).unit("Hz"),
      kit::choice(4, "Mode", 3));
};

class SmoothCore {
public:
  explicit SmoothCore(const kit::Setup &setup)
      : rate_(setup.sample_rate),
        controls_(SmoothParameters::parameters, setup.sample_rate, setup.values) {
    controls_.exclude(1);
    update();
  }
  void reset() noexcept {
    controls_.reset();
    state_ = {};
  }
  void set(size_t index, float value) noexcept { controls_.set(index, value); }
  kit::Frame step(const kit::Frame &input) noexcept {
    if (controls_.advance()) {
      update();
    }
    const auto level = static_cast<float>(controls_[1]);
    const auto offset = static_cast<float>(controls_[3] * 0.01);
    kit::Frame output{};
    for (size_t c = 0; c < kit::max_channels; ++c) {
      state_[c] += coefficient_ * (input[c] - state_[c]);
      output[c] = state_[c] * level + offset;
    }
    return output;
  }
  static uint32_t refreshes;

private:
  void update() noexcept {
    ++refreshes;
    const double hz = SmoothParameters::parameters[2].plain(controls_[2]);
    coefficient_ = static_cast<float>(1.0 - std::exp(-2.0 * 3.141592653589793 * hz / rate_));
  }
  double rate_;
  kit::Controls<4> controls_;
  kit::Frame state_{};
  float coefficient_ = 0.0f;
};
uint32_t SmoothCore::refreshes = 0;

struct SmoothFrame : SmoothParameters {
  static constexpr const char *id = "test.kit.smooth";
  static constexpr const char *name = "Smooth";
  static constexpr const char *version = "1.2.3";
  static constexpr uint32_t layouts = VS_LAYOUT_MONO | VS_LAYOUT_STEREO;
  struct Processor {
    static constexpr uint32_t latency_frames = 0;
    static constexpr uint64_t tail_frames = 0;
    SmoothCore core;
    explicit Processor(const kit::Setup &setup) : core(setup) {}
    void reset() noexcept { core.reset(); }
    void set_parameter(size_t index, float value) noexcept { core.set(index, value); }
    kit::Frame process_frame(const kit::Frame &input) noexcept { return core.step(input); }
  };
};

struct SmoothBlock : SmoothParameters {
  static constexpr const char *id = "test.kit.smooth-block";
  static constexpr const char *name = "Smooth Block";
  static constexpr const char *version = "1.2.3";
  static constexpr uint32_t layouts = VS_LAYOUT_MONO | VS_LAYOUT_STEREO;
  struct Processor {
    static constexpr uint32_t latency_frames = 0;
    static constexpr uint64_t tail_frames = 0;
    SmoothCore core;
    uint32_t channels;
    explicit Processor(const kit::Setup &setup) : core(setup), channels(setup.channels) {
      spans.reserve(4096);
    }
    void reset() noexcept { core.reset(); }
    void set_parameter(size_t index, float value) noexcept { core.set(index, value); }
    void process(kit::Block &block) noexcept {
      if (spans.size() < spans.capacity()) {
        spans.push_back(block.frames);
      }
      for (uint32_t f = 0; f < block.frames; ++f) {
        kit::Frame input{};
        for (uint32_t c = 0; c < channels; ++c) {
          input[c] = block.input(c)[f];
        }
        const kit::Frame output = core.step(input);
        for (uint32_t c = 0; c < channels; ++c) {
          block.output(c)[f] = output[c];
        }
      }
    }
  };
  static std::vector<uint32_t> spans;
};
std::vector<uint32_t> SmoothBlock::spans;

// Delays its input by a latency known at compile time and applies a gain.
template <uint32_t Latency> struct StaticDelay {
  static constexpr const char *id = "test.kit.delay";
  static constexpr const char *name = "Delay";
  static constexpr const char *version = "1";
  static constexpr uint32_t layouts = VS_LAYOUT_MONO | VS_LAYOUT_STEREO;
  static constexpr auto parameters =
      kit::parameters(kit::toggle(1, "Enabled").bypass(), kit::knob(2, "Gain", 1.5f).range(0, 2));
  struct Processor {
    static constexpr uint32_t latency_frames = Latency;
    static constexpr uint64_t tail_frames = Latency;
    std::array<kit::Frame, Latency + 1> line{};
    size_t cursor = 0;
    float gain;
    explicit Processor(const kit::Setup &setup) : gain(setup.values[1]) {}
    void reset() noexcept {
      line = {};
      cursor = 0;
    }
    void set_parameter(size_t index, float value) noexcept {
      if (index == 1) {
        gain = value;
      }
    }
    kit::Frame process_frame(const kit::Frame &input) noexcept {
      line[cursor] = input;
      cursor = (cursor + 1) % line.size();
      const kit::Frame delayed = line[cursor];
      return {delayed[0] * gain, delayed[1] * gain};
    }
  };
};

// Delays its input by a latency chosen by a parameter when prepared, processing whole blocks.
struct PreparedDelay {
  static constexpr const char *id = "test.kit.prepared-delay";
  static constexpr const char *name = "Prepared Delay";
  static constexpr const char *version = "1";
  static constexpr uint32_t layouts = VS_LAYOUT_STEREO;
  static constexpr auto parameters =
      kit::parameters(kit::knob(1, "Gain", 1.5f).range(0, 2), kit::choice(2, "Length", 8, 3),
                      kit::toggle(3, "Power").bypass());
  struct Processor {
    std::vector<kit::Frame> line;
    size_t cursor = 0;
    float gain;
    uint32_t channels;
    explicit Processor(const kit::Setup &setup)
        : line(static_cast<size_t>(setup.values[1]) + 1), gain(setup.values[0]),
          channels(setup.channels) {}
    uint32_t latency_frames() const noexcept { return static_cast<uint32_t>(line.size() - 1); }
    uint64_t tail_frames() const noexcept { return line.size() - 1; }
    void reset() noexcept {
      std::fill(line.begin(), line.end(), kit::Frame{});
      cursor = 0;
    }
    void set_parameter(size_t index, float value) noexcept {
      if (index == 0) {
        gain = value;
      }
    }
    void process(kit::Block &block) noexcept {
      for (uint32_t f = 0; f < block.frames; ++f) {
        line[cursor] = {block.input(0)[f], block.input(1)[f]};
        cursor = (cursor + 1) % line.size();
        block.output(0)[f] = line[cursor][0] * gain;
        block.output(1)[f] = line[cursor][1] * gain;
      }
    }
  };
};

// Handles its own bypass parameter, which is not parameter 1.
struct CustomBypass {
  static constexpr const char *id = "test.kit.custom";
  static constexpr const char *name = "Custom";
  static constexpr const char *version = "1";
  static constexpr uint32_t layouts = VS_LAYOUT_MONO;
  static constexpr auto bypass = kit::BypassPolicy::custom;
  static constexpr auto parameters =
      kit::parameters(kit::knob(3, "Drive"), kit::toggle(7, "On", true).bypass());
  struct Processor {
    static constexpr uint32_t latency_frames = 0;
    static constexpr uint64_t tail_frames = 0;
    bool on;
    explicit Processor(const kit::Setup &setup) : on(setup.values[1] >= 0.5f) {}
    void reset() noexcept {}
    void set_parameter(size_t index, float value) noexcept {
      if (index == 1) {
        on = value >= 0.5f;
      }
    }
    kit::Frame process_frame(const kit::Frame &input) noexcept {
      return on ? kit::Frame{input[0] * 2.0f, 0.0f} : kit::Frame{input[0] * 0.5f, 0.0f};
    }
  };
};

// Passes input through; reports what reached it.
struct Plain {
  static constexpr const char *id = "test.kit.plain";
  static constexpr const char *name = "Plain";
  static constexpr const char *version = "2";
  static constexpr uint32_t layouts = VS_LAYOUT_MONO | VS_LAYOUT_STEREO;
  static constexpr auto parameters =
      kit::parameters(kit::knob(5, "A", 0.25f), kit::knob(9, "B").range(-1, 1).initial(0));
  struct Processor {
    static constexpr uint32_t latency_frames = 0;
    double rate;
    uint32_t resets = 0;
    explicit Processor(const kit::Setup &setup) : rate(setup.sample_rate) {}
    uint64_t tail_frames() const noexcept { return static_cast<uint64_t>(rate); }
    void reset() noexcept { ++resets; }
    kit::Frame process_frame(const kit::Frame &input) noexcept { return input; }
  };
};

struct Described : Plain {
  static constexpr const char *modes[] = {"Clean", "Crunch", "Lead"};
  static constexpr auto parameters = kit::parameters(
      kit::toggle(1, "Enabled", true).bypass(), kit::knob(2, "Drive", 0.3f).maps(1, 20).skew(0.5),
      kit::knob(3, "Level").range(-24, 12).initial(0).decibels(),
      kit::knob(4, "HPF", 0).maps(20, 500).skew(0.6).hertz().off_at_minimum(0.5),
      kit::knob(5, "Mix").percent(), kit::choice(6, "Voice", modes, 1), kit::choice(7, "Range", 4),
      kit::knob(8, "Voices").range(1, 8).initial(2).steps(8).display_unit(" voices"),
      kit::knob(9, "Pan").shows(-50, 50).decimals(1).display_unit(" L/R").show_sign(),
      kit::knob(10, "Cut", 1).maps(1000, 20000).skew(0.6).hertz().off_at_maximum());
};

struct Verbatim : Plain {
  static constexpr bool sanitise_input = false;
  static constexpr bool flush_subnormals = false;
};

struct Tempo {
  static constexpr const char *id = "test.kit.tempo";
  static constexpr const char *name = "Tempo";
  static constexpr const char *version = "1";
  static constexpr uint32_t layouts = VS_LAYOUT_MONO;
  static constexpr auto parameters = kit::parameters();
  struct Processor {
    static constexpr uint32_t latency_frames = 0;
    static constexpr uint64_t tail_frames = 0;
    float bpm = 0;
    explicit Processor(const kit::Setup &) {}
    void reset() noexcept {}
    void tempo(const vs_tempo_context *context) noexcept {
      bpm = context ? static_cast<float>(context->bpm) : -1.0f;
    }
    kit::Frame process_frame(const kit::Frame &) noexcept { return {bpm, 0}; }
  };
};

struct Log {
  char kind;
  uint32_t frame;
  uint32_t value;
};

struct Midi {
  static constexpr const char *id = "test.kit.midi";
  static constexpr const char *name = "MIDI";
  static constexpr const char *version = "1";
  static constexpr uint32_t layouts = VS_LAYOUT_MONO;
  static constexpr auto parameters = kit::parameters(kit::knob(1, "Level"));
  static std::vector<Log> log;
  struct Processor {
    static constexpr uint32_t latency_frames = 0;
    static constexpr uint64_t tail_frames = 0;
    uint32_t frame = 0;
    explicit Processor(const kit::Setup &) { log.reserve(64); }
    void reset() noexcept { frame = 0; }
    void set_parameter(size_t, float value) noexcept {
      log.push_back({'p', frame, static_cast<uint32_t>(value * 100)});
    }
    void midi(const vs_midi_event &event) noexcept { log.push_back({'m', frame, event.data1}); }
    void panic() noexcept { log.push_back({'!', frame, 0}); }
    kit::Frame process_frame(const kit::Frame &) noexcept {
      ++frame;
      return {};
    }
  };
};
std::vector<Log> Midi::log;

// Schema 2 adds a one-byte mode after the parameters; schema 1 held the parameters alone.
struct Stateful {
  static constexpr const char *id = "test.kit.stateful";
  static constexpr const char *name = "Stateful";
  static constexpr const char *version = "1";
  static constexpr uint32_t layouts = VS_LAYOUT_MONO;
  static constexpr uint32_t state_schema = 2;
  static constexpr auto parameters =
      kit::parameters(kit::knob(1, "A"), kit::knob(2, "B").range(-10, 10).initial(2));
  struct Extra {
    uint8_t mode = 1;
    uint64_t size() const noexcept { return 1; }
    void save(std::span<uint8_t> bytes) const noexcept { bytes[0] = mode; }
    bool load(std::span<const uint8_t> bytes) {
      if (bytes.size() != 1 || bytes[0] > 3) {
        return false;
      }
      mode = bytes[0];
      return true;
    }
  };
  static vs_result migrate(uint32_t schema, std::span<const uint8_t> bytes,
                           std::vector<uint8_t> &current) {
    if (schema != 1) {
      return VS_ERROR_UNSUPPORTED;
    }
    current.assign(bytes.begin(), bytes.end());
    current.push_back(0);
    return VS_OK;
  }
  struct Processor {
    static constexpr uint32_t latency_frames = 0;
    static constexpr uint64_t tail_frames = 0;
    float mode;
    Processor(const kit::Setup &, const Extra &extra) : mode(extra.mode) {}
    void reset() noexcept {}
    kit::Frame process_frame(const kit::Frame &) noexcept { return {mode, 0}; }
  };
};

struct Fragile {
  static constexpr const char *id = "test.kit.fragile";
  static constexpr const char *name = "Fragile";
  static constexpr const char *version = "1";
  static constexpr uint32_t layouts = VS_LAYOUT_MONO;
  static constexpr auto parameters = kit::parameters(kit::toggle(1, "Fail"));
  struct Processor {
    static constexpr uint32_t latency_frames = 0;
    static constexpr uint64_t tail_frames = 0;
    explicit Processor(const kit::Setup &setup) {
      if (setup.values[0] >= 0.5f) {
        throw std::runtime_error("refused");
      }
    }
    void reset() noexcept {}
    kit::Frame process_frame(const kit::Frame &input) noexcept { return input; }
  };
};

template <class Config> using M = kit::Module<Config>;

vs_process_block make_block(const float *const *in, float *const *out, uint32_t channels,
                            uint32_t frames) {
  return {sizeof(vs_process_block),
          in,
          out,
          channels,
          frames,
          0,
          0,
          nullptr,
          0,
          nullptr,
          nullptr,
          0,
          0};
}

void test_descriptors() {
  const auto &smooth = M<SmoothFrame>::api;
  CHECK(smooth.struct_size == sizeof(vs_module_api));
  CHECK(smooth.abi_version == VS_MODULE_ABI_VERSION);
  CHECK(std::string_view(smooth.id) == "test.kit.smooth");
  CHECK(std::string_view(smooth.name) == "Smooth");
  CHECK(std::string_view(smooth.version) == "1.2.3");
  CHECK(smooth.supported_layouts == (VS_LAYOUT_MONO | VS_LAYOUT_STEREO));
  CHECK(smooth.parameter_count == 4);
  CHECK(smooth.capabilities == VS_MODULE_BYPASS_PARAMETER && smooth.bypass_parameter == 1);
  const vs_parameter_descriptor &enabled = smooth.parameters[0];
  CHECK(enabled.id == 1 && std::string_view(enabled.unit) == "bool" && enabled.minimum == 0 &&
        enabled.maximum == 1 && enabled.default_value == 1);
  const vs_parameter_descriptor &tone = smooth.parameters[2];
  CHECK(tone.id == 3 && std::string_view(tone.name) == "Tone" &&
        std::string_view(tone.unit) == "Hz" && tone.minimum == 0 && tone.maximum == 1 &&
        tone.default_value == 0.5f);
  const vs_parameter_descriptor &mode = smooth.parameters[3];
  CHECK(mode.minimum == 0 && mode.maximum == 2 && mode.default_value == 0);

  CHECK(M<CustomBypass>::api.capabilities == VS_MODULE_BYPASS_PARAMETER);
  CHECK(M<CustomBypass>::api.bypass_parameter == 7);
  CHECK(M<CustomBypass>::api.supported_layouts == VS_LAYOUT_MONO);
  CHECK(M<PreparedDelay>::api.bypass_parameter == 3);
  CHECK(M<PreparedDelay>::api.supported_layouts == VS_LAYOUT_STEREO);
  CHECK(M<Plain>::api.capabilities == 0 && M<Plain>::api.bypass_parameter == 0);
  CHECK(M<Plain>::api.parameters[1].minimum == -1 && M<Plain>::api.parameters[1].maximum == 1);
  CHECK(M<Tempo>::api.capabilities == VS_MODULE_HOST_TEMPO && M<Tempo>::api.parameter_count == 0);
  CHECK(M<Midi>::api.capabilities == VS_MODULE_MIDI_INPUT);
  CHECK(M<Stateful>::state_api.schema_version == 2 && M<Plain>::state_api.schema_version == 1);
  CHECK(M<Plain>::state_api.struct_size == sizeof(vs_module_state_api));
  CHECK(M<Plain>::parameter_api.struct_size == sizeof(vs_module_parameter_api));

  static_assert(SmoothParameters::parameters.index(3) == 2);
  static_assert(SmoothParameters::parameters.index(99) == 4);
  static_assert(CustomBypass::parameters.bypass_index() == 1);
  static_assert(!Plain::parameters.has_bypass());
}

// Builds an expected metadata entry, leaving fields the case does not name at their defaults.
vs_parameter_metadata entry(uint32_t id, uint32_t kind, uint32_t steps = 0) {
  return {id, kind, steps, 0, nullptr, 0, 0, 0, nullptr, nullptr, 0, VS_PARAMETER_OFF_NONE, 0};
}

bool same(const char *a, const char *b) {
  return (a == nullptr || b == nullptr) ? a == b : std::string_view(a) == b;
}

bool same(const vs_parameter_metadata &a, const vs_parameter_metadata &b) {
  return a.parameter_id == b.parameter_id && a.kind == b.kind && a.steps == b.steps &&
         a.flags == b.flags && a.labels == b.labels && a.shown_minimum == b.shown_minimum &&
         a.shown_maximum == b.shown_maximum && a.skew == b.skew && same(a.unit, b.unit) &&
         same(a.kilo_unit, b.kilo_unit) && a.decimals == b.decimals && a.off == b.off &&
         a.off_tolerance == b.off_tolerance;
}

vs_parameter_metadata mapped(vs_parameter_metadata m, double low, double high, double skew,
                             uint32_t decimals, const char *unit, const char *kilo = "") {
  m.flags = VS_PARAMETER_DISPLAY_MAPPED;
  m.shown_minimum = low;
  m.shown_maximum = high;
  m.skew = skew;
  m.decimals = decimals;
  m.unit = unit;
  m.kilo_unit = kilo;
  return m;
}

void test_metadata() {
  const vs_module_metadata_api &plain = M<Plain>::metadata_api;
  CHECK(plain.struct_size == sizeof(vs_module_metadata_api) &&
        plain.abi_version == VS_MODULE_METADATA_ABI_VERSION && plain.parameter_count == 2 &&
        plain.parameter_size == sizeof(vs_parameter_metadata));
  CHECK(same(plain.parameters[0], entry(5, VS_PARAMETER_CONTINUOUS)));
  CHECK(same(M<SmoothFrame>::metadata_api.parameters[0], entry(1, VS_PARAMETER_TOGGLE, 2)));
  CHECK(same(M<SmoothFrame>::metadata_api.parameters[2], entry(3, VS_PARAMETER_CONTINUOUS)));
  CHECK(same(M<SmoothFrame>::metadata_api.parameters[3], entry(4, VS_PARAMETER_STEPPED, 3)));
  CHECK(M<Tempo>::metadata_api.parameter_count == 0);

  const vs_module_metadata_api &described = M<Described>::metadata_api;
  CHECK(described.parameter_count == 10);
  const vs_parameter_metadata *m = described.parameters;
  CHECK(same(m[0], entry(1, VS_PARAMETER_TOGGLE, 2)));
  CHECK(same(m[1], entry(2, VS_PARAMETER_CONTINUOUS)));
  vs_parameter_metadata level = mapped(entry(3, VS_PARAMETER_CONTINUOUS), -24, 12, 1, 1, " dB");
  level.flags |= VS_PARAMETER_DISPLAY_SIGNED;
  CHECK(same(m[2], level));
  vs_parameter_metadata hpf =
      mapped(entry(4, VS_PARAMETER_CONTINUOUS), 20, 500, 0.6, 0, " Hz", " kHz");
  hpf.off = VS_PARAMETER_OFF_MINIMUM;
  hpf.off_tolerance = 0.5;
  CHECK(same(m[3], hpf));
  CHECK(same(m[4], mapped(entry(5, VS_PARAMETER_CONTINUOUS), 0, 100, 1, 0, "%")));
  vs_parameter_metadata voice = entry(6, VS_PARAMETER_ENUMERATED, 3);
  voice.labels = Described::modes;
  CHECK(same(m[5], voice));
  CHECK(same(m[6], entry(7, VS_PARAMETER_STEPPED, 4)));
  CHECK(same(m[7], mapped(entry(8, VS_PARAMETER_STEPPED, 8), 1, 8, 1, 0, " voices")));
  vs_parameter_metadata pan = mapped(entry(9, VS_PARAMETER_CONTINUOUS), -50, 50, 1, 1, " L/R");
  pan.flags |= VS_PARAMETER_DISPLAY_SIGNED;
  CHECK(same(m[8], pan));
  vs_parameter_metadata cut =
      mapped(entry(10, VS_PARAMETER_CONTINUOUS), 1000, 20000, 0.6, 0, " Hz", " kHz");
  cut.off = VS_PARAMETER_OFF_MAXIMUM;
  CHECK(same(m[9], cut));

  std::string error;
  const auto parameters = vespera::describe_parameters(M<Described>::api, &described, &error);
  CHECK(parameters.has_value());
  if (parameters) {
    const auto &p = *parameters;
    CHECK(p[0].text(1) == "On" && p[1].text(0.5) == "50%");
    CHECK(p[2].text(0) == "0.0 dB" && p[2].text(-6) == "-6.0 dB" && p[2].text(3.3) == "+3.3 dB");
    CHECK(p[3].text(0) == "OFF" && p[3].text(1) == "500 Hz");
    CHECK(p[4].text(0.255) == "26%" && p[5].text(2) == "Lead" && p[6].text(2.4) == "2");
    CHECK(p[7].text(3.2) == "3 voices" && p[8].text(0.75) == "+25.0 L/R");
    CHECK(p[9].text(1) == "OFF" && p[9].text(0) == "1.00 kHz");
  }
  // The display mapping is the DSP scale: one declaration feeds both.
  for (double v : {0.0, 0.25, 0.5, 1.0}) {
    const auto shown = vespera::shown_number(*(*parameters)[3].mapping, 0, 1, v).shown;
    CHECK(shown == Described::parameters[3].plain(v));
  }
  CHECK(M<Described>::api.capabilities == VS_MODULE_BYPASS_PARAMETER);
  CHECK(M<Plain>::api.capabilities == 0);
}

void test_scale() {
  const kit::Scale hpf{20, 500, 0.6};
  for (double v : {0.0, 0.1, 0.37, 0.5, 0.9, 1.0}) {
    CHECK(hpf.plain(v) == 20.0 + 480.0 * std::pow(v, 1.0 / 0.6));
    CHECK(std::abs(hpf.normalized(hpf.plain(v)) - v) < 1e-12);
  }
  CHECK(hpf.plain(0) == 20 && hpf.plain(1) == 500);
  const kit::Scale linear{-12, 12};
  CHECK(linear.plain(0.25) == -6 && linear.normalized(6) == 0.75);
  constexpr auto table = kit::parameters(kit::knob(1, "LPF", 1).maps(1000, 20000).skew(0.6),
                                         kit::knob(2, "Gain").range(-24, 24).initial(0));
  CHECK(table[0].plain(0.3) == 1000.0 + 19000.0 * std::pow(0.3, 1.0 / 0.6));
  CHECK(table[0].descriptor.minimum == 0 && table[0].descriptor.maximum == 1);
  CHECK(table[1].plain(table[1].normalized(-3)) == -3);
}

void test_controls() {
  constexpr auto table = kit::parameters(kit::knob(1, "A").smoothed(0.001),
                                         kit::knob(2, "B").smoothed(0.0005), kit::knob(3, "C"));
  const std::array<float, 3> values{0.0f, 0.0f, 0.0f};
  kit::Controls<3> controls(table, 48000, values, 16);
  CHECK(!controls.advance());
  for (int i = 0; i < 100; ++i) {
    CHECK(!controls.advance());
  }
  controls.set(2, 1.0f);
  CHECK(controls[2] == 1.0 && !controls.active(2));

  const std::array<double, 3> precise{0.3, 0.7, 0.45};
  kit::Controls<3> offline(table, 48000, precise, 16);
  CHECK(offline[0] == 0.3 && offline[1] == 0.7 && offline[2] == 0.45);
  offline.set(2, 0.1);
  CHECK(offline[2] == 0.1 && offline.target(2) == 0.1);
  const std::array<double, 2> short_values{0.0, 0.0};
  bool rejected = false;
  try {
    kit::Controls<3> mismatched(table, 48000, short_values, 16);
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  CHECK(rejected);
  controls.set(0, 1.0f);
  CHECK(controls.target(0) == 1.0 && controls.active(0));
  int refreshes = 0;
  int last = -1;
  for (int frame = 0; frame < 200; ++frame) {
    if (controls.advance()) {
      ++refreshes;
      CHECK(frame % 16 == 11);
      last = frame;
    }
    if (frame == 47) {
      CHECK(controls[0] == 1.0 && !controls.active(0));
    } else if (frame < 47) {
      CHECK(controls[0] < 1.0);
    }
  }
  CHECK(refreshes == 4 && last == 59);
  controls.exclude(1);
  controls.set(1, 1.0f);
  for (int frame = 0; frame < 64; ++frame) {
    CHECK(!controls.advance());
  }
  CHECK(controls[1] == 1.0);
  controls.set(1, 0.0f);
  controls.reset();
  CHECK(controls[1] == 0.0 && !controls.active(1));
  CHECK(controls.advance());
  CHECK(!controls.advance());

  bool threw = false;
  try {
    kit::Controls<3> wrong(table, 48000, std::span<const float>(values.data(), 2));
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  CHECK(threw);
}

void test_lifecycle() {
  const auto &api = M<Plain>::api;
  CHECK(api.create(nullptr) == VS_ERROR_ARGUMENT);
  Instance module(api);
  CHECK(module.get() != nullptr);
  float in[4]{}, out[4]{};
  const float *ins[] = {in};
  float *outs[] = {out};
  vs_process_block block = make_block(ins, outs, 1, 4);
  CHECK(api.process(module.get(), &block) == VS_ERROR_STATE);
  api.reset(module.get());
  api.release(module.get());
  api.release(nullptr);
  api.reset(nullptr);
  api.destroy(nullptr);
  vs_module_timing timing{sizeof(timing), 99, 99};
  CHECK(module.prepare(44100, 64, 1, &timing) == VS_OK);
  CHECK(timing.latency_frames == 0 && timing.tail_frames == 44100);
  CHECK(module.prepare() == VS_ERROR_STATE);
  CHECK(api.process(module.get(), &block) == VS_OK);
  const uint8_t bytes[8]{};
  CHECK(M<Plain>::state_api.load(module.get(), 1, bytes, 8) == VS_ERROR_STATE);
  api.release(module.get());
  CHECK(api.process(module.get(), &block) == VS_ERROR_STATE);
  api.release(module.get());
  CHECK(module.prepare(96000, 64, 1, &timing) == VS_OK);
  CHECK(timing.tail_frames == 96000);
  CHECK(api.process(module.get(), &block) == VS_OK);

  Instance fragile(M<Fragile>::api);
  CHECK(M<Fragile>::parameter_api.set_value(fragile.get(), 1, 1) == VS_OK);
  CHECK(fragile.prepare(48000, 64, 1) == VS_ERROR_PROCESS);
  block.channels = 1;
  CHECK(M<Fragile>::api.process(fragile.get(), &block) == VS_ERROR_STATE);
  CHECK(M<Fragile>::parameter_api.set_value(fragile.get(), 1, 0) == VS_OK);
  CHECK(fragile.prepare(48000, 64, 1) == VS_OK);
  CHECK(M<Fragile>::api.process(fragile.get(), &block) == VS_OK);
}

void test_prepare_rejections() {
  const auto &api = M<Plain>::api;
  Instance module(api);
  vs_module_timing timing{sizeof(timing), 0, 0};
  vs_prepare_info info{sizeof(info), 48000, 64, 2};
  CHECK(api.prepare(nullptr, &info, &timing) == VS_ERROR_ARGUMENT);
  CHECK(api.prepare(module.get(), nullptr, &timing) == VS_ERROR_ARGUMENT);
  CHECK(api.prepare(module.get(), &info, nullptr) == VS_ERROR_ARGUMENT);
  info.struct_size = sizeof(info) - 1;
  CHECK(api.prepare(module.get(), &info, &timing) == VS_ERROR_ARGUMENT);
  info.struct_size = sizeof(info);
  timing.struct_size = sizeof(timing) - 1;
  CHECK(api.prepare(module.get(), &info, &timing) == VS_ERROR_ARGUMENT);
  timing.struct_size = sizeof(timing);
  for (double rate : {double(nan), 7999.0, 768001.0, -48000.0, double(infinity)}) {
    CHECK(module.prepare(rate) == VS_ERROR_ARGUMENT);
  }
  CHECK(module.prepare(48000, 0) == VS_ERROR_ARGUMENT);
  CHECK(module.prepare(48000, kit::max_block_frames + 1) == VS_ERROR_ARGUMENT);
  CHECK(module.prepare(48000, 64, 0) == VS_ERROR_ARGUMENT);
  CHECK(module.prepare(48000, 64, 3) == VS_ERROR_ARGUMENT);
  CHECK(module.prepare(8000, kit::max_block_frames) == VS_OK);
  api.release(module.get());
  CHECK(module.prepare(768000, 1, 1) == VS_OK);

  Instance mono(M<CustomBypass>::api);
  CHECK(mono.prepare(48000, 64, 2) == VS_ERROR_ARGUMENT);
  CHECK(mono.prepare(48000, 64, 1) == VS_OK);
  Instance stereo(M<PreparedDelay>::api);
  CHECK(stereo.prepare(48000, 64, 1) == VS_ERROR_ARGUMENT);
  CHECK(stereo.prepare(48000, 64, 2) == VS_OK);
}

void test_process_rejections() {
  const auto &api = M<SmoothFrame>::api;
  Instance module(api);
  CHECK(module.prepare(48000, 16, 2) == VS_OK);
  float l[17]{}, r[17]{}, ol[17]{}, orr[17]{};
  const float *ins[] = {l, r};
  float *outs[] = {ol, orr};
  vs_process_block block = make_block(ins, outs, 2, 16);
  CHECK(api.process(module.get(), &block) == VS_OK);
  CHECK(api.process(nullptr, &block) == VS_ERROR_ARGUMENT);
  CHECK(api.process(module.get(), nullptr) == VS_ERROR_ARGUMENT);
  auto rejects = [&](auto change) {
    vs_process_block bad = block;
    change(bad);
    return api.process(module.get(), &bad) == VS_ERROR_ARGUMENT;
  };
  CHECK(rejects([](auto &b) { b.struct_size = VS_PROCESS_BLOCK_V1_SIZE - 1; }));
  CHECK(rejects([](auto &b) { b.channels = 1; }));
  CHECK(rejects([](auto &b) { b.frames = 0; }));
  CHECK(rejects([](auto &b) { b.frames = 17; }));
  CHECK(rejects([](auto &b) { b.inputs = nullptr; }));
  CHECK(rejects([](auto &b) { b.outputs = nullptr; }));
  const float *missing_input[] = {l, nullptr};
  float *missing_output[] = {nullptr, orr};
  CHECK(rejects([&](auto &b) { b.inputs = missing_input; }));
  CHECK(rejects([&](auto &b) { b.outputs = missing_output; }));
  CHECK(rejects([](auto &b) { b.event_count = 1; }));
  const std::vector<std::vector<vs_parameter_event>> invalid = {
      {{8, 0, 0.5f}},  {{2, 0, nan}},   {{2, 0, infinity}},           {{2, 0, 1.5f}},
      {{2, 0, -0.1f}}, {{2, 16, 0.5f}}, {{2, 5, 0.5f}, {3, 4, 0.5f}},
  };
  for (const auto &events : invalid) {
    CHECK(rejects([&](auto &b) {
      b.events = events.data();
      b.event_count = static_cast<uint32_t>(events.size());
    }));
  }
  float value = 0;
  CHECK(M<SmoothFrame>::parameter_api.get_value(module.get(), 2, &value) == VS_OK && value == 0.5f);
  vs_process_block v1 = block;
  v1.struct_size = VS_PROCESS_BLOCK_V1_SIZE;
  CHECK(api.process(module.get(), &v1) == VS_OK);
}

void test_parameter_values() {
  const auto &values = M<SmoothFrame>::parameter_api;
  Instance module(M<SmoothFrame>::api);
  float value = -1;
  CHECK(values.get_value(module.get(), 3, &value) == VS_OK && value == 0.5f);
  CHECK(values.get_value(module.get(), 4, &value) == VS_OK && value == 0);
  CHECK(values.get_value(nullptr, 3, &value) == VS_ERROR_ARGUMENT);
  CHECK(values.get_value(module.get(), 3, nullptr) == VS_ERROR_ARGUMENT);
  CHECK(values.get_value(module.get(), 99, &value) == VS_ERROR_ARGUMENT);
  CHECK(values.set_value(module.get(), 99, 0) == VS_ERROR_ARGUMENT);
  CHECK(values.set_value(nullptr, 3, 0) == VS_ERROR_ARGUMENT);
  for (float bad : {nan, infinity, -infinity, 1.01f, -0.01f}) {
    CHECK(values.set_value(module.get(), 3, bad) == VS_ERROR_ARGUMENT);
  }
  CHECK(values.set_value(module.get(), 4, 2) == VS_OK);
  CHECK(values.set_value(module.get(), 4, 3) == VS_ERROR_ARGUMENT);
  CHECK(values.get_value(module.get(), 4, &value) == VS_OK && value == 2);

  CHECK(module.prepare(48000, 64, 1) == VS_OK);
  Audio input(1, std::vector<float>(64, 0.0f));
  Audio output(1, std::vector<float>(64));
  CHECK(module.process(input, output, 0, 64, {{4, 10, 1.0f}}) == VS_OK);
  CHECK(output[0][9] == 0.02f && output[0][10] == 0.01f);
  CHECK(values.get_value(module.get(), 4, &value) == VS_OK && value == 1);
  CHECK(values.set_value(module.get(), 4, 0) == VS_OK);
  CHECK(module.process(input, output, 0, 64) == VS_OK);
  CHECK(output[0][0] == 0);
}

void test_event_accuracy() {
  Instance module(M<SmoothFrame>::api);
  CHECK(module.prepare(48000, 64, 1) == VS_OK);
  Audio input(1, std::vector<float>(64, 0.0f));
  Audio output(1, std::vector<float>(64));
  CHECK(module.process(input, output, 0, 64, {{4, 0, 1}, {4, 17, 2}, {4, 17, 0}, {4, 40, 2}}) ==
        VS_OK);
  for (uint32_t f = 0; f < 64; ++f) {
    const float expected = f < 17 ? 0.01f : f < 40 ? 0.0f : 0.02f;
    CHECK(output[0][f] == expected);
  }

  Instance block(M<SmoothBlock>::api);
  CHECK(block.prepare(48000, 64, 1) == VS_OK);
  SmoothBlock::spans.clear();
  CHECK(block.process(input, output, 0, 16, {{4, 3, 1}, {3, 10, 0.2f}, {4, 10, 2}}) == VS_OK);
  CHECK(block.process(input, output, 16, 16, {{4, 0, 0}}) == VS_OK);
  CHECK(block.process(input, output, 32, 16) == VS_OK);
  CHECK((SmoothBlock::spans == std::vector<uint32_t>{3, 7, 6, 16, 16}));
  for (uint32_t f = 0; f < 48; ++f) {
    const float expected = f < 3 ? 0.0f : f < 10 ? 0.01f : f < 16 ? 0.02f : 0.0f;
    CHECK(output[0][f] == expected);
  }
}

void test_partitions_and_forms() {
  for (uint32_t channels : {1u, 2u}) {
    const Audio input = signal(channels, 3000, channels);
    const std::vector<Event> events = {{0, 2, 0.1f},   {0, 3, 0.9f},    {5, 2, 0.8f},
                                       {200, 3, 0.1f}, {200, 1, 0.0f},  {701, 4, 2.0f},
                                       {701, 3, 0.6f}, {1500, 1, 1.0f}, {2999, 2, 0.3f}};
    Audio reference;
    for (uint32_t partition : {1u, 7u, 64u, 512u}) {
      for (const vs_module_api *api : {&M<SmoothFrame>::api, &M<SmoothBlock>::api}) {
        Instance module(*api);
        CHECK(module.prepare(48000, 512, channels) == VS_OK);
        SmoothCore::refreshes = 0;
        const Audio output = module.render(input, events, partition);
        CHECK(!output.empty());
        CHECK(SmoothCore::refreshes > 1 && SmoothCore::refreshes < 3000 / 32 + 2);
        if (reference.empty()) {
          reference = output;
        } else {
          CHECK(same_bits(output, reference));
        }
      }
    }
    bool moved = false;
    for (size_t f = 1; f < reference[0].size(); ++f) {
      moved = moved || reference[0][f] != reference[0][f - 1];
    }
    CHECK(moved);
  }
}

template <class Config>
Audio render_delay(const Audio &input, float enabled, const std::vector<Event> &events,
                   uint32_t partition, uint32_t *latency = nullptr, float length = -1) {
  Instance module(M<Config>::api);
  const uint32_t bypass = M<Config>::api.bypass_parameter;
  CHECK(M<Config>::parameter_api.set_value(module.get(), bypass, enabled) == VS_OK);
  if (length >= 0) {
    CHECK(M<Config>::parameter_api.set_value(module.get(), 2, length) == VS_OK);
  }
  vs_module_timing timing{sizeof(timing), 0, 0};
  CHECK(module.prepare(48000, 512, static_cast<uint32_t>(input.size()), &timing) == VS_OK);
  if (latency) {
    *latency = timing.latency_frames;
  }
  return module.render(input, events, partition);
}

Audio delayed(const Audio &input, uint32_t latency, float gain) {
  Audio output(input.size(), std::vector<float>(input[0].size()));
  for (size_t c = 0; c < input.size(); ++c) {
    for (size_t f = latency; f < input[c].size(); ++f) {
      const float sample = std::isfinite(input[c][f - latency]) ? input[c][f - latency] : 0.0f;
      output[c][f] = sample * gain;
    }
  }
  return output;
}

void test_exact_bypass() {
  Audio special = signal(2, 2000, 7);
  const float values[] = {std::numeric_limits<float>::denorm_min(),
                          -std::numeric_limits<float>::denorm_min() * 9,
                          -0.0f,
                          1e30f,
                          -std::numeric_limits<float>::max(),
                          nan,
                          infinity,
                          -infinity,
                          17.0f};
  for (size_t i = 0; i < std::size(values); ++i) {
    special[0][10 + i] = values[i];
    special[1][600 + i] = values[i];
  }
  Audio clean = signal(2, 2000, 8);
  for (auto &channel : clean) {
    for (float &sample : channel) {
      sample += 0.75f;
    }
  }

  struct Case {
    const char *name;
    uint32_t latency;
    uint32_t bypass;
    Audio (*render)(const Audio &, float, const std::vector<Event> &, uint32_t, uint32_t *);
  };
  const Case cases[] = {
      {"static 0", 0, 1,
       [](const Audio &a, float e, const std::vector<Event> &v, uint32_t p, uint32_t *l) {
         return render_delay<StaticDelay<0>>(a, e, v, p, l);
       }},
      {"static 71", 71, 1,
       [](const Audio &a, float e, const std::vector<Event> &v, uint32_t p, uint32_t *l) {
         return render_delay<StaticDelay<71>>(a, e, v, p, l);
       }},
      {"prepared 3", 3, 3,
       [](const Audio &a, float e, const std::vector<Event> &v, uint32_t p, uint32_t *l) {
         return render_delay<PreparedDelay>(a, e, v, p, l);
       }},
      {"prepared 0", 0, 3,
       [](const Audio &a, float e, const std::vector<Event> &v, uint32_t p, uint32_t *l) {
         return render_delay<PreparedDelay>(a, e, v, p, l, 0);
       }},
      {"prepared 7", 7, 3,
       [](const Audio &a, float e, const std::vector<Event> &v, uint32_t p, uint32_t *l) {
         return render_delay<PreparedDelay>(a, e, v, p, l, 7);
       }},
  };
  for (const Case &c : cases) {
    kit_test::current_test = c.name;
    uint32_t latency = 99;
    for (uint32_t partition : {1u, 64u, 512u}) {
      CHECK(
          same_bits(c.render(special, 0, {}, partition, &latency), delayed(special, c.latency, 1)));
      CHECK(latency == c.latency);
      CHECK(same_bits(c.render(clean, 1, {}, partition, nullptr), delayed(clean, c.latency, 1.5f)));
    }

    const uint32_t bypass = c.bypass;
    const uint32_t fade = 480;
    const uint32_t on = 300;
    const uint32_t off = 1200;
    const Audio dry = delayed(clean, c.latency, 1);
    const Audio wet = delayed(clean, c.latency, 1.5f);
    for (uint32_t partition : {1u, 7u, 512u}) {
      const Audio output =
          c.render(clean, 0, {{on, bypass, 1.0f}, {off, bypass, 0.2f}}, partition, nullptr);
      CHECK(!output.empty());
      for (size_t ch = 0; ch < 2 && !output.empty(); ++ch) {
        for (uint32_t f = 0; f < 2000; ++f) {
          const float sample = output[ch][f];
          if (f < on) {
            CHECK(same_bits(sample, dry[ch][f]));
          } else if (f >= on && f < on + fade - 1) {
            const double mix = static_cast<double>(f - on + 1) / fade;
            const auto expected = static_cast<float>(dry[ch][f] * (1.0 - mix) + wet[ch][f] * mix);
            CHECK(same_bits(sample, expected));
            CHECK(!same_bits(sample, wet[ch][f]));
          } else if (f >= on + fade - 1 && f < off) {
            CHECK(same_bits(sample, wet[ch][f]));
          } else if (f >= off && f < off + fade - 1) {
            CHECK(!same_bits(sample, dry[ch][f]) && !same_bits(sample, wet[ch][f]));
          } else if (f >= off + fade - 1) {
            CHECK(same_bits(sample, dry[ch][f]));
          }
        }
      }
    }
  }
  kit_test::current_test = "exact bypass";

  Instance module(M<StaticDelay<4>>::api);
  CHECK(M<StaticDelay<4>>::parameter_api.set_value(module.get(), 1, 1) == VS_OK);
  CHECK(module.prepare(48000, 16, 2) == VS_OK);
  Audio ones(2, std::vector<float>(16, 1.0f));
  Audio output(2, std::vector<float>(16));
  CHECK(module.process(ones, output, 0, 8) == VS_OK);
  CHECK(module.process(ones, output, 0, 8, {}, VS_PROCESS_DISCONTINUITY) == VS_OK);
  CHECK(output[0][3] == 0.0f && output[0][4] == 1.5f);
  M<StaticDelay<4>>::api.reset(module.get());
  CHECK(module.process(ones, output, 0, 8) == VS_OK);
  CHECK(output[1][3] == 0.0f && output[1][4] == 1.5f);
}

void test_custom_bypass() {
  Instance module(M<CustomBypass>::api);
  CHECK(module.prepare(48000, 64, 1) == VS_OK);
  Audio input(1, std::vector<float>(64, 1.0f));
  Audio output(1, std::vector<float>(64));
  CHECK(module.process(input, output, 0, 64, {{7, 20, 0.0f}, {3, 30, 1.0f}, {7, 40, 1.0f}}) ==
        VS_OK);
  for (uint32_t f = 0; f < 64; ++f) {
    CHECK(output[0][f] == (f < 20 || f >= 40 ? 2.0f : 0.5f));
  }
}

void test_sanitising() {
  Instance module(M<Plain>::api);
  CHECK(module.prepare(48000, 16, 2) == VS_OK);
  Audio input(2, std::vector<float>(16, 0.5f));
  input[0][1] = nan;
  input[0][2] = infinity;
  input[1][3] = -infinity;
  input[1][4] = -0.0f;
  input[1][5] = 1e38f;
  Audio output(2, std::vector<float>(16, 9.0f));
  CHECK(module.process(input, output, 0, 16) == VS_OK);
  CHECK(output[0][0] == 0.5f && same_bits(output[0][1], 0.0f) && same_bits(output[0][2], 0.0f));
  CHECK(same_bits(output[1][3], 0.0f) && same_bits(output[1][4], -0.0f) && output[1][5] == 1e38f);

  Instance block(M<PreparedDelay>::api);
  CHECK(M<PreparedDelay>::parameter_api.set_value(block.get(), 2, 0) == VS_OK);
  CHECK(M<PreparedDelay>::parameter_api.set_value(block.get(), 3, 1) == VS_OK);
  CHECK(M<PreparedDelay>::parameter_api.set_value(block.get(), 1, 1) == VS_OK);
  CHECK(block.prepare(48000, 16, 2) == VS_OK);
  CHECK(block.process(input, output, 0, 16) == VS_OK);
  CHECK(output[0][0] == 0.5f && same_bits(output[0][1], 0.0f) && same_bits(output[0][2], 0.0f));
  CHECK(same_bits(output[1][3], 0.0f) && output[1][5] == 1e38f);

  Instance verbatim(M<Verbatim>::api);
  CHECK(verbatim.prepare(48000, 16, 2) == VS_OK);
  input[0][6] = std::numeric_limits<float>::denorm_min();
  input[0][7] = std::numeric_limits<float>::signaling_NaN();
  CHECK(verbatim.process(input, output, 0, 16) == VS_OK);
  for (size_t c = 0; c < 2; ++c) {
    for (size_t f = 0; f < 16; ++f) {
      CHECK(same_bits(output[c][f], input[c][f]));
    }
  }
}

void test_state() {
  const auto &state = M<SmoothFrame>::state_api;
  const auto &values = M<SmoothFrame>::parameter_api;
  Instance module(M<SmoothFrame>::api);
  uint64_t size = 0;
  CHECK(state.size(module.get(), &size) == VS_OK && size == 16);
  CHECK(state.size(nullptr, &size) == VS_ERROR_ARGUMENT);
  CHECK(state.size(module.get(), nullptr) == VS_ERROR_ARGUMENT);
  CHECK(values.set_value(module.get(), 2, 0.25f) == VS_OK);
  CHECK(values.set_value(module.get(), 4, 2.0f) == VS_OK);
  std::array<uint8_t, 20> bytes{};
  uint64_t written = 0;
  CHECK(state.save(module.get(), bytes.data(), 15, &written) == VS_ERROR_ARGUMENT);
  CHECK(state.save(module.get(), nullptr, 16, &written) == VS_ERROR_ARGUMENT);
  CHECK(state.save(module.get(), bytes.data(), 16, nullptr) == VS_ERROR_ARGUMENT);
  CHECK(state.save(nullptr, bytes.data(), 16, &written) == VS_ERROR_ARGUMENT);
  CHECK(state.save(module.get(), bytes.data(), 20, &written) == VS_OK && written == 16);
  const std::array<uint8_t, 16> expected{0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x80, 0x3e,
                                         0x00, 0x00, 0x00, 0x3f, 0x00, 0x00, 0x00, 0x40};
  CHECK(std::equal(expected.begin(), expected.end(), bytes.begin()));

  Instance copy(M<SmoothFrame>::api);
  CHECK(state.load(copy.get(), 1, bytes.data(), 16) == VS_OK);
  float value = 0;
  CHECK(values.get_value(copy.get(), 2, &value) == VS_OK && value == 0.25f);
  CHECK(values.get_value(copy.get(), 4, &value) == VS_OK && value == 2.0f);

  auto rejected = [&](uint32_t schema, const uint8_t *data, uint64_t count) {
    Instance target(M<SmoothFrame>::api);
    const vs_result result = state.load(target.get(), schema, data, count);
    std::array<uint8_t, 16> after{};
    state.save(target.get(), after.data(), 16, &written);
    const std::array<uint8_t, 16> defaults{0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x00, 0x3f,
                                           0x00, 0x00, 0x00, 0x3f, 0x00, 0x00, 0x00, 0x00};
    CHECK(after == defaults);
    return result;
  };
  CHECK(rejected(2, bytes.data(), 16) == VS_ERROR_UNSUPPORTED);
  CHECK(rejected(0, bytes.data(), 16) == VS_ERROR_UNSUPPORTED);
  CHECK(rejected(1, bytes.data(), 15) == VS_ERROR_ARGUMENT);
  CHECK(rejected(1, bytes.data(), 17) == VS_ERROR_ARGUMENT);
  CHECK(rejected(1, nullptr, 16) == VS_ERROR_ARGUMENT);
  CHECK(state.load(nullptr, 1, bytes.data(), 16) == VS_ERROR_ARGUMENT);
  for (float bad : {nan, infinity, 1.5f, -0.5f}) {
    auto corrupt = bytes;
    kit::store_binary32(bad, corrupt.data() + 8);
    CHECK(rejected(1, corrupt.data(), 16) == VS_ERROR_ARGUMENT);
  }
  auto choice = bytes;
  kit::store_binary32(3.0f, choice.data() + 12);
  CHECK(rejected(1, choice.data(), 16) == VS_ERROR_ARGUMENT);

  CHECK(copy.prepare(48000, 16, 1) == VS_OK);
  CHECK(state.load(copy.get(), 1, bytes.data(), 16) == VS_ERROR_STATE);
  CHECK(state.save(copy.get(), bytes.data(), 16, &written) == VS_OK);
}

void test_state_extra_and_migration() {
  const auto &state = M<Stateful>::state_api;
  const auto &values = M<Stateful>::parameter_api;
  Instance module(M<Stateful>::api);
  uint64_t size = 0;
  CHECK(state.size(module.get(), &size) == VS_OK && size == 9);
  std::array<uint8_t, 9> bytes{};
  uint64_t written = 0;
  CHECK(state.save(module.get(), bytes.data(), 9, &written) == VS_OK && written == 9);
  CHECK(bytes[8] == 1 && kit::load_binary32(bytes.data() + 4) == 2.0f);

  bytes[8] = 3;
  kit::store_binary32(-7.5f, bytes.data() + 4);
  Instance copy(M<Stateful>::api);
  CHECK(state.load(copy.get(), 2, bytes.data(), 9) == VS_OK);
  float value = 0;
  CHECK(values.get_value(copy.get(), 2, &value) == VS_OK && value == -7.5f);
  CHECK(copy.prepare(48000, 4, 1) == VS_OK);
  Audio input(1, std::vector<float>(4)), output(1, std::vector<float>(4));
  CHECK(copy.process(input, output, 0, 4) == VS_OK && output[0][0] == 3.0f);

  auto unchanged = [&](uint32_t schema, const uint8_t *data, uint64_t count) {
    Instance target(M<Stateful>::api);
    const vs_result result = state.load(target.get(), schema, data, count);
    std::array<uint8_t, 9> after{};
    state.save(target.get(), after.data(), 9, &written);
    CHECK(after[8] == 1 && kit::load_binary32(after.data() + 4) == 2.0f);
    return result;
  };
  auto bad_extra = bytes;
  bad_extra[8] = 4;
  CHECK(unchanged(2, bad_extra.data(), 9) == VS_ERROR_ARGUMENT);
  CHECK(unchanged(2, bytes.data(), 8) == VS_ERROR_ARGUMENT);
  CHECK(unchanged(2, bytes.data(), 7) == VS_ERROR_ARGUMENT);
  auto bad_value = bytes;
  kit::store_binary32(10.5f, bad_value.data() + 4);
  CHECK(unchanged(2, bad_value.data(), 9) == VS_ERROR_ARGUMENT);
  CHECK(unchanged(3, bytes.data(), 9) == VS_ERROR_UNSUPPORTED);
  CHECK(unchanged(1, bytes.data(), 9) == VS_ERROR_ARGUMENT);
  CHECK(unchanged(1, bad_value.data(), 8) == VS_ERROR_ARGUMENT);

  Instance migrated(M<Stateful>::api);
  CHECK(state.load(migrated.get(), 1, bytes.data(), 8) == VS_OK);
  CHECK(values.get_value(migrated.get(), 2, &value) == VS_OK && value == -7.5f);
  CHECK(state.save(migrated.get(), bytes.data(), 9, &written) == VS_OK && bytes[8] == 0);
}

void test_tempo() {
  float in[4]{}, out[4]{};
  const float *ins[] = {in};
  float *outs[] = {out};
  vs_process_block block = make_block(ins, outs, 1, 4);
  block.tempo = reinterpret_cast<const vs_tempo_context *>(uintptr_t{1});
  Instance plain(M<Plain>::api);
  CHECK(plain.prepare(48000, 4, 1) == VS_OK);
  CHECK(M<Plain>::api.process(plain.get(), &block) == VS_OK);

  Instance tempo(M<Tempo>::api);
  CHECK(tempo.prepare(48000, 4, 1) == VS_OK);
  vs_tempo_context context{sizeof(context), VS_TEMPO_VALID, 133.0, 2.0};
  block.tempo = &context;
  CHECK(M<Tempo>::api.process(tempo.get(), &block) == VS_OK && out[3] == 133.0f);
  block.tempo = nullptr;
  CHECK(M<Tempo>::api.process(tempo.get(), &block) == VS_OK && out[3] == -1.0f);
  block.tempo = &context;
  block.struct_size = VS_PROCESS_BLOCK_V1_SIZE;
  CHECK(M<Tempo>::api.process(tempo.get(), &block) == VS_OK && out[3] == -1.0f);
  block.struct_size = sizeof(block);
  context.struct_size = sizeof(context) - 1;
  CHECK(M<Tempo>::api.process(tempo.get(), &block) == VS_ERROR_ARGUMENT);
}

void test_midi() {
  float in[8]{}, out[8]{};
  const float *ins[] = {in};
  float *outs[] = {out};
  vs_process_block block = make_block(ins, outs, 1, 8);
  struct Wide {
    vs_midi_event event;
    uint32_t appended;
  };
  const Wide midi[] = {{{0, 0x90, 60, 100, 0}, 0},
                       {{3, 0x90, 61, 100, 0}, 0},
                       {{3, 0x80, 62, 0, 0}, 0},
                       {{7, 0xb0, 63, 1, 0}, 0}};
  const vs_parameter_event events[] = {{1, 3, 0.5f}, {1, 5, 0.25f}};
  block.events = events;
  block.event_count = 2;
  block.midi_events = midi;
  block.midi_event_count = 4;
  block.midi_event_size = sizeof(Wide);
  block.flags = VS_PROCESS_MIDI_PANIC;

  Instance plain(M<Plain>::api);
  CHECK(plain.prepare(48000, 8, 1) == VS_OK);
  vs_process_block ignored = block;
  ignored.events = nullptr;
  ignored.event_count = 0;
  ignored.midi_events = nullptr;
  CHECK(M<Plain>::api.process(plain.get(), &ignored) == VS_OK);

  Instance module(M<Midi>::api);
  CHECK(module.prepare(48000, 8, 1) == VS_OK);
  Midi::log.clear();
  CHECK(M<Midi>::api.process(module.get(), &block) == VS_OK);
  const std::vector<std::array<uint32_t, 3>> expected = {{'!', 0, 0},  {'m', 0, 60}, {'p', 3, 50},
                                                         {'m', 3, 61}, {'m', 3, 62}, {'p', 5, 25},
                                                         {'m', 7, 63}};
  CHECK(Midi::log.size() == expected.size());
  for (size_t i = 0; i < std::min(Midi::log.size(), expected.size()); ++i) {
    CHECK(uint32_t(Midi::log[i].kind) == expected[i][0] && Midi::log[i].frame == expected[i][1] &&
          Midi::log[i].value == expected[i][2]);
  }

  auto rejects = [&](auto change) {
    vs_process_block bad = block;
    Wide copy[4];
    std::memcpy(copy, midi, sizeof(copy));
    bad.midi_events = copy;
    change(bad, copy);
    return M<Midi>::api.process(module.get(), &bad) == VS_ERROR_ARGUMENT;
  };
  CHECK(rejects([](vs_process_block &b, Wide *) { b.midi_events = nullptr; }));
  CHECK(rejects([](vs_process_block &b, Wide *) { b.midi_event_size = 7; }));
  CHECK(rejects([](vs_process_block &, Wide *w) { w[1].event.frame_offset = 8; }));
  CHECK(rejects([](vs_process_block &, Wide *w) { w[2].event.frame_offset = 2; }));
  CHECK(rejects([](vs_process_block &, Wide *w) { w[0].event.status = 0xf0; }));
  CHECK(rejects([](vs_process_block &, Wide *w) { w[0].event.data1 = 128; }));
  CHECK(rejects([](vs_process_block &, Wide *w) { w[0].event.reserved = 1; }));
  vs_process_block v1 = block;
  v1.struct_size = VS_PROCESS_BLOCK_V1_SIZE;
  v1.midi_events = nullptr;
  v1.flags = 0;
  Midi::log.clear();
  CHECK(M<Midi>::api.process(module.get(), &v1) == VS_OK);
  CHECK(Midi::log.size() == 2 && Midi::log[0].kind == 'p');
}

}

int main() {
  const std::pair<const char *, void (*)()> tests[] = {
      {"descriptors", test_descriptors},
      {"metadata", test_metadata},
      {"scale", test_scale},
      {"controls", test_controls},
      {"lifecycle", test_lifecycle},
      {"prepare rejections", test_prepare_rejections},
      {"process rejections", test_process_rejections},
      {"parameter values", test_parameter_values},
      {"event accuracy", test_event_accuracy},
      {"partitions and forms", test_partitions_and_forms},
      {"exact bypass", test_exact_bypass},
      {"custom bypass", test_custom_bypass},
      {"sanitising", test_sanitising},
      {"state", test_state},
      {"state extra and migration", test_state_extra_and_migration},
      {"tempo", test_tempo},
      {"midi", test_midi},
  };
  for (const auto &[name, test] : tests) {
    kit_test::current_test = name;
    test();
  }
  if (kit_test::failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", kit_test::failures);
    return 1;
  }
  return 0;
}
