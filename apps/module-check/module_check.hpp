#ifndef VESPERA_MODULE_CHECK_HPP
#define VESPERA_MODULE_CHECK_HPP

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace vespera::module_check {

enum class Status { pass, warn, fail, skip, info };

struct Result {
  // "package" for manifest results, otherwise the binary's role: "module" or "eq".
  std::string target;
  std::string check;
  Status status = Status::pass;
  std::string detail;
};

struct Options {
  // Checks to run, by name; empty runs all of them.
  std::vector<std::string> checks;
  std::vector<double> rates{44100.0, 48000.0, 96000.0};
  uint32_t max_frames = 4096;
  uint32_t cpu_block = 128;
  double cpu_seconds = 2.0;
  double cpu_limit_percent = 50.0;
  // Largest accepted absolute sample difference for block-size and automation comparisons; zero
  // requires identical bits.
  float tolerance = 0.0f;
  // Largest accepted absolute difference between settled bypassed output and delayed input; zero
  // requires identical bits.
  float bypass_tolerance = 0.0f;
  double bypass_settle_ms = 50.0;
  // A block of hostile input may take at most this many times as long as an ordinary block.
  double stall_factor = 20.0;
  uint64_t seed = 1;
  // Called before each check starts, for progress reporting and crash attribution.
  std::function<void(const std::string &target, const std::string &check)> starting;
};

struct Report {
  std::string package;
  std::vector<Result> results;

  bool passed() const;
};

// The route a selector value chooses among count routes: floor(value + 0.5), clamped, which is
// the nearest route with ties upward. Host discovery and the audio side use the same rule.
size_t route_index(double value, size_t count);

const std::vector<std::string> &check_names();
const char *status_name(Status status);

Report check_package(const std::filesystem::path &package, const Options &options);

std::string format_text(const Report &report);
std::string format_json(const Report &report);

}

#endif
