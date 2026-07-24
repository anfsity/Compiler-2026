#pragma once

#include "../../high/ir.hpp"
#include "../AnalysisManager.hpp"

namespace exodus::high_ir::opt {

class IdempotentLoopSimplify {
public:
  explicit IdempotentLoopSimplify(Module *module) : module(module) {}

  auto run(Module &, exodus::opt::ModuleAnalysisManager &)
    -> exodus::opt::PreservedAnalysis;

private:
  Module *module;
};

} // namespace exodus::high_ir::opt
