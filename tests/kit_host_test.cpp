// kit::Host, kit::Derived, kit::crossfade and kit::shared against the runtime's host services in
// all three modes and without services.
#include "kit_harness.hpp"

#include <vespera/host_services.hpp>
#include <vespera/kit/module.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

namespace kit = vespera::kit;
namespace fs = std::filesystem;
using kit_test::Audio;
using kit_test::Event;
using kit_test::same_bits;
using kit_test::signal;
using vespera::HostServices;
using vespera::HostWorkMode;
using Call = HostServices::Client::Call;
using Scope = HostServices::Client::Scope;

std::atomic<int> builds{0};

// A waveshaper whose transfer table is rebuilt from the Shape parameter. Shapes above 0.9 fail to
// build, so that a failing build can be observed.
struct Curve {
  std::array<float, 257> points{};
  explicit Curve(float shape) {
    if (shape > 0.9f) {
      throw std::runtime_error("shape is out of reach");
    }
    const double drive = 1.0 + 9.0 * shape;
    for (size_t i = 0; i < points.size(); ++i) {
      const double x = static_cast<double>(i) / 128.0 - 1.0;
      points[i] = static_cast<float>(std::tanh(drive * x) / std::tanh(drive));
    }
    ++builds;
  }
  float operator()(float x) const noexcept {
    const float position = (std::clamp(x, -1.0f, 1.0f) + 1.0f) * 128.0f;
    const auto index = std::min<size_t>(static_cast<size_t>(position), 255);
    const float fraction = position - static_cast<float>(index);
    return points[index] + (points[index + 1] - points[index]) * fraction;
  }
};

struct Shaper {
  static constexpr const char *id = "test.kit.shaper";
  static constexpr const char *name = "Shaper";
  static constexpr const char *version = "1.0.0";
  static constexpr uint32_t layouts = VS_LAYOUT_MONO | VS_LAYOUT_STEREO;
  static constexpr auto parameters =
      kit::parameters(kit::toggle(1, "Enabled", true).bypass(), kit::knob(2, "Shape", 0.3f));

  struct Processor {
    static constexpr uint32_t latency_frames = 0;
    static constexpr uint64_t tail_frames = 0;
    kit::Derived<Curve, float> curve;

    Processor(const kit::Setup &setup, kit::Host &host)
        : curve(host.derive<Curve>(setup.values[1], [](float shape) { return Curve(shape); })) {}
    void reset() noexcept {}
    void set_parameter(size_t index, float value) noexcept {
      if (index == 1) {
        curve.request(value);
      }
    }
    kit::Frame process_frame(const kit::Frame &in) noexcept {
      const Curve &c = *curve.current();
      return {c(in[0]), c(in[1])};
    }
  };
};

using ShaperModule = kit::Module<Shaper>;

// A block processor whose gain value crossfades, so the mix can be checked exactly.
struct Fader {
  static constexpr const char *id = "test.kit.fader";
  static constexpr const char *name = "Fader";
  static constexpr const char *version = "1.0.0";
  static constexpr uint32_t layouts = VS_LAYOUT_MONO | VS_LAYOUT_STEREO;
  static constexpr auto parameters = kit::parameters(kit::knob(1, "Gain", 0.25f));

  struct Gain {
    float value;
  };

  struct Processor {
    static constexpr uint32_t latency_frames = 0;
    static constexpr uint64_t tail_frames = 0;
    kit::Derived<Gain, float> gain;

    Processor(const kit::Setup &setup, kit::Host &host)
        : gain(host.derive<Gain>(setup.values[0], [](float g, kit::Job &) { return Gain{g}; })) {}
    void reset() noexcept {}
    void set_parameter(size_t, float value) noexcept { gain.request(value); }
    void process(kit::Block &block) noexcept {
      kit::crossfade(gain, block, [](Gain &g, kit::Block &span) {
        for (uint32_t c = 0; c < span.channels; ++c) {
          for (uint32_t i = 0; i < span.frames; ++i) {
            span.outputs[c][i] = span.inputs[c][i] * g.value;
          }
        }
      });
    }
  };
};

using FaderModule = kit::Module<Fader>;

struct Cached {
  static constexpr const char *id = "test.kit.cached";
  static constexpr const char *name = "Cached";
  static constexpr const char *version = "1.0.0";
  static constexpr uint32_t layouts = VS_LAYOUT_STEREO;
  static constexpr auto parameters = kit::parameters(kit::knob(1, "Input", 0));
  struct Processor {
    static constexpr uint32_t latency_frames = 0;
    static constexpr uint64_t tail_frames = 0;
    kit::Derived<int, float> value;
    Processor(const kit::Setup &, kit::Host &host)
        : value(host.derive<int>(0.0f, [](float input) {
            return input == 0 ? 0 : *kit::shared<int>("cancellation", [] { return 99; });
          })) {}
    void reset() noexcept {}
    void set_parameter(size_t, float input) noexcept { value.request(input); }
    kit::Frame process_frame(const kit::Frame &in) noexcept { return in; }
  };
};

// Reports a status and logs while it is constructed, and reads a bundled file chosen by a choice
// parameter, the way a module loads factory resources.
std::string last_resources;

struct Bundled {
  static constexpr const char *id = "test.kit.bundled";
  static constexpr const char *name = "Bundled";
  static constexpr const char *version = "1.0.0";
  static constexpr uint32_t layouts = VS_LAYOUT_MONO | VS_LAYOUT_STEREO;
  static constexpr const char *files[] = {"first.txt", "second.txt"};
  static constexpr auto parameters = kit::parameters(kit::choice(1, "File", files));

  struct Contents {
    float level = 0.0f;
  };

  struct Processor {
    static constexpr uint32_t latency_frames = 0;
    static constexpr uint64_t tail_frames = 0;
    kit::Derived<Contents, int> contents;

    Processor(const kit::Setup &setup, kit::Host &host)
        : contents(host.derive<Contents>(
              static_cast<int>(setup.values[0]), [directory = host.resources()](int index) {
                const fs::path path = directory / files[index];
                std::ifstream file(path);
                float level = 0.0f;
                if (!(file >> level)) {
                  throw std::runtime_error(path.filename().string() + " is unreadable");
                }
                return Contents{level};
              })) {
      last_resources = host.resources().string();
      host.report(kit::Status::warning, "a remark");
      host.log(kit::Level::info, "constructed");
    }
    void reset() noexcept {}
    void set_parameter(size_t, float value) noexcept { contents.request(static_cast<int>(value)); }
    kit::Frame process_frame(const kit::Frame &) noexcept {
      return {contents.current()->level, contents.current()->level};
    }
  };
};

using BundledModule = kit::Module<Bundled>;

// Drives a kit module as a host does, declaring each call to the attached services.
template <class Module> class Hosted {
public:
  explicit Hosted(std::shared_ptr<HostServices> services = nullptr, std::string resources = "",
                  bool decline_work = false)
      : services_(std::move(services)) {
    Module::api.create(&instance_);
    if (services_) {
      client_ = services_->client(nullptr, Module::api.id, std::move(resources));
      Scope scope(*client_, Call::attach);
      descriptor_ = *client_->descriptor();
      if (decline_work) {
        descriptor_.open_work = [](void *, const vs_work_desc *) -> vs_work * { return nullptr; };
      }
      attached_ = Module::services_api.attach(instance_, &descriptor_);
    }
  }
  ~Hosted() {
    release();
    Module::api.destroy(instance_);
  }

  vs_result attached() const { return attached_; }
  HostServices::Client *client() const { return client_.get(); }
  void *instance() const { return instance_; }

  vs_result prepare(uint32_t max_frames = 512, uint32_t channels = 2) {
    std::optional<Scope> scope;
    if (client_) {
      scope.emplace(*client_, Call::prepare);
    }
    vs_prepare_info info{sizeof(info), 48000, max_frames, channels};
    vs_module_timing timing{sizeof(timing), 0, 0};
    return Module::api.prepare(instance_, &info, &timing);
  }

  void release() {
    std::optional<Scope> scope;
    if (client_) {
      scope.emplace(*client_, Call::release);
    }
    Module::api.release(instance_);
    if (client_) {
      client_->close_all();
    }
  }

  void set_value(uint32_t id, float value) {
    std::optional<Scope> scope;
    if (client_) {
      scope.emplace(*client_, Call::control);
    }
    Module::parameter_api.set_value(instance_, id, value);
  }

  vs_module_status status() const {
    vs_module_status status{};
    status.struct_size = sizeof(status);
    Module::services_api.status(instance_, &status);
    return status;
  }

  vs_result process(const Audio &input, Audio &output, uint64_t begin, uint32_t frames,
                    const std::vector<vs_parameter_event> &events = {},
                    uint32_t flags = VS_PROCESS_OFFLINE) {
    std::optional<Scope> scope;
    if (client_) {
      scope.emplace(*client_, Call::process);
    }
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
    return Module::api.process(instance_, &block);
  }

  // Blocks of partition frames, or of pseudo-random sizes when partition is 0.
  Audio render(const Audio &input, const std::vector<Event> &events, uint32_t partition) {
    Audio output(input.size(), std::vector<float>(input[0].size()));
    const uint64_t total = input[0].size();
    size_t next = 0;
    uint32_t seed = 7;
    for (uint64_t begin = 0; begin < total;) {
      seed = seed * 1664525u + 1013904223u;
      const uint32_t size = partition ? partition : 1 + (seed >> 8) % 300;
      const auto frames = static_cast<uint32_t>(std::min<uint64_t>(size, total - begin));
      std::vector<vs_parameter_event> block_events;
      while (next < events.size() && events[next].frame < begin + frames) {
        block_events.push_back({events[next].id, static_cast<uint32_t>(events[next].frame - begin),
                                events[next].value});
        ++next;
      }
      if (process(input, output, begin, frames, block_events) != VS_OK) {
        return {};
      }
      begin += frames;
    }
    return output;
  }

private:
  std::shared_ptr<HostServices> services_;
  std::shared_ptr<HostServices::Client> client_;
  vs_host_services descriptor_{};
  void *instance_ = nullptr;
  vs_result attached_ = VS_OK;
};

std::shared_ptr<HostServices> make(HostWorkMode mode) {
  HostServices::Options options;
  options.mode = mode;
  return HostServices::create(options);
}

float shaped(float shape, float x) { return Curve(shape)(x); }

void test_exports() {
  kit_test::current_test = "exports";
  CHECK(ShaperModule::services_api.struct_size == sizeof(vs_module_services_api));
  CHECK(ShaperModule::services_api.abi_version == VS_MODULE_SERVICES_ABI_VERSION);
  auto services = make(HostWorkMode::manual);
  Hosted<ShaperModule> hosted(services);
  CHECK(hosted.attached() == VS_OK);
  CHECK(ShaperModule::services_api.attach(hosted.instance(), hosted.client()->descriptor()) ==
        VS_ERROR_STATE);
  void *other = nullptr;
  ShaperModule::api.create(&other);
  vs_host_services broken = *hosted.client()->descriptor();
  broken.retire = nullptr;
  CHECK(ShaperModule::services_api.attach(other, &broken) == VS_ERROR_ARGUMENT);
  broken = *hosted.client()->descriptor();
  broken.abi_version = 2;
  CHECK(ShaperModule::services_api.attach(other, &broken) == VS_ERROR_ARGUMENT);
  CHECK(ShaperModule::services_api.attach(other, nullptr) == VS_ERROR_ARGUMENT);
  ShaperModule::api.destroy(other);
  const auto status = hosted.status();
  CHECK(status.kind == VS_STATUS_READY && status.message[0] == '\0' && status.progress < 0);
  CHECK(ShaperModule::services_api.status(hosted.instance(), nullptr) == VS_ERROR_ARGUMENT);
}

// Each change requests a build; offline the result is adopted at the change's frame, so every
// partition renders the same bits, with or without services.
void test_inline_partitions() {
  kit_test::current_test = "inline partitions";
  const Audio input = signal(2, 4096, 3);
  std::vector<Event> events;
  for (uint64_t frame = 5; frame < 4096; frame += 397) {
    events.push_back({frame, 2, static_cast<float>(frame % 9) / 10.0f});
  }
  std::optional<Audio> reference;
  for (const bool with_services : {false, true}) {
    for (const uint32_t partition : {1u, 7u, 64u, 512u, 0u}) {
      auto services = with_services ? make(HostWorkMode::synchronous) : nullptr;
      Hosted<ShaperModule> hosted(services);
      CHECK(hosted.prepare() == VS_OK);
      builds = 0;
      const Audio output = hosted.render(input, events, partition);
      CHECK(!output.empty());
      CHECK(builds == static_cast<int>(events.size()));
      if (!reference) {
        reference = output;
      } else {
        CHECK(same_bits(*reference, output));
      }
    }
  }
  const auto &out = *reference;
  CHECK(same_bits(out[0][4], shaped(0.3f, input[0][4])));
  CHECK(same_bits(out[0][5], shaped(0.5f, input[0][5])));
  CHECK(same_bits(out[1][401], shaped(0.5f, input[1][401])));
  CHECK(same_bits(out[1][402], shaped(0.6f, input[1][402])));
}

void test_without_services_set_value() {
  kit_test::current_test = "no host";
  Hosted<ShaperModule> hosted;
  CHECK(hosted.prepare() == VS_OK);
  hosted.set_value(2, 0.7f);
  Audio input = signal(2, 64, 2);
  Audio output(2, std::vector<float>(64));
  const auto before = builds.load();
  CHECK(hosted.process(input, output, 0, 64, {{2, 0, 0.6f}}, 0) == VS_ERROR_UNSUPPORTED);
  CHECK(builds == before);
  CHECK(hosted.process(input, output, 0, 64) == VS_OK);
  CHECK(same_bits(output[0][0], shaped(0.7f, input[0][0])));

  Hosted<ShaperModule> declined(make(HostWorkMode::manual), "", true);
  CHECK(declined.prepare() == VS_OK);
  CHECK(declined.status().kind == VS_STATUS_ERROR);
  const auto prepared_builds = builds.load();
  CHECK(declined.process(input, output, 0, 64, {{2, 1, 0.2f}}, 0) == VS_ERROR_UNSUPPORTED);
  CHECK(builds == prepared_builds);
  CHECK(declined.process(input, output, 0, 64, {{2, 1, 0.2f}}) == VS_OK);
  CHECK(same_bits(output[0][1], shaped(0.2f, input[0][1])));
}

void test_timing_notification() {
  auto services = make(HostWorkMode::manual);
  auto client = services->client(nullptr, "test.kit.timing", "");
  kit::detail::Link link;
  CHECK(link.attach(client->descriptor()) == VS_OK);
  kit::Host host(link, kit::Setup{48000, 2, 512, {}});
  host.timing_changed();
  CHECK(client->take_notices() == VS_NOTIFY_TIMING);
}

// A live host builds later; audio keeps the old value until a block start after the build, then
// the gain crossfades linearly over 20 ms (960 frames at 48 kHz).
void test_live_adoption_and_crossfade() {
  kit_test::current_test = "live crossfade";
  auto services = make(HostWorkMode::manual);
  {
    Hosted<FaderModule> hosted(services);
    CHECK(hosted.prepare(256, 1) == VS_OK);
    Audio input(1, std::vector<float>(4096, 1.0f));
    Audio output(1, std::vector<float>(4096));
    CHECK(hosted.process(input, output, 0, 256, {{1, 100, 0.75f}}) == VS_OK);
    CHECK(output[0][255] == 0.25f);
    CHECK(services->run_pending() == 1);
    CHECK(hosted.process(input, output, 256, 256) == VS_OK);
    CHECK(output[0][256] == 0.25f);
    for (uint64_t begin = 512; begin < 2048; begin += 256) {
      CHECK(hosted.process(input, output, begin, 256) == VS_OK);
    }
    CHECK(std::abs(output[0][256 + 480] - 0.5f) < 1e-6f);
    CHECK(output[0][256 + 959] < 0.75f && output[0][256 + 960] == 0.75f);
    CHECK(services->statistics().adopted == 1);
    // The faded-out first value belongs to the processor, so nothing is handed back yet.
    CHECK(services->statistics().handed_back == 0);
    CHECK(hosted.process(input, output, 2048, 256, {{1, 0, 0.5f}}) == VS_OK);
    services->run_pending();
    for (uint64_t begin = 2304; begin < 4096; begin += 256) {
      CHECK(hosted.process(input, output, begin, 256) == VS_OK);
    }
    CHECK(output[0][4095] == 0.5f);
    CHECK(services->statistics().handed_back == 1);
  }
  services->run_pending();
  const auto stats = services->statistics();
  CHECK(stats.produced == 2 && stats.disposed == 2 && stats.opened == stats.closed);
  CHECK(stats.left_open == 0);
}

void test_release_with_pending_work() {
  kit_test::current_test = "release pending";
  auto services = make(HostWorkMode::manual);
  {
    Hosted<ShaperModule> hosted(services);
    CHECK(hosted.prepare() == VS_OK);
    hosted.set_value(2, 0.6f);
    hosted.release();
    CHECK(services->run_pending() == 0);
    CHECK(hosted.prepare() == VS_OK);
    hosted.set_value(2, 0.4f);
    CHECK(services->run_pending() == 1);
  }
  const auto stats = services->statistics();
  CHECK(stats.opened == 2 && stats.closed == 2 && stats.left_open == 0);
  CHECK(stats.produced == stats.disposed);
}

void test_crossfade_partitions() {
  kit_test::current_test = "crossfade partitions";
  const Audio input = signal(2, 8192, 17);
  const std::vector<Event> events{{13, 1, 0.7f}, {3001, 1, 0.1f}, {3999, 1, 0.9f}};
  std::optional<Audio> reference;
  for (uint32_t partition : {1u, 7u, 64u, 512u, 0u}) {
    Hosted<FaderModule> hosted(make(HostWorkMode::synchronous));
    CHECK(hosted.prepare() == VS_OK);
    const auto output = hosted.render(input, events, partition);
    if (!reference) {
      reference = output;
    } else {
      CHECK(same_bits(*reference, output));
    }
  }
}

void test_build_errors() {
  kit_test::current_test = "build errors";
  auto services = make(HostWorkMode::synchronous);
  Hosted<ShaperModule> hosted(services);
  CHECK(hosted.prepare() == VS_OK);
  hosted.client()->take_notices();
  hosted.set_value(2, 0.95f);
  auto status = hosted.status();
  CHECK(status.kind == VS_STATUS_ERROR && std::string(status.message) == "shape is out of reach");
  CHECK(hosted.client()->take_notices() == VS_NOTIFY_STATUS);
  Audio input = signal(2, 32, 4);
  Audio output(2, std::vector<float>(32));
  CHECK(hosted.process(input, output, 0, 32) == VS_OK);
  CHECK(same_bits(output[0][0], shaped(0.3f, input[0][0])));
  hosted.set_value(2, 0.5f);
  CHECK(hosted.status().kind == VS_STATUS_READY);
  CHECK(hosted.client()->take_notices() == VS_NOTIFY_STATUS);
  CHECK(hosted.process(input, output, 0, 32) == VS_OK);
  CHECK(same_bits(output[0][0], shaped(0.5f, input[0][0])));

  Hosted<ShaperModule> failing(services);
  failing.set_value(2, 0.95f);
  CHECK(failing.prepare() == VS_ERROR_PROCESS);
}

void test_resources_status_and_log() {
  kit_test::current_test = "resources";
  const fs::path directory =
      fs::temp_directory_path() / ("vespera-kit-host-test-" + std::to_string(getpid()));
  fs::create_directories(directory);
  std::ofstream(directory / "first.txt") << "0.25";
  std::ofstream(directory / "second.txt") << "0.5";
  auto services = make(HostWorkMode::synchronous);
  std::vector<vespera::ModuleLogLine> lines;
  services->set_log_sink([&](const vespera::ModuleLogLine &line) { lines.push_back(line); });
  {
    Hosted<BundledModule> hosted(services, directory.string());
    CHECK(hosted.prepare() == VS_OK);
    CHECK(last_resources == directory.string());
    auto status = hosted.status();
    CHECK(status.kind == VS_STATUS_WARNING && std::string(status.message) == "a remark");
    CHECK(lines.size() == 1 && lines[0].message == "constructed" &&
          lines[0].module == "test.kit.bundled");
    Audio input(2, std::vector<float>(16));
    Audio output(2, std::vector<float>(16));
    CHECK(hosted.process(input, output, 0, 16, {{1, 8, 1.0f}}) == VS_OK);
    CHECK(output[0][7] == 0.25f && output[0][8] == 0.5f);
    vs_module_status small{};
    small.struct_size = static_cast<uint32_t>(offsetof(vs_module_status, message) + 4);
    std::memset(small.message, 'x', sizeof(small.message));
    CHECK(BundledModule::services_api.status(hosted.instance(), &small) == VS_OK);
    CHECK(std::string(small.message) == "a r" && small.message[4] == 'x');
    fs::remove(directory / "first.txt");
    hosted.set_value(1, 0.0f);
    status = hosted.status();
    CHECK(status.kind == VS_STATUS_ERROR &&
          std::string(status.message) == "first.txt is unreadable");
  }
  fs::remove_all(directory);
}

// A processing thread renders while parameter changes request builds on two worker threads.
void test_threaded() {
  kit_test::current_test = "threaded";
  auto services = make(HostWorkMode::threaded);
  {
    Hosted<ShaperModule> hosted(services);
    CHECK(hosted.prepare(64) == VS_OK);
    Audio input = signal(2, 64, 5);
    Audio output(2, std::vector<float>(64));
    std::thread audio([&] {
      for (uint32_t block = 0; block < 4000; ++block) {
        const float shape = static_cast<float>(block % 90) / 100.0f;
        hosted.process(input, output, 0, 64, {{2, block % 64, shape}});
      }
    });
    audio.join();
    const HostServices::Client *clients[] = {hosted.client()};
    CHECK(services->wait_idle(clients, std::chrono::steady_clock::now() + std::chrono::seconds(5)));
    // A fade of 960 frames must finish before the newest value is adopted, then another fade.
    for (int i = 0; i < 40; ++i) {
      CHECK(hosted.process(input, output, 0, 64) == VS_OK);
    }
    CHECK(same_bits(output[0][10], shaped(static_cast<float>(3999 % 90) / 100.0f, input[0][10])));
  }
  const auto stats = services->statistics();
  CHECK(stats.produced == stats.disposed && stats.opened == stats.closed && stats.left_open == 0);
}

struct Plain {
  static constexpr const char *id = "test.kit.plain";
  static constexpr const char *name = "Plain";
  static constexpr const char *version = "1.0.0";
  static constexpr uint32_t layouts = VS_LAYOUT_STEREO;
  static constexpr auto parameters = kit::parameters(kit::knob(1, "Gain", 0.5f));
  struct Processor {
    static constexpr uint32_t latency_frames = 0;
    static constexpr uint64_t tail_frames = 0;
    float gain;
    explicit Processor(const kit::Setup &setup) : gain(setup.values[0]) {}
    void reset() noexcept {}
    void set_parameter(size_t, float value) noexcept { gain = value; }
    kit::Frame process_frame(const kit::Frame &in) noexcept { return {in[0] * gain, in[1] * gain}; }
  };
};

using PlainModule = kit::Module<Plain>;

// Existing modules without host features process identically with and without services.
void test_plain_modules_unchanged() {
  kit_test::current_test = "plain modules";
  const Audio input = signal(2, 2048, 9);
  const std::vector<Event> events{{100, 1, 0.75f}, {900, 1, 0.1f}};
  Hosted<PlainModule> plain;
  CHECK(plain.prepare() == VS_OK);
  auto services = make(HostWorkMode::threaded);
  Hosted<PlainModule> hosted(services);
  CHECK(hosted.prepare() == VS_OK);
  CHECK(same_bits(plain.render(input, events, 64), hosted.render(input, events, 64)));
  CHECK(services->statistics().opened == 0);
}

struct Table {
  int id;
  explicit Table(int i) : id(i) {}
};

std::chrono::steady_clock::time_point test_now{};
std::chrono::steady_clock::time_point fake_clock() { return test_now; }

void test_shared() {
  kit_test::current_test = "shared";
  kit::detail::shared_clock = fake_clock;
  int made = 0;
  auto make = [&] { return Table(++made); };
  auto first = kit::shared<Table>("a", make);
  auto second = kit::shared<Table>("a", make);
  CHECK(first == second && made == 1);
  {
    auto different_builder = kit::shared<Table>("a", [] { return Table(-1); });
    CHECK(different_builder == first);
  }
  auto other = kit::shared<Table>("b", make);
  CHECK(other->id == 2 && made == 2);
  const Table *address = first.get();
  first.reset();
  second.reset();
  test_now += std::chrono::seconds(29);
  auto again = kit::shared<Table>("a", make);
  CHECK(again.get() == address && made == 2);
  again.reset();
  test_now += std::chrono::seconds(31);
  auto rebuilt = kit::shared<Table>("a", make);
  CHECK(rebuilt->id == 3 && made == 3);

  // A second caller waits for the build in progress instead of building again.
  std::mutex mutex;
  std::condition_variable changed;
  bool building = false;
  bool release = false;
  std::atomic<int> slow_made{0};
  auto slow = [&] {
    std::unique_lock lock(mutex);
    building = true;
    changed.notify_all();
    changed.wait(lock, [&] { return release; });
    return Table(100 + ++slow_made);
  };
  std::shared_ptr<const Table> from_thread;
  std::thread builder([&] { from_thread = kit::shared<Table>("slow", slow); });
  {
    std::unique_lock lock(mutex);
    changed.wait(lock, [&] { return building; });
  }
  std::shared_ptr<const Table> waited;
  std::thread waiter([&] { waited = kit::shared<Table>("slow", slow); });
  {
    std::lock_guard lock(mutex);
    release = true;
    changed.notify_all();
  }
  builder.join();
  waiter.join();
  CHECK(from_thread == waited && slow_made == 1 && waited->id == 101);

  bool threw = false;
  try {
    kit::shared<Table>("fails", []() -> Table { throw std::runtime_error("no"); });
  } catch (const std::runtime_error &) {
    threw = true;
  }
  CHECK(threw && kit::shared<Table>("fails", make)->id == 4);
  kit::detail::shared_clock = std::chrono::steady_clock::now;
}

void test_cancel_while_waiting_for_a_shared_build() {
  kit_test::current_test = "cancel shared wait";
  std::mutex mutex;
  std::condition_variable changed;
  bool entered = false, finish = false;
  std::thread builder([&] {
    kit::shared<int>("cancellation", [&] {
      std::unique_lock lock(mutex);
      entered = true;
      changed.notify_all();
      changed.wait_until(lock, std::chrono::steady_clock::now() + std::chrono::seconds(5),
                         [&] { return finish; });
      return 42;
    });
  });
  {
    std::unique_lock lock(mutex);
    CHECK(changed.wait_until(lock, std::chrono::steady_clock::now() + std::chrono::seconds(5),
                             [&] { return entered; }));
  }
  auto services = make(HostWorkMode::manual);
  Hosted<kit::Module<Cached>> hosted(services);
  CHECK(hosted.prepare() == VS_OK);
  hosted.set_value(1, 1);
  std::thread worker([&] { services->run_one(); });
  CHECK(services->wait_running(std::chrono::steady_clock::now() + std::chrono::seconds(5)));
  const auto start = std::chrono::steady_clock::now();
  hosted.release();
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
  worker.join();
  {
    std::lock_guard lock(mutex);
    CHECK(!finish);
    finish = true;
    changed.notify_all();
  }
  builder.join();
  CHECK(services->statistics().closed == 1);
}

void test_shared_cache_bounds_and_ownership() {
  kit_test::current_test = "shared bounds and ownership";
  struct Object {
    std::shared_ptr<int> marker;
    std::shared_ptr<const Object> child;
  };
  std::array<std::weak_ptr<int>, 12> markers;
  {
    kit::detail::SharedCache<Object> cache;
    for (size_t i = 0; i < markers.size(); ++i) {
      auto marker = std::make_shared<int>(static_cast<int>(i));
      markers[i] = marker;
      cache.get(std::to_string(i), [marker] { return Object{marker, {}}; });
    }
    CHECK(std::count_if(markers.begin(), markers.end(),
                        [](const auto &marker) { return !marker.expired(); }) == 8);
    bool rejected = false;
    try {
      cache.get(std::string(1025, 'x'), [] { return Object{}; });
    } catch (const std::invalid_argument &) {
      rejected = true;
    }
    CHECK(rejected);
  }
  CHECK(std::all_of(markers.begin(), markers.end(), [](const auto &m) { return m.expired(); }));

  std::shared_ptr<const Object> survivor;
  std::weak_ptr<int> leaf_marker, parent_marker;
  {
    kit::detail::SharedCache<Object> cache;
    auto marker = std::make_shared<int>(1);
    leaf_marker = marker;
    auto leaf = cache.get("leaf", [marker] { return Object{marker, {}}; });
    marker = std::make_shared<int>(2);
    parent_marker = marker;
    survivor = cache.get("parent", [marker, leaf] { return Object{marker, leaf}; });
  }
  CHECK(!leaf_marker.expired() && !parent_marker.expired());
  survivor.reset();
  CHECK(leaf_marker.expired() && parent_marker.expired());
}

void test_cached_and_file_reference() {
  struct Value {
    int number;
  };
  CHECK(!kit::cached<Value>("missing"));
  auto value = kit::shared<Value>("ready", [] { return Value{42}; });
  CHECK(kit::cached<Value>("ready") == value);
  value.reset();
  CHECK(kit::cached<Value>("ready")->number == 42);
  std::mutex mutex;
  std::condition_variable changed;
  bool entered = false, finish = false;
  std::thread builder([&] {
    kit::shared<Value>("building", [&] {
      std::unique_lock lock(mutex);
      entered = true;
      changed.notify_all();
      CHECK(changed.wait_until(lock, std::chrono::steady_clock::now() + std::chrono::seconds(5),
                               [&] { return finish; }));
      return Value{7};
    });
  });
  {
    std::unique_lock lock(mutex);
    CHECK(changed.wait_until(lock, std::chrono::steady_clock::now() + std::chrono::seconds(5),
                             [&] { return entered; }));
  }
  CHECK(!kit::cached<Value>("building"));
  {
    std::lock_guard lock(mutex);
    finish = true;
    changed.notify_all();
  }
  builder.join();
  CHECK(kit::cached<Value>("building")->number == 7);
  kit::detail::Link link;
  link.declare_files({{1, "File", "wav", "", 100, true}});
  kit::Host host(link, kit::Setup{48000, 2, 512, {}});
  CHECK(host.file_reference(1).state == VS_FILE_NONE);
  link.set_file(1, {VS_FILE_MISSING, "", "missing.wav", 10, {}});
  CHECK(host.file_reference(1).name == "missing.wav");
  bool rejected = false;
  try {
    host.file_reference(2);
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  CHECK(rejected);
}

}

int main() {
  test_exports();
  test_inline_partitions();
  test_without_services_set_value();
  test_timing_notification();
  test_live_adoption_and_crossfade();
  test_crossfade_partitions();
  test_release_with_pending_work();
  test_build_errors();
  test_resources_status_and_log();
  test_threaded();
  test_plain_modules_unchanged();
  test_shared();
  test_cancel_while_waiting_for_a_shared_build();
  test_shared_cache_bounds_and_ownership();
  test_cached_and_file_reference();
  if (kit_test::failures != 0) {
    std::fprintf(stderr, "%d kit host check(s) failed\n", kit_test::failures);
    return 1;
  }
  std::puts("kit host tests passed");
  return 0;
}
