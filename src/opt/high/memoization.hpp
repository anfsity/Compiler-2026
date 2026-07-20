#pragma once

#include "../../high/ir.hpp"
#include "../AnalysisManager.hpp"

namespace exodus::high_ir::opt {

class Memoization {
public:
  explicit Memoization(Module *) {}

  auto run(Module &m, exodus::opt::ModuleAnalysisManager &am)
    -> exodus::opt::PreservedAnalysis;
};

} // namespace exodus::high_ir::opt
