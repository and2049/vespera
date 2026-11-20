#include <vespera/app_audio.h>
#include <vespera/audio_session.hpp>
#include <vespera/file_content.hpp>
#include <vespera/graph_loader.hpp>
#include <vespera/host_services.hpp>
#include <vespera/instance_cache.hpp>
#include <vespera/midi_input.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <os/log.h>
#include <pthread.h>
#endif

namespace {
using Json = nlohmann::json;
using namespace vespera;

CoreIoSettings core_settings(const Json &j) {
  CoreIoSettings result;
  result.input_gain_db = j.value("inputGain", 0.0f);
  result.output_gain_db = j.value("outputGain", 0.0f);
  result.gate_enabled = j.value("gateEnabled", false);
  result.gate_threshold_db = j.value("gateThreshold", -80.0f);
  result.transpose_semitones = j.value("transpose", 0.0f);
  result.doubler_enabled = j.value("doublerEnabled", false);
  result.doubler_delay_ms = j.value("doublerTime", 6.0f);
  if (!validate_core_io_settings(result)) {
    throw std::invalid_argument("Invalid global controls");
  }
  return result;
}

TempoSettings tempo_settings(const Json &j) {
  TempoSettings result;
  result.bpm = j.value("bpm", 120.0);
  result.metronome_enabled = j.value("enabled", false);
  result.metronome_level_db = j.value("level", -18.0f);
  if (!validate_tempo_settings(result)) {
    throw std::invalid_argument("Invalid tempo settings");
  }
  return result;
}

Json tempo_json(const TempoSettings &settings) {
  return {{"bpm", settings.bpm},
          {"enabled", settings.metronome_enabled},
          {"level", settings.metronome_level_db}};
}

// Matches the native preset encoding: an even number of lowercase hex digits.
ModuleState module_state(const Json &j) {
  const auto text = j.at("data").get<std::string>();
  if (text.size() % 2 != 0 || text.size() / 2 > max_module_state_bytes) {
    throw std::invalid_argument("Module state must be an even number of lowercase hex digits "
                                "within the state size limit");
  }
  const auto &schema = j.at("schema");
  if (!schema.is_number_unsigned() ||
      schema.get<uint64_t>() > std::numeric_limits<uint32_t>::max()) {
    throw std::invalid_argument("Module state schema must be an unsigned 32-bit integer");
  }
  ModuleState state;
  state.schema_version = schema.get<uint32_t>();
  state.bytes.reserve(text.size() / 2);
  for (size_t i = 0; i < text.size(); i += 2) {
    const int high = hex_digit(text[i]);
    const int low = hex_digit(text[i + 1]);
    if (high < 0 || low < 0) {
      throw std::invalid_argument("Module state must contain only lowercase hex digits");
    }
    state.bytes.push_back(static_cast<uint8_t>(high * 16 + low));
  }
  return state;
}

// Module and bridge log lines go to the unified log (subsystem dev.vespera, category the module
// id), and to stderr as well when the environment sets VESPERA_MODULE_LOG=1.
void log_line(const ModuleLogLine &line) {
#if defined(__APPLE__)
  static std::mutex mutex;
  static auto *logs = new std::map<std::string, os_log_t>;
  os_log_t log = nullptr;
  {
    std::lock_guard lock(mutex);
    auto &entry = (*logs)[line.module];
    if (!entry) {
      entry = os_log_create("dev.vespera", line.module.c_str());
    }
    log = entry;
  }
  static constexpr os_log_type_t types[] = {OS_LOG_TYPE_DEBUG, OS_LOG_TYPE_INFO,
                                            OS_LOG_TYPE_DEFAULT, OS_LOG_TYPE_ERROR};
  os_log_with_type(log, types[std::min<uint32_t>(line.level, VS_LOG_ERROR)], "%{public}s",
                   line.message.c_str());
#endif
  const char *enabled = std::getenv("VESPERA_MODULE_LOG");
  if (enabled && std::string_view(enabled) == "1") {
    static constexpr const char *levels[] = {"debug", "info", "warning", "error"};
    std::fprintf(stderr, "%s: %s: %s\n", line.module.c_str(),
                 levels[std::min<uint32_t>(line.level, VS_LOG_ERROR)], line.message.c_str());
  }
}

const char *status_name(uint32_t kind) {
  switch (kind) {
  case VS_STATUS_BUSY:
    return "busy";
  case VS_STATUS_WARNING:
    return "warning";
  default:
    return "error";
  }
}

// Adds one described module: its declared identity must match the binary, its opaque state (when
// described) is loaded first, and its parameter values are then applied on top. The identity
// check catches a binary replaced since the description was built; it does not compare a
// preset's saved module version, which the application reports separately. An optional
// "resources" names the package's resources directory, which the module receives through host
// services.
NodeId add_described(Graph &graph, const Json &description) {
  ModuleOptions options;
  if (description.contains("resources") && !description.at("resources").is_null()) {
    options.resource_directory = description.at("resources").get<std::string>();
  }
  const auto node = graph.add_module(description.at("binary").get<std::string>(), options);
  if (description.contains("module") || description.contains("version")) {
    for (const auto &metadata : graph.nodes()) {
      if (metadata.instance_id == node &&
          (description.value("module", metadata.module.id) != metadata.module.id ||
           description.value("version", metadata.module.version) != metadata.module.version)) {
        throw std::invalid_argument("Module " + metadata.module.id + " version " +
                                    metadata.module.version +
                                    " does not match its audio description");
      }
    }
  }
  if (description.contains("state") && !description.at("state").is_null()) {
    graph.load_module_state(node, module_state(description.at("state")));
  }
  if (description.contains("files")) {
    const auto &files = description.at("files");
    if (!files.is_array() || files.size() > max_file_slots) {
      throw std::invalid_argument("Invalid file references");
    }
    std::set<uint32_t> seen;
    for (const auto &item : files) {
      const auto slot = item.at("slot").get<uint32_t>();
      if (!seen.insert(slot).second) {
        throw std::invalid_argument("Repeated file slot");
      }
      FileReference file;
      file.state = VS_FILE_MISSING;
      file.path = item.at("path").get<std::string>();
      file.name = item.at("name").get<std::string>();
      file.bytes = item.at("bytes").get<uint64_t>();
      const auto hash = item.at("sha256").get<std::string>();
      const bool explicitly_missing = item.value("state", "present") == "missing";
      if (hash.size() != 64 ||
          (!explicitly_missing && !std::filesystem::path(file.path).is_absolute())) {
        throw std::invalid_argument("Invalid file hash or resolved path");
      }
      for (size_t i = 0; i < file.sha256.size(); ++i) {
        const int high = hex_digit(hash[i * 2]);
        const int low = hex_digit(hash[i * 2 + 1]);
        if (high < 0 || low < 0) {
          throw std::invalid_argument("Invalid file hash");
        }
        file.sha256[i] = static_cast<uint8_t>((high << 4) | low);
      }
      uint64_t limit = 0;
      for (const auto &metadata : graph.nodes()) {
        if (metadata.instance_id == node) {
          for (const auto &declared : metadata.module.file_slots) {
            if (declared.id == slot) {
              limit = declared.max_bytes;
            }
          }
        }
      }
      if (!limit || !valid_file_reference(file, limit)) {
        throw std::invalid_argument("Invalid file slot or oversized reference");
      }
      graph.set_file(node, slot,
                     explicitly_missing ? file : resolve_file_reference(std::move(file), limit));
    }
  }
  for (const auto &p : description.at("parameters")) {
    graph.set_parameter_value(node, p.at("id").get<uint32_t>(), p.at("value").get<float>());
  }
  return node;
}

// The section a slot belongs to: the single "cabinet" and "eq" slots, "specialFX" for the legacy
// bare slot and every "specialFX.<suffix>" slot, and "pedal" for the user-arranged Pre and Post
// FX slots. Empty slots and an empty Special FX suffix belong to no section.
std::optional<std::string_view> slot_section(std::string_view slot) {
  using namespace std::string_view_literals;
  if (slot.empty() || slot == "specialFX."sv) {
    return std::nullopt;
  }
  if (slot == "cabinet"sv) {
    return "cabinet"sv;
  }
  if (slot == "eq"sv) {
    return "eq"sv;
  }
  if (slot == "specialFX"sv || slot.starts_with("specialFX."sv)) {
    return "specialFX"sv;
  }
  return "pedal"sv;
}

bool slot_fits(std::string_view section, std::string_view slot) {
  const auto belongs = slot_section(slot);
  return belongs && *belongs == (section == "preFX" || section == "postFX" ? "pedal" : section);
}

// The gaps of the host chain a Special FX module can process in, in signal order. Gap k lies
// before the k-th of Pre FX, amp, cabinet, EQ and Post FX, whether or not that neighbour exists.
constexpr std::array<std::string_view, 6> placements{
    "before-pre-fx", "before-amp", "before-cabinet", "after-cabinet", "after-eq", "after-post-fx"};
constexpr size_t default_placement = 3;

size_t placement_index(const Json &value) {
  if (value.is_null()) {
    return default_placement;
  }
  if (value.is_string()) {
    const auto found =
        std::find(placements.begin(), placements.end(), value.get_ref<const std::string &>());
    if (found != placements.end()) {
      return static_cast<size_t>(found - placements.begin());
    }
  }
  throw std::invalid_argument("Invalid Special FX placement; expected before-pre-fx, "
                              "before-amp, before-cabinet, after-cabinet, after-eq or "
                              "after-post-fx");
}

void require_keys(const Json &object, std::initializer_list<std::string_view> allowed,
                  const char *what) {
  if (!object.is_object()) {
    throw std::invalid_argument(std::string(what) + " must be an object");
  }
  for (const auto &[key, value] : object.items()) {
    if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
      throw std::invalid_argument(std::string(what) + " has unknown key " + key);
    }
  }
}

uint32_t parameter_id(const Json &value, const char *what) {
  if (!value.is_number_unsigned() || value.get<uint64_t>() > std::numeric_limits<uint32_t>::max()) {
    throw std::invalid_argument(std::string(what) + " must name a module parameter");
  }
  return value.get<uint32_t>();
}

// A route's "routing": absent or null for a serial route, else a branch that listens to the
// chain input and is mixed in by the host.
std::optional<Branch> branch_of(const Json &route) {
  if (!route.contains("routing") || route.at("routing").is_null()) {
    return std::nullopt;
  }
  const auto &routing = route.at("routing");
  require_keys(routing, {"input", "mix"}, "Special FX routing");
  if (!routing.contains("input") || routing.at("input") != "chain-input") {
    throw std::invalid_argument("Special FX routing input must be chain-input");
  }
  if (!routing.contains("mix")) {
    throw std::invalid_argument("Special FX routing needs a mix");
  }
  const auto &mix = routing.at("mix");
  require_keys(mix, {"parameter", "law"}, "Special FX routing mix");
  if (!mix.contains("parameter") || !mix.contains("law") || mix.at("law") != "crossfade") {
    throw std::invalid_argument("Special FX routing mix needs a parameter and the law crossfade");
  }
  return Branch{parameter_id(mix.at("parameter"), "Special FX routing mix"), MixLaw::crossfade};
}

// The route index a routing selector value chooses: nearest, ties upward, clamped to the routes.
uint32_t route_index(double value, uint32_t count) {
  return static_cast<uint32_t>(
      std::clamp(std::floor(value + 0.5), 0.0, static_cast<double>(count - 1)));
}

// Where a Special FX description processes: a static route (an optional "placement" and
// "routing") or the route its "routingSelector" parameter chooses from the description's value.
struct SpecialRoute {
  size_t placement = default_placement;
  std::optional<Branch> branch;
  std::optional<uint32_t> selector;
  uint32_t route_count = 1;
  uint32_t index = 0;
};

SpecialRoute special_route(const Json &module) {
  SpecialRoute result;
  if (!module.contains("routingSelector") || module.at("routingSelector").is_null()) {
    result.placement = placement_index(module.value("placement", Json()));
    result.branch = branch_of(module);
    return result;
  }
  if (module.contains("placement") || module.contains("routing")) {
    throw std::invalid_argument(
        "A Special FX description has either a routing selector or a placement and routing");
  }
  const auto &selector = module.at("routingSelector");
  require_keys(selector, {"parameter", "routes"}, "Special FX routing selector");
  if (!selector.contains("parameter") || !selector.contains("routes") ||
      !selector.at("routes").is_array() || selector.at("routes").size() < 2 ||
      selector.at("routes").size() > 8) {
    throw std::invalid_argument(
        "A Special FX routing selector needs a parameter and 2 to 8 routes");
  }
  result.selector = parameter_id(selector.at("parameter"), "Special FX routing selector");
  const auto &routes = selector.at("routes");
  result.route_count = static_cast<uint32_t>(routes.size());
  std::vector<SpecialRoute> choices;
  for (const auto &route : routes) {
    require_keys(route, {"placement", "routing"}, "Special FX route");
    if (!route.contains("placement") || route.at("placement").is_null()) {
      throw std::invalid_argument("A Special FX route needs a placement");
    }
    SpecialRoute choice;
    choice.placement = placement_index(route.at("placement"));
    choice.branch = branch_of(route);
    if (choice.branch && choice.branch->mix_parameter == *result.selector) {
      throw std::invalid_argument("A routing selector cannot be a route's mix parameter");
    }
    choices.push_back(choice);
  }
  const Json *value = nullptr;
  if (module.contains("parameters") && module.at("parameters").is_array()) {
    for (const auto &parameter : module.at("parameters")) {
      if (parameter.is_object() && parameter.value("id", Json()) == *result.selector) {
        value = &parameter.at("value");
        break;
      }
    }
  }
  if (!value || !value->is_number() || !std::isfinite(value->get<float>())) {
    throw std::invalid_argument("The routing selector parameter is missing from the description");
  }
  result.index = route_index(value->get<float>(), result.route_count);
  result.placement = choices[result.index].placement;
  result.branch = choices[result.index].branch;
  return result;
}

// The module side of a routing selector: an integer parameter over exactly the routes, described
// as discrete when the module exports parameter metadata, and not the bypass parameter.
void check_selector(const ModuleMetadata &module, const SpecialRoute &route) {
  const auto found = std::find_if(module.parameters.begin(), module.parameters.end(),
                                  [&](const auto &p) { return p.id == *route.selector; });
  const auto last = static_cast<float>(route.route_count - 1);
  if (found == module.parameters.end() || found->minimum != 0.0f || found->maximum != last ||
      (module.parameter_metadata &&
       (!found->display.described || found->display.kind == ParameterKind::continuous ||
        found->display.steps != route.route_count)) ||
      module.bypass_parameter == route.selector || !module.parameter_values) {
    throw std::invalid_argument("Module " + module.id + " has no routing selector parameter " +
                                std::to_string(*route.selector) + " ranging over " +
                                std::to_string(route.route_count) + " discrete routes");
  }
}

// Placement, routing and routing selectors belong to Special FX descriptions only, so they cannot
// be silently ignored elsewhere.
void reject_routing(const Json &module) {
  if (module.is_object() && (module.contains("placement") || module.contains("routing") ||
                             module.contains("routingSelector"))) {
    throw std::invalid_argument("Only Special FX modules declare a placement or routing");
  }
}

bool same_stream(const Json &active, const Json &next) {
  if (active.is_null() || active.value("audio", Json()) != next.value("audio", Json())) {
    return false;
  }
  return (core_settings(active.value("globals", Json::object())).transpose_semitones == 0) ==
         (core_settings(next.value("globals", Json::object())).transpose_semitones == 0);
}

void validate_configuration(const Json &configuration) {
  core_settings(configuration.value("globals", Json::object()));
  tempo_settings(configuration.value("tempo", Json::object()));
  std::set<std::string> slots;
  size_t count = 0;
  const auto description = [&](const Json &module) {
    if (++count > max_preset_nodes || !module.is_object()) {
      throw std::invalid_argument("Too many modules or invalid module description");
    }
    const auto path = module.at("binary").get<std::string>();
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
      throw std::invalid_argument("Module binary is missing: " + path);
    }
    const auto &parameters = module.at("parameters");
    if (!parameters.is_array() || parameters.size() > 1024) {
      throw std::invalid_argument("Invalid module parameters");
    }
    if (module.contains("state") && !module["state"].is_null()) {
      module_state(module["state"]);
    }
    if (module.contains("bypassed")) {
      module["bypassed"].get<bool>();
    }
  };
  if (configuration.contains("amp") && !configuration["amp"].is_null()) {
    reject_routing(configuration["amp"]);
    description(configuration["amp"]);
  }
  std::set<std::string> special_modules;
  for (const char *section : {"preFX", "postFX", "cabinet", "specialFX", "eq"}) {
    if (!configuration.contains(section)) {
      continue;
    }
    const auto &modules = configuration[section];
    const bool single = section == std::string_view("cabinet") || section == std::string_view("eq");
    if (!modules.is_array() || (single && modules.size() > 1)) {
      throw std::invalid_argument("Invalid module section");
    }
    for (const auto &module : modules) {
      const auto slot = module.at("slot").get<std::string>();
      if (!slot_fits(section, slot) || !slots.insert(slot).second) {
        throw std::invalid_argument("Invalid or repeated module slot");
      }
      if (section == std::string_view("specialFX")) {
        special_route(module);
        if (module.contains("module") &&
            !special_modules.insert(module.at("module").get<std::string>()).second) {
          throw std::invalid_argument("A Special FX module can be described only once");
        }
      } else {
        reject_routing(module);
      }
      description(module);
    }
  }
  if (configuration.contains("audio")) {
    const auto &audio = configuration["audio"];
    const auto inputs = audio.at("inputChannels").get<std::vector<uint32_t>>();
    const auto outputs = audio.at("outputChannels").get<std::vector<uint32_t>>();
    if (inputs.empty() || inputs.size() > 2 || outputs.empty() || outputs.size() > 2) {
      throw std::invalid_argument("Select one or two input and output channels");
    }
  }
}

struct AppAudio {
  // Declared first so that every graph has released its modules, closing their work, and idle
  // plugin instances are disposed after every graph has released them.
  std::shared_ptr<HostServices> services = HostServices::shared();
  std::shared_ptr<InstanceCache> cache = std::make_shared<InstanceCache>();
  GraphLoader loader;
  AudioSession session;
  AudioSession inspector;
  MidiInput midi;
  int32_t midi_source = 0;
  uint32_t midi_channel = 0;
  // A slot, or "specialFX" for every MIDI-capable module of that section.
  std::string midi_slot = "specialFX";
  Json configuration;
  Json active_configuration;
  NodeId node = invalid_node_id;
  struct PedalNode {
    NodeId node;
    std::vector<ParameterMetadata> parameters;
    std::string section;
    enum class Bypass {
      // No bypass parameter: only the slot's host bypass flag leaves the module out of the graph.
      host,
      // Through the module's bypass parameter; the module is left out of the graph while off.
      parameter_removes,
      // Through the module's bypass parameter as a live parameter update, keeping the module in
      // its shared engine run while off.
      parameter_live
    } bypass = Bypass::host;
    uint32_t bypass_parameter = 0;
    bool midi_input = false;
    // A Special FX module's routing selector, its route count and the route the graph was built
    // for.
    std::optional<uint32_t> selector;
    uint32_t route_count = 1;
    uint32_t route = 0;
  };
  std::map<std::string, PedalNode> pedal_nodes;
  // The slots of pedal_nodes in processing order, including those left out of the graph.
  std::vector<std::string> pedal_order;
  struct Rig {
    NodeId amp = invalid_node_id;
    std::map<std::string, PedalNode> pedals;
    std::vector<std::string> order;
  };
  struct LoadedRig {
    std::mutex mutex;
    std::optional<Rig> rig;
  };
  // The lifecycle of one desired configuration travelling toward the audible graph. `loading`
  // carries the desired configuration and its off-thread rig; `failed` carries the reason the
  // last candidate did not activate and the previous audio stayed active. Transitions happen
  // only in swap (idle or failed -> loading), advance_candidate (loading -> idle or failed),
  // close (any -> idle) and the cancelCandidate request. The struct also owns the graph the
  // last swap retired, which only the loader destroys, and the bookkeeping of slots that still
  // owe a timing-driven reprepare.
  struct Candidate {
    enum class Phase : uint8_t { idle, loading, failed };
    Phase phase = Phase::idle;
    uint64_t generation = 0;
    Json desired;
    std::shared_ptr<LoadedRig> loaded;
    bool reset_beat = false;
    std::string error;
    std::unique_ptr<Graph> retiring;
    std::map<std::string, size_t> timing_attempts;
    std::set<std::string> timing_pending;
    std::optional<std::chrono::steady_clock::time_point> last_timing_preparation;

    bool loading() const { return phase == Phase::loading; }
    bool failed() const { return phase == Phase::failed; }
  };
  Candidate candidate;
  uint64_t active_generation = 0;
  std::optional<CoreIoSettings> pending_core;
  TempoSettings tempo;
  std::optional<TempoSettings> pending_tempo;
  TapTempo tap;
  // Held for the whole duration of one `request` call, and claimed by the destructor so
  // teardown never races a request: at most one bridge request runs at a time, and a second
  // concurrent caller is refused with the busy response rather than queued.
  std::atomic<bool> busy{false};
  std::string response;
  // Statuses of the active graph's modules that are not ready, refreshed when a module notifies a
  // change and after the graph is replaced.
  std::map<NodeId, ModuleStatus> module_statuses;
  bool module_statuses_stale = true;
  std::map<NodeId, std::chrono::steady_clock::time_point> timing_logged;

  AppAudio() { services->set_log_sink(log_line); }
  ~AppAudio() {
    busy.store(true);
    loader.retire(candidate.retiring);
    loader.request_stop();
#if defined(__APPLE__)
    if (pthread_main_np()) {
      using Clock = std::chrono::steady_clock;
      const auto deadline = Clock::now() + std::chrono::seconds(10);
      while (!loader.stopped() && Clock::now() < deadline) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.010, false);
      }
    }
#endif
    // A running module call can outlast the pump. The worker must still finish before teardown.
    loader.shutdown();
    services->set_log_sink(nullptr);
  }
  AppAudio(const AppAudio &) = delete;
  AppAudio &operator=(const AppAudio &) = delete;

  void close() {
    loader.cancel();
    candidate.phase = Candidate::Phase::idle;
    candidate.desired = nullptr;
    candidate.loaded.reset();
    candidate.error.clear();
    candidate.timing_pending.clear();
    candidate.timing_attempts.clear();
    candidate.last_timing_preparation.reset();
    midi.disconnect();
    if (session.close() != CloseResult::closed) {
      throw std::runtime_error("Audio stream could not close safely");
    }
    loader.retire(candidate.retiring);
    active_generation = 0;
    active_configuration = nullptr;
    node = invalid_node_id;
    pedal_nodes.clear();
    pedal_order.clear();
    module_statuses.clear();
    module_statuses_stale = true;
    pending_core.reset();
    pending_tempo.reset();
  }

  void open(const Json &j, double initial_beat) {
    const auto &audio = j.at("audio");
    auto inputs = audio.at("inputChannels").get<std::vector<uint32_t>>();
    auto outputs = audio.at("outputChannels").get<std::vector<uint32_t>>();
    if (inputs.empty() || inputs.size() > 2 || outputs.empty() || outputs.size() > 2) {
      throw std::invalid_argument("Select one or two input and output channels");
    }
    StreamConfig config;
    config.backend_id = audio.value("backend", "coreaudio");
    config.device_id = audio.at("inputDevice").get<std::string>();
    config.output_device_id = audio.at("outputDevice").get<std::string>();
    config.input_channel = inputs[0];
    if (inputs.size() == 2) {
      config.input_mode = InputMode::stereo;
      config.second_input_channel = inputs[1];
    }
    config.output_channel_count = static_cast<uint32_t>(outputs.size());
    config.output_channels = {outputs[0], outputs.size() == 2 ? outputs[1] : 0};
    config.sample_rate = audio.at("sampleRate").get<uint32_t>();
    config.frames_per_block = audio.at("bufferSize").get<uint32_t>();
    config.output_mode = audio.value("monitor", true) ? OutputMode::monitor : OutputMode::silent;
    config.core_io = core_settings(j.at("globals"));
    config.tempo = tempo_settings(j.value("tempo", Json::object()));
    config.initial_beat = initial_beat;
    auto &graph = session.graph();
    for (const auto &old : graph.nodes()) {
      graph.remove_module(old.instance_id);
    }
    node = invalid_node_id;
    pedal_nodes.clear();
    pedal_order.clear();
    session.prepare(config);
    module_statuses_stale = true;
    tempo = config.tempo;
    swap(j);
  }

  // Adds the configuration's modules to an empty editing graph in signal-chain order, which is
  // the order the graph processes them. Each Special FX module goes into the gap its placement
  // names; within one gap, modules follow ascending bytewise slot order, whatever the request
  // order. A live graph shares idle plugin instances and runs module work on the shared worker
  // threads; an offline one gets its own services.
  Rig populate(Graph &graph, const Json &j, std::shared_ptr<HostServices> offline = nullptr) {
    graph.set_sharing(true);
    if (!offline) {
      graph.set_instance_cache(cache);
    }
    graph.set_host_services(offline ? std::move(offline) : services);
    Rig rig;
    std::set<std::string> special_modules;
    auto append = [&](const char *section, const Json &pedal, const SpecialRoute *route = nullptr) {
      const auto slot = pedal.at("slot").get<std::string>();
      if (!slot_fits(section, slot)) {
        throw std::invalid_argument("Invalid module slot for its section");
      }
      if (rig.pedals.contains(slot)) {
        throw std::invalid_argument("Duplicate pedal slot");
      }
      const bool special = section == std::string_view("specialFX");
      if (!special) {
        reject_routing(pedal);
      }
      const auto pedal_node = add_described(graph, pedal);
      PedalNode entry{.node = pedal_node, .section = section};
      for (const auto &metadata : graph.nodes()) {
        if (metadata.instance_id == pedal_node) {
          if (special && !special_modules.insert(metadata.module.id).second) {
            throw std::invalid_argument("Special FX module " + metadata.module.id +
                                        " can be described only once");
          }
          entry.parameters = metadata.module.parameters;
          entry.midi_input = metadata.module.midi_input;
          if (metadata.module.bypass_parameter) {
            // A branch keeps its node while off: the graph realises its bypass as gains.
            entry.bypass = metadata.module.shared_engine || (route && route->branch)
                               ? PedalNode::Bypass::parameter_live
                               : PedalNode::Bypass::parameter_removes;
            entry.bypass_parameter = *metadata.module.bypass_parameter;
          }
          if (route && route->selector) {
            check_selector(metadata.module, *route);
            if (route_index(graph.parameter_value(pedal_node, *route->selector),
                            route->route_count) != route->index) {
              throw std::invalid_argument("Routing selector value does not match the state of " +
                                          metadata.module.id);
            }
            entry.selector = route->selector;
            entry.route_count = route->route_count;
            entry.route = route->index;
          }
        }
      }
      // Set even on a node about to be removed, so that a bypassed branch's mix parameter is
      // checked like an active one's; removal discards the route with the node.
      if (route && route->branch) {
        graph.set_branch(pedal_node, route->branch);
      }
      // A bypassed slot keeps its position and parameters but processes nothing.
      const bool bypassed = pedal.value("bypassed", false);
      if (bypassed || (entry.bypass == PedalNode::Bypass::parameter_removes &&
                       graph.parameter_value(pedal_node, entry.bypass_parameter) < 0.5f)) {
        graph.remove_module(pedal_node);
        entry.node = invalid_node_id;
      }
      rig.pedals.emplace(slot, std::move(entry));
      rig.order.push_back(slot);
    };
    auto section_modules = [&](const char *section) -> const Json * {
      if (!j.contains(section)) {
        return nullptr;
      }
      const auto &modules = j.at(section);
      const bool single =
          section == std::string_view("cabinet") || section == std::string_view("eq");
      if (!modules.is_array() || (single && modules.size() > 1)) {
        throw std::invalid_argument("Invalid pedal slots");
      }
      return &modules;
    };
    auto append_pedals = [&](const char *section) {
      if (const auto *modules = section_modules(section)) {
        for (const auto &pedal : *modules) {
          append(section, pedal);
        }
      }
    };
    using Placed = std::pair<const Json *, SpecialRoute>;
    std::array<std::vector<Placed>, placements.size()> special;
    if (const auto *modules = section_modules("specialFX")) {
      for (const auto &module : *modules) {
        const auto route = special_route(module);
        special[route.placement].emplace_back(&module, route);
      }
    }
    for (auto &gap : special) {
      std::sort(gap.begin(), gap.end(), [](const Placed &a, const Placed &b) {
        return a.first->at("slot").get_ref<const std::string &>() <
               b.first->at("slot").get_ref<const std::string &>();
      });
    }
    auto append_special = [&](size_t gap) {
      for (const auto &[module, route] : special[gap]) {
        append("specialFX", *module, &route);
      }
    };
    append_special(0);
    append_pedals("preFX");
    append_special(1);
    if (j.contains("amp") && !j["amp"].is_null()) {
      reject_routing(j["amp"]);
      rig.amp = add_described(graph, j["amp"]);
    }
    append_special(2);
    append_pedals("cabinet");
    append_special(3);
    append_pedals("eq");
    append_special(4);
    append_pedals("postFX");
    append_special(5);
    return rig;
  }

  // Note MIDI goes to the addressed slot, or for "specialFX" to every active MIDI-capable module
  // of that section in processing order, up to MidiInput::max_targets.
  void connect_midi() {
    midi.disconnect();
    if (session.state() != SessionState::running || !midi_source) {
      return;
    }
    std::vector<NodeId> targets;
    for (const auto &slot : pedal_order) {
      const auto &pedal = pedal_nodes.at(slot);
      const bool addressed =
          midi_slot == "specialFX" ? pedal.section == "specialFX" : slot == midi_slot;
      if (addressed && pedal.midi_input && pedal.node != invalid_node_id) {
        targets.push_back(pedal.node);
      }
    }
    if (targets.size() > MidiInput::max_targets) {
      log_line({VS_LOG_WARNING, "audio",
                std::to_string(targets.size()) + " active modules accept MIDI; only the first " +
                    std::to_string(MidiInput::max_targets) + " in processing order receive it"});
      targets.resize(MidiInput::max_targets);
    }
    midi.connect(session.midi_input(), targets, midi_source, midi_channel);
  }

  // Accepts desired settings; only advance_candidate changes the audible graph.
  bool swap(const Json &j, bool reset_beat = false) {
    const auto &info = session.stream_info();
    const Format format{static_cast<double>(info.sample_rate), info.max_frames_per_block,
                        info.graph_channels};
    auto loaded = std::make_shared<LoadedRig>();
    const auto generation =
        loader.submit([this, j, loaded, format](const GraphLoader::Cancelled &cancelled) {
          auto built = std::make_unique<Graph>();
          auto rig = populate(*built, j);
          {
            std::lock_guard lock(loaded->mutex);
            loaded->rig = std::move(rig);
          }
          if (!cancelled()) {
            built->prepare(format);
          }
          return built;
        });
    candidate.phase = Candidate::Phase::loading;
    candidate.generation = generation;
    candidate.desired = j;
    candidate.loaded = std::move(loaded);
    candidate.reset_beat = reset_beat;
    candidate.error.clear();
    configuration = j;
    return true;
  }

  void advance_candidate() {
    if (candidate.retiring) {
      if (!loader.retire(candidate.retiring)) {
        return;
      }
    }
    auto ready = loader.take_ready();
    if (!ready) {
      return;
    }
    if (!candidate.loading() || ready->generation != candidate.generation ||
        (session.state() != SessionState::running && session.state() != SessionState::prepared)) {
      candidate.retiring = std::move(ready->graph);
      return;
    }
    Rig rig;
    {
      std::lock_guard lock(candidate.loaded->mutex);
      rig = *candidate.loaded->rig;
    }
    auto staged = session.stage_graph(std::move(ready->graph));
    const bool was_running = session.state() == SessionState::running;
    const auto previous_beat = session.beat_position();
    const auto previous_tempo = tempo;
    const auto previous_core = core_settings(
        active_configuration.is_object() ? active_configuration.value("globals", Json::object())
                                         : Json::object());
    midi.disconnect();
    if (was_running) {
      session.stop();
    }
    if (session.state() != SessionState::prepared) {
      candidate.retiring = staged.take_graph();
      candidate.phase = Candidate::Phase::failed;
      candidate.error = "Audio did not stop safely; candidate was not activated";
      candidate.desired = nullptr;
      candidate.loaded.reset();
      return;
    }
    auto previous = session.swap_staged_graph(std::move(staged));
    module_statuses_stale = true;
    try {
      const auto next_tempo = tempo_settings(candidate.desired.value("tempo", Json::object()));
      session.set_core_io_settings(
          core_settings(candidate.desired.value("globals", Json::object())));
      if (candidate.reset_beat) {
        session.reset_tempo(next_tempo, 0);
      } else {
        session.set_tempo_settings(next_tempo);
      }
      session.start();
    } catch (const std::exception &error) {
      const std::string detail = error.what();
      // The failed calls above left the staged candidate installed, so recovery swaps the
      // staged previous graph back while the session is quiescent. reconfigure's catch cannot
      // do this: its failure happened while opening a stream, so it can only close and reopen.
      try {
        if (session.state() != SessionState::prepared) {
          throw std::logic_error("session is not prepared");
        }
        candidate.retiring = session.swap_staged_graph(std::move(previous)).take_graph();
        session.reset_tempo(previous_tempo, previous_beat);
        session.set_core_io_settings(previous_core);
        if (was_running) {
          session.start();
        }
        connect_midi();
      } catch (...) {
        throw std::runtime_error(detail + "; previous audio configuration could not be restored");
      }
      candidate.phase = Candidate::Phase::failed;
      candidate.error = detail + "; previous audio remains active";
      candidate.desired = nullptr;
      candidate.loaded.reset();
      pending_core.reset();
      pending_tempo.reset();
      return;
    }
    candidate.retiring = previous.take_graph();
    node = rig.amp;
    pedal_nodes = std::move(rig.pedals);
    pedal_order = std::move(rig.order);
    std::erase_if(candidate.timing_attempts, [&](const auto &entry) {
      return entry.first != "amp" && !pedal_nodes.contains(entry.first);
    });
    active_configuration = candidate.desired;
    active_generation = ready->generation;
    tempo = tempo_settings(active_configuration.value("tempo", Json::object()));
    pending_core.reset();
    pending_tempo.reset();
    candidate.phase = Candidate::Phase::idle;
    candidate.desired = nullptr;
    candidate.loaded.reset();
    candidate.error.clear();
    connect_midi();
  }

  void reconfigure(const Json &j, bool preserve_beat = true) {
    validate_configuration(j);
    const bool was_running = session.state() == SessionState::running;
    if (was_running && same_stream(active_configuration, j) && swap(j, !preserve_beat)) {
      return;
    }
    auto previous = active_configuration;
    close();
    const double checkpoint = was_running ? session.beat_position() : 0;
    const double beat = preserve_beat ? checkpoint : 0;
    try {
      open(j, beat);
    } catch (const std::exception &error) {
      const std::string detail = error.what();
      // The failure happened while opening a stream, so no staged previous graph exists to swap
      // back; advance_candidate's catch swaps one in instead because its failure happened after
      // the session had already staged the candidate.
      if (was_running && !previous.is_null()) {
        try {
          close();
          open(previous, checkpoint);
        } catch (...) {
          throw std::runtime_error(detail + "; previous audio configuration could not be restored");
        }
      } else {
        close();
      }
      throw std::runtime_error(detail);
    }
  }

  Json devices() {
    inspector.refresh_devices();
    Json result = Json::array();
    for (const auto &device : inspector.devices()) {
      Json item{{"id", device.device_id},
                {"backend", device.backend_id},
                {"name", device.display_name},
                {"pairable", device.pairable}};
      try {
        auto caps = inspector.probe(device);
        item["rates"] = caps.tested_rates;
        item["currentRate"] = caps.current_sample_rate;
        item["minimumBuffer"] = caps.min_frames;
        item["maximumBuffer"] = caps.max_frames;
        item["preferredBuffer"] = caps.preferred_frames;
        item["granularity"] = caps.frame_granularity;
        auto channels = [](const auto &entries) {
          Json array = Json::array();
          for (const auto &entry : entries) {
            if (entry.supported) {
              array.push_back({{"id", entry.index}, {"name", entry.name}});
            }
          }
          return array;
        };
        item["inputs"] = channels(caps.inputs);
        item["outputs"] = channels(caps.outputs);
        result.push_back(item);
      } catch (const std::exception &) {
        // A disconnected device must not prevent discovery of the remaining devices.
      }
    }
    return result;
  }

  Json status() {
    advance_candidate();
    if (midi_source) {
      const auto sources = MidiInput::sources();
      if (std::none_of(sources.begin(), sources.end(),
                       [&](const auto &source) { return source.id == midi_source; })) {
        midi.disconnect();
        midi_source = 0;
      }
    }
    auto status = session.poll();
    if (status.state == SessionState::running) {
      midi.refresh_generation();
    } else {
      midi.disconnect();
    }
    if (pending_tempo && status.state == SessionState::running) {
      try {
        session.set_tempo_settings(*pending_tempo);
        pending_tempo.reset();
      } catch (const std::logic_error &) {
      }
    }
    if (pending_core && status.state == SessionState::running) {
      try {
        session.set_core_io_settings(*pending_core);
        pending_core.reset();
      } catch (const std::logic_error &) {
      }
    }
    auto meters = session.read_meters();
    Json result{{"modules", read_module_statuses()},
                {"state", to_string(status.state)},
                {"midiSource", midi_source},
                {"midiDropped", session.midi_input().dropped()},
                {"recovery", to_string(status.recovery)},
                {"lastError", status.last_error},
                {"blocks", status.telemetry.blocks},
                {"beat", session.beat_position()},
                {"input", {meters.inputs[0].peak, meters.inputs[1].peak}},
                {"output", {meters.outputs[0].peak, meters.outputs[1].peak}}};
    const auto loader_state = loader.snapshot();
    static constexpr const char *states[] = {"idle",  "queued", "preparing", "waiting",
                                             "ready", "failed", "cancelled", "stopped"};
    result["candidate"] = {
        {"generation", loader_state.generation},
        {"state", candidate.failed() ? "failed" : states[static_cast<size_t>(loader_state.state)]},
        {"error", candidate.failed() ? candidate.error : loader_state.error},
        {"activeGeneration", active_generation}};
    if (candidate.loading()) {
      Json modules = Json::array();
      std::optional<Rig> rig;
      {
        std::lock_guard lock(candidate.loaded->mutex);
        rig = candidate.loaded->rig;
      }
      if (rig) {
        for (const auto &module : loader_state.modules) {
          std::string slot = module.node == rig->amp ? "amp" : "";
          for (const auto &[name, pedal] : rig->pedals) {
            if (pedal.node == module.node) {
              slot = name;
            }
          }
          if (!slot.empty()) {
            modules.push_back({{"slot", slot},
                               {"status", status_name(module.kind)},
                               {"message", module.message},
                               {"fileSlot", module.file_slot}});
            if (module.progress >= 0.0f) {
              modules.back()["progress"] = module.progress;
            }
          }
        }
      }
      result["candidate"]["modules"] = std::move(modules);
    }
    if (status.state == SessionState::running || status.state == SessionState::prepared) {
      const auto &info = session.stream_info();
      result["sampleRate"] = info.sample_rate;
      result["bufferSize"] = info.frames_per_block;
      result["latencyFrames"] = info.graph_latency_frames + info.core_io_latency_frames +
                                status.stream.input_latency_frames +
                                status.stream.output_latency_frames;
    }
    return result;
  }

  // The slot a node of the active graph serves, or empty.
  std::string slot_of(NodeId id) const {
    if (id == node) {
      return "amp";
    }
    for (const auto &[slot, pedal] : pedal_nodes) {
      if (pedal.node == id) {
        return slot;
      }
    }
    return {};
  }

  void refresh_module_status(NodeId id) {
    const auto status = session.module_status(id);
    if (status && status->kind != VS_STATUS_READY) {
      module_statuses[id] = *status;
    } else {
      module_statuses.erase(id);
    }
  }

  // Applies the notices modules raised since the last tick and lists the modules whose status is
  // not ready. Timing notices are not acted on yet; they are logged at most once a second per
  // module.
  Json read_module_statuses() {
    const auto notices = session.take_module_notices();
    std::optional<size_t> fingerprint_cache;
    const auto timing_fingerprint = [&] {
      if (!fingerprint_cache) {
        fingerprint_cache = std::hash<std::string>{}(configuration.dump());
      }
      return *fingerprint_cache;
    };
    if (module_statuses_stale) {
      module_statuses.clear();
      timing_logged.clear();
      for (const auto &metadata : session.nodes()) {
        if (metadata.module.services || !metadata.module.file_slots.empty()) {
          refresh_module_status(metadata.instance_id);
        }
      }
      module_statuses_stale = false;
    }
    for (const auto &notice : notices) {
      if (notice.what & VS_NOTIFY_STATUS) {
        refresh_module_status(notice.node);
      }
      if (notice.what & VS_NOTIFY_TIMING) {
        const auto now = std::chrono::steady_clock::now();
        auto [entry, added] = timing_logged.try_emplace(notice.node, now);
        if (added || now - entry->second >= std::chrono::seconds(1)) {
          entry->second = now;
          log_line({VS_LOG_INFO, "audio",
                    "module at node " + std::to_string(notice.node) + " reports new timing"});
        }
        if (!candidate.loading() && configuration == active_configuration &&
            session.state() == SessionState::running) {
          const auto slot = slot_of(notice.node);
          const auto fingerprint = timing_fingerprint();
          const auto attempted = candidate.timing_attempts.find(slot);
          if (!slot.empty() &&
              (attempted == candidate.timing_attempts.end() || attempted->second != fingerprint)) {
            candidate.timing_pending.insert(slot);
          }
        }
      }
    }
    const auto now = std::chrono::steady_clock::now();
    if (!candidate.timing_pending.empty() && !candidate.loading() &&
        configuration == active_configuration &&
        (!candidate.last_timing_preparation ||
         now - *candidate.last_timing_preparation >= std::chrono::seconds(1))) {
      const auto fingerprint = timing_fingerprint();
      for (const auto &slot : candidate.timing_pending) {
        candidate.timing_attempts[slot] = fingerprint;
      }
      candidate.timing_pending.clear();
      candidate.last_timing_preparation = now;
      swap(configuration);
    }
    Json modules = Json::array();
    for (const auto &[id, status] : module_statuses) {
      auto slot = slot_of(id);
      if (slot.empty()) {
        continue;
      }
      Json entry{{"slot", std::move(slot)},
                 {"status", status_name(status.kind)},
                 {"message", status.message}};
      if (status.file_slot) {
        entry["fileSlot"] = status.file_slot;
      }
      if (status.progress >= 0.0f) {
        entry["progress"] = status.progress;
      }
      modules.push_back(std::move(entry));
    }
    return modules;
  }

  // Renders a configuration's module sections offline through a new graph built exactly as the
  // live graph is, with fresh module instances, no reused plugin instances and module work run
  // inline, so the output depends only on the configuration and the input. Never touches the
  // running session.
  //
  // A verification tool, not an application feature: it runs synchronously on the request
  // thread, so it is refused unless the process opts in with VESPERA_ENABLE_RENDER_GRAPH=1.
  Json render_graph(const Json &j) {
    const char *enabled = std::getenv("VESPERA_ENABLE_RENDER_GRAPH");
    if (!enabled || std::string_view(enabled) != "1") {
      throw std::invalid_argument(
          "Offline rendering is disabled; set VESPERA_ENABLE_RENDER_GRAPH=1");
    }
    TempoSettings tempo_check;
    tempo_check.bpm = j.value("bpm", 120.0);
    if (!validate_tempo_settings(tempo_check)) {
      throw std::invalid_argument("Invalid offline render tempo");
    }
    const double bpm = tempo_check.bpm;
    const auto sample_rate = j.value("sampleRate", 48000u);
    const auto block_size = j.value("blockSize", 128u);
    const auto &input = j.at("input");
    if (sample_rate < 8000 || sample_rate > 384000 || block_size == 0 || block_size > 4096 ||
        !input.is_array() || input.empty() || input.size() > 2) {
      throw std::invalid_argument("Invalid offline render request");
    }
    const auto channels = static_cast<uint32_t>(input.size());
    std::vector<std::vector<float>> source;
    for (const auto &channel : input) {
      source.push_back(channel.get<std::vector<float>>());
      if (source.back().size() != source.front().size()) {
        throw std::invalid_argument("Offline render channels differ in length");
      }
    }
    const size_t frames = source.front().size();
    if (frames == 0 || frames > 30u * sample_rate) {
      throw std::invalid_argument("Invalid offline render length");
    }
    validate_configuration(j.at("configuration"));
    auto graph = std::make_unique<Graph>();
    populate(*graph, j.at("configuration"),
             HostServices::create({HostWorkMode::synchronous, 0, false}));
    graph->prepare(Format{static_cast<double>(sample_rate), block_size, channels});
    std::vector<std::vector<float>> output(channels, std::vector<float>(frames));
    for (size_t offset = 0; offset < frames; offset += block_size) {
      const auto count = static_cast<uint32_t>(std::min<size_t>(block_size, frames - offset));
      std::array<const float *, 2> inputs{};
      std::array<float *, 2> outputs{};
      for (uint32_t c = 0; c < channels; ++c) {
        inputs[c] = source[c].data() + offset;
        outputs[c] = output[c].data() + offset;
      }
      vs_tempo_context timing{sizeof(vs_tempo_context),
                              VS_TEMPO_VALID | (offset == 0 ? VS_TEMPO_DISCONTINUITY : 0u), bpm,
                              static_cast<double>(offset) * bpm / (60.0 * sample_rate)};
      Block block{inputs.data(),
                  outputs.data(),
                  channels,
                  count,
                  offset,
                  VS_PROCESS_OFFLINE | (offset == 0 ? uint32_t{VS_PROCESS_DISCONTINUITY} : 0u),
                  &timing};
      if (const auto result = graph->process(block); result != VS_OK) {
        throw std::runtime_error("Offline render failed at frame " + std::to_string(offset) + " (" +
                                 std::to_string(result) + ")");
      }
    }
    const auto latency = graph->latency_frames();
    graph->release();
    return {{"output", output}, {"latencyFrames", latency}};
  }

  // A parameter of a configuration that is still travelling toward the graph: the value joins
  // the candidate as it is prepared, so the playing graph receives no event.
  void apply_parameter_to_candidate(const Json &j, const std::string &slot) {
    auto next = configuration;
    Json *description = nullptr;
    if (slot.empty()) {
      const auto amp = next.find("amp");
      if (amp != next.end()) {
        description = &*amp;
      }
    } else {
      for (const char *section : {"preFX", "postFX", "cabinet", "specialFX", "eq"}) {
        const auto modules = next.find(section);
        if (modules == next.end()) {
          continue;
        }
        for (auto &module : *modules) {
          if (module.value("slot", "") == slot) {
            description = &module;
          }
        }
      }
    }
    const auto id = j.at("id").get<uint32_t>();
    const auto value = j.at("value").get<float>();
    bool found = false;
    if (description && description->is_object() && std::isfinite(value)) {
      for (auto &parameter : description->at("parameters")) {
        if (parameter.at("id") == id) {
          parameter["value"] = value;
          found = true;
        }
      }
    }
    if (!found) {
      throw std::invalid_argument("No matching candidate parameter");
    }
    swap(next);
  }

  // A value that changes the graph rather than the playing module: a bypass that removes the
  // module, or a routing selector value that resolves to another route. The new value and the
  // new graph are prepared together as a candidate; the playing graph receives no event.
  void apply_graph_changing_parameter(const std::string &slot, uint32_t id, float value) {
    auto next = configuration;
    for (auto &description : next[pedal_nodes.at(slot).section]) {
      if (description["slot"] == slot) {
        for (auto &p : description["parameters"]) {
          if (p["id"] == id) {
            p["value"] = value;
          }
        }
      }
    }
    reconfigure(next);
  }

  // Sends the value to the playing module and records it in the configuration, which becomes
  // the active one with no graph change.
  void apply_live_parameter(const std::string &slot, NodeId target, uint32_t id, float value) {
    if (target != invalid_node_id) {
      auto result = session.set_parameter(session.parameter_token(), target, id, value);
      if (result != ParameterResult::accepted) {
        throw std::runtime_error("Audio parameter update was not accepted");
      }
    }
    Json *parameters = nullptr;
    if (slot.empty()) {
      parameters = &configuration["amp"]["parameters"];
    } else {
      for (auto &pedal : configuration[pedal_nodes.at(slot).section]) {
        if (pedal["slot"] == slot) {
          parameters = &pedal["parameters"];
        }
      }
    }
    if (!parameters) {
      throw std::runtime_error("Live module configuration is missing");
    }
    for (auto &p : *parameters) {
      if (p["id"] == id) {
        p["value"] = value;
      }
    }
    active_configuration = configuration;
  }

  Json request(const Json &j) {
    const auto op = j.at("op").get<std::string>();
    if (op == "devices") {
      return devices();
    }
    if (op == "midiDevices") {
      Json result = Json::array();
      for (const auto &source : MidiInput::sources()) {
        result.push_back({{"id", source.id}, {"name", source.name}});
      }
      return result;
    }
    if (op == "midiPanic") {
      session.midi_input().panic();
      return nullptr;
    }
    if (op == "midi") {
      const auto source = j.at("source").get<int32_t>();
      const auto channel = j.at("channel").get<uint32_t>();
      const auto slot = j.at("slot").get<std::string>();
      if (channel > 16) {
        throw std::invalid_argument("Invalid MIDI channel");
      }
      if (source) {
        const auto sources = MidiInput::sources();
        if (std::none_of(sources.begin(), sources.end(),
                         [&](const auto &entry) { return entry.id == source; })) {
          throw std::invalid_argument("MIDI source is unavailable");
        }
      }
      const auto previous_source = midi_source;
      const auto previous_channel = midi_channel;
      const auto previous_slot = midi_slot;
      midi_source = source;
      midi_channel = channel;
      midi_slot = slot;
      try {
        connect_midi();
      } catch (...) {
        midi_source = previous_source;
        midi_channel = previous_channel;
        midi_slot = previous_slot;
        connect_midi();
        throw;
      }
      return nullptr;
    }
    if (op == "start") {
      reconfigure(j.at("configuration"), false);
      return status();
    }
    if (op == "stop") {
      close();
      return status();
    }
    if (op == "status") {
      return status();
    }
    if (op == "cancelCandidate") {
      loader.cancel();
      candidate.desired = nullptr;
      candidate.loaded.reset();
      candidate.timing_pending.clear();
      if (j.value("discard", false)) {
        candidate.phase = Candidate::Phase::idle;
        candidate.error.clear();
        configuration = active_configuration;
      } else {
        candidate.phase = Candidate::Phase::failed;
        candidate.error = "Candidate cancelled; previous audio remains active";
      }
      return status();
    }
    if (op == "tempo" || op == "tap") {
      auto next = op == "tempo" ? tempo_settings(j.at("tempo")) : tempo;
      if (op == "tap") {
        const double now =
            std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
                .count();
        if (auto bpm = tap.push(now)) {
          next.bpm = *bpm;
        }
      }
      tempo = next;
      configuration["tempo"] = tempo_json(tempo);
      active_configuration["tempo"] = tempo_json(tempo);
      if (candidate.loading()) {
        candidate.desired["tempo"] = tempo_json(tempo);
      }
      if (session.state() == SessionState::running) {
        pending_tempo = tempo;
      }
      return tempo_json(tempo);
    }
    if (op == "amp" || op == "rig") {
      if (session.state() != SessionState::running && !candidate.loading()) {
        return nullptr;
      }
      auto next = configuration;
      next["amp"] = j.at("amp");
      next["eq"] = j.value("eq", Json::array());
      if (op == "rig") {
        next["cabinet"] = j.at("cabinet");
      }
      reconfigure(next);
      return status();
    }
    if (op == "parameter") {
      const auto slot = j.value("slot", std::string{});
      if ((candidate.loading() || configuration != active_configuration) &&
          (session.state() == SessionState::prepared || session.state() == SessionState::running)) {
        apply_parameter_to_candidate(j, slot);
        return nullptr;
      }
      const auto target = slot.empty()                 ? node
                          : pedal_nodes.contains(slot) ? pedal_nodes.at(slot).node
                                                       : invalid_node_id;
      if (session.state() != SessionState::running || (slot.empty() && target == invalid_node_id) ||
          (!slot.empty() && !pedal_nodes.contains(slot))) {
        throw std::runtime_error("No matching live module is running");
      }
      const auto id = j.at("id").get<uint32_t>();
      const auto value = j.at("value").get<float>();
      if (!slot.empty()) {
        const auto &parameters = pedal_nodes.at(slot).parameters;
        auto descriptor = std::find_if(parameters.begin(), parameters.end(),
                                       [&](const auto &p) { return p.id == id; });
        if (descriptor == parameters.end() || !std::isfinite(value) ||
            value < descriptor->minimum || value > descriptor->maximum) {
          throw std::invalid_argument("Invalid pedal parameter");
        }
        // Changes the graph rather than the playing module: a bypass that removes the module, or
        // a routing selector value that resolves to another route. The new value and the new
        // graph are prepared together as a candidate; the playing graph receives no event.
        const auto &pedal = pedal_nodes.at(slot);
        if ((pedal.bypass == PedalNode::Bypass::parameter_removes &&
             id == pedal.bypass_parameter) ||
            (pedal.selector && id == *pedal.selector &&
             route_index(value, pedal.route_count) != pedal.route)) {
          apply_graph_changing_parameter(slot, id, value);
          return nullptr;
        }
      }
      apply_live_parameter(slot, target, id, value);
      return nullptr;
    }
    if (op == "preFX" || op == "postFX" || op == "specialFX") {
      if (session.state() != SessionState::running && !candidate.loading()) {
        return nullptr;
      }
      auto next = configuration;
      next[op] = j.at("modules");
      reconfigure(next);
      return status();
    }
    if (op == "preset") {
      // Replaces every module section and the global controls in one reconfiguration, so a
      // rejected preset leaves the previous graph and settings in place.
      const auto settings = core_settings(j.at("globals"));
      if (session.state() != SessionState::running && !candidate.loading()) {
        return nullptr;
      }
      auto next = configuration;
      for (const char *section : {"amp", "cabinet", "eq", "preFX", "postFX", "specialFX"}) {
        next[section] = j.at(section);
      }
      next["globals"] = j.at("globals");
      reconfigure(next);
      if (candidate.loading()) {
        return status();
      }
      try {
        session.set_core_io_settings(settings);
        pending_core.reset();
      } catch (const std::logic_error &) {
        pending_core = settings;
      }
      return status();
    }
    if (op == "renderGraph") {
      return render_graph(j);
    }
    if (op == "globals") {
      auto settings = core_settings(j.at("globals"));
      if (session.state() != SessionState::running && !candidate.loading()) {
        return nullptr;
      }
      auto next = configuration;
      next["globals"] = j.at("globals");
      if (candidate.loading()) {
        reconfigure(next);
        return nullptr;
      }
      if (settings.transpose_semitones != 0 && session.stream_info().core_io_latency_frames == 0) {
        reconfigure(next);
      } else {
        pending_core = settings;
        configuration = next;
        active_configuration = next;
      }
      return nullptr;
    }
    throw std::invalid_argument("Unknown audio operation");
  }
};

void *create() {
  try {
    return new AppAudio;
  } catch (...) {
    return nullptr;
  }
}
void destroy(void *instance) { delete static_cast<AppAudio *>(instance); }
const char *request(void *instance, const char *text) {
  if (!instance || !text) {
    return "{\"ok\":false,\"error\":\"Invalid audio request\"}";
  }
  auto &self = *static_cast<AppAudio *>(instance);
  if (self.busy.exchange(true)) {
    return R"({"ok":false,"error":"Audio backend is busy"})";
  }
  struct ReleaseRequest {
    std::atomic<bool> &busy;
    ~ReleaseRequest() { busy.store(false); }
  } release{self.busy};
  try {
    self.response = Json{{"ok", true}, {"value", self.request(Json::parse(text))}}.dump();
  } catch (const std::exception &error) {
    self.response = Json{{"ok", false}, {"error", error.what()}}.dump();
  } catch (...) {
    self.response = "{\"ok\":false,\"error\":\"Unknown audio failure\"}";
  }
  return self.response.c_str();
}
const vs_app_audio_api api{sizeof(vs_app_audio_api), 1, create, destroy, request};
}

extern "C" VS_MODULE_EXPORT const vs_app_audio_api *vespera_app_audio_entry(uint32_t version) {
  return version == 1 ? &api : nullptr;
}
