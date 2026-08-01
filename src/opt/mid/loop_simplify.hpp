#pragma once

#include "../../mid/cfg_editor.hpp"
#include "../../mid/loop.hpp"
#include "../../mid/rewriter.hpp"
#include <unordered_set>
#include <vector>

namespace exodus::mid_ir::opt {

class LoopSimplify {
public:
  explicit LoopSimplify(MidModule *m) : module(m) {}

  auto run(LinearFunction &func, exodus::opt::LinearFunctionAnalysisManager &am)
    -> exodus::opt::PreservedAnalysis;

private:
  MidModule *module;

  auto create_preheader(CFGEditor &cfg, Loop &loop) -> bool;
  auto create_single_latch(CFGEditor &cfg, Loop &loop) -> bool;
  auto rewrite_header_phis(
    CFGEditor &cfg,
    Block *header,
    Block *preheader,
    const std::unordered_set<Block *> &outside_preds
  ) -> void;
  static auto identical_binary_update(
    const std::vector<std::pair<Block *, Value *>> &incoming
  ) -> Op *;
};

} // namespace exodus::mid_ir::opt
