#pragma once

#include "../../mid/ir.hpp"

namespace exodus::mid_ir::opt {

auto eliminate_dead_functions(MidModule &module) -> bool;

} // namespace exodus::mid_ir::opt
