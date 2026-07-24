#pragma once

#include "../../high/ir.hpp"
#include "../AnalysisManager.hpp"

namespace exodus::high_ir::opt {

class GlobalInitPromotion {
public:
  explicit GlobalInitPromotion(Module *m) : module(m) {}

  auto run(Module &, exodus::opt::ModuleAnalysisManager &)
    -> exodus::opt::PreservedAnalysis;

private:
  Module *module;
};

} // namespace exodus::high_ir::opt
