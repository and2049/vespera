#include <vespera/file_content.hpp>
#include <vespera/preset.hpp>
#include <vespera/runtime.hpp>

#include <nlohmann/json.hpp>

#include <cerrno>
#include <charconv>
#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#define VESPERA_PRESET_POSIX 1
#else
#include <fstream>
#endif

namespace vespera {
namespace {

using Json = nlohmann::ordered_json;

constexpr uint32_t preset_schema_version = 2;
constexpr int max_json_depth = 16;
constexpr char state_encoding[] = "hex";

[[noreturn]] void invalid(const std::string &message) {
  throw std::invalid_argument("invalid preset: " + message);
}

class StructureGuard {
public:
  bool operator()(int, Json::parse_event_t event, Json &parsed) {
    switch (event) {
    case Json::parse_event_t::object_start:
      enter();
      keys_.emplace_back();
      break;
    case Json::parse_event_t::array_start:
      enter();
      break;
    case Json::parse_event_t::object_end:
      keys_.pop_back();
      --depth_;
      break;
    case Json::parse_event_t::array_end:
      --depth_;
      break;
    case Json::parse_event_t::key:
      if (!keys_.back().insert(parsed.get_ref<const std::string &>()).second) {
        invalid("duplicate object key '" + parsed.get_ref<const std::string &>() + "'");
      }
      break;
    case Json::parse_event_t::value:
      break;
    }
    return true;
  }

private:
  void enter() {
    if (++depth_ > max_json_depth) {
      invalid("JSON nesting exceeds " + std::to_string(max_json_depth) + " levels");
    }
  }

  int depth_ = 0;
  std::vector<std::set<std::string>> keys_;
};

const Json &field(const Json &object, const char *name, const std::string &where) {
  const auto it = object.find(name);
  if (it == object.end()) {
    invalid(where + " is missing '" + name + "'");
  }
  return *it;
}

void require_object(const Json &value, const std::string &where,
                    std::initializer_list<const char *> fields) {
  if (!value.is_object()) {
    invalid(where + " must be an object");
  }
  for (const auto &[key, unused] : value.items()) {
    bool known = false;
    for (const char *name : fields) {
      known = known || key == name;
    }
    if (!known) {
      invalid(where + " has unknown field '" + key + "'");
    }
  }
  for (const char *name : fields) {
    field(value, name, where);
  }
}

uint32_t read_u32(const Json &value, const std::string &where) {
  if (!value.is_number_unsigned()) {
    invalid(where + " must be an unsigned integer");
  }
  const auto number = value.get<uint64_t>();
  if (number > std::numeric_limits<uint32_t>::max()) {
    invalid(where + " exceeds the 32-bit range");
  }
  return static_cast<uint32_t>(number);
}

std::string read_string(const Json &value, const std::string &where) {
  if (!value.is_string()) {
    invalid(where + " must be a string");
  }
  return value.get<std::string>();
}

uint64_t read_id(const Json &value, const std::string &where) {
  if (!value.is_string()) {
    invalid(where + " must be a decimal string");
  }
  const auto &text = value.get_ref<const std::string &>();
  if (text.empty() || (text.size() > 1 && text.front() == '0')) {
    invalid(where + " must be a canonical decimal integer");
  }
  uint64_t number = 0;
  const char *end = text.data() + text.size();
  const auto [stop, error] = std::from_chars(text.data(), end, number);
  if (error == std::errc::result_out_of_range) {
    invalid(where + " exceeds the 64-bit range");
  }
  if (error != std::errc{} || stop != end || text.front() == '-' || text.front() == '+') {
    invalid(where + " must be a canonical decimal integer");
  }
  return number;
}

std::vector<uint8_t> read_hex(const Json &value, const std::string &where, size_t &total) {
  if (!value.is_string()) {
    invalid(where + " must be a string");
  }
  const auto &text = value.get_ref<const std::string &>();
  if (text.size() % 2 != 0) {
    invalid(where + " must contain an even number of hex digits");
  }
  const size_t bytes = text.size() / 2;
  if (bytes > max_module_state_bytes) {
    invalid(where + " exceeds " + std::to_string(max_module_state_bytes) + " bytes");
  }
  if (bytes > max_preset_state_bytes - total) {
    invalid("module state exceeds " + std::to_string(max_preset_state_bytes) + " bytes in total");
  }
  std::vector<uint8_t> result(bytes);
  for (size_t i = 0; i < bytes; ++i) {
    const int high = hex_digit(text[2 * i]);
    const int low = hex_digit(text[2 * i + 1]);
    if (high < 0 || low < 0) {
      invalid(where + " must contain only lowercase hex digits");
    }
    result[i] = static_cast<uint8_t>(high << 4 | low);
  }
  total += bytes;
  return result;
}

PresetNode read_node(const Json &value, size_t index, size_t &state_total, uint32_t schema) {
  const std::string where = "nodes[" + std::to_string(index) + "]";
  if (schema == 2) {
    require_object(value, where, {"instance_id", "module", "state", "files"});
  } else {
    require_object(value, where, {"instance_id", "module", "state"});
  }

  PresetNode node;
  node.instance_id = read_id(value["instance_id"], where + ".instance_id");

  const Json &module = value["module"];
  const std::string module_where = where + ".module";
  require_object(module, module_where, {"id", "version", "abi_version"});
  node.module_id = read_string(module["id"], module_where + ".id");
  node.module_version = read_string(module["version"], module_where + ".version");
  node.module_abi_version = read_u32(module["abi_version"], module_where + ".abi_version");

  const Json &state = value["state"];
  const std::string state_where = where + ".state";
  require_object(state, state_where, {"schema_version", "encoding", "data"});
  node.state.schema_version = read_u32(state["schema_version"], state_where + ".schema_version");
  if (read_string(state["encoding"], state_where + ".encoding") != state_encoding) {
    invalid(state_where + ".encoding must be \"hex\"");
  }
  node.state.bytes = read_hex(state["data"], state_where + ".data", state_total);
  if (schema == 2) {
    const auto &files = value["files"];
    if (!files.is_array() || files.size() > max_file_slots) {
      invalid(where + ".files must be an array of at most 16 references");
    }
    for (const auto &file : files) {
      require_object(file, where + ".files[]", {"slot", "path", "name", "bytes", "sha256"});
      PresetNode::File item;
      item.slot = read_u32(file["slot"], where + ".files[].slot");
      item.reference.state = VS_FILE_MISSING;
      item.reference.path = read_string(file["path"], where + ".files[].path");
      item.reference.name = read_string(file["name"], where + ".files[].name");
      if (!file["bytes"].is_number_unsigned()) {
        invalid("file bytes must be an unsigned integer");
      }
      item.reference.bytes = file["bytes"].get<uint64_t>();
      size_t ignored = 0;
      const auto hash = read_hex(file["sha256"], where + ".files[].sha256", ignored);
      if (hash.size() != 32) {
        invalid("file sha256 must be 32 bytes");
      }
      std::copy(hash.begin(), hash.end(), item.reference.sha256.begin());
      node.files.push_back(std::move(item));
    }
  }
  return node;
}

std::string encode_hex(const std::vector<uint8_t> &bytes) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string text(bytes.size() * 2, '\0');
  for (size_t i = 0; i < bytes.size(); ++i) {
    text[2 * i] = digits[bytes[i] >> 4];
    text[2 * i + 1] = digits[bytes[i] & 0x0f];
  }
  return text;
}

[[noreturn]] void io_failure(int error, const char *what) {
  throw std::system_error(error, std::generic_category(), what);
}

#if VESPERA_PRESET_POSIX

class FileDescriptor {
public:
  explicit FileDescriptor(int fd = -1) noexcept : fd_(fd) {}
  FileDescriptor(const FileDescriptor &) = delete;
  FileDescriptor &operator=(const FileDescriptor &) = delete;
  ~FileDescriptor() { reset(); }

  int get() const noexcept { return fd_; }

  int close() noexcept {
    const int fd = std::exchange(fd_, -1);
    return fd < 0 ? 0 : ::close(fd);
  }

  void reset() noexcept { close(); }

private:
  int fd_;
};

std::string read_bounded(const std::filesystem::path &path) {
  FileDescriptor file(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK));
  if (file.get() < 0) {
    io_failure(errno, "cannot open preset file");
  }
  struct stat info{};
  if (::fstat(file.get(), &info) != 0) {
    io_failure(errno, "cannot inspect preset file");
  }
  if (!S_ISREG(info.st_mode)) {
    throw std::runtime_error("preset path is not a regular file");
  }
  if (info.st_size < 0 || static_cast<uint64_t>(info.st_size) > max_preset_file_bytes) {
    throw std::length_error("preset file exceeds " + std::to_string(max_preset_file_bytes) +
                            " bytes");
  }

  std::string text(static_cast<size_t>(info.st_size), '\0');
  size_t used = 0;
  for (;;) {
    if (used == text.size()) {
      if (text.size() > max_preset_file_bytes) {
        throw std::length_error("preset file exceeds " + std::to_string(max_preset_file_bytes) +
                                " bytes");
      }
      text.resize(std::min(max_preset_file_bytes + 1, std::max<size_t>(text.size() * 2, 4096)));
    }
    const ssize_t count = ::read(file.get(), text.data() + used, text.size() - used);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      io_failure(errno, "cannot read preset file");
    }
    if (count == 0) {
      break;
    }
    used += static_cast<size_t>(count);
  }
  text.resize(used);
  return text;
}

void write_all(int fd, const std::string &text) {
  size_t written = 0;
  while (written < text.size()) {
    const ssize_t count = ::write(fd, text.data() + written, text.size() - written);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      io_failure(errno, "cannot write preset file");
    }
    written += static_cast<size_t>(count);
  }
}

void sync_file(int fd) {
#ifdef F_FULLFSYNC
  if (::fcntl(fd, F_FULLFSYNC) == 0) {
    return;
  }
#endif
  if (::fsync(fd) != 0) {
    io_failure(errno, "cannot flush preset file");
  }
}

void replace_file(const std::filesystem::path &path, const std::string &text) {
  const std::filesystem::path directory =
      path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
  std::string temporary = (directory / ".vespera-preset-XXXXXX").string();

  FileDescriptor file(::mkstemp(temporary.data()));
  if (file.get() < 0) {
    io_failure(errno, "cannot create temporary preset file");
  }
  bool committed = false;
  struct Cleanup {
    const std::string &name;
    const bool &committed;
    ~Cleanup() {
      if (!committed) {
        ::unlink(name.c_str());
      }
    }
  } cleanup{temporary, committed};

  struct stat existing{};
  const mode_t mode = ::stat(path.c_str(), &existing) == 0 && S_ISREG(existing.st_mode)
                          ? static_cast<mode_t>(existing.st_mode & 07777)
                          : static_cast<mode_t>(0644);
  if (::fchmod(file.get(), mode) != 0) {
    io_failure(errno, "cannot set preset file permissions");
  }
  write_all(file.get(), text);
  sync_file(file.get());
  if (file.close() != 0) {
    io_failure(errno, "cannot close preset file");
  }
  if (::rename(temporary.c_str(), path.c_str()) != 0) {
    io_failure(errno, "cannot replace preset file");
  }
  committed = true;

  FileDescriptor parent(::open(directory.c_str(), O_RDONLY | O_CLOEXEC));
  if (parent.get() >= 0) {
    ::fsync(parent.get());
  }
}

#else

std::string read_bounded(const std::filesystem::path &path) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error)) {
    throw std::runtime_error("preset path is not a readable regular file");
  }
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("cannot open preset file");
  }
  std::string text(max_preset_file_bytes + 1, '\0');
  file.read(text.data(), static_cast<std::streamsize>(text.size()));
  if (file.bad()) {
    throw std::runtime_error("cannot read preset file");
  }
  const auto used = static_cast<size_t>(file.gcount());
  if (used > max_preset_file_bytes) {
    throw std::length_error("preset file exceeds " + std::to_string(max_preset_file_bytes) +
                            " bytes");
  }
  text.resize(used);
  return text;
}

void replace_file(const std::filesystem::path &, const std::string &) {
  throw std::runtime_error("atomic preset writes are not supported on this platform");
}

#endif

}

std::string serialize_preset(const Preset &preset) {
  validate_preset(preset);

  Json nodes = Json::array();
  for (const PresetNode &node : preset.nodes) {
    nodes.push_back({
        {"instance_id", std::to_string(node.instance_id)},
        {"module",
         {{"id", node.module_id},
          {"version", node.module_version},
          {"abi_version", node.module_abi_version}}},
        {"state",
         {{"schema_version", node.state.schema_version},
          {"encoding", state_encoding},
          {"data", encode_hex(node.state.bytes)}}},
    });
    if (preset.schema_version == 2) {
      Json files = Json::array();
      for (const auto &file : node.files) {
        files.push_back({{"slot", file.slot},
                         {"path", file.reference.path},
                         {"name", file.reference.name},
                         {"bytes", file.reference.bytes},
                         {"sha256", file_hash_text(file.reference.sha256)}});
      }
      nodes.back()["files"] = std::move(files);
    }
  }
  const Json document = {
      {"schema_version", preset.schema_version},
      {"next_instance_id", std::to_string(preset.next_instance_id)},
      {"nodes", std::move(nodes)},
  };
  try {
    return document.dump(2) + "\n";
  } catch (const Json::type_error &) {
    invalid("module strings must be valid UTF-8");
  }
}

Preset parse_preset(std::string_view text) {
  if (text.size() > max_preset_file_bytes) {
    throw std::length_error("preset exceeds " + std::to_string(max_preset_file_bytes) + " bytes");
  }

  Json document;
  try {
    document = Json::parse(text.begin(), text.end(), StructureGuard{});
  } catch (const Json::parse_error &error) {
    invalid("malformed JSON at byte " + std::to_string(error.byte));
  } catch (const Json::out_of_range &) {
    invalid("JSON number exceeds the supported range");
  }

  if (!document.is_object()) {
    invalid("document must be an object");
  }
  const uint32_t schema = read_u32(field(document, "schema_version", "document"), "schema_version");
  if (schema != 1 && schema != preset_schema_version) {
    invalid("unsupported schema version " + std::to_string(schema));
  }
  require_object(document, "document", {"schema_version", "next_instance_id", "nodes"});

  Preset preset;
  preset.schema_version = schema;
  preset.next_instance_id = read_id(document["next_instance_id"], "next_instance_id");

  const Json &nodes = document["nodes"];
  if (!nodes.is_array()) {
    invalid("nodes must be an array");
  }
  if (nodes.size() > max_preset_nodes) {
    invalid("preset cannot hold more than " + std::to_string(max_preset_nodes) + " nodes");
  }
  preset.nodes.reserve(nodes.size());
  size_t state_total = 0;
  for (size_t i = 0; i < nodes.size(); ++i) {
    preset.nodes.push_back(read_node(nodes[i], i, state_total, schema));
  }

  validate_preset(preset);
  return preset;
}

Preset read_preset(const std::filesystem::path &path) {
  auto preset = parse_preset(read_bounded(path));
  const auto parent = std::filesystem::absolute(path).parent_path();
  for (auto &node : preset.nodes) {
    for (auto &file : node.files) {
      if (!file.reference.path.empty()) {
        file.reference.path = (parent / file.reference.path).lexically_normal().string();
      }
    }
  }
  return preset;
}

void write_preset(const std::filesystem::path &path, const Preset &preset) {
  auto relative = preset;
  const auto parent = std::filesystem::absolute(path).parent_path();
  for (auto &node : relative.nodes) {
    for (auto &file : node.files) {
      if (file.reference.path.empty()) {
        continue;
      }
      const auto absolute = std::filesystem::absolute(file.reference.path).lexically_normal();
      const auto relative_path = absolute.lexically_relative(parent);
      file.reference.path = (relative_path.empty() ? absolute : relative_path).string();
      file.reference.state = VS_FILE_MISSING;
    }
  }
  replace_file(path, serialize_preset(relative));
}

}
