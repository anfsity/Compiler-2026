#pragma once

#include "../../mid/ir.hpp"
#include "../AnalysisManager.hpp"

namespace exodus::mid_ir::opt {

class DeadFunctions {
public:
  explicit DeadFunctions(MidModule *) {}

  auto run(MidModule &module, exodus::opt::AnalysisManager<MidModule> &)
    -> exodus::opt::PreservedAnalysis;
};

auto eliminate_dead_functions(MidModule &module) -> bool;

} // namespace exodus::mid_ir::opt
