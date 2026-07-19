#pragma once

#include "../../high/ir.hpp"
#include "../../high/rewriter.hpp"
#include "../AnalysisManager.hpp"

namespace exodus::high_ir::opt {

struct RegionSimplify {
  IRRewriter rewriter;
  bool changed = false;

  explicit RegionSimplify(Module *) {}

  auto run(Function &function, exodus::opt::FunctionAnalysisManager &)
    -> exodus::opt::PreservedAnalysis;

private:
  auto simplify_region(Region &region) -> void;
  auto simplify_if(Op *op) -> void;
  auto simplify_while(Op *op) -> void;
};

} // namespace exodus::high_ir::opt
