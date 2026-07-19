#pragma once

#include "../opt/AnalysisManager.hpp"
#include "ir.hpp"
#include "rewriter.hpp"

namespace exodus::mid_ir {

class CFGSimplify {
  MidIRRewriter rewriter;
  bool changed = false;

public:
  explicit CFGSimplify(MidModule *) {}

  auto run(LinearFunction &func, exodus::opt::LinearFunctionAnalysisManager &am)
    -> exodus::opt::PreservedAnalysis;

private:
  auto fold_constant_branches(LinearFunction &func) -> void;
  auto rebuild_cfg(LinearFunction &func) -> void;
  auto remove_unreachable_blocks(LinearFunction &func) -> void;
  auto simplify_phis(LinearFunction &func) -> void;
  auto merge_linear_blocks(LinearFunction &func) -> void;
};

} // namespace exodus::mid_ir
