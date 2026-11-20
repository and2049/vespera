// Checks vespera-module-check's route resolver against the values the Swift host tests share.
#include "module_check.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <fstream>

int main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: route_index_test manifest_values.json\n");
    return 2;
  }
  std::ifstream stream(argv[1]);
  const nlohmann::json values = nlohmann::json::parse(stream);
  int failures = 0;
  for (const auto &item : values.at("routeIndex")) {
    const double value = item.at(0).get<double>();
    const auto count = item.at(1).get<size_t>();
    const auto expected = item.at(2).get<size_t>();
    const size_t index = vespera::module_check::route_index(value, count);
    if (index != expected) {
      std::fprintf(stderr, "route_index(%g, %zu) is %zu, expected %zu\n", value, count, index,
                   expected);
      ++failures;
    }
  }
  return failures == 0 && !values.at("routeIndex").empty() ? 0 : 1;
}
