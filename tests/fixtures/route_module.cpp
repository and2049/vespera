// Routing fixture for the audio bridge: a module whose rendering depends on its own route
// selector, so audio shows both where the host placed it and which route it rendered.
// Output is in * gain + offset + 0.25 * route + bias, with the input delayed by the route's
// latency. The bias is opaque state, not a parameter: saved state (schema 2) holds the parameter
// values followed by the bias as one little-endian binary32, and it defaults to 0.
// Parameter 1 is the bypass parameter, 4 selects among ROUTE_COUNT routes and 5 is a mix
// parameter the module itself ignores, as a branch route requires. Compile definitions choose the
// module ID, the route count, each route's latency and a route whose preparation fails.
#include <vespera/kit/module.hpp>

#include <array>
#include <cmath>
#include <span>
#include <stdexcept>

#ifndef ROUTE_ID
#define ROUTE_ID "test.route"
#endif
#ifndef ROUTE_COUNT
#define ROUTE_COUNT 2
#endif
#ifndef ROUTE_LATENCY_0
#define ROUTE_LATENCY_0 0
#endif
#ifndef ROUTE_LATENCY_1
#define ROUTE_LATENCY_1 0
#endif
#ifndef ROUTE_FAILS
#define ROUTE_FAILS -1
#endif

namespace {

namespace kit = vespera::kit;

constexpr size_t max_delay = 64;
static_assert(ROUTE_LATENCY_0 < max_delay && ROUTE_LATENCY_1 < max_delay);

struct Route {
  static constexpr const char *id = ROUTE_ID;
  static constexpr const char *name = "Test Route";
  static constexpr const char *version = "0.1.0";
  static constexpr uint32_t layouts = VS_LAYOUT_MONO | VS_LAYOUT_STEREO;
  static constexpr auto parameters = kit::parameters(
      kit::toggle(1, "Enabled", true).bypass(), kit::knob(2, "Gain").range(0, 4).initial(1),
      kit::knob(3, "Offset", 0), kit::choice(4, "Route", ROUTE_COUNT), kit::knob(5, "Mix", 0.5f));
  static constexpr uint32_t state_schema = 2;

  struct Extra {
    float bias = 0.0f;
    uint64_t size() const noexcept { return kit::binary32_bytes; }
    void save(std::span<uint8_t> bytes) const noexcept { kit::store_binary32(bias, bytes.data()); }
    bool load(std::span<const uint8_t> bytes) {
      if (bytes.size() != kit::binary32_bytes || !std::isfinite(kit::load_binary32(bytes.data()))) {
        return false;
      }
      bias = kit::load_binary32(bytes.data());
      return true;
    }
  };

  struct Processor {
    static constexpr uint64_t tail_frames = 0;
    std::array<float, 5> values{};
    uint32_t route = 0;
    uint32_t delay = 0;
    float bias = 0.0f;
    std::array<kit::Frame, max_delay> line{};
    size_t position = 0;

    Processor(const kit::Setup &setup, const Extra &extra) : bias(extra.bias) {
      std::copy(setup.values.begin(), setup.values.end(), values.begin());
      route = static_cast<uint32_t>(std::lround(values[3]));
      if (static_cast<int>(route) == ROUTE_FAILS) {
        throw std::runtime_error("route fails to prepare");
      }
      delay = route == 0 ? ROUTE_LATENCY_0 : route == 1 ? ROUTE_LATENCY_1 : 0;
    }
    uint32_t latency_frames() const noexcept { return delay; }
    void reset() noexcept {
      line.fill({});
      position = 0;
    }
    void set_parameter(size_t index, float value) noexcept { values[index] = value; }
    kit::Frame process_frame(const kit::Frame &in) noexcept {
      kit::Frame source = in;
      if (delay) {
        source = line[position];
        line[position] = in;
        position = (position + 1) % delay;
      }
      const float shift = 0.25f * static_cast<float>(std::lround(values[3]));
      return {source[0] * values[1] + values[2] + shift + bias,
              source[1] * values[1] + values[2] + shift + bias};
    }
  };
};

}

VESPERA_KIT_MODULE(Route)
