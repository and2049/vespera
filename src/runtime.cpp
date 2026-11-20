#include "graph_branch.hpp"
#include "module_loader.hpp"
#include "samples.hpp"

#include <vespera/file_content.hpp>
#include <vespera/host_services.hpp>
#include <vespera/instance_cache.hpp>
#include <vespera/parameter_display.hpp>
#include <vespera/runtime.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vespera {
namespace {

using loader::canonical_module_path;
using loader::Library;
using loader::LibraryHandle;
using loader::load_failure;
using loader::make_library;
using loader::open_library;
using loader::ParameterRange;

using graph_branch::BranchPath;
using graph_branch::Gains;

constexpr uint32_t max_channels = 2;
constexpr uint32_t max_nodes = 128;
constexpr uint32_t max_assignments_per_parameter = 4;
constexpr uint32_t max_frames_limit = 65536;
constexpr double min_sample_rate = 1000.0;
constexpr double max_sample_rate = 768000.0;
constexpr uint32_t known_flags =
    VS_PROCESS_DISCONTINUITY | VS_PROCESS_OFFLINE | VS_PROCESS_MIDI_PANIC;

std::atomic<uint64_t> next_generation{1};

uint64_t reserve_generation() {
  uint64_t value = next_generation.load(std::memory_order_relaxed);
  do {
    if (value == std::numeric_limits<uint64_t>::max()) {
      throw std::overflow_error("graph generation counter is exhausted");
    }
  } while (!next_generation.compare_exchange_weak(value, value + 1, std::memory_order_relaxed));
  return value;
}

std::string describe_node(const std::string &module_id, NodeId id) {
  return "module '" + module_id + "' at node " + std::to_string(id);
}

// hosted: processed by the engine of an earlier node in the same run, not prepared itself.
enum class NodeState { created, prepared, hosted };

struct Node {
  NodeId id = invalid_node_id;
  std::shared_ptr<Library> library;
  void *instance = nullptr;
  NodeState state = NodeState::created;
  // The cache descriptor given to this node's engine, which the module may use until its release
  // returns.
  std::shared_ptr<const vs_host_instance_cache> cache;
  // The services attached to the instance; kept until the instance is destroyed.
  std::shared_ptr<HostServices::Client> services;
  std::map<uint32_t, FileReference> files;
  std::optional<Branch> branch;
};

uint64_t saturating_add(uint64_t a, uint64_t b) noexcept {
  return b > std::numeric_limits<uint64_t>::max() - a ? std::numeric_limits<uint64_t>::max()
                                                      : a + b;
}

// Reads a status report as untrusted data: an error code, an unknown kind or a message that is
// not NUL-terminated UTF-8 becomes an error status describing the fault.
ModuleStatus read_status(const Node &node) {
  ModuleStatus status;
  status.node = node.id;
  vs_module_status raw{};
  raw.struct_size = sizeof(vs_module_status);
  raw.kind = VS_STATUS_READY;
  raw.progress = -1.0f;
  const vs_result result = node.library->services->status(node.instance, &raw);
  const size_t length = strnlen(raw.message, sizeof(raw.message));
  if (result != VS_OK) {
    status.kind = VS_STATUS_ERROR;
    status.message = "the module failed to report its status (" + std::to_string(result) + ")";
  } else if (raw.kind > VS_STATUS_ERROR) {
    status.kind = VS_STATUS_ERROR;
    status.message = "the module reported an unknown status kind " + std::to_string(raw.kind);
  } else if (length == sizeof(raw.message) ||
             !display_detail::valid_utf8(std::string_view(raw.message, length))) {
    status.kind = VS_STATUS_ERROR;
    status.message = "the module reported a malformed status message";
  } else if (raw.file_slot != 0 &&
             std::none_of(node.library->metadata.file_slots.begin(),
                          node.library->metadata.file_slots.end(),
                          [&](const auto &slot) { return slot.id == raw.file_slot; })) {
    status.kind = VS_STATUS_ERROR;
    status.message = "the module reported an undeclared file slot";
  } else {
    status.kind = raw.kind;
    status.message.assign(raw.message, length);
    status.file_slot = raw.file_slot;
    status.progress =
        std::isfinite(raw.progress) && raw.progress >= 0.0f ? std::min(raw.progress, 1.0f) : -1.0f;
    if (status.kind == VS_STATUS_BUSY && status.progress < 0.0f && node.services) {
      status.progress = node.services->progress();
    }
  }
  return status;
}

struct RunTarget {
  const vs_shared_target *target;
  bool owned = false;
  bool required = false;
};

// Accumulates the targets of a run of modules sharing one engine and decides whether the next
// module may join it, following the rules documented for vs_module_shared_api.
class EngineRun {
public:
  explicit EngineRun(const Library &leader) : engine_(leader.shared->engine) { add(leader); }

  bool accepts(const Library &library) const {
    return library.shared && library.shared->stage > last_stage_ && accepts_native(library);
  }

  bool accepts_native(const Library &library) const {
    const vs_module_shared_api *shared = library.shared;
    if (shared == nullptr || std::strcmp(shared->engine, engine_) != 0 ||
        (shared->flags & VS_SHARED_LEADING_ONLY) != 0 || library.metadata.midi_input) {
      return false;
    }
    for (const vs_shared_target &target : library.shared_targets) {
      const RunTarget *existing = find(target.id);
      if (existing == nullptr) {
        continue;
      }
      const vs_shared_target &known = *existing->target;
      if (std::strcmp(known.name, target.name) != 0 || known.minimum != target.minimum ||
          known.maximum != target.maximum) {
        return false;
      }
      if (target.role == VS_SHARED_TARGET_OWNED && (existing->owned || existing->required)) {
        return false;
      }
      if (target.role == VS_SHARED_TARGET_REQUIRED &&
          (existing->owned || (existing->required && required_value(target.id) != target.value))) {
        return false;
      }
    }
    return true;
  }

  void add(const Library &library) {
    last_stage_ = library.shared->stage;
    add_native(library);
  }

  void add_native(const Library &library) {
    for (const vs_shared_target &target : library.shared_targets) {
      RunTarget *existing = find_mutable(target.id);
      if (existing == nullptr) {
        targets_.push_back({&target});
        existing = &targets_.back();
      }
      existing->owned = existing->owned || target.role == VS_SHARED_TARGET_OWNED;
      if (target.role == VS_SHARED_TARGET_REQUIRED && !existing->required) {
        existing->required = true;
        required_.emplace_back(target.id, target.value);
      }
    }
  }

  const std::vector<RunTarget> &targets() const noexcept { return targets_; }
  const std::vector<std::pair<uint32_t, float>> &required() const noexcept { return required_; }

  const RunTarget *find(uint32_t id) const {
    for (const RunTarget &target : targets_) {
      if (target.target->id == id) {
        return &target;
      }
    }
    return nullptr;
  }

private:
  RunTarget *find_mutable(uint32_t id) {
    return const_cast<RunTarget *>(std::as_const(*this).find(id));
  }

  float required_value(uint32_t id) const {
    for (const auto &[target, value] : required_) {
      if (target == id) {
        return value;
      }
    }
    return std::numeric_limits<float>::quiet_NaN();
  }

  const char *engine_;
  uint32_t last_stage_ = 0;
  std::vector<RunTarget> targets_;
  std::vector<std::pair<uint32_t, float>> required_;
};

bool valid_native_assignments(const Library &library, const vs_shared_assignment *values,
                              uint32_t count) noexcept {
  for (uint32_t i = 0; i < count; ++i) {
    const auto &assignment = values[i];
    const auto &targets = library.shared_targets;
    const auto target = std::find_if(targets.begin(), targets.end(),
                                     [&](const auto &t) { return t.id == assignment.target; });
    if (target == targets.end() || target->role != VS_SHARED_TARGET_OWNED ||
        !std::isfinite(assignment.value) || assignment.value < target->minimum ||
        assignment.value > target->maximum ||
        std::any_of(values, values + i,
                    [&](const auto &previous) { return previous.target == assignment.target; })) {
      return false;
    }
  }
  return true;
}

vs_result translate_member(const Node &member, bool native, uint32_t parameter, float value,
                           vs_shared_assignment *out, uint32_t &written) noexcept {
  const Library &library = *member.library;
  const auto translate =
      native ? library.shared_branch->native_translate : library.shared->translate;
  const vs_result result =
      translate(member.instance, parameter, value, out, max_assignments_per_parameter, &written);
  if (result != VS_OK) {
    return result;
  }
  return written <= max_assignments_per_parameter &&
                 (!native || valid_native_assignments(library, out, written))
             ? VS_OK
             : VS_ERROR_PROCESS;
}

bool ranges_overlap(const float *a, const float *b, uint32_t frames) noexcept {
  const std::less<const float *> less;
  return less(a, b + frames) && less(b, a + frames);
}

}

class Graph::Impl {
public:
  ~Impl() {
    release_nodes(nodes_.size());
    for (auto it = nodes_.rbegin(); it != nodes_.rend(); ++it) {
      it->library->api->destroy(it->instance);
      it->instance = nullptr;
    }
    nodes_.clear();
    libraries_.clear();
  }

  NodeId add_module(const std::filesystem::path &binary, const ModuleOptions &options) {
    require_editing("add_module");
    if (nodes_.size() >= max_nodes) {
      throw std::length_error("graph cannot hold more than " + std::to_string(max_nodes) +
                              " nodes");
    }
    if (next_node_id_ == invalid_node_id) {
      throw std::overflow_error("graph node IDs are exhausted");
    }
    const NodeId id = next_node_id_;
    insert_node(binary, id, options);
    ++next_node_id_;
    return id;
  }

  Preset capture_preset() const {
    if (state_ == GraphState::failed) {
      throw std::logic_error("capture_preset requires an editing or prepared graph");
    }
    Preset preset;
    preset.next_instance_id = next_node_id_;
    preset.nodes.reserve(nodes_.size());
    size_t total = 0;
    for (const Node &node : nodes_) {
      const Library &library = *node.library;
      const std::string where = describe_node(library.id, node.id);
      if (library.state == nullptr) {
        throw std::runtime_error(where + " does not support state capture");
      }
      uint64_t size = 0;
      const vs_result sized = library.state->size(node.instance, &size);
      if (sized != VS_OK) {
        throw std::runtime_error(where + " failed to report its state size (" +
                                 std::to_string(sized) + ")");
      }
      if (size > max_module_state_bytes) {
        throw std::length_error(where + " reported " + std::to_string(size) +
                                " state bytes, more than the limit of " +
                                std::to_string(max_module_state_bytes));
      }
      if (size > max_preset_state_bytes - total) {
        throw std::length_error("graph state exceeds the preset limit of " +
                                std::to_string(max_preset_state_bytes) + " bytes");
      }
      total += static_cast<size_t>(size);

      PresetNode captured;
      captured.instance_id = node.id;
      captured.module_id = library.metadata.id;
      captured.module_version = library.metadata.version;
      captured.module_abi_version = library.api->abi_version;
      captured.state.schema_version = library.state->schema_version;
      for (const auto &[slot, file] : node.files) {
        if (file.state != VS_FILE_NONE) {
          captured.files.push_back({slot, file});
          preset.schema_version = 2;
        }
      }
      captured.state.bytes.resize(static_cast<size_t>(size));
      uint8_t empty = 0;
      uint8_t *buffer = size > 0 ? captured.state.bytes.data() : &empty;
      uint64_t written = 0;
      const vs_result saved = library.state->save(node.instance, buffer, size, &written);
      if (saved != VS_OK) {
        throw std::runtime_error(where + " failed to save its state (" + std::to_string(saved) +
                                 ")");
      }
      if (written != size) {
        throw std::runtime_error(where + " wrote " + std::to_string(written) +
                                 " state bytes after reporting " + std::to_string(size));
      }
      preset.nodes.push_back(std::move(captured));
    }
    return preset;
  }

  void restore_node(const PresetNode &preset_node, const std::filesystem::path &binary) {
    Node &node = insert_node(binary, preset_node.instance_id, ModuleOptions{});
    const Library &library = *node.library;
    if (library.metadata.id != preset_node.module_id ||
        library.metadata.version != preset_node.module_version ||
        !restores_saved_module_abi(preset_node.module_abi_version)) {
      throw std::runtime_error(
          "module '" + binary.string() + "' provides '" + library.metadata.id + "' version '" +
          library.metadata.version + "' but node " + std::to_string(preset_node.instance_id) +
          " requires '" + preset_node.module_id + "' version '" + preset_node.module_version + "'");
    }
    load_state(node, preset_node.state);
    for (const auto &file : preset_node.files) {
      set_file(node.id, file.slot, resolve_file_reference(file.reference, max_asset_bytes));
    }
  }

  void load_module_state(NodeId id, const ModuleState &state) {
    require_editing("load_module_state");
    load_state(*find_node(id), state);
  }

  void set_file(NodeId id, uint32_t slot, const FileReference &file) {
    require_editing("set_file");
    Node &node = *find_node(id);
    const auto &slots = node.library->metadata.file_slots;
    const auto found =
        std::find_if(slots.begin(), slots.end(), [slot](const auto &s) { return s.id == slot; });
    auto raw = file.descriptor();
    if (found == slots.end() || !valid_file_reference(file, found->max_bytes) ||
        !display_detail::valid_utf8(file.path) || !display_detail::valid_utf8(file.name)) {
      throw std::invalid_argument("invalid file slot or reference");
    }
    auto [stored, inserted] = node.files.try_emplace(slot);
    FileReference previous = stored->second;
    stored->second = file;
    raw = stored->second.descriptor();
    const auto result = node.library->files->set_file(node.instance, slot, &raw);
    if (result != VS_OK) {
      stored->second = std::move(previous);
      if (inserted) {
        node.files.erase(stored);
      }
      throw std::runtime_error("module rejected file reference (" + std::to_string(result) + ")");
    }
  }

  void set_next_node_id(NodeId next) noexcept { next_node_id_ = next; }

  void remove_module(NodeId id) {
    require_editing("remove_module");
    const auto it = find_node(id);
    std::shared_ptr<Library> library = it->library;
    library->api->destroy(it->instance);
    nodes_.erase(it);
    const bool in_use = std::any_of(nodes_.begin(), nodes_.end(),
                                    [&](const Node &node) { return node.library == library; });
    if (!in_use) {
      libraries_.erase(std::find(libraries_.begin(), libraries_.end(), library));
    }
  }

  void move_module(NodeId id, uint32_t destination) {
    require_editing("move_module");
    const auto it = find_node(id);
    if (destination >= nodes_.size()) {
      throw std::out_of_range("move destination " + std::to_string(destination) +
                              " is outside a graph of " + std::to_string(nodes_.size()) + " nodes");
    }
    const auto target = nodes_.begin() + destination;
    if (it < target) {
      std::rotate(it, it + 1, target + 1);
    } else {
      std::rotate(target, it, it + 1);
    }
  }

  void set_branch(NodeId id, std::optional<Branch> branch) {
    require_editing("set_branch");
    Node &node = *find_node(id);
    if (branch) {
      const Library &library = *node.library;
      const std::string where = describe_node(library.id, node.id);
      if (branch->law != MixLaw::crossfade) {
        throw std::invalid_argument(where + " branch names an unknown mix law");
      }
      const ParameterRange *range = library.find_parameter(branch->mix_parameter);
      if (range == nullptr) {
        throw std::invalid_argument(where + " has no mix parameter " +
                                    std::to_string(branch->mix_parameter));
      }
      if (range->minimum != 0.0f || range->maximum != 1.0f) {
        throw std::invalid_argument(where + " mix parameter " +
                                    std::to_string(branch->mix_parameter) +
                                    " must range from 0 to 1");
      }
      if (library.metadata.bypass_parameter == branch->mix_parameter) {
        throw std::invalid_argument(where + " mix parameter " +
                                    std::to_string(branch->mix_parameter) +
                                    " is its bypass parameter");
      }
      if (library.parameter_values == nullptr) {
        throw std::runtime_error(where + " needs the parameter value extension to run as a branch");
      }
    }
    node.branch = branch;
  }

  std::vector<NodeId> branch_nodes() const {
    std::vector<NodeId> ids;
    for (const Node &node : nodes_) {
      if (node.branch) {
        ids.push_back(node.id);
      }
    }
    return ids;
  }

  std::vector<NodeMetadata> nodes() const {
    std::vector<NodeMetadata> result;
    result.reserve(nodes_.size());
    for (const Node &node : nodes_) {
      result.push_back(NodeMetadata{node.id, node.library->metadata});
    }
    return result;
  }

  uint64_t generation() const noexcept { return generation_; }

  float parameter_value(NodeId id, uint32_t parameter_id) const {
    const Node &node = parameter_node("parameter_value", id, parameter_id);
    const Library &library = *node.library;
    const ParameterRange &range = *library.find_parameter(parameter_id);
    const std::string where = describe_node(library.id, node.id);
    float value = std::numeric_limits<float>::quiet_NaN();
    const vs_result result =
        library.parameter_values->get_value(node.instance, parameter_id, &value);
    if (result != VS_OK) {
      throw std::runtime_error(where + " failed to report parameter " +
                               std::to_string(parameter_id) + " (" + std::to_string(result) + ")");
    }
    if (!std::isfinite(value) || value < range.minimum || value > range.maximum) {
      throw std::runtime_error(where + " reported a value outside the bounds of parameter " +
                               std::to_string(parameter_id));
    }
    return value;
  }

  void set_parameter_value(NodeId id, uint32_t parameter_id, float value) {
    const Node &node = parameter_node("set_parameter_value", id, parameter_id);
    const Library &library = *node.library;
    const ParameterRange &range = *library.find_parameter(parameter_id);
    if (!std::isfinite(value) || value < range.minimum || value > range.maximum) {
      throw std::invalid_argument("value for parameter " + std::to_string(parameter_id) +
                                  " must be finite and within [" + std::to_string(range.minimum) +
                                  ", " + std::to_string(range.maximum) + "]");
    }
    if (node.state == NodeState::hosted) {
      set_hosted_parameter(node, parameter_id, value);
      return;
    }
    const vs_result result =
        library.parameter_values->set_value(node.instance, parameter_id, value);
    if (result != VS_OK) {
      throw std::runtime_error(describe_node(library.id, node.id) + " rejected parameter " +
                               std::to_string(parameter_id) + " value (" + std::to_string(result) +
                               ")");
    }
    if (state_ == GraphState::prepared) {
      const Stage &stage = stages_[stage_of_[static_cast<size_t>(&node - nodes_.data())]];
      if (stage.branch != no_branch) {
        BranchPath &path = branches_[stage.branch];
        if (parameter_id == path.mix_parameter || parameter_id == path.bypass_parameter) {
          path.apply(parameter_id, value);
          path.retarget(true);
        }
      }
    }
  }

  void prepare(const Format &format) {
    require_editing("prepare");
    if (!std::isfinite(format.sample_rate) || format.sample_rate < min_sample_rate ||
        format.sample_rate > max_sample_rate) {
      throw std::invalid_argument("sample rate must be within [1000, 768000] Hz");
    }
    if (format.max_frames == 0 || format.max_frames > max_frames_limit) {
      throw std::invalid_argument("max_frames must be within [1, 65536]");
    }
    if (format.channels != 1 && format.channels != 2) {
      throw std::invalid_argument("graph supports mono or stereo only");
    }
    const uint32_t layout = format.channels == 1 ? VS_LAYOUT_MONO : VS_LAYOUT_STEREO;
    for (const Node &node : nodes_) {
      if ((node.library->api->supported_layouts & layout) == 0) {
        throw std::runtime_error("module '" + node.library->id + "' does not support " +
                                 (format.channels == 1 ? "mono" : "stereo"));
      }
    }

    const uint64_t prepared_generation = reserve_generation();
    std::vector<std::pair<NodeId, uint32_t>> lookup;
    lookup.reserve(nodes_.size());
    for (size_t i = 0; i < nodes_.size(); ++i) {
      lookup.emplace_back(nodes_[i].id, static_cast<uint32_t>(i));
    }
    std::sort(lookup.begin(), lookup.end());

    const size_t samples = static_cast<size_t>(format.channels) * format.max_frames;
    try {
      for (auto &buffer : scratch_) {
        buffer.assign(samples, 0.0f);
      }
    } catch (...) {
      clear_prepared_storage();
      throw;
    }
    for (size_t b = 0; b < scratch_.size(); ++b) {
      for (uint32_t c = 0; c < max_channels; ++c) {
        float *channel = c < format.channels
                             ? scratch_[b].data() + static_cast<size_t>(c) * format.max_frames
                             : nullptr;
        scratch_out_[b][c] = channel;
        scratch_in_[b][c] = channel;
      }
    }

    const vs_prepare_info info{sizeof(vs_prepare_info), format.sample_rate, format.max_frames,
                               format.channels};
    const auto ramp_frames = static_cast<uint32_t>(
        std::max(1.0, std::round(format.sample_rate * Graph::branch_ramp_seconds)));
    const auto max_delay =
        static_cast<uint64_t>(std::floor(format.sample_rate * Graph::max_branch_delay_seconds));
    std::vector<Stage> stages;
    std::vector<uint32_t> stage_of;
    std::vector<BranchPath> branches;
    try {
      stages = plan_stages();
      stage_of.resize(nodes_.size());
      for (size_t s = 0; s < stages.size(); ++s) {
        for (uint32_t i = stages[s].begin; i < stages[s].end; ++i) {
          stage_of[i] = static_cast<uint32_t>(s);
        }
        if (stages[s].branch != no_branch) {
          const Node &node = nodes_[stages[s].begin];
          BranchPath path;
          path.mix_parameter = node.branch->mix_parameter;
          for (const ParameterMetadata &parameter : node.library->metadata.parameters) {
            if (parameter.id == path.mix_parameter) {
              path.mix_display = parameter.display;
              break;
            }
          }
          path.bypass_parameter = node.library->metadata.bypass_parameter;
          path.ramp_length = ramp_frames;
          branches.push_back(std::move(path));
        }
      }
      for (auto *buffer : {&tap_, &branch_output_}) {
        buffer->assign(branches.empty() ? 0 : samples, 0.0f);
      }
      for (uint32_t c = 0; c < max_channels; ++c) {
        const bool used = !branches.empty() && c < format.channels;
        const size_t offset = static_cast<size_t>(c) * format.max_frames;
        tap_out_[c] = used ? tap_.data() + offset : nullptr;
        tap_in_[c] = tap_out_[c];
        branch_out_[c] = used ? branch_output_.data() + offset : nullptr;
        branch_in_[c] = branch_out_[c];
      }
    } catch (...) {
      clear_prepared_storage();
      throw;
    }
    uint64_t latency = 0;
    uint64_t tail = 0;
    for (const Stage &stage : stages) {
      Node &node = nodes_[stage.begin];
      vs_module_timing timing{sizeof(vs_module_timing), 0, 0};
      node.state = NodeState::prepared;
      vs_result result = VS_OK;
      try {
        std::optional<HostServices::Client::Scope> scope;
        if (node.services) {
          scope.emplace(*node.services, HostServices::Client::Call::prepare);
        }
        result = stage.engine ? prepare_engine(stage, info, timing)
                              : node.library->api->prepare(node.instance, &info, &timing);
      } catch (...) {
        abort_prepare(nodes_.size());
        throw;
      }
      if (result != VS_OK) {
        abort_prepare(nodes_.size());
        throw std::runtime_error(
            "module '" + node.library->id + "' at node " + std::to_string(node.id) +
            " failed to prepare" +
            (stage.end - stage.begin > 1
                 ? " a shared engine for " + std::to_string(stage.end - stage.begin) + " modules"
                 : std::string()) +
            " (" + std::to_string(result) + ")");
      }
      for (uint32_t i = stage.begin + 1; i < stage.end; ++i) {
        nodes_[i].state = NodeState::hosted;
      }
      if (stage.branch != no_branch) {
        BranchPath &path = branches[stage.branch];
        const std::string where = describe_node(node.library->id, node.id);
        try {
          read_branch_values(node, path);
        } catch (...) {
          abort_prepare(nodes_.size());
          throw;
        }
        const uint64_t branch_latency = timing.latency_frames;
        path.branch_delay = static_cast<uint32_t>(std::min<uint64_t>(
            latency > branch_latency ? latency - branch_latency : 0, max_delay + 1));
        path.main_delay = static_cast<uint32_t>(std::min<uint64_t>(
            branch_latency > latency ? branch_latency - latency : 0, max_delay + 1));
        if (path.branch_delay > max_delay || path.main_delay > max_delay) {
          abort_prepare(nodes_.size());
          throw std::runtime_error(where + " needs an alignment delay longer than " +
                                   std::to_string(Graph::max_branch_delay_seconds) + " seconds");
        }
        tail = std::max(saturating_add(tail, path.main_delay),
                        saturating_add(timing.tail_frames, path.branch_delay));
        latency = std::max(latency, branch_latency);
      } else {
        latency += timing.latency_frames;
        tail = saturating_add(tail, timing.tail_frames);
      }
      if (latency > std::numeric_limits<uint32_t>::max()) {
        abort_prepare(nodes_.size());
        throw std::overflow_error("graph latency exceeds the representable frame count");
      }
    }
    try {
      for (BranchPath &path : branches) {
        path.main_line.assign(static_cast<size_t>(path.main_delay) * format.channels, 0.0f);
        path.branch_line.assign(static_cast<size_t>(path.branch_delay) * format.channels, 0.0f);
      }
    } catch (...) {
      abort_prepare(nodes_.size());
      throw;
    }

    branches_ = std::move(branches);
    stages_ = std::move(stages);
    stage_of_ = std::move(stage_of);
    format_ = format;
    latency_ = static_cast<uint32_t>(latency);
    tail_ = tail;
    node_lookup_ = std::move(lookup);
    generation_ = prepared_generation;
    state_ = GraphState::prepared;
  }

  void release() {
    if (state_ == GraphState::editing) {
      return;
    }
    release_nodes(nodes_.size());
    clear_prepared_storage();
    state_ = GraphState::editing;
  }

  void reset() {
    if (state_ != GraphState::prepared) {
      throw std::logic_error("reset requires a prepared graph");
    }
    for (Node &node : nodes_) {
      if (node.state == NodeState::prepared) {
        node.library->api->reset(node.instance);
      }
    }
    for (BranchPath &path : branches_) {
      path.clear();
    }
  }

  void set_sharing(bool enabled) {
    require_editing("set_sharing");
    sharing_ = enabled;
  }

  void set_instance_cache(std::shared_ptr<InstanceCache> cache) {
    require_editing("set_instance_cache");
    cache_ = std::move(cache);
  }

  void set_host_services(std::shared_ptr<HostServices> services) {
    require_editing("set_host_services");
    if (!nodes_.empty() && services != services_) {
      throw std::logic_error("set_host_services requires a graph without nodes");
    }
    services_ = std::move(services);
  }

  bool wait_for_work(std::chrono::steady_clock::time_point deadline) const {
    if (!services_) {
      return true;
    }
    std::vector<const HostServices::Client *> clients;
    clients.reserve(nodes_.size());
    for (const Node &node : nodes_) {
      if (node.services) {
        clients.push_back(node.services.get());
      }
    }
    return services_->wait_idle(clients, deadline);
  }

  std::vector<ModuleNotice> take_notices() {
    std::vector<ModuleNotice> notices;
    for (const Node &node : nodes_) {
      if (node.services) {
        if (const uint32_t what = node.services->take_notices()) {
          notices.push_back({node.id, what});
        }
      }
    }
    return notices;
  }

  std::optional<ModuleStatus> module_status(NodeId id) const {
    const Node &node = *find_node(id);
    for (const auto &slot : node.library->metadata.file_slots) {
      const auto file = node.files.find(slot.id);
      if ((file == node.files.end() || file->second.state == VS_FILE_NONE) && !slot.optional) {
        return ModuleStatus{id, VS_STATUS_ERROR, -1.0f, slot.id, "Choose a file for " + slot.name};
      }
      if (file != node.files.end() && file->second.state == VS_FILE_MISSING) {
        return ModuleStatus{id, VS_STATUS_ERROR, -1.0f, slot.id,
                            file->second.name + " is missing or changed; Locate it to load"};
      }
    }
    if (!node.services) {
      return std::nullopt;
    }
    return read_status(node);
  }

  std::vector<NodeId> node_ids() const {
    std::vector<NodeId> ids;
    ids.reserve(nodes_.size());
    for (const auto &node : nodes_) {
      ids.push_back(node.id);
    }
    return ids;
  }

  std::vector<std::vector<NodeId>> processing_groups() const {
    std::vector<std::vector<NodeId>> groups;
    if (state_ != GraphState::prepared) {
      return groups;
    }
    for (const Stage &stage : stages_) {
      std::vector<NodeId> ids;
      for (uint32_t i = stage.begin; i < stage.end; ++i) {
        ids.push_back(nodes_[i].id);
      }
      groups.push_back(std::move(ids));
    }
    return groups;
  }

  Format prepared_format() const noexcept {
    return state_ == GraphState::editing ? Format{0.0, 0, 0} : format_;
  }

  vs_result process(const Block &block, std::span<const Event> events) noexcept {
    if (state_ != GraphState::prepared) {
      silence(block);
      return VS_ERROR_STATE;
    }
    if (!valid_block(block) || !resolve_events(block, events)) {
      silence(block);
      return VS_ERROR_ARGUMENT;
    }

    const uint32_t channels = format_.channels;
    const uint32_t frames = block.frames;
    const size_t bytes = static_cast<size_t>(frames) * sizeof(float);

    if (nodes_.empty()) {
      if (inputs_overlap_outputs(block, true)) {
        stage_inputs(block);
        for (uint32_t c = 0; c < channels; ++c) {
          std::memcpy(block.outputs[c], scratch_in_[0][c], bytes);
        }
      } else {
        for (uint32_t c = 0; c < channels; ++c) {
          if (block.outputs[c] != block.inputs[c]) {
            std::memcpy(block.outputs[c], block.inputs[c], bytes);
          }
        }
      }
      return VS_OK;
    }

    const float *const *source = block.inputs;
    bool source_is_scratch_zero = false;
    if (!branches_.empty()) {
      if ((block.flags & VS_PROCESS_DISCONTINUITY) != 0) {
        for (BranchPath &path : branches_) {
          path.clear();
        }
      }
      for (uint32_t c = 0; c < channels; ++c) {
        std::memcpy(tap_out_[c], block.inputs[c], bytes);
      }
      source = tap_in_.data();
    } else if (stages_.size() == 1 && inputs_overlap_outputs(block, false)) {
      stage_inputs(block);
      source = scratch_in_[0].data();
      source_is_scratch_zero = true;
    }

    for (size_t s = 0; s < stages_.size(); ++s) {
      const Stage &stage = stages_[s];
      const size_t i = stage.begin;
      const bool last = s + 1 == stages_.size();
      const size_t target_index = source_is_scratch_zero ? 1 : 0;
      float *const *target = last ? block.outputs : scratch_out_[target_index].data();

      uint32_t count = 0;
      uint32_t midi_count = 0;
      uint32_t shared_count = 0;
      for (size_t e = 0; e < events.size(); ++e) {
        const uint32_t node_index = event_targets_[e];
        if (stage_of_[node_index] != s) {
          continue;
        }
        if (node_index == i) {
          if (events[e].kind == Event::Kind::midi) {
            node_midi_[midi_count++] = events[e].midi;
          } else {
            node_events_[count++] = events[e].parameter;
          }
          continue;
        }
        const Node &member = nodes_[node_index];
        std::optional<HostServices::Client::Scope> member_scope;
        if (member.services) {
          member_scope.emplace(*member.services, HostServices::Client::Call::process);
        }
        std::array<vs_shared_assignment, max_assignments_per_parameter> changed{};
        uint32_t written = 0;
        const vs_parameter_event &parameter = events[e].parameter;
        if (translate_member(member, node_index == stage.absorbed_node, parameter.parameter_id,
                             parameter.value, changed.data(), written) != VS_OK) {
          fail(block);
          return VS_ERROR_PROCESS;
        }
        for (uint32_t a = 0; a < written; ++a) {
          shared_events_[shared_count++] = {changed[a].target, parameter.frame_offset,
                                            changed[a].value};
        }
      }

      BranchPath *path = stage.branch != no_branch ? &branches_[stage.branch] : nullptr;
      float *const *module_target = path ? branch_out_.data() : target;
      const vs_process_block module_block{sizeof(vs_process_block),
                                          path ? tap_in_.data() : source,
                                          module_target,
                                          channels,
                                          frames,
                                          block.frame_position,
                                          nodes_[i].library->metadata.midi_input
                                              ? block.flags
                                              : block.flags & ~uint32_t(VS_PROCESS_MIDI_PANIC),
                                          count > 0 ? node_events_.data() : nullptr,
                                          count,
                                          stage.tempo ? block.tempo : nullptr,
                                          node_midi_.data(),
                                          midi_count,
                                          sizeof(vs_midi_event)};
      const Node &node = nodes_[i];
      std::optional<HostServices::Client::Scope> scope;
      if (node.services) {
        scope.emplace(*node.services, HostServices::Client::Call::process);
      }
      const vs_result result =
          stage.engine ? node.library->shared->engine_process(
                             node.instance, &module_block,
                             shared_count > 0 ? shared_events_.data() : nullptr, shared_count)
                       : node.library->api->process(node.instance, &module_block);
      if (result != VS_OK || !detail::finite_samples(module_target, channels, frames)) {
        fail(block);
        return VS_ERROR_PROCESS;
      }
      if (path) {
        graph_branch::merge(*path, source, branch_in_.data(), target, channels, frames,
                            node_events_.data(), count);
      }
      source = last ? nullptr : scratch_in_[target_index].data();
      source_is_scratch_zero = target_index == 0;
    }
    return VS_OK;
  }

  GraphState state() const noexcept { return state_; }
  uint32_t latency_frames() const noexcept { return latency_; }
  uint64_t tail_frames() const noexcept { return tail_; }
  uint32_t node_count() const noexcept { return static_cast<uint32_t>(nodes_.size()); }

private:
  void require_editing(const char *operation) const {
    if (state_ != GraphState::editing) {
      throw std::logic_error(std::string(operation) +
                             " requires an editing graph; release it first");
    }
  }

  void load_state(Node &node, const ModuleState &state) {
    const Library &library = *node.library;
    const std::string where = describe_node(library.id, node.id);
    if (library.state == nullptr) {
      throw std::runtime_error(where + " does not support state restore");
    }
    if (state.bytes.size() > max_module_state_bytes) {
      throw std::length_error(where + " state exceeds the limit of " +
                              std::to_string(max_module_state_bytes) + " bytes");
    }
    const std::vector<uint8_t> &bytes = state.bytes;
    const uint8_t empty = 0;
    const vs_result loaded = library.state->load(
        node.instance, state.schema_version, bytes.empty() ? &empty : bytes.data(), bytes.size());
    if (loaded == VS_ERROR_UNSUPPORTED) {
      throw std::runtime_error(where + " does not support state schema version " +
                               std::to_string(state.schema_version));
    }
    if (loaded != VS_OK) {
      throw std::runtime_error(where + " rejected its state (" + std::to_string(loaded) + ")");
    }
  }

  std::vector<Node>::iterator find_node(NodeId id) {
    return nodes_.begin() + (std::as_const(*this).find_node(id) - nodes_.cbegin());
  }

  std::vector<Node>::const_iterator find_node(NodeId id) const {
    const auto it = std::find_if(nodes_.begin(), nodes_.end(),
                                 [id](const Node &node) { return node.id == id; });
    if (it == nodes_.end()) {
      throw std::invalid_argument("graph has no node with ID " + std::to_string(id));
    }
    return it;
  }

  const Node &parameter_node(const char *operation, NodeId id, uint32_t parameter_id) const {
    if (state_ == GraphState::failed) {
      throw std::logic_error(std::string(operation) + " requires an editing or prepared graph");
    }
    const Node &node = *find_node(id);
    const Library &library = *node.library;
    if (library.find_parameter(parameter_id) == nullptr) {
      throw std::invalid_argument(describe_node(library.id, node.id) + " has no parameter " +
                                  std::to_string(parameter_id));
    }
    if (library.parameter_values == nullptr) {
      throw std::runtime_error(describe_node(library.id, node.id) +
                               " does not support parameter value access");
    }
    return node;
  }

  std::shared_ptr<Library> find_library(const std::filesystem::path &path) const {
    for (const auto &library : libraries_) {
      if (library->path == path) {
        return library;
      }
    }
    return nullptr;
  }

  std::shared_ptr<Library> find_library(void *handle) const {
    for (const auto &library : libraries_) {
      if (library->handle.get() == handle) {
        return library;
      }
    }
    return nullptr;
  }

  std::shared_ptr<Library> load_library(const std::filesystem::path &path) {
    std::shared_ptr<Library> library = find_library(path);
    if (library == nullptr) {
      LibraryHandle handle = open_library(path);
      library = find_library(handle.get());
      if (library == nullptr) {
        library = make_library(path, std::move(handle));
        for (const auto &existing : libraries_) {
          if (existing->id == library->id &&
              existing->metadata.version == library->metadata.version) {
            load_failure(path, "module ID '" + existing->id + "' is already provided by '" +
                                   existing->path.string() + "'");
          }
        }
      }
    }
    return library;
  }

  Node &insert_node(const std::filesystem::path &binary, NodeId id, const ModuleOptions &options) {
    const std::filesystem::path path = canonical_module_path(binary);
    std::shared_ptr<Library> library = load_library(path);
    std::string resources;
    if (!options.resource_directory.empty()) {
      if (!options.resource_directory.is_absolute()) {
        throw std::invalid_argument("a module's resource directory must be an absolute path");
      }
      resources = options.resource_directory.string();
      if (!display_detail::valid_utf8(resources) || resources.find('\0') != std::string::npos) {
        throw std::invalid_argument("a module's resource directory must be UTF-8");
      }
    }
    std::shared_ptr<HostServices::Client> services;
    if (services_ && library->services) {
      services = services_->client(library, library->id, std::move(resources));
    }

    nodes_.reserve(nodes_.size() + 1);
    void *instance = nullptr;
    const vs_result result = library->api->create(&instance);
    if (result != VS_OK || instance == nullptr) {
      throw std::runtime_error("module '" + library->id + "' failed to create an instance (" +
                               std::to_string(result) + ")");
    }
    if (services) {
      vs_result attached = VS_ERROR_PROCESS;
      {
        HostServices::Client::Scope scope(*services, HostServices::Client::Call::attach);
        attached = library->services->attach(instance, services->descriptor());
      }
      if (attached != VS_OK) {
        library->api->destroy(instance);
        throw std::runtime_error("module '" + library->id + "' rejected host services (" +
                                 std::to_string(attached) + ")");
      }
    }
    if (std::find(libraries_.begin(), libraries_.end(), library) == libraries_.end()) {
      try {
        libraries_.push_back(library);
      } catch (...) {
        library->api->destroy(instance);
        throw;
      }
    }
    nodes_.push_back(Node{id,
                          std::move(library),
                          instance,
                          NodeState::created,
                          nullptr,
                          std::move(services),
                          {},
                          std::nullopt});
    return nodes_.back();
  }

  // Release closes the work a module opened in its preparation, so no module code of the
  // instance runs once it returns; the host closes works the module left open.
  void release_nodes(size_t count) noexcept {
    for (size_t i = count; i-- > 0;) {
      Node &node = nodes_[i];
      if (node.state == NodeState::prepared) {
        if (node.services) {
          HostServices::Client::Scope scope(*node.services, HostServices::Client::Call::release);
          node.library->api->release(node.instance);
          node.services->close_all();
        } else {
          node.library->api->release(node.instance);
        }
      }
      node.state = NodeState::created;
    }
  }

  static constexpr uint32_t no_branch = std::numeric_limits<uint32_t>::max();

  struct Stage {
    uint32_t begin = 0;
    uint32_t end = 0;
    // Prepared and processed through the leader's shared engine functions.
    bool engine = false;
    // Some module of the stage declares VS_MODULE_HOST_TEMPO, so the leader receives the tempo.
    bool tempo = false;
    // Index into branches_ when the stage is a single branch node; no_branch otherwise.
    uint32_t branch = no_branch;
    uint32_t absorbed_node = no_branch;
  };

  std::optional<Stage> plan_native_branch() const {
    if (!sharing_ || nodes_.empty() || nodes_[0].branch || !nodes_[0].library->shared) {
      return std::nullopt;
    }
    const auto count = static_cast<uint32_t>(nodes_.size());
    uint32_t member = no_branch;
    for (uint32_t i = 1; i < count; ++i) {
      if (nodes_[i].branch) {
        if (member != no_branch) {
          return std::nullopt;
        }
        member = i;
      }
    }
    if (member == no_branch) {
      return std::nullopt;
    }
    const Node &branch = nodes_[member];
    const auto *native = branch.library->shared_branch;
    if (!native || branch.branch->mix_parameter != native->mix_parameter ||
        branch.branch->law != MixLaw::crossfade) {
      return std::nullopt;
    }
    EngineRun run(*nodes_[0].library);
    bool tempo = false;
    for (uint32_t i = 0; i < count; ++i) {
      const Library &library = *nodes_[i].library;
      if (!library.shared) {
        return std::nullopt;
      }
      tempo |= library.metadata.host_tempo;
      if (i == member) {
        if (!run.accepts_native(library)) {
          return std::nullopt;
        }
        run.add_native(library);
        continue;
      }
      const uint32_t stage = library.shared->stage;
      if (stage == branch.library->shared->stage ||
          (stage < native->first_after_stage) != (i < member)) {
        return std::nullopt;
      }
      if (i != 0) {
        if (!run.accepts(library)) {
          return std::nullopt;
        }
        run.add(library);
      }
    }
    if (native->flags & VS_SHARED_BRANCH_HAS_SELECTOR) {
      float value = std::numeric_limits<float>::quiet_NaN();
      if (branch.library->parameter_values->get_value(branch.instance, native->selector_parameter,
                                                      &value) != VS_OK ||
          value != native->selector_value) {
        return std::nullopt;
      }
    }
    return Stage{0, count, true, tempo, no_branch, member};
  }

  std::vector<Stage> plan_stages() const {
    if (auto native = plan_native_branch()) {
      return {*native};
    }
    std::vector<Stage> stages;
    const auto count = static_cast<uint32_t>(nodes_.size());
    uint32_t branches = 0;
    for (uint32_t i = 0; i < count;) {
      const Library &leader = *nodes_[i].library;
      const bool branch = nodes_[i].branch.has_value();
      uint32_t end = i + 1;
      if (leader.shared != nullptr && sharing_ && !branch) {
        EngineRun run(leader);
        while (end < count && !nodes_[end].branch && run.accepts(*nodes_[end].library)) {
          run.add(*nodes_[end].library);
          ++end;
        }
      }
      const bool engine = leader.shared != nullptr && (end - i > 1 || cache_ != nullptr);
      const bool tempo =
          std::any_of(nodes_.begin() + i, nodes_.begin() + end,
                      [](const Node &node) { return node.library->metadata.host_tempo; });
      stages.push_back({i, end, engine, tempo, branch ? branches++ : no_branch});
      i = end;
    }
    return stages;
  }

  void read_branch_values(const Node &node, BranchPath &path) const {
    const Library &library = *node.library;
    auto read = [&](uint32_t parameter_id) {
      float value = std::numeric_limits<float>::quiet_NaN();
      const vs_result result =
          library.parameter_values->get_value(node.instance, parameter_id, &value);
      if (result != VS_OK || !std::isfinite(value) || value < 0.0f || value > 1.0f) {
        throw std::runtime_error(describe_node(library.id, node.id) +
                                 " failed to report branch parameter " +
                                 std::to_string(parameter_id) + " within [0, 1]");
      }
      return value;
    };
    path.mix = path.mix_display.nearest(read(path.mix_parameter));
    path.on = !path.bypass_parameter || read(*path.bypass_parameter) >= 0.5f;
    path.retarget(true);
  }

  // Forms the engine state documented for vs_module_shared_api and prepares the run's leader.
  vs_result prepare_engine(const Stage &stage, const vs_prepare_info &info,
                           vs_module_timing &timing) {
    Node &leader = nodes_[stage.begin];
    EngineRun run(*leader.library);
    for (uint32_t i = stage.begin + 1; i < stage.end; ++i) {
      if (i == stage.absorbed_node) {
        run.add_native(*nodes_[i].library);
      } else {
        run.add(*nodes_[i].library);
      }
    }
    std::vector<vs_shared_target> targets;
    targets.reserve(run.targets().size());
    for (const RunTarget &target : run.targets()) {
      targets.push_back(*target.target);
    }
    std::vector<vs_shared_assignment> state;
    std::vector<uint32_t> assigned;
    uint32_t flags = stage.tempo ? uint32_t(VS_SHARED_HOST_TEMPO) : 0u;
    std::string signature;
    if (stage.absorbed_node != no_branch) {
      const auto &native = *nodes_[stage.absorbed_node].library->shared_branch;
      signature = "native-branch/v1/index=" + std::to_string(stage.absorbed_node) +
                  "/fence=" + std::to_string(native.first_after_stage) + ":";
    }
    for (uint32_t i = stage.begin; i < stage.end; ++i) {
      const Library &library = *nodes_[i].library;
      flags |= library.shared->flags;
      signature += (i == stage.begin ? "" : ",") + library.id;
      for (const vs_shared_target &target : library.shared_targets) {
        const RunTarget &merged = *run.find(target.id);
        if (target.role != VS_SHARED_TARGET_NEUTRAL || merged.owned || merged.required ||
            std::find(assigned.begin(), assigned.end(), target.id) != assigned.end()) {
          continue;
        }
        assigned.push_back(target.id);
        state.push_back({target.id, target.value});
      }
    }
    for (const auto &[target, value] : run.required()) {
      state.push_back({target, value});
    }
    for (uint32_t i = stage.begin; i < stage.end; ++i) {
      const Node &node = nodes_[i];
      const Library &library = *node.library;
      std::vector<vs_shared_assignment> owned(library.owned_targets);
      uint32_t written = 0;
      const bool native = i == stage.absorbed_node;
      const auto assignments =
          native ? library.shared_branch->native_assignments : library.shared->assignments;
      const vs_result result =
          assignments(node.instance, owned.data(), static_cast<uint32_t>(owned.size()), &written);
      if (result != VS_OK) {
        return result;
      }
      if (written > owned.size() ||
          (native && (written != owned.size() ||
                      !valid_native_assignments(library, owned.data(), written)))) {
        return VS_ERROR_PROCESS;
      }
      for (uint32_t a = 0; a < written; ++a) {
        const RunTarget *merged = run.find(owned[a].target);
        if (merged == nullptr || !merged->owned) {
          return VS_ERROR_PROCESS;
        }
        state.push_back(owned[a]);
      }
    }
    leader.cache =
        cache_ && stage.absorbed_node == no_branch ? cache_->client(leader.library) : nullptr;
    return leader.library->shared->engine_prepare(
        leader.instance, &info, leader.cache.get(), signature.c_str(), flags, targets.data(),
        static_cast<uint32_t>(targets.size()), sizeof(vs_shared_target), state.data(),
        static_cast<uint32_t>(state.size()), &timing);
  }

  // translate makes the value current in the member, so a failed engine update is undone by
  // translating and applying the previous value, which also restores targets the engine applied
  // before failing.
  void set_hosted_parameter(const Node &node, uint32_t parameter_id, float value) {
    const auto index = static_cast<size_t>(&node - nodes_.data());
    const Stage &stage = stages_[stage_of_[index]];
    const Node &leader = nodes_[stage.begin];
    const std::string where = describe_node(node.library->id, node.id);
    float previous = 0.0f;
    if (node.library->parameter_values->get_value(node.instance, parameter_id, &previous) !=
        VS_OK) {
      throw std::runtime_error(where + " failed to report parameter " +
                               std::to_string(parameter_id));
    }
    auto update = [&](float target_value, vs_result &applied) -> vs_result {
      std::array<vs_shared_assignment, max_assignments_per_parameter> changed{};
      uint32_t written = 0;
      const vs_result translated = translate_member(
          node, index == stage.absorbed_node, parameter_id, target_value, changed.data(), written);
      if (translated != VS_OK) {
        applied = VS_ERROR_PROCESS;
        return translated;
      }
      applied = leader.library->shared->engine_apply(leader.instance, changed.data(), written);
      return VS_OK;
    };
    vs_result applied = VS_OK;
    const vs_result translated = update(value, applied);
    if (translated == VS_OK && applied == VS_OK) {
      return;
    }
    vs_result restored = VS_OK;
    const vs_result retranslated = update(previous, restored);
    if (retranslated != VS_OK || restored != VS_OK) {
      throw std::runtime_error(where + " could not apply parameter " +
                               std::to_string(parameter_id) +
                               " to its shared engine and could not restore the previous value");
    }
    throw std::runtime_error(where + " rejected parameter " + std::to_string(parameter_id) +
                             " value (" +
                             std::to_string(translated != VS_OK ? translated : applied) + ")");
  }

  void abort_prepare(size_t attempted) noexcept {
    release_nodes(attempted);
    clear_prepared_storage();
  }

  void clear_prepared_storage() noexcept {
    for (auto &buffer : scratch_) {
      buffer.clear();
      buffer.shrink_to_fit();
    }
    for (size_t b = 0; b < scratch_.size(); ++b) {
      scratch_out_[b].fill(nullptr);
      scratch_in_[b].fill(nullptr);
    }
    format_ = Format{};
    latency_ = 0;
    tail_ = 0;
    node_lookup_.clear();
    node_lookup_.shrink_to_fit();
    stages_.clear();
    stage_of_.clear();
    branches_.clear();
    branches_.shrink_to_fit();
    for (auto *buffer : {&tap_, &branch_output_}) {
      buffer->clear();
      buffer->shrink_to_fit();
    }
    tap_out_.fill(nullptr);
    tap_in_.fill(nullptr);
    branch_out_.fill(nullptr);
    branch_in_.fill(nullptr);
    generation_ = 0;
  }

  bool valid_block(const Block &block) const noexcept {
    if (block.channels != format_.channels || block.frames == 0 ||
        block.frames > format_.max_frames || block.inputs == nullptr || block.outputs == nullptr ||
        (block.flags & ~known_flags) != 0) {
      return false;
    }
    for (uint32_t c = 0; c < block.channels; ++c) {
      if (block.inputs[c] == nullptr || block.outputs[c] == nullptr) {
        return false;
      }
      for (uint32_t d = 0; d < c; ++d) {
        if (ranges_overlap(block.outputs[c], block.outputs[d], block.frames)) {
          return false;
        }
      }
    }
    for (uint32_t c = 0; c < block.channels; ++c) {
      if (!std::all_of(block.inputs[c], block.inputs[c] + block.frames,
                       [](float sample) { return std::isfinite(sample); })) {
        return false;
      }
    }
    return true;
  }

  bool resolve_events(const Block &block, std::span<const Event> events) noexcept {
    if (events.size() > max_events_per_block) {
      return false;
    }
    uint32_t previous_offset = 0;
    for (size_t e = 0; e < events.size(); ++e) {
      const Event &event = events[e];
      const vs_parameter_event &parameter = event.parameter;
      if (event.frame_offset() >= block.frames || event.frame_offset() < previous_offset) {
        return false;
      }
      const auto target = std::lower_bound(
          node_lookup_.begin(), node_lookup_.end(), event.node,
          [](const std::pair<NodeId, uint32_t> &entry, NodeId id) { return entry.first < id; });
      if (target == node_lookup_.end() || target->first != event.node) {
        return false;
      }
      event_targets_[e] = target->second;
      previous_offset = event.frame_offset();
      if (event.kind == Event::Kind::midi) {
        if (!nodes_[target->second].library->metadata.midi_input ||
            !vs_midi_event_valid(&event.midi)) {
          return false;
        }
        continue;
      }
      if (event.kind != Event::Kind::parameter || !std::isfinite(parameter.value)) {
        return false;
      }
      const ParameterRange *range =
          nodes_[target->second].library->find_parameter(parameter.parameter_id);
      if (range == nullptr || parameter.value < range->minimum ||
          parameter.value > range->maximum) {
        return false;
      }
      previous_offset = parameter.frame_offset;
    }
    return true;
  }

  bool inputs_overlap_outputs(const Block &block, bool allow_identical) const noexcept {
    for (uint32_t c = 0; c < block.channels; ++c) {
      for (uint32_t d = 0; d < block.channels; ++d) {
        if (allow_identical && c == d && block.inputs[c] == block.outputs[d]) {
          continue;
        }
        if (ranges_overlap(block.inputs[c], block.outputs[d], block.frames)) {
          return true;
        }
      }
    }
    return false;
  }

  void stage_inputs(const Block &block) noexcept {
    for (uint32_t c = 0; c < block.channels; ++c) {
      std::memcpy(scratch_out_[0][c], block.inputs[c],
                  static_cast<size_t>(block.frames) * sizeof(float));
    }
  }

  void silence(const Block &block) const noexcept {
    if (block.outputs == nullptr) {
      return;
    }
    const uint32_t frame_limit =
        state_ == GraphState::editing ? max_frames_limit : format_.max_frames;
    const uint32_t frames = std::min(block.frames, frame_limit);
    const uint32_t channels = std::min(block.channels, max_channels);
    for (uint32_t c = 0; c < channels; ++c) {
      if (block.outputs[c] != nullptr) {
        std::fill_n(block.outputs[c], frames, 0.0f);
      }
    }
  }

  void fail(const Block &block) noexcept {
    silence(block);
    state_ = GraphState::failed;
  }

  GraphState state_ = GraphState::editing;
  Format format_{};
  uint32_t latency_ = 0;
  uint64_t tail_ = 0;
  uint64_t generation_ = 0;
  NodeId next_node_id_ = 0;
  bool sharing_ = false;
  std::shared_ptr<InstanceCache> cache_;
  std::shared_ptr<HostServices> services_;
  std::vector<std::shared_ptr<Library>> libraries_;
  std::vector<Node> nodes_;
  std::vector<Stage> stages_;
  std::vector<uint32_t> stage_of_;
  std::vector<BranchPath> branches_;
  // The graph input as every branch reads it, copied before the first stage.
  std::vector<float> tap_;
  std::vector<float> branch_output_;
  std::array<float *, max_channels> tap_out_{};
  std::array<const float *, max_channels> tap_in_{};
  std::array<float *, max_channels> branch_out_{};
  std::array<const float *, max_channels> branch_in_{};
  std::array<vs_parameter_event, max_events_per_block * max_assignments_per_parameter>
      shared_events_{};
  std::vector<std::pair<NodeId, uint32_t>> node_lookup_;
  std::array<std::vector<float>, 2> scratch_;
  std::array<std::array<float *, max_channels>, 2> scratch_out_{};
  std::array<std::array<const float *, max_channels>, 2> scratch_in_{};
  std::array<vs_parameter_event, max_events_per_block> node_events_{};
  std::array<vs_midi_event, max_events_per_block> node_midi_{};
  std::array<uint32_t, max_events_per_block> event_targets_{};
};

Graph::Graph() : impl_(std::make_unique<Impl>()) {}

Graph::~Graph() = default;

NodeId Graph::add_module(const std::filesystem::path &binary, const ModuleOptions &options) {
  return impl_->add_module(binary, options);
}

void Graph::remove_module(NodeId node) { impl_->remove_module(node); }

void Graph::move_module(NodeId node, uint32_t destination) {
  impl_->move_module(node, destination);
}

void Graph::set_branch(NodeId node, std::optional<Branch> branch) {
  impl_->set_branch(node, branch);
}

std::vector<NodeId> Graph::branch_nodes() const { return impl_->branch_nodes(); }

void Graph::prepare(const Format &format) { impl_->prepare(format); }

void Graph::release() { impl_->release(); }

void Graph::reset() { impl_->reset(); }

vs_result Graph::process(const Block &block, std::span<const Event> events) noexcept {
  return impl_->process(block, events);
}

GraphState Graph::state() const noexcept { return impl_->state(); }

uint32_t Graph::latency_frames() const noexcept { return impl_->latency_frames(); }

uint64_t Graph::tail_frames() const noexcept { return impl_->tail_frames(); }

uint32_t Graph::node_count() const noexcept { return impl_->node_count(); }

void Graph::set_sharing(bool enabled) { impl_->set_sharing(enabled); }

void Graph::set_instance_cache(std::shared_ptr<InstanceCache> cache) {
  impl_->set_instance_cache(std::move(cache));
}

void Graph::set_host_services(std::shared_ptr<HostServices> services) {
  impl_->set_host_services(std::move(services));
}

bool Graph::wait_for_work(std::chrono::steady_clock::time_point deadline) const {
  return impl_->wait_for_work(deadline);
}

std::vector<ModuleNotice> Graph::take_notices() { return impl_->take_notices(); }

std::optional<ModuleStatus> Graph::module_status(NodeId node) const {
  return impl_->module_status(node);
}

std::vector<NodeId> Graph::node_ids() const { return impl_->node_ids(); }

std::vector<std::vector<NodeId>> Graph::processing_groups() const {
  return impl_->processing_groups();
}

Format Graph::prepared_format() const noexcept { return impl_->prepared_format(); }

std::vector<NodeMetadata> Graph::nodes() const { return impl_->nodes(); }

uint64_t Graph::generation() const noexcept { return impl_->generation(); }

Preset Graph::capture_preset() const { return impl_->capture_preset(); }

float Graph::parameter_value(NodeId node, uint32_t parameter_id) const {
  return impl_->parameter_value(node, parameter_id);
}

void Graph::set_parameter_value(NodeId node, uint32_t parameter_id, float value) {
  impl_->set_parameter_value(node, parameter_id, value);
}

void Graph::load_module_state(NodeId node, const ModuleState &state) {
  impl_->load_module_state(node, state);
}

void Graph::set_file(NodeId node, uint32_t slot, const FileReference &file) {
  impl_->set_file(node, slot, file);
}

std::unique_ptr<Graph> Graph::restore_preset(const Preset &preset, const ModuleCatalog &catalog,
                                             std::shared_ptr<HostServices> services) {
  validate_preset(preset);
  std::vector<std::filesystem::path> binaries;
  binaries.reserve(preset.nodes.size());
  for (const PresetNode &node : preset.nodes) {
    binaries.push_back(catalog.resolve(node));
  }
  auto graph = std::make_unique<Graph>();
  graph->impl_->set_host_services(std::move(services));
  for (size_t i = 0; i < preset.nodes.size(); ++i) {
    graph->impl_->restore_node(preset.nodes[i], binaries[i]);
  }
  graph->impl_->set_next_node_id(preset.next_instance_id);
  return graph;
}

}
