#pragma once

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

  auto create_preheader(LinearFunction &func, Loop &loop) -> bool;
  auto rewrite_header_phis(
    Block *header,
    Block *preheader,
    const std::unordered_set<Block *> &outside_preds
  ) -> void;
  static auto
  reset_phi_incoming(Op *phi, std::vector<std::pair<Block *, Value *>> incoming)
    -> void;
  static auto rebuild_cfg(LinearFunction &func) -> void;
  static auto renumber_blocks(LinearFunction &func) -> void;
};

} // namespace exodus::mid_ir::opt
