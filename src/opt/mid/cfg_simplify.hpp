#pragma once

#include "../../mid/cfg_editor.hpp"
#include "../../mid/ir.hpp"
#include "../../mid/rewriter.hpp"
#include "../AnalysisManager.hpp"

namespace exodus::mid_ir::opt {

class CFGSimplify {
  MidModule *module;
  MidIRRewriter rewriter;
  bool changed = false;

public:
  explicit CFGSimplify(MidModule *m) : module(m) {}

  auto run(LinearFunction &func, exodus::opt::LinearFunctionAnalysisManager &am)
    -> exodus::opt::PreservedAnalysis;

private:
  auto fold_constant_branches(CFGEditor &cfg) -> void;
  auto remove_unreachable_blocks(CFGEditor &cfg) -> void;
  auto simplify_phis(LinearFunction &func) -> void;
  auto merge_linear_blocks(CFGEditor &cfg) -> void;
};

} // namespace exodus::mid_ir::opt
