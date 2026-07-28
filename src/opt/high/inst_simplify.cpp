#include "inst_simplify.hpp"

#include "../../high/effects.hpp"

namespace exodus::high_ir::opt {
namespace {

struct CallExpression {
  std::string callee;
  Type *type = nullptr;
  std::vector<Value *> operands;

  auto operator==(const CallExpression &other) const -> bool {
    return callee == other.callee && type == other.type &&
           operands == other.operands;
  }
};

struct CallExpressionHash {
  auto operator()(const CallExpression &expression) const -> size_t {
    size_t hash = std::hash<std::string>{}(expression.callee);
    hash ^= std::hash<Type *>{}(expression.type) + (hash << 6) + (hash >> 2);
    for (auto *operand : expression.operands) {
      hash ^= std::hash<Value *>{}(operand) + (hash << 6) + (hash >> 2);
    }
    return hash;
  }
};

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
  functions.clear();
  for (auto &candidate : module->functions)
    functions.emplace(candidate->name, candidate.get());
  function_effects = get_function_effects(*module);

  visit(function.body);
  simplify_pure_calls(function.body);
  rewriter.finalize(function);
  return changed ? exodus::opt::PreservedAnalysis::none()
                 : exodus::opt::PreservedAnalysis::all();
}

auto InstSimplify::is_readnone_scalar_call(Op *op) const -> bool {
  if (
    !op || op->code != OpCode::Call || !op->result ||
    !(op->result->type->is_i32() || op->result->type->is_f32() ||
      op->result->type->is_bool())
  ) {
    return false;
  }

  const auto &payload = std::get<CallPayload>(op->payload);
  auto function = functions.find(payload.func_name);
  if (function == functions.end() || function->second->is_decl)
    return false;
  auto summary = function_effects.find(function->second);
  if (summary == function_effects.end())
    return false;

  auto effects = get_call_effects(*op, *function->second, summary->second);
  return !effects.has_unknown_effect && !effects.has_control_effect &&
         !effects.reads_memory() && !effects.writes_memory() &&
         !effects.has_unique_identity;
}

auto InstSimplify::simplify_pure_calls(Region &region) -> void {
  std::unordered_map<CallExpression, Value *, CallExpressionHash> available;
  for (auto *op : region) {
    if (op->code == OpCode::If) {
      auto &payload = std::get<IfPayload>(op->payload);
      simplify_pure_calls(*payload.then_region);
      if (payload.else_region)
        simplify_pure_calls(*payload.else_region);
    } else if (op->code == OpCode::While) {
      auto &payload = std::get<WhilePayload>(op->payload);
      simplify_pure_calls(*payload.cond_region);
      simplify_pure_calls(*payload.loop_region);
    }

    if (!is_readnone_scalar_call(op))
      continue;
    const auto &payload = std::get<CallPayload>(op->payload);
    CallExpression expression{
      payload.func_name, op->result->type.get(), op->operands
    };
    auto previous = available.find(expression);
    if (previous == available.end()) {
      available.emplace(std::move(expression), op->result);
      continue;
    }
    rewriter.replace_op(op, previous->second);
    changed = true;
  }
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
