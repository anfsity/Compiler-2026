#pragma once

#include "../../mid/affine_loop.hpp"
#include "../../mid/cfg_editor.hpp"
#include "../../mid/rewriter.hpp"
#include <unordered_map>

namespace exodus::mid_ir::opt {

class MonotonicGuardTighten {
public:
  explicit MonotonicGuardTighten(MidModule *m) : module(m) {}

  auto run(LinearFunction &func, exodus::opt::LinearFunctionAnalysisManager &am)
    -> exodus::opt::PreservedAnalysis;

private:
  MidModule *module;
  MidIRRewriter rewriter;
  std::unordered_map<Op *, Block *> op_blocks;

  auto build_op_block_map(LinearFunction &func) -> void;
  auto try_tighten(CFGEditor &cfg, LinearFunction &func, const Loop &loop)
    -> bool;
  auto is_loop_invariant(Value *value, const Loop &loop) const -> bool;
  auto is_increment_by_one(Op *op, Value *induction) const -> bool;
  auto has_live_out_use(
    const LinearFunction &func, Value *value, const Loop &loop
  ) const -> bool;
  auto has_only_current_user(
    const LinearFunction &func, Value *value, Op *expected
  ) const -> bool;
};

} // namespace exodus::mid_ir::opt
