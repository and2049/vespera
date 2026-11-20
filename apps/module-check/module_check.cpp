#include "module_check.hpp"

#include "allocation.hpp"
#include "checks.hpp"

#include <vespera/runtime.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <optional>
#include <set>

namespace vespera::module_check {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

struct Target {
  std::string role;
  std::string id;
  fs::path binary;
  std::optional<LayoutTarget> layout;
  std::optional<RoutingTarget> routing;
};

bool inside(const fs::path &root, const fs::path &path) {
  auto r = root.begin();
  auto p = path.begin();
  for (; r != root.end(); ++r, ++p) {
    if (p == path.end() || *r != *p) {
      return false;
    }
  }
  return p != path.end();
}

// Resolves a package-relative manifest path as discovery does: relative, without "..", and still
// inside the package after following symbolic links.
std::optional<fs::path> resolve(const fs::path &package, const std::string &relative,
                                const std::string &key, Findings &findings) {
  const fs::path path(relative);
  if (relative.empty() || path.is_absolute() ||
      std::any_of(path.begin(), path.end(), [](const fs::path &part) { return part == ".."; })) {
    findings.fail(key + " \"" + relative + "\" must be a relative path inside the package");
    return std::nullopt;
  }
  std::error_code error;
  const fs::path root = fs::weakly_canonical(package, error);
  const fs::path resolved = fs::weakly_canonical(package / path, error);
  if (error || !inside(root, resolved)) {
    findings.fail(key + " \"" + relative + "\" escapes the package");
    return std::nullopt;
  }
  if (!fs::exists(resolved, error)) {
    findings.fail(key + " \"" + relative + "\" does not exist");
    return std::nullopt;
  }
  return resolved;
}

std::optional<std::string> text_field(const json &object, const std::string &key,
                                      const std::string &where, bool required, Findings &findings) {
  const auto it = object.find(key);
  if (it == object.end()) {
    if (required) {
      findings.fail(where + key + " is missing");
    }
    return std::nullopt;
  }
  if (!it->is_string()) {
    findings.fail(where + key + " must be a string");
    return std::nullopt;
  }
  return it->get<std::string>();
}

bool whole(const json &value) {
  return value.is_number_integer() ||
         (value.is_number_float() && std::floor(value.get<double>()) == value.get<double>());
}

std::optional<LayoutTarget> layout_of(const json &owner, const std::string &where,
                                      const fs::path &package,
                                      const std::optional<fs::path> &resources,
                                      Findings &findings) {
  const auto ui = owner.find("ui");
  if (ui == owner.end()) {
    return std::nullopt;
  }
  if (!ui->is_object()) {
    findings.fail(where + "ui must be an object");
    return std::nullopt;
  }
  const auto layout = text_field(*ui, "layout", where + "ui.", true, findings);
  if (!layout || !resources) {
    return std::nullopt;
  }
  const auto path = resolve(package, *layout, where + "ui.layout", findings);
  if (!path) {
    return std::nullopt;
  }
  if (!inside(*resources, *path)) {
    findings.fail(where + "ui.layout \"" + *layout + "\" must be inside the resources directory");
    return std::nullopt;
  }
  return LayoutTarget{*resources, path->lexically_relative(*resources).generic_string()};
}

constexpr uint32_t max_background_side = 4096;
constexpr uint64_t max_background_pixels = 8'000'000;

uint32_t crc32(const unsigned char *bytes, size_t size) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < size; ++i) {
    crc ^= bytes[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  return ~crc;
}

uint32_t big_endian(const unsigned char *bytes) {
  return uint32_t{bytes[0]} << 24 | uint32_t{bytes[1]} << 16 | uint32_t{bytes[2]} << 8 |
         uint32_t{bytes[3]};
}

// Checks only the PNG signature and image header against the window background budget, as host
// discovery does. The macOS host additionally decodes the whole image when it loads the amp and
// rejects corrupt, truncated or animated image data, which this header check does not see.
std::string png_header_problem(const fs::path &path, uint32_t &width, uint32_t &height) {
  constexpr std::array<unsigned char, 8> signature{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  std::array<unsigned char, 33> bytes{};
  std::ifstream stream(path, std::ios::binary);
  stream.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
  const auto count = static_cast<size_t>(stream.gcount());
  if (count < signature.size() || !std::equal(signature.begin(), signature.end(), bytes.begin())) {
    return "is not a PNG image";
  }
  if (count < bytes.size()) {
    return "is truncated";
  }
  if (big_endian(&bytes[8]) != 13 || std::string(&bytes[12], &bytes[16]) != "IHDR" ||
      crc32(&bytes[12], 17) != big_endian(&bytes[29])) {
    return "has no valid PNG image header";
  }
  width = big_endian(&bytes[16]);
  height = big_endian(&bytes[20]);
  const unsigned depth = bytes[24];
  const unsigned colour = bytes[25];
  const bool depth_valid = [&] {
    switch (colour) {
    case 0:
      return depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
    case 3:
      return depth == 1 || depth == 2 || depth == 4 || depth == 8;
    case 2:
    case 4:
    case 6:
      return depth == 8 || depth == 16;
    default:
      return false;
    }
  }();
  if (!depth_valid || bytes[26] != 0 || bytes[27] != 0 || bytes[28] > 1) {
    return "has an invalid PNG image header";
  }
  if (width == 0 || height == 0) {
    return "has no pixels";
  }
  if (width > max_background_side || height > max_background_side ||
      uint64_t{width} * height > max_background_pixels) {
    return "is " + std::to_string(width) + "x" + std::to_string(height) +
           " pixels; window backgrounds may be at most " + std::to_string(max_background_side) +
           " pixels per side and " + std::to_string(max_background_pixels) + " pixels in total";
  }
  return "";
}

// An amp's optional window background; null is the same as absent. Returns its pixel size.
std::string window_background(const json &manifest, const fs::path &package,
                              const std::optional<std::string> &category,
                              const std::optional<fs::path> &resources, Findings &findings) {
  const auto field = manifest.find("windowBackground");
  if (field == manifest.end() || field->is_null()) {
    return "";
  }
  if (!field->is_string()) {
    findings.fail("windowBackground must be a string or null");
    return "";
  }
  if (category && *category != "amp") {
    findings.fail("only amps declare a windowBackground");
    return "";
  }
  const std::string relative = field->get<std::string>();
  const auto path = resolve(package, relative, "windowBackground", findings);
  if (!path || !resources) {
    return "";
  }
  if (!inside(*resources, *path)) {
    findings.fail("windowBackground \"" + relative + "\" must be inside the resources directory");
    return "";
  }
  std::error_code error;
  if (!fs::is_regular_file(*path, error)) {
    findings.fail("windowBackground \"" + relative + "\" is not a file");
    return "";
  }
  uint32_t width = 0;
  uint32_t height = 0;
  if (const std::string problem = png_header_problem(*path, width, height); !problem.empty()) {
    findings.fail("windowBackground \"" + relative + "\" " + problem);
    return "";
  }
  return std::to_string(width) + "x" + std::to_string(height);
}

const std::set<std::string> &placements() {
  static const std::set<std::string> values{"before-pre-fx", "before-amp", "before-cabinet",
                                            "after-cabinet", "after-eq",   "after-post-fx"};
  return values;
}

constexpr const char *placement_names =
    "before-pre-fx, before-amp, before-cabinet, after-cabinet, after-eq, after-post-fx";

// A parameter id: a whole, non-negative number that fits 32 bits.
std::optional<uint32_t> parameter_id(const json &value) {
  if (!value.is_number() || !whole(value) || value.get<double>() < 0 ||
      value.get<double>() > 4294967295.0) {
    return std::nullopt;
  }
  return static_cast<uint32_t>(value.get<double>());
}

// Fails for a member of object other than those allowed, as the audio side rejects them.
bool known_keys(const json &object, const std::string &name,
                std::initializer_list<const char *> allowed, Findings &findings) {
  if (!object.is_object()) {
    return true;
  }
  for (const auto &[key, value] : object.items()) {
    if (std::none_of(allowed.begin(), allowed.end(),
                     [&](const char *name) { return key == name; })) {
      findings.fail("unknown key \"" + key + "\" in " + name);
      return false;
    }
  }
  return true;
}

// A route's optional branch routing; null means serial. Returns false after a failure.
bool branch_of(const json &owner, const std::string &where, std::optional<uint32_t> &mix,
               Findings &findings) {
  const auto routing = owner.find("routing");
  if (routing == owner.end() || routing->is_null()) {
    return true;
  }
  if (!known_keys(*routing, where + "routing", {"input", "mix"}, findings) ||
      (routing->is_object() && routing->contains("mix") &&
       !known_keys((*routing)["mix"], where + "routing.mix", {"parameter", "law"}, findings))) {
    return false;
  }
  const auto input = routing->is_object() ? routing->find("input") : routing->end();
  if (!routing->is_object() || input == routing->end() || !input->is_string() ||
      *input != "chain-input") {
    findings.fail(where + "routing.input must be chain-input");
    return false;
  }
  const auto mix_object = routing->find("mix");
  const auto law = mix_object != routing->end() && mix_object->is_object() ? mix_object->find("law")
                                                                           : routing->end();
  if (mix_object == routing->end() || !mix_object->is_object() || law == mix_object->end() ||
      !law->is_string() || *law != "crossfade") {
    findings.fail(where + "routing.mix must name a parameter and the law crossfade");
    return false;
  }
  const auto parameter = mix_object->find("parameter");
  mix = parameter == mix_object->end() ? std::nullopt : parameter_id(*parameter);
  if (!mix) {
    findings.fail(where + "routing.mix.parameter must be a parameter id");
    return false;
  }
  return true;
}

// Validates routing and routingSelector as host discovery does; parameters are checked against
// the binary by the routing check. Returns the routes of a module that declares either.
std::optional<RoutingTarget> routing_of(const json &manifest,
                                        const std::optional<std::string> &category, bool schema_two,
                                        Findings &findings) {
  const auto routing = manifest.find("routing");
  const auto selector = manifest.find("routingSelector");
  const bool has_routing = routing != manifest.end() && !routing->is_null();
  const bool has_selector = selector != manifest.end() && !selector->is_null();
  if (!has_routing && !has_selector) {
    return std::nullopt;
  }
  if (category && *category != "special-fx") {
    findings.fail("routing and routingSelector apply only to special-fx modules");
    return std::nullopt;
  }
  if (!schema_two) {
    findings.fail("routing requires schemaVersion 2");
    return std::nullopt;
  }
  RoutingTarget result;
  if (!has_selector) {
    const auto placement = manifest.find("placement");
    RouteTarget route;
    route.placement = placement != manifest.end() && placement->is_string()
                          ? placement->get<std::string>()
                          : "after-cabinet";
    if (!branch_of(manifest, "", route.mix, findings)) {
      return std::nullopt;
    }
    result.routes.push_back(route);
    return result;
  }
  const auto placement = manifest.find("placement");
  if ((placement != manifest.end() && !placement->is_null()) || has_routing) {
    findings.fail("routingSelector cannot be combined with a top-level placement or routing");
    return std::nullopt;
  }
  if (!known_keys(*selector, "routingSelector", {"parameter", "routes"}, findings)) {
    return std::nullopt;
  }
  const auto parameter = selector->is_object() ? selector->find("parameter") : selector->end();
  result.selector = parameter == selector->end() ? std::nullopt : parameter_id(*parameter);
  const auto routes = selector->is_object() ? selector->find("routes") : selector->end();
  if (!result.selector || routes == selector->end() || !routes->is_array()) {
    findings.fail("routingSelector must have a parameter id and an array of routes");
    return std::nullopt;
  }
  if (routes->size() < 2 || routes->size() > 8) {
    findings.fail("routingSelector must list 2 to 8 routes");
    return std::nullopt;
  }
  for (size_t index = 0; index < routes->size(); ++index) {
    const json &element = (*routes)[index];
    const std::string where = "routingSelector.routes[" + std::to_string(index) + "].";
    if (!known_keys(element, where.substr(0, where.size() - 1), {"placement", "routing"},
                    findings)) {
      return std::nullopt;
    }
    const auto name = element.is_object() ? element.find("placement") : element.end();
    if (name == element.end() || !name->is_string() || !placements().count(*name)) {
      findings.fail(where + "placement must be one of " + placement_names);
      return std::nullopt;
    }
    RouteTarget route;
    route.placement = name->get<std::string>();
    if (!branch_of(element, where, route.mix, findings)) {
      return std::nullopt;
    }
    if (route.mix == result.selector) {
      findings.fail("the routingSelector parameter cannot be a route's mix parameter");
      return std::nullopt;
    }
    result.routes.push_back(route);
  }
  return result;
}

Result read_manifest(const fs::path &package, std::vector<Target> &targets) {
  Findings findings;
  json manifest;
  const fs::path file = package / "module.json";
  std::ifstream stream(file);
  if (!stream) {
    findings.fail("cannot read " + file.string());
    return findings.summarize("package", "manifest", "");
  }
  try {
    manifest = json::parse(stream);
  } catch (const json::exception &error) {
    findings.fail("module.json is not valid JSON: " + std::string(error.what()));
    return findings.summarize("package", "manifest", "");
  }
  if (!manifest.is_object()) {
    findings.fail("module.json must contain an object");
    return findings.summarize("package", "manifest", "");
  }

  const auto schema = manifest.find("schemaVersion");
  const bool schema_known = schema != manifest.end() && whole(*schema) &&
                            (schema->get<double>() == 1.0 || schema->get<double>() == 2.0);
  if (!schema_known) {
    findings.fail("schemaVersion must be 1 or 2");
  }
  const auto id = text_field(manifest, "id", "", true, findings);
  if (id && id->empty()) {
    findings.fail("id must not be empty");
  }
  const auto category = text_field(manifest, "category", "", true, findings);
  static const std::set<std::string> categories{"amp", "cab", "pre-fx", "post-fx", "special-fx"};
  if (category && !categories.count(*category)) {
    findings.fail("category \"" + *category +
                  "\" is not one of amp, cab, pre-fx, post-fx, "
                  "special-fx");
  }
  const auto binary = text_field(manifest, "binary", "", true, findings);
  const auto icon = text_field(manifest, "icon", "", true, findings);
  const auto resources_name = text_field(manifest, "resources", "", true, findings);
  const auto binary_path =
      binary ? resolve(package, *binary, "binary", findings) : std::optional<fs::path>{};
  if (icon) {
    resolve(package, *icon, "icon", findings);
  }
  const auto resources = resources_name ? resolve(package, *resources_name, "resources", findings)
                                        : std::optional<fs::path>{};
  text_field(manifest, "pluginFamily", "", false, findings);
  if (const auto preview = text_field(manifest, "cabinetPreview", "", false, findings)) {
    resolve(package, *preview, "cabinetPreview", findings);
  }
  const std::string background =
      window_background(manifest, package, category, resources, findings);
  if (const auto placement = manifest.find("placement");
      placement != manifest.end() && !placement->is_null()) {
    static const std::set<std::string> placements{"before-pre-fx", "before-amp", "before-cabinet",
                                                  "after-cabinet", "after-eq",   "after-post-fx"};
    if (category && *category != "special-fx") {
      findings.fail("placement applies only to special-fx modules");
    } else if (!placement->is_string() || !placements.count(placement->get<std::string>())) {
      findings.fail("placement must be null or one of before-pre-fx, before-amp, before-cabinet, "
                    "after-cabinet, after-eq, after-post-fx");
    }
  }
  if (const auto former = manifest.find("formerCategories");
      former != manifest.end() && !former->is_null()) {
    bool valid = former->is_array();
    std::set<std::string> seen;
    for (const json &item : valid ? *former : json::array()) {
      valid = valid && item.is_string() && categories.count(item.get<std::string>()) &&
              (!category || item.get<std::string>() != *category) &&
              seen.insert(item.get<std::string>()).second;
    }
    if (!valid) {
      findings.fail("formerCategories must list distinct categories other than the package's "
                    "own, from amp, cab, post-fx, pre-fx, special-fx");
    }
  }
  const std::optional<RoutingTarget> routing =
      routing_of(manifest, category, schema_known && schema->get<double>() == 2.0, findings);
  for (const char *key : {"cabinetPreviewHeight", "boardWidth"}) {
    if (manifest.contains(key) && !manifest[key].is_number()) {
      findings.fail(std::string(key) + " must be a number");
    }
  }
  if (manifest.contains("boardWidth") && manifest["boardWidth"].is_number()) {
    const double width = manifest["boardWidth"].get<double>();
    if (category && *category != "pre-fx" && *category != "post-fx") {
      findings.fail("boardWidth applies only to pre-fx and post-fx modules");
    } else if (!whole(manifest["boardWidth"]) || width < 1 || width > 4) {
      findings.fail("boardWidth must be a whole number of board units from 1 to 4");
    }
  }
  if (const auto presentation = text_field(manifest, "presentation", "", false, findings);
      presentation && *presentation != "pedal" && *presentation != "rack") {
    findings.fail("presentation must be \"pedal\" or \"rack\"");
  }
  if (manifest.contains("viewSize")) {
    const json &size = manifest["viewSize"];
    if (!size.is_object() || !size.contains("width") || !size.contains("height") ||
        !size["width"].is_number() || !size["height"].is_number()) {
      findings.fail("viewSize must be an object with numeric width and height");
    } else if (!(size["width"].get<double>() > 0 && size["height"].get<double>() > 0)) {
      findings.warn("viewSize is not positive; the host uses its default size instead");
    }
  }
  if (manifest.contains("visualInsets")) {
    const json &insets = manifest["visualInsets"];
    bool valid = insets.is_object();
    bool negative = false;
    for (const char *side : {"top", "left", "bottom", "right"}) {
      if (valid && insets.contains(side)) {
        valid = insets[side].is_number();
        negative = negative || (valid && !(insets[side].get<double>() >= 0));
      }
    }
    if (!valid) {
      findings.fail("visualInsets must be an object of numeric top, left, bottom and right");
    } else if (negative) {
      findings.warn("visualInsets has a negative side; the host ignores all insets then");
    }
  }
  if (manifest.contains("cabinet")) {
    const json &cabinet = manifest["cabinet"];
    if (!cabinet.is_object() || !cabinet.contains("supported") ||
        !cabinet["supported"].is_boolean()) {
      findings.fail("cabinet must be an object with a boolean supported");
    } else {
      const auto module = text_field(cabinet, "defaultModule", "cabinet.", false, findings);
      if (category && *category != "amp") {
        findings.fail("only amps declare cabinet support");
      } else if (cabinet["supported"].get<bool>() ? !module || module->empty()
                                                  : cabinet.contains("defaultModule")) {
        findings.fail("cabinet.defaultModule must name a module exactly when supported is true");
      }
    }
  }
  const auto main_layout = layout_of(manifest, "", package, resources, findings);
  if (binary_path && id && !id->empty()) {
    targets.push_back({"module", *id, *binary_path, main_layout, routing});
  }
  if (manifest.contains("eq")) {
    const json &eq = manifest["eq"];
    if (!eq.is_object()) {
      findings.fail("eq must be an object");
    } else {
      const auto eq_id = text_field(eq, "id", "eq.", true, findings);
      const auto eq_binary = text_field(eq, "binary", "eq.", true, findings);
      if (category && *category != "amp") {
        findings.fail("only amps declare an EQ companion");
      }
      if (eq_id && (eq_id->empty() || eq_id == id)) {
        findings.fail("eq.id must be non-empty and differ from the package id");
      }
      if (eq_binary && eq_binary == binary) {
        findings.fail("eq.binary must differ from the package binary");
      }
      const auto eq_path = eq_binary ? resolve(package, *eq_binary, "eq.binary", findings)
                                     : std::optional<fs::path>{};
      const auto eq_layout = layout_of(eq, "eq.", package, resources, findings);
      if (eq_path && eq_id && !eq_id->empty()) {
        targets.push_back({"eq", *eq_id, *eq_path, eq_layout, std::nullopt});
      }
    }
  }
  std::string passed = "module.json declares " + (id ? *id : std::string()) + " (" +
                       (category ? *category : std::string()) + ")";
  if (targets.size() > 1) {
    passed += " with EQ companion " + targets.back().id;
  }
  if (!background.empty()) {
    passed += "; window background " + background + " (PNG header only)";
  }
  return findings.summarize("package", "manifest", passed);
}

std::string layouts(uint32_t supported) {
  std::vector<std::string> names;
  if (supported & VS_LAYOUT_MONO) {
    names.push_back("mono");
  }
  if (supported & VS_LAYOUT_STEREO) {
    names.push_back("stereo");
  }
  std::string result;
  for (const auto &name : names) {
    result += (result.empty() ? "" : "+") + name;
  }
  return result.empty() ? "none" : result;
}

void check_abi(const Context &context, const std::string &expected_id) {
  const Binary &binary = context.binary;
  if (!binary.unusable.empty()) {
    context.add("abi", Status::fail, binary.unusable);
    return;
  }
  const vs_module_api &api = *binary.api;
  Findings findings;
  std::string rejection;
  try {
    ModuleCatalog catalog;
    catalog.add(binary.path);
  } catch (const std::exception &error) {
    rejection = error.what();
    findings.fail("the host loader rejects it: " + rejection);
  }
  const std::string id = api.id ? api.id : "";
  if (id != expected_id) {
    findings.fail("descriptor id \"" + id + "\" does not match the manifest id \"" + expected_id +
                  "\"");
  }
  std::set<uint32_t> ids;
  for (const Descriptor &d : binary.descriptors) {
    const std::string where = "parameter " + std::to_string(d.id);
    if (d.id == 0) {
      findings.fail("a parameter has id 0");
    }
    if (!ids.insert(d.id).second) {
      findings.fail(where + " is declared twice");
    }
    if (d.name.empty()) {
      findings.fail(where + " has no name");
    }
    if (!std::isfinite(d.minimum) || !std::isfinite(d.maximum) || d.minimum > d.maximum) {
      findings.fail(where + " has range " + range_text(d.minimum, d.maximum));
    } else if (!(d.default_value >= d.minimum && d.default_value <= d.maximum)) {
      findings.fail(where + " defaults to " + number(d.default_value) + ", outside " +
                    range_text(d.minimum, d.maximum));
    }
  }
  if (api.supported_layouts == 0 ||
      (api.supported_layouts & ~uint32_t{VS_LAYOUT_MONO | VS_LAYOUT_STEREO}) != 0) {
    findings.fail("supported_layouts " + std::to_string(api.supported_layouts) +
                  " must combine VS_LAYOUT_MONO and VS_LAYOUT_STEREO only");
  }
  constexpr uint32_t known =
      VS_MODULE_BYPASS_PARAMETER | VS_MODULE_HOST_TEMPO | VS_MODULE_MIDI_INPUT;
  if (api.capabilities & ~known) {
    findings.fail("capabilities declares unknown flags " +
                  std::to_string(api.capabilities & ~known));
  }
  if ((api.capabilities & VS_MODULE_BYPASS_PARAMETER) && !binary.bypass) {
    findings.fail("declares VS_MODULE_BYPASS_PARAMETER, but bypass_parameter " +
                  std::to_string(api.bypass_parameter) +
                  " is not a declared parameter ranging from 0 to 1");
  }
  if (!(api.capabilities & VS_MODULE_BYPASS_PARAMETER) && api.bypass_parameter != 0) {
    findings.fail("bypass_parameter is " + std::to_string(api.bypass_parameter) +
                  " without VS_MODULE_BYPASS_PARAMETER");
  }
  for (const std::string *error :
       {&binary.state_error, &binary.parameters_error, &binary.metadata_error,
        &binary.services_error, &binary.files_error}) {
    if (!error->empty() && rejection.find(*error) == std::string::npos) {
      findings.fail(*error);
    }
  }
  size_t described = 0;
  for (const ParameterDisplay &display : binary.displays) {
    described += display.described ? 1 : 0;
    if (display.kind == ParameterKind::toggle && display.default_value != display.minimum &&
        display.default_value != display.maximum) {
      findings.warn("parameter " + std::to_string(display.id) + " is a toggle but defaults to " +
                    number(display.default_value) + ", which is neither off nor on");
    }
  }
  if (binary.metadata && binary.bypass) {
    const ParameterDisplay *bypass = binary.display(*binary.bypass);
    if (bypass && !bypass->described) {
      findings.warn("the parameter metadata does not describe bypass parameter " +
                    std::to_string(*binary.bypass) + " as a toggle");
    }
  }
  std::vector<std::string> capabilities;
  if (binary.bypass) {
    capabilities.push_back("bypass parameter " + std::to_string(*binary.bypass));
  }
  if (api.capabilities & VS_MODULE_HOST_TEMPO) {
    capabilities.push_back("host tempo");
  }
  if (api.capabilities & VS_MODULE_MIDI_INPUT) {
    capabilities.push_back("MIDI input");
  }
  std::vector<std::string> extensions;
  if (binary.state) {
    extensions.push_back("state schema " + std::to_string(binary.state->schema_version));
  }
  if (binary.parameters) {
    extensions.push_back("parameter values");
  }
  if (binary.services) {
    extensions.push_back("services");
  }
  if (binary.files) {
    extensions.push_back("files (" + std::to_string(binary.file_slots.size()) + " slots)");
  }
  if (binary.metadata) {
    extensions.push_back("parameter metadata (" + std::to_string(described) + " of " +
                         std::to_string(binary.displays.size()) + " parameters described)");
  }
  const auto join = [](const std::vector<std::string> &items) {
    std::string result;
    for (const auto &item : items) {
      result += (result.empty() ? "" : ", ") + item;
    }
    return result.empty() ? std::string("none") : result;
  };
  findings.report(context, "abi",
                  "ABI " + std::to_string(api.abi_version) + "; " + id + " " +
                      (api.version ? api.version : "") + "; " +
                      std::to_string(binary.descriptors.size()) + " parameters; layouts " +
                      layouts(api.supported_layouts) + "; capabilities: " + join(capabilities) +
                      "; extensions: " + join(extensions) + "; accepted by the host loader");
}

using Check = void (*)(const Context &);

const std::vector<std::pair<std::string, Check>> &dsp_checks() {
  static const std::vector<std::pair<std::string, Check>> checks{{"lifecycle", check_lifecycle},
                                                                 {"blocks", check_blocks},
                                                                 {"automation", check_automation},
                                                                 {"bypass", check_bypass},
                                                                 {"state", check_state},
                                                                 {"robustness", check_robustness},
                                                                 {"latency", check_latency},
                                                                 {"services", check_services},
                                                                 {"files", check_files},
                                                                 {"pending", check_pending},
                                                                 {"cancel", check_cancel},
                                                                 {"offline", check_offline},
                                                                 {"threads", check_threads},
                                                                 {"realtime", check_realtime},
                                                                 {"cpu", check_cpu}};
  return checks;
}

}

size_t route_index(double value, size_t count) {
  const double rounded = std::floor(value + 0.5);
  if (!(rounded > 0.0)) {
    return 0;
  }
  return rounded >= static_cast<double>(count - 1) ? count - 1 : static_cast<size_t>(rounded);
}

bool Report::passed() const {
  return std::none_of(results.begin(), results.end(),
                      [](const Result &result) { return result.status == Status::fail; });
}

const std::vector<std::string> &check_names() {
  static const std::vector<std::string> names = [] {
    std::vector<std::string> result{"manifest", "abi"};
    for (const auto &[name, check] : dsp_checks()) {
      result.push_back(name);
    }
    result.push_back("layout");
    result.push_back("routing");
    return result;
  }();
  return names;
}

const char *status_name(Status status) {
  switch (status) {
  case Status::pass:
    return "pass";
  case Status::warn:
    return "warn";
  case Status::fail:
    return "fail";
  case Status::skip:
    return "skip";
  case Status::info:
    return "info";
  }
  return "fail";
}

Report check_package(const fs::path &package, const Options &options) {
  allocation::mechanism();
  Report report;
  report.package = package.string();
  const auto selected = [&](const std::string &name) {
    return options.checks.empty() ||
           std::find(options.checks.begin(), options.checks.end(), name) != options.checks.end();
  };
  const auto starting = [&](const std::string &target, const std::string &check) {
    if (options.starting) {
      options.starting(target, check);
    }
  };

  starting("package", "manifest");
  std::vector<Target> targets;
  const Result manifest = read_manifest(package, targets);
  if (selected("manifest") || targets.empty()) {
    report.results.push_back(manifest);
  }
  for (const Target &target : targets) {
    starting(target.role, "abi");
    Binary binary(target.binary);
    if (target.routing) {
      const auto &routes = target.routing->routes;
      const auto serial = std::find_if(routes.begin(), routes.end(),
                                       [](const RouteTarget &route) { return !route.mix; });
      if (serial == routes.end()) {
        binary.no_serial_route = true;
      } else if (target.routing->selector) {
        binary.serial_route = Binary::RouteSetting{*target.routing->selector,
                                                   static_cast<float>(serial - routes.begin())};
      }
    }
    const Context context{binary, options, target.role, report.results, process_allocations()};
    if (selected("abi")) {
      check_abi(context, target.id);
    }
    std::string blocked = binary.unusable;
    if (blocked.empty() && binary.channel_counts.empty()) {
      blocked = "declares no supported channel layout";
    }
    for (const auto &[name, check] : dsp_checks()) {
      if (!selected(name)) {
        continue;
      }
      if (!blocked.empty()) {
        context.add(name, Status::fail, "cannot run: " + blocked);
        continue;
      }
      starting(target.role, name);
      try {
        check(context);
      } catch (const std::exception &error) {
        context.add(name, Status::fail, error.what());
      }
    }
    if (selected("routing") && target.role == "module") {
      if (!target.routing) {
        context.add("routing", Status::skip, "the manifest declares no routing");
      } else if (!blocked.empty()) {
        context.add("routing", Status::fail, "cannot run: " + blocked);
      } else {
        starting(target.role, "routing");
        try {
          check_routing(context, *target.routing);
        } catch (const std::exception &error) {
          context.add("routing", Status::fail, error.what());
        }
      }
    }
    if (selected("layout")) {
      if (!target.layout) {
        context.add("layout", Status::skip, "the manifest declares no ui layout for this binary");
      } else if (!blocked.empty()) {
        context.add("layout", Status::fail, "cannot run: " + blocked);
      } else {
        starting(target.role, "layout");
        check_layout(context, *target.layout);
      }
    }
  }
  return report;
}

std::string format_text(const Report &report) {
  std::string text = "vespera-module-check " + report.package + "\n";
  size_t counts[5] = {};
  for (const Result &result : report.results) {
    std::string status = status_name(result.status);
    std::transform(status.begin(), status.end(), status.begin(),
                   [](char c) { return static_cast<char>(c - 'a' + 'A'); });
    std::string target = result.target;
    target.resize(std::max<size_t>(target.size(), 8), ' ');
    std::string check = result.check;
    check.resize(std::max<size_t>(check.size(), 11), ' ');
    text += status + "  " + target + check + result.detail + "\n";
    ++counts[static_cast<size_t>(result.status)];
  }
  text += std::to_string(counts[static_cast<size_t>(Status::pass)]) + " passed, " +
          std::to_string(counts[static_cast<size_t>(Status::warn)]) + " warnings, " +
          std::to_string(counts[static_cast<size_t>(Status::fail)]) + " failed, " +
          std::to_string(counts[static_cast<size_t>(Status::skip)]) + " skipped, " +
          std::to_string(counts[static_cast<size_t>(Status::info)]) +
          " informational: " + (report.passed() ? "conforms" : "does not conform") + "\n";
  return text;
}

std::string format_json(const Report &report) {
  nlohmann::ordered_json document;
  document["package"] = report.package;
  document["passed"] = report.passed();
  document["results"] = nlohmann::ordered_json::array();
  for (const Result &result : report.results) {
    document["results"].push_back({{"target", result.target},
                                   {"check", result.check},
                                   {"status", status_name(result.status)},
                                   {"detail", result.detail}});
  }
  return document.dump(2) + "\n";
}

}
