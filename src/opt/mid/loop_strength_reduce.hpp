#pragma once

#include "../../mid/ir.hpp"
#include "../AnalysisManager.hpp"

namespace exodus::mid_ir::opt {

class LoopStrengthReduce {
public:
  explicit LoopStrengthReduce(MidModule *m) : module(m) {}

  auto run(LinearFunction &func, exodus::opt::LinearFunctionAnalysisManager &am)
    -> exodus::opt::PreservedAnalysis;

private:
  MidModule *module;
};

} // namespace exodus::mid_ir::opt
