#include "checks.hpp"

namespace vespera::module_check {

void check_layout(const Context &context, const LayoutTarget &) {
  context.add("layout", Status::skip,
              "layout validation uses the host's AppKit renderer and runs on macOS only");
}

}
