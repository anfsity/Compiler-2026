#pragma once

#include "../../high/ir.hpp"
#include "../AnalysisManager.hpp"

namespace exodus::high_ir::opt {

class LocalArrayLoopSpecialize {
public:
  explicit LocalArrayLoopSpecialize(Module *module) : module(module) {}

  auto run(Function &, exodus::opt::FunctionAnalysisManager &)
    -> exodus::opt::PreservedAnalysis;

private:
  Module *module;
};

} // namespace exodus::high_ir::opt
