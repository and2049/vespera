#include "module_check.hpp"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace check = vespera::module_check;

constexpr const char *usage = R"(usage: vespera-module-check [options] <package.vespera>

Loads a built vespera module package (its manifest, binary and any EQ companion) through the
public module ABI and reports whether it conforms. Exits 0 when no check fails, 1 when one does,
and 2 for invalid arguments.

options:
  --checks LIST          comma-separated checks to run (default: all):
                         manifest, abi, lifecycle, blocks, automation, bypass, state,
                         robustness, latency, services, files, pending, cancel, offline, threads,
                         realtime, cpu, layout
  --rates LIST           sample rates for robustness, latency and cpu (default 44100,48000,96000)
  --max-frames N         prepared maximum block size (default 4096)
  --tolerance X          largest sample difference accepted by blocks, automation, reset and
                         discontinuity comparisons (default 0: identical bits)
  --bypass-tolerance X   largest difference accepted between bypassed output and the delayed
                         input (default 0: identical bits)
  --bypass-settle-ms MS  time allowed for a bypass switch to settle (default 50)
  --stall-factor X       how many times slower than ordinary input a block of hostile input may
                         be (default 20)
  --cpu-block N          block size for the cpu measurement (default 128)
  --cpu-seconds S        audio duration rendered per cpu measurement (default 2)
  --cpu-limit PERCENT    fail when a module uses more of real time (default 50)
  --timeout S            abort when one check runs longer (default 300)
  --seed N               seed for generated signals, settings and automation (default 1)
  --json                 print results as JSON
  --help                 print this help
)";

std::mutex progress_mutex;
std::condition_variable progress_changed;
uint64_t progress_generation = 0;
bool finished = false;
char current_check[128] = "startup";

void starting(const std::string &target, const std::string &name) {
  {
    std::lock_guard lock(progress_mutex);
    ++progress_generation;
    std::snprintf(current_check, sizeof(current_check), "%s %s", target.c_str(), name.c_str());
  }
  progress_changed.notify_all();
}

struct sigaction previous_actions[NSIG];

void crashed(int signal) {
  const char prefix[] = "vespera-module-check: crashed during ";
  [[maybe_unused]] ssize_t written = write(STDERR_FILENO, prefix, sizeof(prefix) - 1);
  written = write(STDERR_FILENO, current_check, strnlen(current_check, sizeof(current_check)));
  written = write(STDERR_FILENO, "\n", 1);
  sigaction(signal, &previous_actions[signal], nullptr);
}

void install_crash_report() {
  struct sigaction action{};
  action.sa_handler = crashed;
  sigemptyset(&action.sa_mask);
  for (int signal : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) {
    sigaction(signal, &action, &previous_actions[signal]);
  }
}

std::optional<double> parse_number(const std::string &text) {
  char *end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (text.empty() || *end != '\0' || !std::isfinite(value)) {
    return std::nullopt;
  }
  return value;
}

std::vector<std::string> split(const std::string &text) {
  std::vector<std::string> items;
  size_t start = 0;
  while (start <= text.size()) {
    const size_t comma = std::min(text.find(',', start), text.size());
    items.push_back(text.substr(start, comma - start));
    start = comma + 1;
  }
  return items;
}

[[noreturn]] void invalid(const std::string &message) {
  std::cerr << "vespera-module-check: " << message << "\n(see --help)\n";
  std::exit(2);
}

}

int main(int argc, char **argv) {
  check::Options options;
  std::optional<std::string> package;
  bool json = false;
  double timeout = 300.0;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    const auto value = [&]() -> std::string {
      if (i + 1 >= argc) {
        invalid(argument + " requires a value");
      }
      return argv[++i];
    };
    const auto ranged = [&](double minimum, double maximum) {
      const std::string text = value();
      const auto number = parse_number(text);
      if (!number || *number < minimum || *number > maximum) {
        invalid(argument + " expects a number from " + std::to_string(minimum) + " to " +
                std::to_string(maximum) + ", got \"" + text + "\"");
      }
      return *number;
    };
    const auto whole = [&](double minimum, double maximum) {
      const double number = ranged(minimum, maximum);
      if (number != std::floor(number)) {
        invalid(argument + " expects a whole number");
      }
      return number;
    };
    if (argument == "--help" || argument == "-h") {
      std::cout << usage;
      return 0;
    } else if (argument == "--json") {
      json = true;
    } else if (argument == "--checks") {
      options.checks = split(value());
      for (const auto &name : options.checks) {
        const auto &names = check::check_names();
        if (std::find(names.begin(), names.end(), name) == names.end()) {
          invalid("unknown check \"" + name + "\"");
        }
      }
    } else if (argument == "--rates") {
      options.rates.clear();
      for (const auto &text : split(value())) {
        const auto rate = parse_number(text);
        if (!rate || *rate < 8000.0 || *rate > 768000.0) {
          invalid("--rates expects sample rates from 8000 to 768000 Hz, got \"" + text + "\"");
        }
        options.rates.push_back(*rate);
      }
    } else if (argument == "--max-frames") {
      options.max_frames = static_cast<uint32_t>(whole(64, 65536));
    } else if (argument == "--tolerance") {
      options.tolerance = static_cast<float>(ranged(0, 1));
    } else if (argument == "--bypass-tolerance") {
      options.bypass_tolerance = static_cast<float>(ranged(0, 1));
    } else if (argument == "--bypass-settle-ms") {
      options.bypass_settle_ms = ranged(0, 10000);
    } else if (argument == "--stall-factor") {
      options.stall_factor = ranged(1, 1e6);
    } else if (argument == "--cpu-block") {
      options.cpu_block = static_cast<uint32_t>(whole(1, 65536));
    } else if (argument == "--cpu-seconds") {
      options.cpu_seconds = ranged(0.01, 600);
    } else if (argument == "--cpu-limit") {
      options.cpu_limit_percent = ranged(0, 1e6);
    } else if (argument == "--timeout") {
      timeout = ranged(1, 86400);
    } else if (argument == "--seed") {
      options.seed = static_cast<uint64_t>(whole(0, 9007199254740992.0));
    } else if (!argument.empty() && argument[0] == '-') {
      invalid("unknown option " + argument);
    } else if (package) {
      invalid("expected one package, got \"" + *package + "\" and \"" + argument + "\"");
    } else {
      package = argument;
    }
  }
  if (!package) {
    invalid("missing package");
  }

  install_crash_report();
  options.starting = starting;
  std::thread watchdog([timeout] {
    std::unique_lock lock(progress_mutex);
    while (!finished) {
      const uint64_t generation = progress_generation;
      if (!progress_changed.wait_for(lock, std::chrono::duration<double>(timeout), [&] {
            return finished || progress_generation != generation;
          })) {
        std::fprintf(stderr, "vespera-module-check: %s did not finish within %g s\n", current_check,
                     timeout);
        std::_Exit(1);
      }
    }
  });

  const check::Report report = check::check_package(*package, options);
  {
    std::lock_guard lock(progress_mutex);
    finished = true;
  }
  progress_changed.notify_all();
  watchdog.join();

  std::cout << (json ? check::format_json(report) : check::format_text(report));
  return report.passed() ? 0 : 1;
}
