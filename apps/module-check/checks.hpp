#ifndef VESPERA_MODULE_CHECK_CHECKS_HPP
#define VESPERA_MODULE_CHECK_CHECKS_HPP

#include "harness.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace vespera::module_check {

struct Context {
  const Binary &binary;
  const Options &options;
  std::string target;
  std::vector<Result> &results;
  // process_allocations() when this binary's checks began.
  uint64_t allocations_before = 0;

  void add(const std::string &check, Status status, std::string detail) const {
    results.push_back({target, check, status, std::move(detail)});
  }
};

// Collects independent findings of one check and reports them as a single result.
class Findings {
public:
  void fail(std::string message) { failures_.push_back(std::move(message)); }
  void warn(std::string message) { warnings_.push_back(std::move(message)); }
  bool failed() const { return !failures_.empty(); }
  // Runs one scenario; a CheckError it throws becomes a failure prefixed with the scenario name.
  template <class Scenario> void attempt(const std::string &name, Scenario &&scenario) {
    try {
      scenario();
    } catch (const CheckError &error) {
      fail(name + ": " + error.what());
    }
  }
  // Failures make a failed result, otherwise warnings a warning; `passed` describes what held.
  Result summarize(const std::string &target, const std::string &check,
                   const std::string &passed) const;
  void report(const Context &context, const std::string &check, const std::string &passed) const {
    context.results.push_back(summarize(context.target, check, passed));
  }

private:
  std::vector<std::string> failures_;
  std::vector<std::string> warnings_;
};

void check_lifecycle(const Context &context);
void check_blocks(const Context &context);
void check_automation(const Context &context);
void check_bypass(const Context &context);
void check_state(const Context &context);
void check_robustness(const Context &context);
void check_latency(const Context &context);
void check_services(const Context &context);
void check_files(const Context &context);
void check_pending(const Context &context);
void check_cancel(const Context &context);
void check_offline(const Context &context);
void check_threads(const Context &context);
void check_realtime(const Context &context);
void check_cpu(const Context &context);

struct LayoutTarget {
  std::filesystem::path resources;
  std::string layout;
};

// One route a Special FX module declares: a serial position, or with mix a branch from the chain
// input whose mix and bypass the host realises.
struct RouteTarget {
  std::string placement;
  std::optional<uint32_t> mix;
};

// The routes a manifest declares, and the parameter choosing among them when there are several.
struct RoutingTarget {
  std::optional<uint32_t> selector;
  std::vector<RouteTarget> routes;
};

// Checks the parameters the routes name against the binary, and that in every branch route the
// output depends neither on the route's mix parameter nor on the bypass parameter.
void check_routing(const Context &context, const RoutingTarget &routing);

// Validates the layout with the host's renderer and compares its widgets with the binary's
// parameters. Reported as skipped where the renderer is unavailable.
void check_layout(const Context &context, const LayoutTarget &layout);

}

#endif
