#include "dce.hpp"

namespace exodus::mid_ir::opt {

auto DCE::run(
  LinearFunction &func, exodus::opt::LinearFunctionAnalysisManager & /* am */
) -> exodus::opt::PreservedAnalysis {
  std::unordered_set<Op *> scope;
  std::unordered_set<Op *> live;
  std::deque<Op *> worklist;
  for (auto &block : func.blocks) {
    for (auto *op : block->insts) {
      scope.insert(op);
      if (has_observable_effect(op) && live.insert(op).second)
        worklist.push_back(op);
    }
  }

  while (!worklist.empty()) {
    auto *op = worklist.front();
    worklist.pop_front();
    for (auto *operand : op->operands)
      mark_definition(operand, scope, live, worklist);
    if (op->code == OpCode::Phi) {
      for (const auto &[pred, value] :
           std::get<PhiPayload>(op->payload).incoming) {
        (void)pred;
        mark_definition(value, scope, live, worklist);
      }
    }
  }

  bool changed = false;
  rewriter.set_scope(func);
  for (auto &block : func.blocks) {
    for (auto *op : block->insts) {
      if (live.count(op))
        continue;
      rewriter.eraseOp(op);
      changed = true;
    }
  }
  rewriter.finalize(func);

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

auto DCE::mark_definition(
  Value *value,
  const std::unordered_set<Op *> &scope,
  std::unordered_set<Op *> &live,
  std::deque<Op *> &worklist
) -> void {
  if (!value || value->kind != ValueKind::OpResult)
    return;
  auto *definition = static_cast<Op *>(static_cast<OpResult *>(value)->creator);
  if (definition && scope.count(definition) && live.insert(definition).second) {
    worklist.push_back(definition);
  }
}

} // namespace exodus::mid_ir::opt
