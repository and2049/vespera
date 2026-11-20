#include <vespera/preset.hpp>

#include <stdlib.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;

using vespera::ModuleState;
using vespera::NodeId;
using vespera::Preset;
using vespera::PresetNode;

int failures = 0;
const char *current_test = "";

#define CHECK(condition)                                                                           \
  do {                                                                                             \
    if (!(condition)) {                                                                            \
      std::fprintf(stderr, "%s:%d: %s: CHECK(%s) failed\n", __FILE__, __LINE__, current_test,      \
                   #condition);                                                                    \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (false)

#define CHECK_REJECTS(text) CHECK(rejects(text))

constexpr NodeId max_id = std::numeric_limits<NodeId>::max();

const std::string base_document =
    R"({"schema_version":1,"next_instance_id":"8","nodes":[{"instance_id":"3",)"
    R"("module":{"id":"com.example.gain","version":"1.2.0","abi_version":1},)"
    R"("state":{"schema_version":2,"encoding":"hex","data":"00ff10"}}]})";

std::string replace(std::string text, std::string_view from, std::string_view to) {
  const size_t at = text.find(from);
  if (at == std::string::npos) {
    std::fprintf(stderr, "%s: test fixture does not contain '%.*s'\n", current_test,
                 static_cast<int>(from.size()), from.data());
    std::abort();
  }
  return text.replace(at, from.size(), to);
}

std::string with(std::string_view from, std::string_view to) {
  return replace(base_document, from, to);
}

bool is_library_exception(const std::exception &error) {
  return std::string_view(error.what()).starts_with("[json.exception");
}

std::string failure_message(const std::function<void()> &action) {
  try {
    action();
  } catch (const std::exception &error) {
    if (is_library_exception(error)) {
      return {};
    }
    const std::string message = error.what();
    return message.empty() ? std::string("(empty)") : message;
  }
  return {};
}

bool fails(const std::function<void()> &action) { return !failure_message(action).empty(); }

bool rejects(const std::string &text) {
  return fails([&] { vespera::parse_preset(text); });
}

bool accepts(const std::string &text) {
  try {
    vespera::parse_preset(text);
    return true;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s: unexpected rejection: %s\n", current_test, error.what());
    return false;
  }
}

bool same_node(const PresetNode &a, const PresetNode &b) {
  return a.instance_id == b.instance_id && a.module_id == b.module_id &&
         a.module_version == b.module_version && a.module_abi_version == b.module_abi_version &&
         a.state.schema_version == b.state.schema_version && a.state.bytes == b.state.bytes;
}

bool same_preset(const Preset &a, const Preset &b) {
  if (a.schema_version != b.schema_version || a.next_instance_id != b.next_instance_id ||
      a.nodes.size() != b.nodes.size()) {
    return false;
  }
  for (size_t i = 0; i < a.nodes.size(); ++i) {
    if (!same_node(a.nodes[i], b.nodes[i])) {
      return false;
    }
  }
  return true;
}

PresetNode make_node(NodeId id, std::string module_id, std::vector<uint8_t> bytes = {}) {
  PresetNode node;
  node.instance_id = id;
  node.module_id = std::move(module_id);
  node.module_version = "1.0.0";
  node.module_abi_version = VS_MODULE_ABI_VERSION;
  node.state = ModuleState{1, std::move(bytes)};
  return node;
}

std::vector<uint8_t> all_byte_values() {
  std::vector<uint8_t> bytes;
  for (int i = 0; i < 256; ++i) {
    bytes.push_back(static_cast<uint8_t>(i));
  }
  return bytes;
}

Preset sample_preset() {
  Preset preset;
  preset.next_instance_id = max_id;
  preset.nodes.push_back(make_node(max_id - 1, "com.example.gain", all_byte_values()));
  preset.nodes.push_back(make_node(0, "com.example.empty"));
  preset.nodes.push_back(make_node((NodeId{1} << 53) + 1, "com.example.precision", {0x2a}));
  preset.nodes[0].module_version = "2.0.0-beta+build.7";
  preset.nodes[0].state.schema_version = std::numeric_limits<uint32_t>::max();
  preset.nodes[1].module_abi_version = std::numeric_limits<uint32_t>::max();
  preset.nodes[2].module_version = "\xc3\xa9t\xc3\xa9 \"quoted\" \\ \x01";
  return preset;
}

std::string hex_of(size_t bytes, char digit = 'a') { return std::string(bytes * 2, digit); }

std::string node_text(NodeId id, std::string_view hex) {
  return R"({"instance_id":")" + std::to_string(id) +
         R"(","module":{"id":"com.example.m","version":"1","abi_version":1},)"
         R"("state":{"schema_version":1,"encoding":"hex","data":")" +
         std::string(hex) + R"("}})";
}

std::string document_with(size_t count, const std::function<std::string(size_t)> &hex) {
  std::string text =
      R"({"schema_version":1,"next_instance_id":")" + std::to_string(count) + R"(","nodes":[)";
  for (size_t i = 0; i < count; ++i) {
    text += (i == 0 ? "" : ",") + node_text(i, hex(i));
  }
  return text + "]}";
}

class TempDirectory {
public:
  TempDirectory() {
    std::string pattern = (fs::temp_directory_path() / "vespera-preset-test-XXXXXX").string();
    if (::mkdtemp(pattern.data()) == nullptr) {
      std::perror("mkdtemp");
      std::abort();
    }
    path_ = pattern;
  }
  TempDirectory(const TempDirectory &) = delete;
  TempDirectory &operator=(const TempDirectory &) = delete;
  ~TempDirectory() {
    std::error_code ignored;
    fs::permissions(path_, fs::perms::owner_all, fs::perm_options::add, ignored);
    fs::remove_all(path_, ignored);
  }

  const fs::path &path() const { return path_; }

  std::vector<std::string> entries() const {
    std::vector<std::string> names;
    for (const auto &entry : fs::directory_iterator(path_)) {
      names.push_back(entry.path().filename().string());
    }
    return names;
  }

private:
  fs::path path_;
};

std::string read_file(const fs::path &path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void write_file(const fs::path &path, std::string_view text) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(text.data(), static_cast<std::streamsize>(text.size()));
}

void base_document_parses() {
  current_test = "base_document_parses";
  const Preset preset = vespera::parse_preset(base_document);
  CHECK(preset.schema_version == 1);
  CHECK(preset.next_instance_id == 8);
  CHECK(preset.nodes.size() == 1);
  if (preset.nodes.size() == 1) {
    const PresetNode &node = preset.nodes[0];
    CHECK(node.instance_id == 3);
    CHECK(node.module_id == "com.example.gain");
    CHECK(node.module_version == "1.2.0");
    CHECK(node.module_abi_version == 1);
    CHECK(node.state.schema_version == 2);
    CHECK((node.state.bytes == std::vector<uint8_t>{0x00, 0xff, 0x10}));
  }
  CHECK(accepts("\n\t " + base_document + " \r\n"));
  CHECK(accepts(R"({"nodes":[],"next_instance_id":"0","schema_version":1})"));
}

void round_trip_is_exact() {
  current_test = "round_trip_is_exact";
  const Preset original = sample_preset();
  const std::string text = vespera::serialize_preset(original);
  const Preset parsed = vespera::parse_preset(text);
  CHECK(same_preset(original, parsed));
  CHECK(vespera::serialize_preset(parsed) == text);
  CHECK(text.find("\"18446744073709551615\"") != std::string::npos);
  CHECK(text.find("\"18446744073709551614\"") != std::string::npos);
  CHECK(text.find("\"9007199254740993\"") != std::string::npos);
  CHECK(text.find(":18446744073709551614") == std::string::npos);

  Preset empty;
  CHECK(same_preset(vespera::parse_preset(vespera::serialize_preset(empty)), empty));
}

void unknown_modules_keep_opaque_state() {
  current_test = "unknown_modules_keep_opaque_state";
  const std::string text =
      with(R"("id":"com.example.gain","version":"1.2.0","abi_version":1)",
           R"("id":"vendor.never-installed","version":"9.9","abi_version":77)");
  const std::string patched =
      replace(replace(text, R"("schema_version":2)", R"("schema_version":4000000000)"),
              R"("data":"00ff10")", R"("data":"deadbeef00")");
  const Preset preset = vespera::parse_preset(patched);
  CHECK(preset.nodes.size() == 1);
  if (preset.nodes.size() == 1) {
    CHECK(preset.nodes[0].module_id == "vendor.never-installed");
    CHECK(preset.nodes[0].module_abi_version == 77);
    CHECK(preset.nodes[0].state.schema_version == 4000000000u);
    CHECK((preset.nodes[0].state.bytes == std::vector<uint8_t>{0xde, 0xad, 0xbe, 0xef, 0x00}));
    CHECK(same_preset(vespera::parse_preset(vespera::serialize_preset(preset)), preset));
  }
}

void malformed_json_is_rejected() {
  current_test = "malformed_json_is_rejected";
  for (const std::string &text : {
           std::string(),
           std::string("   "),
           std::string("{"),
           std::string("null"),
           std::string("[]"),
           std::string("\"preset\""),
           std::string("1"),
           base_document + "x",
           base_document + base_document,
           base_document.substr(0, base_document.size() - 1),
           with(R"("data":"00ff10"})", R"("data":"00ff10",})"),
           with(R"({"schema_version":1,)", R"({'schema_version':1,)"),
           with(R"({"schema_version":1,)", R"({/* c */"schema_version":1,)"),
           with(R"("schema_version":2)", R"("schema_version":NaN)"),
           with(R"("version":"1.2.0")", "\"version\":\"1.2\xff\""),
           with(R"("version":"1.2.0")", "\"version\":\"1.2\n\""),
       }) {
    CHECK_REJECTS(text);
  }
}

void duplicate_keys_are_rejected() {
  current_test = "duplicate_keys_are_rejected";
  CHECK_REJECTS(with(R"({"schema_version":1,)", R"({"schema_version":1,"schema_version":1,)"));
  CHECK_REJECTS(
      with(R"("next_instance_id":"8")", R"("next_instance_id":"9","next_instance_id":"8")"));
  CHECK_REJECTS(with(R"("nodes":[)", R"("nodes":[],"nodes":[)"));
  CHECK_REJECTS(with(R"({"instance_id":"3",)", R"({"instance_id":"3","instance_id":"4",)"));
  CHECK_REJECTS(with(R"("id":"com.example.gain")", R"("id":"a","id":"com.example.gain")"));
  CHECK_REJECTS(with(R"("data":"00ff10")", R"("data":"00","data":"00ff10")"));
  CHECK_REJECTS(with(R"("encoding":"hex")", R"("encoding":"hex","encoding":"hex")"));
}

void unknown_and_missing_fields_are_rejected() {
  current_test = "unknown_and_missing_fields_are_rejected";
  CHECK_REJECTS(with(R"({"schema_version":1,)", R"({"schema_version":1,"modules":[],)"));
  CHECK_REJECTS(with(R"({"instance_id":"3",)", R"({"instance_id":"3","path":"/tmp/x.so",)"));
  CHECK_REJECTS(with(R"("abi_version":1})", R"("abi_version":1,"binary":"gain.dylib"})"));
  CHECK_REJECTS(with(R"("data":"00ff10")", R"("data":"00ff10","extra":null)"));

  CHECK_REJECTS(with(R"("next_instance_id":"8",)", ""));
  CHECK_REJECTS(with(R"({"schema_version":1,)", "{"));
  CHECK_REJECTS(with(
      R"(,"nodes":[{"instance_id":"3","module":{"id":"com.example.gain","version":"1.2.0","abi_version":1},"state":{"schema_version":2,"encoding":"hex","data":"00ff10"}}])",
      ""));
  CHECK_REJECTS(with(R"("instance_id":"3",)", ""));
  CHECK_REJECTS(with(R"("id":"com.example.gain",)", ""));
  CHECK_REJECTS(with(R"("version":"1.2.0",)", ""));
  CHECK_REJECTS(with(R"(,"abi_version":1)", ""));
  CHECK_REJECTS(with(R"("schema_version":2,)", ""));
  CHECK_REJECTS(with(R"("encoding":"hex",)", ""));
  CHECK_REJECTS(with(R"(,"data":"00ff10")", ""));
}

void unsupported_schema_is_rejected() {
  current_test = "unsupported_schema_is_rejected";
  CHECK_REJECTS(with(R"({"schema_version":1,)", R"({"schema_version":2,)"));
  CHECK_REJECTS(with(R"({"schema_version":1,)", R"({"schema_version":0,)"));
  CHECK_REJECTS(with(R"({"schema_version":1,)", R"({"schema_version":2,"future":{},)"));
  CHECK_REJECTS(R"({"schema_version":2})");
}

void invalid_types_and_numbers_are_rejected() {
  current_test = "invalid_types_and_numbers_are_rejected";
  for (const char *version :
       {R"("1")", "1.0", "1e0", "1e9999", "-1e9999", "-1", "true", "null", "[1]"}) {
    CHECK_REJECTS(
        with(R"({"schema_version":1,)", std::string(R"({"schema_version":)") + version + ","));
  }
  for (const char *id :
       {"8", R"("")", R"("08")", R"("00")", R"("-1")", R"("+8")", R"(" 8")", R"("8 ")", R"("8a")",
        R"("0x8")", R"("8.0")", R"("1e3")", R"("18446744073709551616")",
        R"("99999999999999999999999")", "null", R"(["8"])"}) {
    CHECK_REJECTS(with(R"("next_instance_id":"8")", std::string(R"("next_instance_id":)") + id));
    CHECK_REJECTS(with(R"("instance_id":"3")", std::string(R"("instance_id":)") + id));
  }
  for (const char *number : {"4294967296", "-1", "1.5", "1.0", "1e2", R"("1")", "false"}) {
    CHECK_REJECTS(with(R"("abi_version":1)", std::string(R"("abi_version":)") + number));
    CHECK_REJECTS(with(R"("schema_version":2)", std::string(R"("schema_version":)") + number));
  }
  CHECK(accepts(with(R"("abi_version":1)", R"("abi_version":4294967295)")));

  CHECK_REJECTS(with(R"("id":"com.example.gain")", R"("id":7)"));
  CHECK_REJECTS(with(R"("version":"1.2.0")", R"("version":1.2)"));
  CHECK_REJECTS(with(R"("encoding":"hex")", R"("encoding":"base64")"));
  CHECK_REJECTS(with(R"("encoding":"hex")", R"("encoding":"HEX")"));
  CHECK_REJECTS(with(R"("encoding":"hex")", R"("encoding":null)"));
  CHECK_REJECTS(with(R"("data":"00ff10")", R"("data":"00FF10")"));
  CHECK_REJECTS(with(R"("data":"00ff10")", R"("data":"00ff1")"));
  CHECK_REJECTS(with(R"("data":"00ff10")", R"("data":"00ff1g")"));
  CHECK_REJECTS(with(R"("data":"00ff10")", R"("data":"00 ff10")"));
  CHECK_REJECTS(with(R"("data":"00ff10")", R"("data":[0,255,16])"));
  CHECK(accepts(with(R"("data":"00ff10")", R"("data":"")")));

  CHECK_REJECTS(with(R"("nodes":[)", R"("nodes":{"0":)"));
  CHECK_REJECTS(R"({"schema_version":1,"next_instance_id":"0","nodes":null})");
  CHECK_REJECTS(R"({"schema_version":1,"next_instance_id":"0","nodes":[1]})");
  CHECK_REJECTS(R"({"schema_version":1,"next_instance_id":"0","nodes":[[]]})");
  CHECK_REJECTS(with(R"("module":{)", R"("module":[{)"));
  CHECK_REJECTS(with(R"("state":{"schema_version":2,"encoding":"hex","data":"00ff10"})",
                     R"("state":"00ff10")"));
}

void identity_rules_are_enforced() {
  current_test = "identity_rules_are_enforced";
  const std::string two_nodes = document_with(2, [](size_t) { return std::string("01"); });
  CHECK(accepts(two_nodes));
  CHECK_REJECTS(replace(two_nodes, R"("instance_id":"1")", R"("instance_id":"0")"));
  CHECK_REJECTS(replace(two_nodes, R"("next_instance_id":"2")", R"("next_instance_id":"1")"));
  CHECK_REJECTS(replace(two_nodes, R"("next_instance_id":"2")", R"("next_instance_id":"0")"));
  CHECK(accepts(replace(two_nodes, R"("next_instance_id":"2")",
                        R"("next_instance_id":"18446744073709551615")")));
  CHECK_REJECTS(replace(replace(two_nodes, R"("next_instance_id":"2")",
                                R"("next_instance_id":"18446744073709551615")"),
                        R"("instance_id":"1")", R"("instance_id":"18446744073709551615")"));

  Preset duplicate = sample_preset();
  duplicate.nodes[1].instance_id = duplicate.nodes[0].instance_id;
  CHECK(fails([&] { vespera::serialize_preset(duplicate); }));
  Preset stale = sample_preset();
  stale.next_instance_id = stale.nodes[0].instance_id;
  CHECK(fails([&] { vespera::serialize_preset(stale); }));
  Preset bad_schema = sample_preset();
  bad_schema.schema_version = 3;
  CHECK(fails([&] { vespera::serialize_preset(bad_schema); }));
}

void state_budgets_are_enforced() {
  current_test = "state_budgets_are_enforced";
  const size_t module_limit = vespera::max_module_state_bytes;
  const size_t total_limit = vespera::max_preset_state_bytes;
  CHECK(accepts(document_with(1, [&](size_t) { return hex_of(module_limit); })));
  CHECK_REJECTS(document_with(1, [&](size_t) { return hex_of(module_limit + 1); }));

  const size_t full_nodes = total_limit / module_limit;
  CHECK(accepts(document_with(full_nodes, [&](size_t) { return hex_of(module_limit); })));
  CHECK_REJECTS(document_with(
      full_nodes + 1, [&](size_t i) { return i < full_nodes ? hex_of(module_limit) : hex_of(1); }));

  const size_t node_limit = vespera::max_preset_nodes;
  CHECK(accepts(document_with(node_limit, [](size_t) { return std::string(); })));
  CHECK_REJECTS(document_with(node_limit + 1, [](size_t) { return std::string(); }));

  Preset large;
  large.next_instance_id = 1;
  large.nodes.push_back(make_node(0, "com.example.m", std::vector<uint8_t>(module_limit + 1)));
  CHECK(fails([&] { vespera::serialize_preset(large); }));
  large.nodes[0].state.bytes.resize(module_limit);
  CHECK(!fails([&] { vespera::serialize_preset(large); }));

  Preset invalid_text = sample_preset();
  invalid_text.nodes[0].module_version = "bad\xff";
  CHECK(fails([&] { vespera::serialize_preset(invalid_text); }));
}

void structure_limits_are_enforced() {
  current_test = "structure_limits_are_enforced";
  std::string nested;
  for (int depth = 0; depth < 32; ++depth) {
    nested = "[" + nested + "]";
  }
  CHECK_REJECTS(with(R"({"schema_version":1,)", R"({"schema_version":1,"deep":)" + nested + ","));
  CHECK_REJECTS(std::string(200000, '[') + std::string(200000, ']'));
  CHECK_REJECTS(std::string(200000, '['));
  std::string objects;
  for (int depth = 0; depth < 200000; ++depth) {
    objects += R"({"a":)";
  }
  CHECK_REJECTS(objects);

  std::string oversized(vespera::max_preset_file_bytes + 1, ' ');
  oversized.replace(0, base_document.size(), base_document);
  CHECK_REJECTS(oversized);
}

void files_round_trip() {
  current_test = "files_round_trip";
  TempDirectory directory;
  const fs::path path = directory.path() / "song.vspreset";
  const Preset original = sample_preset();
  vespera::write_preset(path, original);
  CHECK(same_preset(vespera::read_preset(path), original));
  CHECK(read_file(path) == vespera::serialize_preset(original));
  CHECK(directory.entries() == std::vector<std::string>{"song.vspreset"});

  Preset replacement;
  replacement.next_instance_id = 5;
  replacement.nodes.push_back(make_node(4, "com.example.other", {1, 2, 3}));
  fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read);
  vespera::write_preset(path, replacement);
  CHECK(same_preset(vespera::read_preset(path), replacement));
  CHECK(directory.entries() == std::vector<std::string>{"song.vspreset"});
  CHECK((fs::status(path).permissions() & fs::perms::all) ==
        (fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read));
}

void file_errors_preserve_target() {
  current_test = "file_errors_preserve_target";
  TempDirectory directory;
  const fs::path path = directory.path() / "keep.vspreset";
  write_file(path, "original contents");

  Preset invalid = sample_preset();
  invalid.nodes[1].instance_id = invalid.nodes[0].instance_id;
  CHECK(fails([&] { vespera::write_preset(path, invalid); }));
  Preset unencodable = sample_preset();
  unencodable.nodes[0].module_version = "bad\xc3";
  CHECK(fails([&] { vespera::write_preset(path, unencodable); }));
  CHECK(read_file(path) == "original contents");
  CHECK(directory.entries() == std::vector<std::string>{"keep.vspreset"});

  const fs::path occupied = directory.path() / "occupied";
  fs::create_directory(occupied);
  write_file(occupied / "inside", "untouched");
  CHECK(fails([&] { vespera::write_preset(occupied, sample_preset()); }));
  CHECK(fs::is_directory(occupied));
  CHECK(read_file(occupied / "inside") == "untouched");
  std::vector<std::string> entries = directory.entries();
  std::sort(entries.begin(), entries.end());
  CHECK((entries == std::vector<std::string>{"keep.vspreset", "occupied"}));

  CHECK(fails([&] { vespera::write_preset(directory.path() / "missing" / "x", sample_preset()); }));
  CHECK(!fs::exists(directory.path() / "missing"));
}

void read_errors_are_bounded_and_private() {
  current_test = "read_errors_are_bounded_and_private";
  TempDirectory directory;
  const fs::path missing = directory.path() / "secret-name-4821.vspreset";
  const std::string message = failure_message([&] { vespera::read_preset(missing); });
  CHECK(!message.empty());
  CHECK(message.find("secret-name-4821") == std::string::npos);

  CHECK(fails([&] { vespera::read_preset(directory.path()); }));

  const fs::path huge = directory.path() / "huge.vspreset";
  write_file(huge, base_document);
  fs::resize_file(huge, vespera::max_preset_file_bytes + 1);
  CHECK(fails([&] { vespera::read_preset(huge); }));

  const fs::path padded = directory.path() / "padded.vspreset";
  std::string text = base_document;
  text.resize(vespera::max_preset_file_bytes, ' ');
  write_file(padded, text);
  CHECK(vespera::read_preset(padded).next_instance_id == 8);

  const fs::path malformed = directory.path() / "malformed.vspreset";
  write_file(malformed, with(R"("data":"00ff10")", R"("data":"00ff10","data":"")"));
  CHECK(fails([&] { vespera::read_preset(malformed); }));
}

}

int main() {
  base_document_parses();
  round_trip_is_exact();
  unknown_modules_keep_opaque_state();
  malformed_json_is_rejected();
  duplicate_keys_are_rejected();
  unknown_and_missing_fields_are_rejected();
  unsupported_schema_is_rejected();
  invalid_types_and_numbers_are_rejected();
  identity_rules_are_enforced();
  state_budgets_are_enforced();
  structure_limits_are_enforced();
  files_round_trip();
  file_errors_preserve_target();
  read_errors_are_bounded_and_private();
  if (failures != 0) {
    std::fprintf(stderr, "%d preset check(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  std::puts("preset tests passed");
  return EXIT_SUCCESS;
}
