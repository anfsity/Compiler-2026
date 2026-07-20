#include "inst_simplify.hpp"

#include "../../high/effects.hpp"

namespace exodus::high_ir::opt {
namespace {

auto int_constant(Value *value) -> std::optional<int> {
  if (!value || value->kind != ValueKind::Constant)
    return std::nullopt;
  const auto &data = static_cast<Constant *>(value)->val;
  if (!std::holds_alternative<int>(data))
    return std::nullopt;
  return std::get<int>(data);
}

auto is_zero(Value *value) -> bool {
  auto constant = int_constant(value);
  return constant && *constant == 0;
}

auto is_one(Value *value) -> bool {
  auto constant = int_constant(value);
  return constant && *constant == 1;
}

auto is_all_ones(Value *value) -> bool {
  auto constant = int_constant(value);
  if (!constant)
    return false;
  return value->type->is_bool() ? *constant == 1 : *constant == -1;
}

} // namespace

auto InstSimplify::run(
  Function &function, exodus::opt::FunctionAnalysisManager &
) -> exodus::opt::PreservedAnalysis {
  rewriter.clear();
  changed = false;
  visit(function.body);
  rewriter.finalize(function);
  return changed ? exodus::opt::PreservedAnalysis::none()
                 : exodus::opt::PreservedAnalysis::all();
}

auto InstSimplify::replace_with(Op *op, Value *value) -> void {
  rewriter.replace_op(op, value);
  changed = true;
}

auto InstSimplify::replace_with_zero(Op *op) -> void {
  replace_with(op, module->ctx.make_zero(op->result->type));
}

auto InstSimplify::replace_with_bool(Op *op, bool value) -> void {
  replace_with(op, module->ctx.make_const(Bool::get(), value ? 1 : 0));
}

auto InstSimplify::visit(Op *op) -> void {
  RecursiveOpVisitor<InstSimplify>::visit(op);
  if (!op->result || !get_op_effects(*op).cseable)
    return;

  auto *lhs = op->operands.empty() ? nullptr : op->operands[0];
  auto *rhs = op->operands.size() < 2 ? nullptr : op->operands[1];

  switch (op->code) {
  case OpCode::Add:
    if (is_zero(rhs))
      replace_with(op, lhs);
    else if (is_zero(lhs))
      replace_with(op, rhs);
    break;
  case OpCode::Sub:
    if (is_zero(rhs))
      replace_with(op, lhs);
    else if (lhs == rhs)
      replace_with_zero(op);
    break;
  case OpCode::Mul:
    if (is_zero(lhs) || is_zero(rhs))
      replace_with_zero(op);
    else if (is_one(rhs))
      replace_with(op, lhs);
    else if (is_one(lhs))
      replace_with(op, rhs);
    break;
  case OpCode::Div:
    if (is_one(rhs))
      replace_with(op, lhs);
    break;
  case OpCode::Mod:
    if (is_one(rhs))
      replace_with_zero(op);
    else if (is_all_ones(rhs) || is_zero(lhs))
      replace_with_zero(op);
    break;
  case OpCode::And:
    if (is_zero(lhs) || is_zero(rhs))
      replace_with_zero(op);
    else if (lhs == rhs || is_all_ones(rhs))
      replace_with(op, lhs);
    else if (is_all_ones(lhs))
      replace_with(op, rhs);
    break;
  case OpCode::Or:
    if (is_zero(rhs) || lhs == rhs)
      replace_with(op, lhs);
    else if (is_zero(lhs))
      replace_with(op, rhs);
    break;
  case OpCode::Xor:
    if (lhs == rhs)
      replace_with_zero(op);
    else if (is_zero(rhs))
      replace_with(op, lhs);
    else if (is_zero(lhs))
      replace_with(op, rhs);
    break;
  case OpCode::Shl:
  case OpCode::Shr:
    if (is_zero(rhs))
      replace_with(op, lhs);
    break;
  case OpCode::Eq:
  case OpCode::Le:
  case OpCode::Ge:
    if (lhs == rhs && !lhs->type->is_f32())
      replace_with_bool(op, true);
    break;
  case OpCode::Ne:
  case OpCode::Lt:
  case OpCode::Gt:
    if (lhs == rhs && !lhs->type->is_f32())
      replace_with_bool(op, false);
    break;
  default:
    break;
  }
}

} // namespace exodus::high_ir::opt
