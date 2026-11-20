#include <vespera/kit/module.hpp>

namespace kit = vespera::kit;

static constexpr const char *modes[] = {"Clean", "Lead"};
static constexpr const char *blank[] = {"Clean", ""};

#if KIT_CASE == 0
constexpr auto table = kit::parameters(
    kit::toggle(7, "On").bypass(), kit::knob(2, "A"), kit::choice(3, "Mode", 2),
    kit::choice(4, "Voice", modes, 1), kit::knob(5, "Level").range(-24, 12).initial(0).decibels(),
    kit::knob(6, "Cut", 0).maps(20, 500).skew(0.6).hertz().off_at_minimum(0.5),
    kit::knob(8, "Mix").percent(), kit::knob(9, "Voices").range(1, 8).initial(2).steps(8),
    kit::knob(10, "Pan").shows(-50, 50).decimals(1).display_unit(" L/R").show_sign());
#elif KIT_CASE == 1
constexpr auto table = kit::parameters(kit::knob(0, "A"));
#elif KIT_CASE == 2
constexpr auto table = kit::parameters(kit::knob(1, "A"), kit::knob(1, "B"));
#elif KIT_CASE == 3
constexpr auto table = kit::parameters(kit::knob(1, ""));
#elif KIT_CASE == 4
constexpr auto table = kit::parameters(kit::knob(1, "A").range(1, 1));
#elif KIT_CASE == 5
constexpr auto table = kit::parameters(kit::knob(1, "A", 2));
#elif KIT_CASE == 6
constexpr auto table = kit::parameters(kit::knob(1, "A").skew(0));
#elif KIT_CASE == 7
constexpr auto table = kit::parameters(kit::knob(1, "A").smoothed(-1));
#elif KIT_CASE == 8
constexpr auto table = kit::parameters(kit::toggle(1, "A").bypass(), kit::toggle(2, "B").bypass());
#elif KIT_CASE == 9
constexpr auto table = kit::parameters(kit::knob(1, "A").range(0, 2).bypass());
#elif KIT_CASE == 13
constexpr auto table = kit::parameters(kit::knob(1, "A").steps(1));
#elif KIT_CASE == 14
constexpr auto table = kit::parameters(kit::knob(1, "A").range(0, 4).initial(0.5f).steps(5));
#elif KIT_CASE == 15
constexpr auto table = kit::parameters(kit::choice(1, "A", blank));
#elif KIT_CASE == 16
constexpr auto table = kit::parameters(kit::toggle(1, "A").decimals(1));
#elif KIT_CASE == 17
constexpr auto table = kit::parameters(kit::knob(1, "A").shows(1, 1));
#elif KIT_CASE == 18
constexpr auto table = kit::parameters(kit::knob(1, "A").skew(2000).percent());
#elif KIT_CASE == 19
constexpr auto table = kit::parameters(kit::knob(1, "A").decimals(7));
#elif KIT_CASE == 20
constexpr auto table = kit::parameters(kit::knob(1, "A").display_unit(
    "01234567890123456789012345678901234567890123456789012345678901234"));
#elif KIT_CASE == 21
constexpr auto table = kit::parameters(kit::knob(1, "A").off_at_minimum(-1));
#elif KIT_CASE == 22
constexpr auto table = kit::parameters(kit::toggle(1, "A").bypass().steps(3));
#else
constexpr auto table = kit::parameters(kit::knob(1, "A"));
#endif

struct Config {
  static constexpr const char *id = "test.kit.invalid";
  static constexpr const char *name = "Invalid";
  static constexpr const char *version = "1";
  static constexpr uint32_t layouts = VS_LAYOUT_MONO;
  static constexpr auto &parameters = table;
#if KIT_CASE == 10
  static constexpr auto bypass = kit::BypassPolicy::custom;
#endif
  struct Processor {
    static constexpr uint32_t latency_frames = 0;
#if KIT_CASE != 12
    static constexpr uint64_t tail_frames = 0;
#endif
    explicit Processor(const kit::Setup &) {}
    void reset() noexcept {}
#if KIT_CASE != 11
    kit::Frame process_frame(const kit::Frame &input) noexcept { return input; }
#endif
  };
};

VESPERA_KIT_MODULE(Config)
