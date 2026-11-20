#include "module_loader.hpp"

#include <vespera/files.hpp>
#include <vespera/parameter_display.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <set>
#include <utility>

#if VESPERA_NATIVE_LOADER
#include <dlfcn.h>
#endif

namespace vespera::loader {
namespace {

constexpr uint32_t max_parameters = 1024;
constexpr uint32_t max_shared_targets = 4096;
constexpr size_t max_string_length = 256;
constexpr uint32_t known_shared_flags = VS_SHARED_LEADING_ONLY | VS_SHARED_RETAINS_AUDIO;
constexpr uint32_t known_capabilities =
    VS_MODULE_BYPASS_PARAMETER | VS_MODULE_HOST_TEMPO | VS_MODULE_MIDI_INPUT;
constexpr uint32_t known_layouts = VS_LAYOUT_MONO | VS_LAYOUT_STEREO;

ModuleMetadata describe(const vs_module_api &api, const vs_module_state_api *state,
                        const vs_module_parameter_api *parameter_values) {
  ModuleMetadata metadata;
  metadata.state_schema_version = state != nullptr ? state->schema_version : 0;
  metadata.parameter_values = parameter_values != nullptr;
  metadata.id = api.id;
  metadata.name = api.name;
  metadata.version = api.version;
  metadata.supported_layouts = api.supported_layouts;
  if ((api.capabilities & VS_MODULE_BYPASS_PARAMETER) != 0) {
    metadata.bypass_parameter = api.bypass_parameter;
  }
  metadata.host_tempo = (api.capabilities & VS_MODULE_HOST_TEMPO) != 0;
  metadata.midi_input = (api.capabilities & VS_MODULE_MIDI_INPUT) != 0;
  metadata.parameters.reserve(api.parameter_count);
  for (uint32_t i = 0; i < api.parameter_count; ++i) {
    const vs_parameter_descriptor &p = api.parameters[i];
    ParameterMetadata parameter;
    parameter.id = p.id;
    parameter.name = p.name;
    parameter.unit = p.unit;
    parameter.minimum = p.minimum;
    parameter.maximum = p.maximum;
    parameter.default_value = p.default_value;
    metadata.parameters.push_back(std::move(parameter));
  }
  return metadata;
}

std::vector<ParameterRange> validate_api(const std::filesystem::path &path,
                                         const vs_module_api *api) {
  if (api == nullptr) {
    load_failure(path,
                 "entry point rejected host ABI version " + std::to_string(VS_MODULE_ABI_VERSION));
  }
  if (api->struct_size < sizeof(vs_module_api)) {
    load_failure(path, "descriptor struct_size " + std::to_string(api->struct_size) +
                           " is smaller than " + std::to_string(sizeof(vs_module_api)));
  }
  if (api->abi_version != VS_MODULE_ABI_VERSION) {
    load_failure(path, "descriptor ABI version " + std::to_string(api->abi_version) +
                           " does not match host ABI version " +
                           std::to_string(VS_MODULE_ABI_VERSION));
  }
  if (!valid_id(api->id)) {
    load_failure(path, "module ID must be a vendor-qualified identifier");
  }
  const std::string id(api->id);
  if (!valid_string(api->name, false)) {
    load_failure(path, "module '" + id + "' has an invalid name");
  }
  if (!valid_string(api->version, false)) {
    load_failure(path, "module '" + id + "' has an invalid version");
  }
  if (api->supported_layouts == 0 || (api->supported_layouts & ~known_layouts) != 0) {
    load_failure(path, "module '" + id + "' declares unsupported channel layouts");
  }
  if (api->create == nullptr || api->destroy == nullptr || api->prepare == nullptr ||
      api->release == nullptr || api->reset == nullptr || api->process == nullptr) {
    load_failure(path, "module '" + id + "' is missing required functions");
  }
  if (api->parameter_count > max_parameters) {
    load_failure(path, "module '" + id + "' declares more than " + std::to_string(max_parameters) +
                           " parameters");
  }
  if (api->parameter_count > 0 && api->parameters == nullptr) {
    load_failure(path, "module '" + id + "' has a null parameter table");
  }

  std::vector<ParameterRange> ranges;
  ranges.reserve(api->parameter_count);
  for (uint32_t i = 0; i < api->parameter_count; ++i) {
    const vs_parameter_descriptor &p = api->parameters[i];
    const std::string where = "module '" + id + "' parameter " + std::to_string(p.id);
    if (!valid_string(p.name, false) || !valid_string(p.unit, true)) {
      load_failure(path, where + " has invalid name or unit");
    }
    if (!std::isfinite(p.minimum) || !std::isfinite(p.maximum) || !std::isfinite(p.default_value) ||
        p.minimum > p.maximum || p.default_value < p.minimum || p.default_value > p.maximum) {
      load_failure(path, where + " has invalid bounds or default");
    }
    ranges.push_back({p.id, p.minimum, p.maximum});
  }
  std::sort(ranges.begin(), ranges.end(),
            [](const ParameterRange &a, const ParameterRange &b) { return a.id < b.id; });
  const auto duplicate = std::adjacent_find(
      ranges.begin(), ranges.end(),
      [](const ParameterRange &a, const ParameterRange &b) { return a.id == b.id; });
  if (duplicate != ranges.end()) {
    load_failure(path,
                 "module '" + id + "' has duplicate parameter ID " + std::to_string(duplicate->id));
  }
  if ((api->capabilities & ~known_capabilities) != 0) {
    load_failure(path, "module '" + id + "' declares unknown capabilities");
  }
  if ((api->capabilities & VS_MODULE_BYPASS_PARAMETER) != 0) {
    const auto bypass = std::find_if(ranges.begin(), ranges.end(), [&](const ParameterRange &r) {
      return r.id == api->bypass_parameter;
    });
    if (bypass == ranges.end() || bypass->minimum != 0.0f || bypass->maximum != 1.0f) {
      load_failure(path, "module '" + id + "' names bypass parameter " +
                             std::to_string(api->bypass_parameter) +
                             ", which is not a declared parameter ranging from 0 to 1");
    }
  } else if (api->bypass_parameter != 0) {
    load_failure(path, "module '" + id + "' names a bypass parameter without declaring the " +
                           "bypass capability");
  }
  return ranges;
}

const vs_module_api *query_entry(const std::filesystem::path &path, const LibraryHandle &handle) {
#if VESPERA_NATIVE_LOADER
  dlerror();
  void *symbol = dlsym(handle.get(), VS_MODULE_ENTRY_NAME);
  if (symbol == nullptr) {
    load_failure(path, "missing entry point '" VS_MODULE_ENTRY_NAME "'");
  }
  const auto entry = reinterpret_cast<vs_module_entry_fn>(symbol);
  const vs_module_api *api = entry(VS_MODULE_ABI_VERSION);
  if (api == nullptr) {
    for (uint32_t older = VS_MODULE_ABI_VERSION - 1; older > 0; --older) {
      if (entry(older) != nullptr) {
        load_failure(path, "module was built for module ABI version " + std::to_string(older) +
                               ", but this host loads only ABI version " +
                               std::to_string(VS_MODULE_ABI_VERSION) +
                               "; rebuild it against the current SDK");
      }
    }
  }
  return api;
#else
  (void)handle;
  load_failure(path, "native module loading is not supported on this platform yet");
#endif
}

void *find_symbol(const LibraryHandle &handle, const char *name) {
#if VESPERA_NATIVE_LOADER
  dlerror();
  return dlsym(handle.get(), name);
#else
  (void)handle;
  (void)name;
  return nullptr;
#endif
}

// Extensions are optional: a missing symbol or an entry that declines the host's extension ABI
// leaves the module loadable without that capability, but a descriptor it does return must be
// valid.
const vs_module_state_api *query_state(const std::filesystem::path &path,
                                       const LibraryHandle &handle, const std::string &id) {
  void *symbol = find_symbol(handle, VS_MODULE_STATE_ENTRY_NAME);
  if (symbol == nullptr) {
    return nullptr;
  }
  const auto entry = reinterpret_cast<vs_module_state_entry_fn>(symbol);
  const vs_module_state_api *state = entry(VS_MODULE_STATE_ABI_VERSION);
  if (state == nullptr) {
    return nullptr;
  }
  if (state->struct_size < sizeof(vs_module_state_api)) {
    load_failure(path, "module '" + id + "' state descriptor struct_size " +
                           std::to_string(state->struct_size) + " is smaller than " +
                           std::to_string(sizeof(vs_module_state_api)));
  }
  if (state->abi_version != VS_MODULE_STATE_ABI_VERSION) {
    load_failure(path, "module '" + id + "' state ABI version " +
                           std::to_string(state->abi_version) +
                           " does not match host state ABI version " +
                           std::to_string(VS_MODULE_STATE_ABI_VERSION));
  }
  if (state->schema_version == 0) {
    load_failure(path, "module '" + id + "' declares state schema version 0");
  }
  if (state->size == nullptr || state->save == nullptr || state->load == nullptr) {
    load_failure(path, "module '" + id + "' is missing required state functions");
  }
  return state;
}

const vs_module_parameter_api *query_parameter_values(const std::filesystem::path &path,
                                                      const LibraryHandle &handle,
                                                      const std::string &id) {
  void *symbol = find_symbol(handle, VS_MODULE_PARAMETER_ENTRY_NAME);
  if (symbol == nullptr) {
    return nullptr;
  }
  const auto entry = reinterpret_cast<vs_module_parameter_entry_fn>(symbol);
  const vs_module_parameter_api *api = entry(VS_MODULE_PARAMETER_ABI_VERSION);
  if (api == nullptr) {
    return nullptr;
  }
  if (api->struct_size < sizeof(vs_module_parameter_api)) {
    load_failure(path, "module '" + id + "' parameter value descriptor struct_size " +
                           std::to_string(api->struct_size) + " is smaller than " +
                           std::to_string(sizeof(vs_module_parameter_api)));
  }
  if (api->abi_version != VS_MODULE_PARAMETER_ABI_VERSION) {
    load_failure(path, "module '" + id + "' parameter value ABI version " +
                           std::to_string(api->abi_version) +
                           " does not match host parameter value ABI version " +
                           std::to_string(VS_MODULE_PARAMETER_ABI_VERSION));
  }
  if (api->get_value == nullptr || api->set_value == nullptr) {
    load_failure(path, "module '" + id + "' is missing required parameter value functions");
  }
  return api;
}

// Reads the parameter metadata extension into the module's metadata. Invalid metadata fails the
// load, like any other extension descriptor a module returns.
void query_parameter_metadata(const std::filesystem::path &path, const LibraryHandle &handle,
                              const vs_module_api &api, Library &library) {
  const vs_module_metadata_api *metadata = nullptr;
  if (void *symbol = find_symbol(handle, VS_MODULE_METADATA_ENTRY_NAME)) {
    metadata =
        reinterpret_cast<vs_module_metadata_entry_fn>(symbol)(VS_MODULE_METADATA_ABI_VERSION);
  }
  std::string error;
  auto parameters = describe_parameters(api, metadata, &error);
  if (!parameters) {
    load_failure(path, "module '" + library.id + "' " + error);
  }
  library.metadata.parameter_metadata = metadata != nullptr;
  for (size_t i = 0; i < parameters->size(); ++i) {
    library.metadata.parameters[i].display = std::move((*parameters)[i]);
  }
}

const vs_module_shared_api *query_shared(const std::filesystem::path &path,
                                         const LibraryHandle &handle, Library &library) {
  void *symbol = find_symbol(handle, VS_MODULE_SHARED_ENTRY_NAME);
  if (symbol == nullptr) {
    return nullptr;
  }
  const auto entry = reinterpret_cast<vs_module_shared_entry_fn>(symbol);
  const vs_module_shared_api *api = entry(VS_MODULE_SHARED_ABI_VERSION);
  if (api == nullptr) {
    return nullptr;
  }
  const std::string where = "module '" + library.id + "' shared engine descriptor";
  if (api->struct_size < sizeof(vs_module_shared_api) ||
      api->abi_version != VS_MODULE_SHARED_ABI_VERSION) {
    load_failure(path, where + " has an incompatible size or ABI version");
  }
  if (!valid_string(api->engine, false)) {
    load_failure(path, where + " has an invalid engine identity");
  }
  if ((api->flags & ~known_shared_flags) != 0) {
    load_failure(path, where + " declares unknown flags");
  }
  if (api->assignments == nullptr || api->translate == nullptr || api->engine_prepare == nullptr ||
      api->engine_process == nullptr || api->engine_apply == nullptr) {
    load_failure(path, where + " is missing required functions");
  }
  if (api->target_count > max_shared_targets || api->target_size < sizeof(vs_shared_target) ||
      (api->target_count > 0 && api->targets == nullptr)) {
    load_failure(path, where + " has an invalid target table");
  }
  if (library.parameter_values == nullptr) {
    load_failure(path, where + " requires the parameter value extension");
  }
  const auto *bytes = reinterpret_cast<const uint8_t *>(api->targets);
  std::vector<vs_shared_target> targets;
  targets.reserve(api->target_count);
  for (uint32_t i = 0; i < api->target_count; ++i) {
    vs_shared_target target;
    std::memcpy(&target, bytes + static_cast<size_t>(i) * api->target_size, sizeof(target));
    const std::string at = where + " target " + std::to_string(target.id);
    if (!valid_string(target.name, false) || !std::isfinite(target.minimum) ||
        !std::isfinite(target.maximum) || target.minimum > target.maximum) {
      load_failure(path, at + " has an invalid name or range");
    }
    if (target.role != VS_SHARED_TARGET_OWNED && target.role != VS_SHARED_TARGET_REQUIRED &&
        target.role != VS_SHARED_TARGET_NEUTRAL) {
      load_failure(path, at + " has an unknown role");
    }
    if (target.role != VS_SHARED_TARGET_OWNED &&
        (!std::isfinite(target.value) || target.value < target.minimum ||
         target.value > target.maximum)) {
      load_failure(path, at + " has a value outside its range");
    }
    for (const auto &previous : targets) {
      if (previous.id == target.id) {
        load_failure(path, at + " is declared twice");
      }
    }
    library.owned_targets += target.role == VS_SHARED_TARGET_OWNED ? 1 : 0;
    targets.push_back(target);
  }
  library.shared_targets = std::move(targets);
  return api;
}

const vs_module_shared_branch_api *query_shared_branch(const std::filesystem::path &path,
                                                       const LibraryHandle &handle,
                                                       const Library &library) {
  void *symbol = find_symbol(handle, VS_MODULE_SHARED_BRANCH_ENTRY_NAME);
  if (!symbol) {
    return nullptr;
  }
  auto entry = reinterpret_cast<vs_module_shared_branch_entry_fn>(symbol);
  const auto *api = entry(VS_MODULE_SHARED_BRANCH_ABI_VERSION);
  if (!api) {
    return nullptr;
  }
  const std::string where = "module '" + library.id + "' shared branch descriptor";
  if (api->struct_size < sizeof(*api) || api->abi_version != VS_MODULE_SHARED_BRANCH_ABI_VERSION) {
    load_failure(path, where + " has an incompatible size or ABI version");
  }
  if (!library.shared || !api->native_assignments || !api->native_translate ||
      api->mix_law != VS_SHARED_BRANCH_CROSSFADE ||
      (api->flags & ~uint32_t{VS_SHARED_BRANCH_HAS_SELECTOR}) ||
      api->first_after_stage <= library.shared->stage) {
    load_failure(path, where + " has invalid shared ABI, callbacks, law, flags or fence");
  }
  const auto *mix = library.find_parameter(api->mix_parameter);
  if (!mix || mix->minimum != 0.0f || mix->maximum != 1.0f ||
      library.metadata.bypass_parameter == api->mix_parameter) {
    load_failure(path, where + " has an invalid mix parameter");
  }
  if (api->flags & VS_SHARED_BRANCH_HAS_SELECTOR) {
    const auto &params = library.metadata.parameters;
    auto selector = std::find_if(params.begin(), params.end(), [&](const ParameterMetadata &p) {
      return p.id == api->selector_parameter;
    });
    if (selector == params.end() || selector->id == api->mix_parameter ||
        library.metadata.bypass_parameter == selector->id || selector->display.steps < 2 ||
        !std::isfinite(api->selector_value) || api->selector_value < selector->minimum ||
        api->selector_value > selector->maximum ||
        selector->display.nearest(api->selector_value) != api->selector_value) {
      load_failure(path, where + " has an invalid selector parameter or value");
    }
  }
  return api;
}

const vs_module_services_api *query_services(const std::filesystem::path &path,
                                             const LibraryHandle &handle, const std::string &id) {
  void *symbol = find_symbol(handle, VS_MODULE_SERVICES_ENTRY_NAME);
  if (symbol == nullptr) {
    return nullptr;
  }
  const auto entry = reinterpret_cast<vs_module_services_entry_fn>(symbol);
  const vs_module_services_api *api = entry(VS_MODULE_SERVICES_ABI_VERSION);
  if (api == nullptr) {
    return nullptr;
  }
  if (api->struct_size < sizeof(vs_module_services_api)) {
    load_failure(path, "module '" + id + "' services descriptor struct_size " +
                           std::to_string(api->struct_size) + " is smaller than " +
                           std::to_string(sizeof(vs_module_services_api)));
  }
  if (api->abi_version != VS_MODULE_SERVICES_ABI_VERSION) {
    load_failure(path, "module '" + id + "' services ABI version " +
                           std::to_string(api->abi_version) +
                           " does not match host services ABI version " +
                           std::to_string(VS_MODULE_SERVICES_ABI_VERSION));
  }
  if (api->attach == nullptr || api->status == nullptr) {
    load_failure(path, "module '" + id + "' is missing required services functions");
  }
  return api;
}

}

void load_failure(const std::filesystem::path &path, const std::string &reason) {
  throw std::runtime_error("cannot load module '" + path.string() + "': " + reason);
}

bool valid_string(const char *text, bool allow_empty) {
  if (text == nullptr) {
    return false;
  }
  const size_t length = strnlen(text, max_string_length);
  return length < max_string_length && (allow_empty || length > 0) &&
         display_detail::valid_utf8(std::string_view(text, length));
}

bool valid_string(const std::string &text, bool allow_empty) {
  return text.find('\0') == std::string::npos && valid_string(text.c_str(), allow_empty);
}

bool valid_id(const char *id) {
  if (!valid_string(id, false)) {
    return false;
  }
  const std::string_view text(id);
  if (text.size() < 3 || text.front() == '.' || text.back() == '.' ||
      text.find('.') == std::string_view::npos || text.find("..") != std::string_view::npos) {
    return false;
  }
  return std::all_of(text.begin(), text.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
           c == '_' || c == '-';
  });
}

LibraryHandle::LibraryHandle(LibraryHandle &&other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)) {}

LibraryHandle &LibraryHandle::operator=(LibraryHandle &&other) noexcept {
  if (this != &other) {
    close();
    handle_ = std::exchange(other.handle_, nullptr);
  }
  return *this;
}

LibraryHandle::~LibraryHandle() { close(); }

void LibraryHandle::close() noexcept {
#if VESPERA_NATIVE_LOADER
  if (handle_ != nullptr) {
    dlclose(handle_);
  }
#endif
  handle_ = nullptr;
}

std::filesystem::path canonical_module_path(const std::filesystem::path &binary) {
  std::error_code error;
  std::filesystem::path path = std::filesystem::canonical(binary, error);
  if (error) {
    load_failure(binary, error.message());
  }
  if (!std::filesystem::is_regular_file(path, error)) {
    load_failure(path, "not a regular file");
  }
  return path;
}

LibraryHandle open_library(const std::filesystem::path &path) {
#if VESPERA_NATIVE_LOADER
  dlerror();
  void *handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr) {
    const char *message = dlerror();
    load_failure(path, message != nullptr ? message : "dlopen failed");
  }
  return LibraryHandle(handle);
#else
  load_failure(path, "native module loading is not supported on this platform yet");
#endif
}

std::shared_ptr<Library> make_library(const std::filesystem::path &path, LibraryHandle handle) {
  const vs_module_api *api = query_entry(path, handle);
  std::vector<ParameterRange> parameters = validate_api(path, api);
  auto library = std::make_shared<Library>();
  library->path = path;
  library->api = api;
  library->id = api->id;
  library->state = query_state(path, handle, library->id);
  library->parameter_values = query_parameter_values(path, handle, library->id);
  library->parameters = std::move(parameters);
  library->metadata = describe(*api, library->state, library->parameter_values);
  query_parameter_metadata(path, handle, *api, *library);
  library->shared = query_shared(path, handle, *library);
  library->shared_branch = query_shared_branch(path, handle, *library);
  library->metadata.shared_engine = library->shared != nullptr;
  library->services = query_services(path, handle, library->id);
  library->metadata.services = library->services != nullptr;
  if (void *symbol = find_symbol(handle, VS_MODULE_FILES_ENTRY_NAME)) {
    library->files =
        reinterpret_cast<vs_module_files_entry_fn>(symbol)(VS_MODULE_FILES_ABI_VERSION);
  }
  std::string files_error;
  auto slots = describe_file_slots(library->files, &files_error);
  if (!slots) {
    load_failure(path, files_error);
  }
  for (const auto &slot : *slots) {
    if (!display_detail::valid_utf8(slot.name)) {
      load_failure(path, "file slot name is not UTF-8");
    }
  }
  library->metadata.file_slots = std::move(*slots);
  library->handle = std::move(handle);
  return library;
}

} // namespace vespera::loader

namespace vespera {

void validate_preset(const Preset &preset) {
  if (preset.schema_version != 1 && preset.schema_version != 2) {
    throw std::invalid_argument("unsupported preset schema version " +
                                std::to_string(preset.schema_version));
  }
  if (preset.nodes.size() > max_preset_nodes) {
    throw std::invalid_argument("preset cannot hold more than " + std::to_string(max_preset_nodes) +
                                " nodes");
  }
  std::vector<NodeId> ids;
  ids.reserve(preset.nodes.size());
  size_t total = 0;
  for (const PresetNode &node : preset.nodes) {
    const std::string where = "preset node " + std::to_string(node.instance_id);
    if (node.files.size() > max_file_slots || (preset.schema_version == 1 && !node.files.empty())) {
      throw std::invalid_argument(where + " files require schema 2 and at most 16 slots");
    }
    std::set<uint32_t> slots;
    for (const auto &file : node.files) {
      if (!file.slot || !slots.insert(file.slot).second || file.reference.state == VS_FILE_NONE ||
          !valid_file_reference(file.reference, max_asset_bytes) ||
          file.reference.path.size() > 4096 ||
          file.reference.path.find('\0') != std::string::npos) {
        throw std::invalid_argument(where + " has an invalid file reference");
      }
    }
    if (node.instance_id == invalid_node_id) {
      throw std::invalid_argument("preset node has the invalid instance ID");
    }
    if (node.instance_id >= preset.next_instance_id) {
      throw std::invalid_argument(where + " is not below next instance ID " +
                                  std::to_string(preset.next_instance_id));
    }
    if (!loader::valid_string(node.module_id, false) || !loader::valid_id(node.module_id.c_str())) {
      throw std::invalid_argument(where + " has an invalid module ID");
    }
    if (!loader::valid_string(node.module_version, false)) {
      throw std::invalid_argument(where + " has an invalid module version");
    }
    if (node.module_abi_version == 0) {
      throw std::invalid_argument(where + " has module ABI version 0");
    }
    if (node.state.schema_version == 0) {
      throw std::invalid_argument(where + " has state schema version 0");
    }
    if (node.state.bytes.size() > max_module_state_bytes) {
      throw std::invalid_argument(where + " state exceeds " +
                                  std::to_string(max_module_state_bytes) + " bytes");
    }
    total += node.state.bytes.size();
    if (total > max_preset_state_bytes) {
      throw std::invalid_argument("preset state exceeds " + std::to_string(max_preset_state_bytes) +
                                  " bytes");
    }
    ids.push_back(node.instance_id);
  }
  std::sort(ids.begin(), ids.end());
  const auto duplicate = std::adjacent_find(ids.begin(), ids.end());
  if (duplicate != ids.end()) {
    throw std::invalid_argument("preset repeats instance ID " + std::to_string(*duplicate));
  }
}

void ModuleCatalog::add(const std::filesystem::path &binary) {
  const std::filesystem::path path = loader::canonical_module_path(binary);
  for (const Entry &entry : entries_) {
    if (entry.binary == path) {
      return;
    }
  }
  ModuleMetadata metadata = loader::make_library(path, loader::open_library(path))->metadata;
  for (const Entry &entry : entries_) {
    if (entry.metadata.id == metadata.id && entry.metadata.version == metadata.version) {
      loader::load_failure(path, "module '" + metadata.id + "' version '" + metadata.version +
                                     "' is already provided by '" + entry.binary.string() + "'");
    }
  }
  entries_.push_back(Entry{path, std::move(metadata)});
}

std::vector<PresetIssue> ModuleCatalog::inspect(const Preset &preset) const {
  validate_preset(preset);
  std::vector<PresetIssue> issues;
  for (const PresetNode &node : preset.nodes) {
    const std::string module =
        "module '" + node.module_id + "' version '" + node.module_version + "'";
    if (!restores_saved_module_abi(node.module_abi_version)) {
      issues.push_back({node.instance_id, module + " requires module ABI version " +
                                              std::to_string(node.module_abi_version) +
                                              " but the host supports " +
                                              std::to_string(VS_MODULE_SETTINGS_ABI_MINIMUM) +
                                              " through " + std::to_string(VS_MODULE_ABI_VERSION)});
      continue;
    }
    const auto entry = std::find_if(entries_.begin(), entries_.end(), [&](const Entry &candidate) {
      return candidate.metadata.id == node.module_id &&
             candidate.metadata.version == node.module_version;
    });
    if (entry == entries_.end()) {
      issues.push_back({node.instance_id, module + " is not in the module catalog"});
    } else if (entry->metadata.state_schema_version == 0) {
      issues.push_back({node.instance_id, module + " does not support state restore"});
    }
  }
  return issues;
}

std::filesystem::path ModuleCatalog::resolve(const PresetNode &node) const {
  const std::string module =
      "module '" + node.module_id + "' version '" + node.module_version + "'";
  if (!restores_saved_module_abi(node.module_abi_version)) {
    throw std::runtime_error(module + " requires unsupported module ABI version " +
                             std::to_string(node.module_abi_version));
  }
  for (const Entry &entry : entries_) {
    if (entry.metadata.id == node.module_id && entry.metadata.version == node.module_version) {
      return entry.binary;
    }
  }
  throw std::runtime_error(module + " is not in the module catalog");
}

}
