#ifndef VESPERA_RUNTIME_HPP
#define VESPERA_RUNTIME_HPP

#include <vespera/files.hpp>
#include <vespera/module.h>
#include <vespera/parameter_display.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace vespera {

struct Format {
  double sample_rate = 48000.0;
  uint32_t max_frames = 128;
  uint32_t channels = 2;
};

struct Block {
  const float *const *inputs = nullptr;
  float *const *outputs = nullptr;
  uint32_t channels = 0;
  uint32_t frames = 0;
  uint64_t frame_position = 0;
  uint32_t flags = 0;
  const vs_tempo_context *tempo = nullptr;
};

using NodeId = uint64_t;

inline constexpr NodeId invalid_node_id = std::numeric_limits<NodeId>::max();

struct Event {
  NodeId node = invalid_node_id;
  vs_parameter_event parameter{};
  enum class Kind { parameter, midi } kind = Kind::parameter;
  vs_midi_event midi{};

  uint32_t frame_offset() const noexcept {
    return kind == Kind::midi ? midi.frame_offset : parameter.frame_offset;
  }
};

struct ParameterMetadata {
  uint32_t id = 0;
  std::string name;
  std::string unit;
  float minimum = 0.0f;
  float maximum = 0.0f;
  float default_value = 0.0f;
  // Kind and display, from the module's parameter metadata when it describes this parameter.
  ParameterDisplay display;
};

struct ModuleMetadata {
  std::string id;
  std::string name;
  std::string version;
  uint32_t supported_layouts = 0;
  std::vector<ParameterMetadata> parameters;
  uint32_t state_schema_version = 0;
  // True when the module implements the parameter value extension.
  bool parameter_values = false;
  // True when the module exports the parameter metadata extension.
  bool parameter_metadata = false;
  // The capabilities the module declares in vs_module_api.
  std::optional<uint32_t> bypass_parameter;
  bool host_tempo = false;
  bool midi_input = false;
  bool shared_engine = false;
  // True when the module exports the services extension.
  bool services = false;
  std::vector<FileSlotMetadata> file_slots;
};

struct ModuleOptions {
  // The package's resources directory, passed to modules through host services. Empty when the
  // module was not found in a package.
  std::filesystem::path resource_directory;
};

// A status reported by a module through the services extension.
struct ModuleStatus {
  NodeId node = invalid_node_id;
  uint32_t kind = VS_STATUS_READY;
  // [0, 1], or negative when unknown.
  float progress = -1.0f;
  uint32_t file_slot = 0;
  std::string message;
};

// VS_NOTIFY_* bits a module raised since they were last taken.
struct ModuleNotice {
  NodeId node = invalid_node_id;
  uint32_t what = 0;
};

struct NodeMetadata {
  NodeId instance_id = invalid_node_id;
  ModuleMetadata module;
};

struct ModuleState {
  uint32_t schema_version = 0;
  std::vector<uint8_t> bytes;
};

struct PresetNode {
  NodeId instance_id = invalid_node_id;
  std::string module_id;
  std::string module_version;
  uint32_t module_abi_version = VS_MODULE_ABI_VERSION;
  ModuleState state;
  struct File {
    uint32_t slot = 0;
    FileReference reference;
  };
  std::vector<File> files{};
};

struct Preset {
  uint32_t schema_version = 1;
  NodeId next_instance_id = 0;
  std::vector<PresetNode> nodes;
};

inline constexpr std::size_t max_module_state_bytes = 1024 * 1024;
inline constexpr std::size_t max_preset_state_bytes = 16 * 1024 * 1024;
inline constexpr std::size_t max_preset_nodes = 128;

void validate_preset(const Preset &preset);

// Whether settings saved by a module built for module ABI saved_abi can be restored into a module
// built for VS_MODULE_ABI_VERSION, given matching module ID and version.
constexpr bool restores_saved_module_abi(uint32_t saved_abi) noexcept {
  return saved_abi >= VS_MODULE_SETTINGS_ABI_MINIMUM && saved_abi <= VS_MODULE_ABI_VERSION;
}

struct PresetIssue {
  NodeId instance_id = invalid_node_id;
  std::string message;
};

class InstanceCache;
class HostServices;

class ModuleCatalog {
public:
  void add(const std::filesystem::path &binary);
  std::vector<PresetIssue> inspect(const Preset &preset) const;
  std::filesystem::path resolve(const PresetNode &node) const;

private:
  struct Entry {
    std::filesystem::path binary;
    ModuleMetadata metadata;
  };
  std::vector<Entry> entries_;
};

enum class GraphState { editing, prepared, failed };

// How a branch's mix parameter value m sets the gains of the two signals meeting at the merge.
// crossfade: main path 1 - m, branch m.
enum class MixLaw { crossfade };

// A node that reads the graph input instead of its predecessor's output. Its output, which must
// not contain its input and must not depend on its mix or bypass parameter, is mixed into the
// main path at the node's position in the node list. The graph realises the mix parameter (range
// exactly 0..1) and the module's declared bypass parameter itself: below 0.5 the bypass parameter
// sets the gains to main 1, branch 0. The mix value m, whether read at prepare, delivered as an
// event or set directly, is the mix parameter's ParameterDisplay::nearest value: the step a stepped
// parameter's module stores, or the value itself for a continuous one. Gains reach new targets
// linearly over Graph::branch_ramp_seconds, starting at the event's frame. The shorter path is
// delayed to the longer one's reported latency, a branch at its output.
struct Branch {
  uint32_t mix_parameter = 0;
  MixLaw law = MixLaw::crossfade;
};

class Graph {
public:
  static constexpr uint32_t max_events_per_block = 512;
  static constexpr double branch_ramp_seconds = 0.010;
  // The largest alignment delay prepare inserts at a merge.
  static constexpr double max_branch_delay_seconds = 10.0;

  Graph();
  ~Graph();
  Graph(const Graph &) = delete;
  Graph &operator=(const Graph &) = delete;

  // Both require an editing graph and take effect at the next prepare. With sharing enabled,
  // prepare runs each maximal run of adjacent modules that may share an engine (see
  // vs_module_shared_api) on one engine hosted by the run's first module; the graph latency then
  // counts each engine once. Hosted modules keep their parameters, state and events. A cache lets
  // engines reuse idle instances, such as hosted plugins, across graphs.
  void set_sharing(bool enabled);
  void set_instance_cache(std::shared_ptr<InstanceCache> cache);
  // Attaches these services to every module added from now on that exports the services
  // extension. Requires an editing graph without nodes. Without services such modules run their
  // work themselves (see vs_module_services_api).
  void set_host_services(std::shared_ptr<HostServices> services);
  // Node IDs in processing order, one entry per prepared processor: an engine run or a single
  // module. Empty unless prepared.
  std::vector<std::vector<NodeId>> processing_groups() const;
  // The format of the last successful prepare; zero sample rate unless prepared or failed.
  Format prepared_format() const noexcept;

  NodeId add_module(const std::filesystem::path &binary, const ModuleOptions &options = {});
  void remove_module(NodeId node);
  void move_module(NodeId node, uint32_t destination);
  // Requires an editing graph; takes effect at the next prepare and stays with the node through
  // move_module. nullopt makes the node serial again. Fails, leaving the node's route unchanged,
  // when the node is unknown, the mix parameter is missing, is not ranged exactly 0..1 or is the
  // module's bypass parameter, the law is unknown, or the module has no parameter value
  // extension. Presets do not carry routes: restore_preset yields serial nodes.
  // A branch node gets its own processing group unless its shared-branch extension absorbs it
  // into one compatible whole-graph engine run. It never leads a run with followers. For a
  // host-mixed branch, starting gains are read from the module when the
  // graph is prepared; set_parameter_value, reset and a block flagged VS_PROCESS_DISCONTINUITY
  // move them to their targets at once, and the latter two also clear the alignment delays.
  void set_branch(NodeId node, std::optional<Branch> branch);
  // Branch nodes in node order.
  std::vector<NodeId> branch_nodes() const;
  void prepare(const Format &format);
  void release();
  void reset();
  vs_result process(const Block &block, std::span<const Event> events = {}) noexcept;
  GraphState state() const noexcept;
  uint32_t latency_frames() const noexcept;
  uint64_t tail_frames() const noexcept;
  uint32_t node_count() const noexcept;
  // Safe to call from the control thread while another thread is inside process().
  std::vector<NodeMetadata> nodes() const;
  std::vector<NodeId> node_ids() const;
  // Nonzero only while prepared or failed; unique across all graphs and successful prepares.
  uint64_t generation() const noexcept;
  // Waits until no work requested by the graph's modules is queued or running, or until the
  // deadline; true when none is. Like nodes(), these three may be called on the control thread
  // while another thread is inside process().
  bool wait_for_work(std::chrono::steady_clock::time_point deadline) const;
  // Notices raised by modules since the last call, in node order.
  std::vector<ModuleNotice> take_notices();
  // The node's current status, including host-reported missing files. Without a file problem or
  // attached services there is no status. A malformed module report becomes an error status.
  std::optional<ModuleStatus> module_status(NodeId node) const;
  Preset capture_preset() const;
  // Read or replace the value a module currently applies, in native units. Both require an
  // editing or prepared graph and quiescence: unlike nodes(), they must not overlap process().
  // A module without the parameter value extension fails with std::runtime_error.
  float parameter_value(NodeId node, uint32_t parameter_id) const;
  void set_parameter_value(NodeId node, uint32_t parameter_id, float value);
  // Loads module state into one node of an editing graph through the module's state interface,
  // which validates it. Fails, leaving the node's previous state to the module, when the module
  // has no state interface, rejects the schema version or the bytes, or the state is too large.
  void load_module_state(NodeId node, const ModuleState &state);
  void set_file(NodeId node, uint32_t slot, const FileReference &file);
  static std::unique_ptr<Graph> restore_preset(const Preset &preset, const ModuleCatalog &catalog,
                                               std::shared_ptr<HostServices> services = nullptr);

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}

#endif
