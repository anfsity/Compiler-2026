#include "region_simplify.hpp"

namespace exodus::high_ir::opt {
namespace {

auto constant_bool(Value *value) -> std::optional<bool> {
  if (!value || value->kind != ValueKind::Constant)
    return std::nullopt;
  const auto &data = static_cast<Constant *>(value)->val;
  if (!std::holds_alternative<int>(data))
    return std::nullopt;
  return std::get<int>(data) != 0;
}

auto while_condition(Op &op) -> Op * {
  auto &payload = std::get<WhilePayload>(op.payload);
  if (payload.cond_region->empty())
    return nullptr;
  auto *condition = payload.cond_region->back();
  return condition->code == OpCode::Condition ? condition : nullptr;
}

} // namespace

auto RegionSimplify::run(
  Function &function, exodus::opt::FunctionAnalysisManager &
) -> exodus::opt::PreservedAnalysis {
  rewriter.clear();
  changed = false;
  simplify_region(function.body);
  rewriter.finalize(function);
  return changed ? exodus::opt::PreservedAnalysis::none()
                 : exodus::opt::PreservedAnalysis::all();
}

auto RegionSimplify::simplify_region(Region &region) -> void {
  for (auto *op : region) {
    if (op->code == OpCode::If)
      simplify_if(op);
    else if (op->code == OpCode::While)
      simplify_while(op);
  }
}

auto RegionSimplify::simplify_if(Op *op) -> void {
  auto &payload = std::get<IfPayload>(op->payload);
  simplify_region(*payload.then_region);
  if (payload.else_region)
    simplify_region(*payload.else_region);

  auto condition = constant_bool(op->operands[0]);
  if (condition) {
    if (*condition) {
      rewriter.replace_op_with_region(op, *payload.then_region);
      if (payload.else_region)
        rewriter.erase_region(*payload.else_region);
    } else if (payload.else_region) {
      rewriter.replace_op_with_region(op, *payload.else_region);
      rewriter.erase_region(*payload.then_region);
    } else {
      rewriter.erase_region(*payload.then_region);
      rewriter.eraseOp(op);
    }
    changed = true;
    return;
  }

  if (
    payload.then_region->empty() &&
    (!payload.else_region || payload.else_region->empty())
  ) {
    rewriter.eraseOp(op);
    changed = true;
    return;
  }

  if (payload.else_region && payload.else_region->empty()) {
    payload.else_region.reset();
    changed = true;
  }
}

auto RegionSimplify::simplify_while(Op *op) -> void {
  auto &payload = std::get<WhilePayload>(op->payload);
  simplify_region(*payload.cond_region);
  simplify_region(*payload.loop_region);

  auto *condition_op = while_condition(*op);
  if (!condition_op || condition_op->operands.empty())
    return;

  auto condition = constant_bool(condition_op->operands[0]);
  if (!condition || *condition)
    return;

  rewriter.eraseOp(condition_op);
  rewriter.erase_region(*payload.loop_region);
  rewriter.replace_op_with_region(op, *payload.cond_region);
  changed = true;
}

} // namespace exodus::high_ir::opt
