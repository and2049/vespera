#include <vespera/audio_session.hpp>
#include <vespera/core_io.hpp>
#include <vespera/file_content.hpp>
#include <vespera/host_services.hpp>
#include <vespera/preset.hpp>
#include <vespera/runtime.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

namespace {

constexpr int exit_ok = 0;
constexpr int exit_failure = 1;
constexpr int exit_usage = 2;
constexpr int exit_shutdown_failure = 3;

constexpr double max_duration_seconds = 3600.0;
constexpr uint32_t max_render_frames = 1u << 24;
constexpr uint32_t max_sample_rate = 768000;
constexpr uint32_t max_block_frames = 65536;
constexpr uint32_t max_channel_index = 255;

class UsageError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

struct DevicesCommand {};

struct ProbeCommand {
  std::string device;
};

struct ParameterChange {
  vespera::NodeId node = 0;
  uint32_t parameter = 0;
  float value = 0.0f;
  std::optional<uint64_t> frame;
};

struct InspectCommand {
  std::vector<std::filesystem::path> modules;
  std::optional<std::filesystem::path> preset;
};

struct FileChange {
  vespera::NodeId node = 0;
  uint32_t slot = 0;
  std::filesystem::path path;
};

struct StreamCommand {
  std::vector<FileChange> files;
  std::string device;
  std::optional<uint32_t> input;
  std::optional<vespera::InputMode> input_mode;
  std::optional<std::array<uint32_t, 2>> inputs;
  std::array<uint32_t, 2> outputs{0, 1};
  uint32_t sample_rate = 48000;
  uint32_t frames = 128;
  uint32_t graph_channels = 2;
  bool allow_adjustment = false;
  bool monitor = false;
  double duration_seconds = 10.0;
  vespera::CoreIoSettings core_io;
  std::vector<std::filesystem::path> modules;
  std::vector<ParameterChange> changes;
  std::optional<std::filesystem::path> preset;
  std::optional<std::filesystem::path> save_preset;
};

struct RenderCommand {
  std::vector<FileChange> files;
  uint32_t sample_rate = 48000;
  uint32_t frames = 128;
  uint32_t graph_channels = 2;
  uint32_t length = 48000;
  vespera::CoreIoSettings core_io;
  std::vector<std::filesystem::path> modules;
  std::vector<ParameterChange> changes;
  std::optional<std::filesystem::path> preset;
  std::optional<std::filesystem::path> save_preset;
};

struct HelpCommand {};

using Command = std::variant<HelpCommand, DevicesCommand, ProbeCommand, InspectCommand,
                             StreamCommand, RenderCommand>;

constexpr const char *usage_text =
    R"(usage: vespera <command> [options]

commands:
  devices                       List devices. Indices are only valid for this enumeration order.
  probe <device>                Report formats and channels of a device while no stream is open.
  inspect --module PATH...      List stable instance IDs and module/parameter metadata, offline.
      --preset FILE             Inspect a preset and report unresolved module requirements.
  stream <device> --input N     Stream physical input through the graph to two outputs.
      --input N                 0-based physical input channel (see probe); required in mono mode.
      --input-mode MODE         mono (default) captures --input; stereo captures --inputs.
      --inputs L,R              Two distinct 0-based physical inputs feeding the graph's left and
                                right channels; requires --input-mode stereo and a stereo graph.
      --outputs L,R             0-based physical output channels (default 0,1).
      --rate HZ                 Requested sample rate (default 48000).
      --frames N                Requested block size (default 128).
      --graph-channels N        Process a mono (1) or stereo (2) graph (default 2).
      --allow-adjustment        Accept a negotiated rate/block size instead of failing.
      --duration SECONDS        Run time in (0, 3600] seconds (default 10).
      --monitor                 Send graph output to the device. Output is silent otherwise.
      --module PATH             Append a module binary to the graph; may repeat.
      --preset FILE             Restore a preset; --module paths then form its module catalog.
      --save-preset FILE        Save actual module settings after a successful stop.
      --set NODE:PARAM=VALUE     Set a native-unit parameter on the next processed block.
      --at FRAME:NODE:PARAM=VALUE
                                Schedule at an absolute sample position in this run.
      (core IO options below also apply)
  render                        Render a unit impulse offline through the graph.
      --rate HZ                 Sample rate (default 48000).
      --frames N                Maximum block size (default 128).
      --graph-channels N        Process a mono (1) or stereo (2) graph (default 2).
      --length N                Frames to render (default 48000).
      --module PATH             Append a module binary to the graph; may repeat.
      --preset FILE             Restore a preset using the supplied --module catalog.
      --save-preset FILE        Save actual module settings after rendering succeeds.
      --set NODE:PARAM=VALUE     Set a parameter at sample zero.
      --at FRAME:NODE:PARAM=VALUE
                                Schedule within the render; equal-frame changes keep CLI order.
      (core IO options below also apply)

core IO options (stream and render), applied around the graph; the defaults are transparent:
      --input-gain-db DB        Gain before the graph in [-60, 24] dB (default 0).
      --output-gain-db DB       Gain after the graph in [-60, 24] dB (default 0).
      --gate                    Enable the per-channel input noise gate.
      --gate-threshold-db DB    Gate threshold in [-120, 0] dBFS (default -80); implies --gate.
      --transpose-st N          Input pitch shift in [-12, 12] semitones (default 0). Render
                                and stream reserve fixed latency only when N is non-zero.
      --doubler                 Enable the output doubler: the right channel is delayed.
      --doubler-ms MS           Doubler delay in [1, 40] ms (default 6); implies --doubler.

--file NODE:SLOT=PATH (stream/render) sets a file before prepare; an empty PATH clears it.
<device> is an exact device ID; otherwise a decimal index into the `devices` listing.
NODE and PARAM are IDs printed by inspect. At most 256 parameter changes may be supplied.
)";

uint32_t parse_uint(std::string_view text, uint32_t minimum, uint32_t maximum, const char *option) {
  uint32_t value = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
      value < minimum || value > maximum) {
    throw UsageError(std::string(option) + " expects an integer in [" + std::to_string(minimum) +
                     ", " + std::to_string(maximum) + "], got '" + std::string(text) + "'");
  }
  return value;
}

uint64_t parse_position(std::string_view text, const char *label) {
  uint64_t value = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
    throw UsageError(std::string(label) + " expects an unsigned 64-bit integer");
  }
  return value;
}

ParameterChange parse_change(std::string_view text, bool scheduled) {
  ParameterChange change;
  if (scheduled) {
    const size_t colon = text.find(':');
    if (colon == std::string_view::npos) {
      throw UsageError("--at expects FRAME:NODE:PARAM=VALUE");
    }
    change.frame = parse_position(text.substr(0, colon), "frame");
    text.remove_prefix(colon + 1);
  }
  const size_t colon = text.find(':');
  const size_t equals = text.find('=');
  if (colon == std::string_view::npos || equals == std::string_view::npos || colon >= equals) {
    throw UsageError("parameter change expects NODE:PARAM=VALUE");
  }
  change.node = parse_position(text.substr(0, colon), "node ID");
  change.parameter = parse_uint(text.substr(colon + 1, equals - colon - 1), 0,
                                std::numeric_limits<uint32_t>::max(), "parameter ID");
  const std::string value(text.substr(equals + 1));
  char *end = nullptr;
  change.value = std::strtof(value.c_str(), &end);
  if (value.empty() || end != value.c_str() + value.size() || !std::isfinite(change.value)) {
    throw UsageError("parameter value must be a finite number");
  }
  return change;
}

void append_change(std::vector<ParameterChange> &changes, const std::string &text, bool scheduled) {
  if (changes.size() == vespera::ParameterQueue::capacity) {
    throw UsageError("at most 256 parameter changes may be supplied");
  }
  changes.push_back(parse_change(text, scheduled));
}

double parse_duration(const std::string &text) {
  char *end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (text.empty() || end != text.c_str() + text.size() || !std::isfinite(value) || value <= 0.0 ||
      value > max_duration_seconds) {
    throw UsageError("--duration expects seconds in (0, 3600], got '" + text + "'");
  }
  return value;
}

float parse_bounded(const std::string &text, double minimum, double maximum,
                    const std::string &option) {
  char *end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (text.empty() || end != text.c_str() + text.size() || !std::isfinite(value) ||
      value < minimum || value > maximum) {
    char range[64];
    std::snprintf(range, sizeof(range), "[%g, %g]", minimum, maximum);
    throw UsageError(option + " expects a finite number in " + range + ", got '" + text + "'");
  }
  return static_cast<float>(value);
}

std::array<uint32_t, 2> parse_channel_pair(std::string_view text, const std::string &option) {
  const size_t comma = text.find(',');
  if (comma == std::string_view::npos) {
    throw UsageError(option + " expects two channels as L,R");
  }
  std::array<uint32_t, 2> channels{
      parse_uint(text.substr(0, comma), 0, max_channel_index, option.c_str()),
      parse_uint(text.substr(comma + 1), 0, max_channel_index, option.c_str())};
  if (channels[0] == channels[1]) {
    throw UsageError(option + " requires two distinct channels");
  }
  return channels;
}

vespera::InputMode parse_input_mode(const std::string &text) {
  if (text == "mono") {
    return vespera::InputMode::mono;
  }
  if (text == "stereo") {
    return vespera::InputMode::stereo;
  }
  throw UsageError("--input-mode expects mono or stereo, got '" + text + "'");
}

class Arguments {
public:
  Arguments(int argc, char **argv) : args_(argv + 1, argv + argc) {}

  bool done() const { return index_ >= args_.size(); }
  std::string next() { return args_[index_++]; }
  std::string value(const std::string &option) {
    if (done()) {
      throw UsageError(option + " requires a value");
    }
    return next();
  }

private:
  std::vector<std::string> args_;
  size_t index_ = 0;
};

FileChange parse_file_change(const std::string &text) {
  const auto colon = text.find(':');
  const auto equal = text.find('=');
  if (colon == text.npos || equal == text.npos || colon >= equal) {
    throw UsageError("--file expects node:slot=path (empty path clears the slot)");
  }
  const auto node = parse_position(text.substr(0, colon), "--file node");
  const auto slot =
      parse_uint(text.substr(colon + 1, equal - colon - 1), 1, UINT32_MAX, "--file slot");
  return {node, slot, text.substr(equal + 1)};
}

vespera::FileReference chosen_file(const FileChange &change) {
  if (change.path.empty()) {
    return {};
  }
  const auto path = std::filesystem::absolute(change.path).lexically_normal();
  const auto bytes = vespera::read_file_content(path, vespera::max_asset_bytes);
  return {VS_FILE_PRESENT, path.string(), path.filename().string(), bytes.size(),
          vespera::content_sha256(bytes)};
}

void apply_files(vespera::Graph &graph, const std::vector<FileChange> &files) {
  for (const auto &file : files) {
    graph.set_file(file.node, file.slot, chosen_file(file));
  }
}

void apply_files(vespera::Preset &preset, const std::vector<FileChange> &files) {
  for (const auto &file : files) {
    const auto found =
        std::find_if(preset.nodes.begin(), preset.nodes.end(),
                     [&](const auto &node) { return node.instance_id == file.node; });
    if (found == preset.nodes.end()) {
      throw UsageError("--file names an unknown node");
    }
    auto &references = found->files;
    std::erase_if(references, [&](const auto &item) { return item.slot == file.slot; });
    if (!file.path.empty()) {
      references.push_back({file.slot, chosen_file(file)});
      preset.schema_version = 2;
    }
  }
}

bool parse_core_io_option(const std::string &arg, Arguments &args,
                          vespera::CoreIoSettings &settings) {
  if (arg == "--input-gain-db") {
    settings.input_gain_db = parse_bounded(args.value(arg), vespera::core_io_min_gain_db,
                                           vespera::core_io_max_gain_db, arg);
  } else if (arg == "--output-gain-db") {
    settings.output_gain_db = parse_bounded(args.value(arg), vespera::core_io_min_gain_db,
                                            vespera::core_io_max_gain_db, arg);
  } else if (arg == "--gate") {
    settings.gate_enabled = true;
  } else if (arg == "--gate-threshold-db") {
    settings.gate_enabled = true;
    settings.gate_threshold_db =
        parse_bounded(args.value(arg), vespera::core_io_min_gate_threshold_db,
                      vespera::core_io_max_gate_threshold_db, arg);
  } else if (arg == "--transpose-st") {
    settings.transpose_semitones =
        parse_bounded(args.value(arg), -vespera::core_io_max_transpose_semitones,
                      vespera::core_io_max_transpose_semitones, arg);
  } else if (arg == "--doubler") {
    settings.doubler_enabled = true;
  } else if (arg == "--doubler-ms") {
    settings.doubler_enabled = true;
    settings.doubler_delay_ms =
        parse_bounded(args.value(arg), vespera::core_io_min_doubler_delay_ms,
                      vespera::core_io_max_doubler_delay_ms, arg);
  } else {
    return false;
  }
  return true;
}

Command parse(int argc, char **argv) {
  Arguments args(argc, argv);
  if (args.done()) {
    throw UsageError("missing command");
  }
  const std::string command = args.next();
  if (command == "help" || command == "--help" || command == "-h") {
    return HelpCommand{};
  }
  if (command == "devices") {
    if (!args.done()) {
      throw UsageError("devices takes no arguments");
    }
    return DevicesCommand{};
  }
  if (command == "probe") {
    if (args.done()) {
      throw UsageError("probe requires a device");
    }
    ProbeCommand probe{args.next()};
    if (!args.done()) {
      throw UsageError("probe takes exactly one device");
    }
    return probe;
  }
  if (command == "stream") {
    StreamCommand stream;
    bool has_device = false;
    while (!args.done()) {
      const std::string arg = args.next();
      if (arg == "--input") {
        stream.input = parse_uint(args.value(arg), 0, max_channel_index, "--input");
      } else if (arg == "--input-mode") {
        stream.input_mode = parse_input_mode(args.value(arg));
      } else if (arg == "--inputs") {
        stream.inputs = parse_channel_pair(args.value(arg), arg);
      } else if (arg == "--outputs") {
        stream.outputs = parse_channel_pair(args.value(arg), arg);
      } else if (parse_core_io_option(arg, args, stream.core_io)) {
        continue;
      } else if (arg == "--rate") {
        stream.sample_rate = parse_uint(args.value(arg), 8000, max_sample_rate, "--rate");
      } else if (arg == "--frames") {
        stream.frames = parse_uint(args.value(arg), 1, max_block_frames, "--frames");
      } else if (arg == "--graph-channels") {
        stream.graph_channels = parse_uint(args.value(arg), 1, 2, "--graph-channels");
      } else if (arg == "--allow-adjustment") {
        stream.allow_adjustment = true;
      } else if (arg == "--monitor") {
        stream.monitor = true;
      } else if (arg == "--duration") {
        stream.duration_seconds = parse_duration(args.value(arg));
      } else if (arg == "--module") {
        stream.modules.emplace_back(args.value(arg));
      } else if (arg == "--file") {
        stream.files.push_back(parse_file_change(args.value(arg)));
      } else if (arg == "--set" || arg == "--at") {
        append_change(stream.changes, args.value(arg), arg == "--at");
      } else if (arg == "--preset") {
        stream.preset = args.value(arg);
      } else if (arg == "--save-preset") {
        stream.save_preset = args.value(arg);
      } else if (arg.rfind("--", 0) == 0) {
        throw UsageError("unknown stream option " + arg);
      } else if (!has_device) {
        stream.device = arg;
        has_device = true;
      } else {
        throw UsageError("stream takes exactly one device");
      }
    }
    if (!has_device) {
      throw UsageError("stream requires a device");
    }
    if (stream.input_mode.value_or(vespera::InputMode::mono) == vespera::InputMode::stereo) {
      if (stream.input) {
        throw UsageError("--input-mode stereo takes --inputs L,R instead of --input");
      }
      if (!stream.inputs) {
        throw UsageError("--input-mode stereo requires --inputs L,R; use probe to choose them");
      }
      if (stream.graph_channels != 2) {
        throw UsageError("--input-mode stereo requires --graph-channels 2");
      }
    } else {
      if (stream.inputs) {
        throw UsageError("--inputs requires --input-mode stereo");
      }
      if (!stream.input) {
        throw UsageError("stream requires an explicit --input channel; use probe to choose one");
      }
    }
    return stream;
  }
  if (command == "render") {
    RenderCommand render;
    while (!args.done()) {
      const std::string arg = args.next();
      if (arg == "--rate") {
        render.sample_rate = parse_uint(args.value(arg), 8000, max_sample_rate, "--rate");
      } else if (arg == "--frames") {
        render.frames = parse_uint(args.value(arg), 1, max_block_frames, "--frames");
      } else if (arg == "--graph-channels") {
        render.graph_channels = parse_uint(args.value(arg), 1, 2, "--graph-channels");
      } else if (arg == "--length") {
        render.length = parse_uint(args.value(arg), 1, max_render_frames, "--length");
      } else if (parse_core_io_option(arg, args, render.core_io)) {
        continue;
      } else if (arg == "--module") {
        render.modules.emplace_back(args.value(arg));
      } else if (arg == "--file") {
        render.files.push_back(parse_file_change(args.value(arg)));
      } else if (arg == "--set" || arg == "--at") {
        append_change(render.changes, args.value(arg), arg == "--at");
      } else if (arg == "--preset") {
        render.preset = args.value(arg);
      } else if (arg == "--save-preset") {
        render.save_preset = args.value(arg);
      } else {
        throw UsageError("unknown render option " + arg);
      }
    }
    return render;
  }
  if (command == "inspect") {
    InspectCommand inspect;
    while (!args.done()) {
      const std::string arg = args.next();
      if (arg == "--module") {
        inspect.modules.emplace_back(args.value(arg));
      } else if (arg == "--preset") {
        inspect.preset = args.value(arg);
      } else {
        throw UsageError("unknown inspect option " + arg);
      }
    }
    return inspect;
  }
  throw UsageError("unknown command " + command);
}

const vespera::AudioDevice &resolve_device(const std::vector<vespera::AudioDevice> &devices,
                                           const std::string &selector) {
  const vespera::AudioDevice *match = nullptr;
  for (const vespera::AudioDevice &device : devices) {
    if (device.device_id == selector) {
      if (match != nullptr) {
        throw UsageError("device ID '" + selector + "' is ambiguous across backends");
      }
      match = &device;
    }
  }
  if (match != nullptr) {
    return *match;
  }
  uint32_t index = 0;
  const auto parsed = std::from_chars(selector.data(), selector.data() + selector.size(), index);
  if (selector.empty() || parsed.ec != std::errc{} ||
      parsed.ptr != selector.data() + selector.size()) {
    throw UsageError("no device has ID '" + selector + "'");
  }
  if (index >= devices.size()) {
    throw UsageError("device index " + selector + " is out of range (" +
                     std::to_string(devices.size()) + " devices)");
  }
  return devices[index];
}

void print_device(size_t index, const vespera::AudioDevice &device) {
  std::printf("[%zu] %s\n", index, device.display_name.c_str());
  std::printf("    backend=%s id=%s\n", device.backend_id.c_str(), device.device_id.c_str());
  if (device.channel_counts_known) {
    std::printf("    inputs=%" PRIu32 " outputs=%" PRIu32, device.input_channels,
                device.output_channels);
  } else {
    std::printf("    channel counts unknown");
  }
  std::printf(" pairable=%s\n", device.pairable ? "yes" : "no");
}

int run_devices() {
  vespera::AudioSession session;
  const std::vector<vespera::AudioDevice> devices = session.devices();
  if (devices.empty()) {
    std::printf("no devices\n");
  }
  for (size_t index = 0; index < devices.size(); ++index) {
    print_device(index, devices[index]);
  }
  return exit_ok;
}

void validate_changes(const std::vector<vespera::NodeMetadata> &nodes,
                      const std::vector<ParameterChange> &changes) {
  for (const ParameterChange &change : changes) {
    const auto node = std::find_if(nodes.begin(), nodes.end(), [&](const auto &entry) {
      return entry.instance_id == change.node;
    });
    if (node == nodes.end()) {
      throw UsageError("unknown node ID " + std::to_string(change.node));
    }
    const auto &parameters = node->module.parameters;
    const auto parameter =
        std::find_if(parameters.begin(), parameters.end(),
                     [&](const auto &entry) { return entry.id == change.parameter; });
    if (parameter == parameters.end()) {
      throw UsageError("unknown parameter ID " + std::to_string(change.parameter));
    }
    if (change.value < parameter->minimum || change.value > parameter->maximum) {
      throw UsageError("parameter value outside bounds for " + parameter->name);
    }
  }
}

vespera::ModuleCatalog module_catalog(const std::vector<std::filesystem::path> &modules) {
  vespera::ModuleCatalog catalog;
  for (const auto &path : modules) {
    catalog.add(path);
  }
  return catalog;
}

int run_inspect(const InspectCommand &command) {
  if (command.preset) {
    const auto preset = vespera::read_preset(*command.preset);
    const auto catalog = module_catalog(command.modules);
    const auto issues = catalog.inspect(preset);
    std::printf("preset: schema=%" PRIu32 " nodes=%zu next_instance_id=%" PRIu64
                " unresolved=%zu\n",
                preset.schema_version, preset.nodes.size(), preset.next_instance_id, issues.size());
    for (const auto &node : preset.nodes) {
      std::printf("node=%" PRIu64 " module=%s version=%s abi=%" PRIu32 " state_schema=%" PRIu32
                  " state_bytes=%zu\n",
                  node.instance_id, node.module_id.c_str(), node.module_version.c_str(),
                  node.module_abi_version, node.state.schema_version, node.state.bytes.size());
      for (const auto &file : node.files) {
        const auto resolved =
            vespera::resolve_file_reference(file.reference, vespera::max_asset_bytes);
        std::printf("  file=%" PRIu32 " name=%s path=%s state=%s\n", file.slot,
                    file.reference.name.c_str(), file.reference.path.c_str(),
                    resolved.state == VS_FILE_PRESENT ? "present" : "missing");
      }
    }
    for (const auto &issue : issues) {
      std::printf("unresolved: node=%" PRIu64 " %s\n", issue.instance_id, issue.message.c_str());
    }
    return exit_ok;
  }
  vespera::Graph graph;
  for (const auto &path : command.modules) {
    graph.add_module(path);
  }
  for (const auto &node : graph.nodes()) {
    const auto &module = node.module;
    std::printf("node=%" PRIu64 " module=%s name=%s version=%s layouts=0x%" PRIx32
                " state_schema=%" PRIu32 "\n",
                node.instance_id, module.id.c_str(), module.name.c_str(), module.version.c_str(),
                module.supported_layouts, module.state_schema_version);
    for (const auto &parameter : module.parameters) {
      std::printf("  parameter=%" PRIu32 " name=%s unit=%s min=%.9g max=%.9g default=%.9g\n",
                  parameter.id, parameter.name.c_str(), parameter.unit.c_str(),
                  static_cast<double>(parameter.minimum), static_cast<double>(parameter.maximum),
                  static_cast<double>(parameter.default_value));
    }
    for (const auto &slot : module.file_slots) {
      std::printf("  file_slot=%" PRIu32 " name=%s types=%s max_bytes=%" PRIu64 " optional=%s\n",
                  slot.id, slot.name.c_str(), slot.types.c_str(), slot.max_bytes,
                  slot.optional ? "yes" : "no");
    }
  }
  return exit_ok;
}

void print_channels(const char *label, const std::vector<vespera::AudioChannel> &channels) {
  std::printf("%s channels:\n", label);
  for (const vespera::AudioChannel &channel : channels) {
    std::printf("  %3" PRIu32 "  %-32s format=%" PRId32 "%s\n", channel.index,
                channel.name.empty() ? "(unnamed)" : channel.name.c_str(), channel.native_format,
                channel.supported ? "" : "  unsupported");
  }
}

int run_probe(const ProbeCommand &command) {
  vespera::AudioSession session;
  const std::vector<vespera::AudioDevice> devices = session.devices();
  const vespera::AudioDevice device = resolve_device(devices, command.device);
  const vespera::DeviceCapabilities caps = session.probe(device);
  std::printf("device: %s (%s)\n", device.display_name.c_str(), device.device_id.c_str());
  std::printf("inputs=%" PRIu32 " outputs=%" PRIu32 " current_rate=%" PRIu32 "\n",
              caps.input_channels, caps.output_channels, caps.current_sample_rate);
  std::printf("frames: min=%" PRIu32 " max=%" PRIu32 " preferred=%" PRIu32 " granularity=%" PRId32
              "\n",
              caps.min_frames, caps.max_frames, caps.preferred_frames, caps.frame_granularity);
  std::printf("tested rates:");
  for (uint32_t rate : caps.tested_rates) {
    std::printf(" %" PRIu32, rate);
  }
  std::printf("%s\n", caps.tested_rates.empty() ? " (none reported)" : "");
  print_channels("input", caps.inputs);
  print_channels("output", caps.outputs);
  return exit_ok;
}

std::atomic<bool> interrupted{false};
static_assert(std::atomic<bool>::is_always_lock_free);

extern "C" void handle_interrupt(int) { interrupted.store(true, std::memory_order_relaxed); }

void print_status(const vespera::SessionStatus &status, uint32_t sample_rate) {
  const vespera::DeviceStreamStatus &stream = status.stream;
  const vespera::CallbackTelemetry &t = status.telemetry;
  std::printf("state=%s", vespera::to_string(status.state));
  if (status.state == vespera::SessionState::recovery_required) {
    std::printf(" reason=%s error=%s", vespera::to_string(status.recovery),
                vespera::audio_result_name(status.last_error));
  }
  if (status.stream_open) {
    std::printf(" stream=%s run=%" PRIu32 " delivered=%" PRIu64 " faults=0x%" PRIx32
                " callbacks=%" PRIu64 " max_cb_us=%.1f overruns=%" PRIu64 " overloads=%" PRIu64,
                vespera::to_string(stream.state), stream.run_generation, stream.delivered_frames,
                stream.fault_flags, stream.callback_count,
                static_cast<double>(stream.max_callback_nanoseconds) / 1000.0,
                stream.deadline_overruns, stream.driver_overloads);
    if (stream.latency_valid && sample_rate != 0) {
      std::printf(" latency_in=%" PRIu32 "(%.2fms) latency_out=%" PRIu32 "(%.2fms)",
                  stream.input_latency_frames, 1000.0 * stream.input_latency_frames / sample_rate,
                  stream.output_latency_frames,
                  1000.0 * stream.output_latency_frames / sample_rate);
    }
    if (stream.last_result != 0) {
      std::printf(" last_result=%s", vespera::audio_result_name(stream.last_result));
    }
  }
  std::printf("\n  graph: blocks=%" PRIu64 " frames=%" PRIu64 " discontinuities=%" PRIu64
              " failures=%" PRIu64 " violations=%" PRIu64 " in_peak=%.6f out_peak=%.6f\n",
              t.blocks, t.frames, t.discontinuities, t.processing_failures, t.contract_violations,
              static_cast<double>(t.input_peak), static_cast<double>(t.output_peak));
  const auto &parameters = t.parameters;
  std::printf("  parameters: accepted=%" PRIu64 " applied=%" PRIu64 " rejected=%" PRIu64
              " full=%" PRIu64 " stale=%" PRIu64 " late=%" PRIu64 " next_frame=%" PRIu64 "\n",
              parameters.accepted, parameters.applied, parameters.rejected, parameters.queue_full,
              parameters.stale, parameters.late, t.next_frame_position);
  std::fflush(stdout);
}

int shutdown(vespera::AudioSession &session) {
  const vespera::CloseResult result = session.close();
  if (result == vespera::CloseResult::closed) {
    return exit_ok;
  }
  const vespera::SessionStatus status = session.poll();
  std::fprintf(stderr, "shutdown failed: %s (%s); %s\n",
               result == vespera::CloseResult::stop_failed ? "stream did not stop"
                                                           : "stream did not close",
               vespera::audio_result_name(status.last_error),
               result == vespera::CloseResult::stop_failed
                   ? "callback state is retained and intentionally leaked"
                   : "device handle is leaked");
  return exit_shutdown_failure;
}

template <class Host> bool report_module_status(Host &host) {
  bool errors = false;
  for (const auto &node : host.nodes()) {
    if (const auto status = host.module_status(node.instance_id);
        status && status->kind != VS_STATUS_READY) {
      std::fprintf(stderr, "module %s: %s\n", node.module.name.c_str(), status->message.c_str());
      errors |= status->kind == VS_STATUS_ERROR;
    }
  }
  return errors;
}

int run_stream(const StreamCommand &command) {
  vespera::AudioSession session;
  int exit_code = exit_ok;
  try {
    const std::vector<vespera::AudioDevice> devices = session.devices();
    const vespera::AudioDevice device = resolve_device(devices, command.device);
    const auto services = vespera::HostServices::shared();
    if (!command.preset) {
      session.graph().set_host_services(services);
      for (const std::filesystem::path &module : command.modules) {
        session.graph().add_module(module);
      }
      apply_files(session.graph(), command.files);
    }

    vespera::StreamConfig config;
    config.backend_id = device.backend_id;
    config.device_id = device.device_id;
    config.input_mode = command.input_mode.value_or(vespera::InputMode::mono);
    if (config.input_mode == vespera::InputMode::stereo) {
      config.input_channel = (*command.inputs)[0];
      config.second_input_channel = (*command.inputs)[1];
    } else {
      config.input_channel = *command.input;
    }
    config.output_channels = command.outputs;
    config.sample_rate = command.sample_rate;
    config.frames_per_block = command.frames;
    config.graph_channels = command.graph_channels;
    config.core_io = command.core_io;
    config.allow_adjustment = command.allow_adjustment;
    config.output_mode =
        command.monitor ? vespera::OutputMode::monitor : vespera::OutputMode::silent;
    if (command.preset) {
      auto preset = vespera::read_preset(*command.preset);
      apply_files(preset, command.files);
      const auto catalog = module_catalog(command.modules);
      session.restore_preset(preset, catalog, config, services);
    } else {
      session.prepare(config);
    }
    if (!session.wait_for_module_work(std::chrono::steady_clock::now() + std::chrono::seconds(2))) {
      std::fprintf(stderr, "warning: module work is still running after 2 s; starting anyway\n");
    }
    report_module_status(session);
    validate_changes(session.nodes(), command.changes);

    const vespera::StreamInfo info = session.stream_info();
    std::printf("device: %s (%s)\n", device.display_name.c_str(), device.device_id.c_str());
    std::printf("format: rate=%" PRIu32 " frames=%" PRIu32 " max_frames=%" PRIu32 " stream=%" PRIu32
                " config=%" PRIu32 "\n",
                info.sample_rate, info.frames_per_block, info.max_frames_per_block, info.stream_id,
                info.configuration_generation);
    if (info.input_mode == vespera::InputMode::stereo) {
      std::printf("routing: inputs %" PRIu32 ",%" PRIu32, info.physical_inputs[0],
                  info.physical_inputs[1]);
    } else {
      std::printf("routing: input %" PRIu32, info.physical_input);
    }
    std::printf(" -> %s -> outputs %" PRIu32 ",%" PRIu32 " (%s)\n",
                info.graph_channels == 1 ? "mono graph duplicated to stereo" : "stereo graph",
                info.physical_outputs[0], info.physical_outputs[1],
                info.output_mode == vespera::OutputMode::monitor ? "monitoring" : "silent");
    std::printf("graph: nodes=%zu latency=%" PRIu32 " frames\n", session.nodes().size(),
                info.graph_latency_frames);
    if (info.core_io_latency_frames != 0) {
      std::printf("core_io: input_latency=%" PRIu32 " frames\n", info.core_io_latency_frames);
    }
    std::fflush(stdout);

    std::signal(SIGINT, handle_interrupt);
    session.start();
    const auto token = session.parameter_token();
    for (const auto &change : command.changes) {
      const auto result =
          session.set_parameter(token, change.node, change.parameter, change.value, change.frame);
      if (result != vespera::ParameterResult::accepted) {
        throw std::runtime_error(std::string("parameter submission rejected: ") +
                                 vespera::to_string(result));
      }
    }
    using Clock = std::chrono::steady_clock;
    const auto begin = Clock::now();
    const auto deadline = begin + std::chrono::duration_cast<Clock::duration>(
                                      std::chrono::duration<double>(command.duration_seconds));
    auto next_report = begin + std::chrono::seconds(1);
    while (!interrupted.load(std::memory_order_relaxed) && Clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      const vespera::SessionStatus status = session.poll();
      if (status.state != vespera::SessionState::running) {
        print_status(status, info.sample_rate);
        break;
      }
      if (Clock::now() >= next_report) {
        print_status(status, info.sample_rate);
        next_report += std::chrono::seconds(1);
      }
    }
    if (interrupted.load(std::memory_order_relaxed)) {
      std::printf("interrupted\n");
    }

    if (session.state() == vespera::SessionState::running ||
        session.state() == vespera::SessionState::recovery_required) {
      session.stop();
    }
    const vespera::SessionStatus final_status = session.poll();
    std::printf("final: ");
    print_status(final_status, info.sample_rate);
    if (final_status.state != vespera::SessionState::prepared) {
      exit_code = exit_failure;
    } else if (command.save_preset) {
      vespera::write_preset(*command.save_preset, session.capture_preset());
    }
  } catch (const UsageError &) {
    const int closed = shutdown(session);
    if (closed != exit_ok) {
      return closed;
    }
    throw;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "error: %s\n", error.what());
    exit_code = exit_failure;
  }
  std::signal(SIGINT, SIG_DFL);
  const int closed = shutdown(session);
  return closed != exit_ok ? closed : exit_code;
}

uint64_t fnv1a(uint64_t hash, float sample) {
  uint32_t bits = 0;
  std::memcpy(&bits, &sample, sizeof(bits));
  for (int shift = 0; shift < 32; shift += 8) {
    hash ^= (bits >> shift) & 0xffu;
    hash *= 0x100000001b3ull;
  }
  return hash;
}

// Offline renders run module work inline, so their output does not depend on timing.
int run_render(const RenderCommand &command) {
  const auto services = vespera::HostServices::create({vespera::HostWorkMode::synchronous});
  std::unique_ptr<vespera::Graph> owned_graph;
  if (command.preset) {
    const auto preset = vespera::read_preset(*command.preset);
    const auto catalog = module_catalog(command.modules);
    owned_graph = vespera::Graph::restore_preset(preset, catalog, services);
  } else {
    owned_graph = std::make_unique<vespera::Graph>();
    owned_graph->set_host_services(services);
    for (const std::filesystem::path &module : command.modules) {
      owned_graph->add_module(module);
    }
  }
  auto &graph = *owned_graph;
  apply_files(graph, command.files);
  validate_changes(graph.nodes(), command.changes);
  auto changes = command.changes;
  for (const auto &change : changes) {
    if (change.frame.value_or(0) >= command.length) {
      throw UsageError("scheduled parameter frame must be within the render length");
    }
  }
  std::stable_sort(changes.begin(), changes.end(), [](const auto &left, const auto &right) {
    return left.frame.value_or(0) < right.frame.value_or(0);
  });
  graph.prepare(vespera::Format{static_cast<double>(command.sample_rate), command.frames,
                                command.graph_channels});
  vespera::MonoStereoAdapter adapter(graph, command.frames, command.graph_channels);
  vespera::CoreIoProcessor core_io;
  core_io.prepare(static_cast<double>(command.sample_rate), command.frames,
                  command.core_io.transpose_semitones == 0.0f ? vespera::TransposeLatency::none
                                                              : vespera::TransposeLatency::fixed);
  if (!core_io.set_settings(command.core_io)) {
    throw UsageError("core IO settings were rejected");
  }
  core_io.reset();
  std::vector<float> conditioned(command.frames);
  std::array<std::vector<float>, 2> output{std::vector<float>(command.frames),
                                           std::vector<float>(command.frames)};

  std::vector<float> input(command.length, 0.0f);
  input[0] = 1.0f;

  const uint32_t pattern[] = {command.frames, 1, command.frames / 3 + 1,
                              command.frames > 1 ? command.frames - 1 : 1};
  uint64_t checksum = 0xcbf29ce484222325ull;
  float peak = 0.0f;
  uint64_t peak_frame = 0;
  uint64_t non_finite = 0;
  uint64_t blocks = 0;
  uint32_t position = 0;
  size_t next_change = 0;
  std::array<vespera::Event, vespera::Graph::max_events_per_block> events{};
  while (position < command.length) {
    const uint32_t frames =
        std::min(pattern[blocks % std::size(pattern)], command.length - position);
    uint32_t flags = VS_PROCESS_OFFLINE;
    if (position == 0) {
      flags |= VS_PROCESS_DISCONTINUITY;
    }
    size_t event_count = 0;
    while (next_change < changes.size() &&
           changes[next_change].frame.value_or(0) < static_cast<uint64_t>(position) + frames) {
      const auto &change = changes[next_change++];
      events[event_count++] = {change.node,
                               {change.parameter,
                                static_cast<uint32_t>(change.frame.value_or(0) - position),
                                change.value}};
    }
    const float *const raw[] = {input.data() + position};
    float *const conditioned_channels[] = {conditioned.data()};
    if (!core_io.process_input(raw, conditioned_channels, 1, frames)) {
      std::fprintf(stderr, "render core IO input failed at frame %" PRIu32 "\n", position);
      graph.release();
      return exit_failure;
    }
    const vs_result result =
        adapter.process(conditioned.data(), frames, position, flags,
                        std::span<const vespera::Event>(events.data(), event_count));
    if (result != VS_OK) {
      std::fprintf(stderr, "render failed at frame %" PRIu32 " with result %" PRId32 "\n", position,
                   result);
      graph.release();
      return exit_failure;
    }
    const float *const graph_output[] = {adapter.output(0), adapter.output(1)};
    float *const output_channels[] = {output[0].data(), output[1].data()};
    if (!core_io.process_output(graph_output, command.graph_channels, output_channels, 2, frames)) {
      std::fprintf(stderr, "render core IO output failed at frame %" PRIu32 "\n", position);
      graph.release();
      return exit_failure;
    }
    for (uint32_t i = 0; i < frames; ++i) {
      for (uint32_t channel = 0; channel < 2; ++channel) {
        const float sample = output[channel][i];
        checksum = fnv1a(checksum, sample);
        if (!std::isfinite(sample)) {
          ++non_finite;
        } else if (std::fabs(sample) > peak) {
          peak = std::fabs(sample);
          peak_frame = position + i;
        }
      }
    }
    position += frames;
    ++blocks;
  }
  std::printf("render: rate=%" PRIu32 " max_frames=%" PRIu32 " length=%" PRIu32 " blocks=%" PRIu64
              " modules=%u nodes=%" PRIu32 " latency=%" PRIu32 "\n",
              command.sample_rate, command.frames, command.length, blocks,
              static_cast<unsigned>(command.modules.size()), graph.node_count(),
              graph.latency_frames());
  if (command.core_io != vespera::CoreIoSettings{}) {
    const auto &io = command.core_io;
    std::printf("core_io: input_gain_db=%.9g output_gain_db=%.9g gate=%s gate_threshold_db=%.9g"
                " transpose_st=%.9g doubler=%s doubler_ms=%.9g input_latency=%" PRIu32 "\n",
                static_cast<double>(io.input_gain_db), static_cast<double>(io.output_gain_db),
                io.gate_enabled ? "on" : "off", static_cast<double>(io.gate_threshold_db),
                static_cast<double>(io.transpose_semitones), io.doubler_enabled ? "on" : "off",
                static_cast<double>(io.doubler_delay_ms), core_io.input_latency_frames());
  }
  std::printf("checksum=%016" PRIx64 " peak=%.9g peak_frame=%" PRIu64 " non_finite=%" PRIu64 "\n",
              checksum, static_cast<double>(peak), peak_frame, non_finite);
  const bool module_errors = report_module_status(graph);
  if (command.save_preset && non_finite == 0 && !module_errors) {
    vespera::write_preset(*command.save_preset, graph.capture_preset());
  }
  graph.release();
  return non_finite == 0 && !module_errors ? exit_ok : exit_failure;
}

}

int main(int argc, char **argv) {
  try {
    const Command command = parse(argc, argv);
    if (std::holds_alternative<HelpCommand>(command)) {
      std::fputs(usage_text, stdout);
      return exit_ok;
    }
    if (std::holds_alternative<DevicesCommand>(command)) {
      return run_devices();
    }
    if (const auto *probe = std::get_if<ProbeCommand>(&command)) {
      return run_probe(*probe);
    }
    if (const auto *inspect = std::get_if<InspectCommand>(&command)) {
      return run_inspect(*inspect);
    }
    if (const auto *stream = std::get_if<StreamCommand>(&command)) {
      return run_stream(*stream);
    }
    return run_render(std::get<RenderCommand>(command));
  } catch (const UsageError &error) {
    std::fprintf(stderr, "error: %s\n\n%s", error.what(), usage_text);
    return exit_usage;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "error: %s\n", error.what());
    return exit_failure;
  }
}
