#include "dce.hpp"

namespace exodus::mid_ir {

auto DCE::run(
  LinearFunction &func, exodus::opt::LinearFunctionAnalysisManager & /* am */
) -> exodus::opt::PreservedAnalysis {
  bool changed = false;
  bool local_changed = false;

  do { // NOLINT
    local_changed = false;
    std::unordered_set<Op *> scope;
    for (auto &block : func.blocks) {
      scope.insert(block->insts.begin(), block->insts.end());
    }

    rewriter.set_scope(func);
    for (auto &block : func.blocks) {
      for (auto *op : block->insts) {
        if (has_observable_effect(op) || has_scoped_users(op, scope))
          continue;
        rewriter.eraseOp(op);
        local_changed = true;
      }
    }
    rewriter.finalize(func);
    changed |= local_changed;
  } while (local_changed);

  return changed ? exodus::opt::PreservedAnalysis::none()
                 : exodus::opt::PreservedAnalysis::all();
}

auto DCE::has_observable_effect(const Op *op) -> bool {
  switch (op->code) {
  case OpCode::Store:
  case OpCode::Call:
  case OpCode::Ret:
  case OpCode::Jump:
  case OpCode::Branch:
  case OpCode::Memset:
    return true;
  default:
    return false;
  }
}

auto DCE::has_scoped_users(const Op *op, const std::unordered_set<Op *> &scope)
  -> bool {
  if (!op->result)
    return false;
  for (auto *user_base : op->result->users) {
    auto *user = dynamic_cast<Op *>(user_base);
    if (user && scope.count(user))
      return true;
  }
  return false;
}

} // namespace exodus::mid_ir
