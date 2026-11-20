#ifndef VESPERA_SRC_MODULE_LOADER_HPP
#define VESPERA_SRC_MODULE_LOADER_HPP

// Loads and validates native module libraries: the dlopen handle, the loaded descriptor set and
// the loader entry points shared by the runtime's graph assembly and the module catalog.

#include <vespera/runtime.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
#define VESPERA_NATIVE_LOADER 1
#else
#define VESPERA_NATIVE_LOADER 0
#endif

namespace vespera::loader {

struct ParameterRange {
  uint32_t id;
  float minimum;
  float maximum;
};

[[noreturn]] void load_failure(const std::filesystem::path &path, const std::string &reason);
bool valid_string(const char *text, bool allow_empty);
bool valid_string(const std::string &text, bool allow_empty);
bool valid_id(const char *id);

class LibraryHandle {
public:
  LibraryHandle() = default;
  explicit LibraryHandle(void *handle) : handle_(handle) {}
  ~LibraryHandle();
  LibraryHandle(const LibraryHandle &) = delete;
  LibraryHandle &operator=(const LibraryHandle &) = delete;
  LibraryHandle(LibraryHandle &&other) noexcept;
  LibraryHandle &operator=(LibraryHandle &&other) noexcept;

  void *get() const noexcept { return handle_; }

private:
  void close() noexcept;

  void *handle_ = nullptr;
};

struct Library {
  std::filesystem::path path;
  LibraryHandle handle;
  const vs_module_api *api = nullptr;
  const vs_module_state_api *state = nullptr;
  const vs_module_parameter_api *parameter_values = nullptr;
  const vs_module_shared_api *shared = nullptr;
  const vs_module_shared_branch_api *shared_branch = nullptr;
  const vs_module_services_api *services = nullptr;
  const vs_module_files_api *files = nullptr;
  // Copies of the shared descriptor's targets with the names validated; names stay owned by the
  // loaded library.
  std::vector<vs_shared_target> shared_targets;
  uint32_t owned_targets = 0;
  std::string id;
  std::vector<ParameterRange> parameters;
  ModuleMetadata metadata;

  const ParameterRange *find_parameter(uint32_t parameter_id) const noexcept {
    const auto it = std::lower_bound(
        parameters.begin(), parameters.end(), parameter_id,
        [](const ParameterRange &range, uint32_t value) { return range.id < value; });
    return it != parameters.end() && it->id == parameter_id ? &*it : nullptr;
  }
};

std::filesystem::path canonical_module_path(const std::filesystem::path &binary);
LibraryHandle open_library(const std::filesystem::path &path);
std::shared_ptr<Library> make_library(const std::filesystem::path &path, LibraryHandle handle);

}

#endif
