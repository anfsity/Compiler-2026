#include "gvn.hpp"

#include "../../base/getptr.hpp"
#include <algorithm>

namespace exodus::mid_ir::opt {

auto GVN::run(
  LinearFunction &func, exodus::opt::LinearFunctionAnalysisManager &am
) -> exodus::opt::PreservedAnalysis {
  if (func.blocks.empty())
    return exodus::opt::PreservedAnalysis::all();

  auto &dom_result = am.get_result<DominanceAnalysis>(func);

  rewriter.set_scope(func);
  value_numbers.clear();
  constant_numbers.clear();
  available.clear();
  changed = false;
  next_number = 0;
  dom = &dom_result;

  visit(func.blocks.front().get());
  rewriter.finalize(func);
  dom = nullptr;

  return changed ? exodus::opt::PreservedAnalysis::none()
                 : exodus::opt::PreservedAnalysis::all();
}

auto GVN::visit(Block *block) -> void {
  std::unordered_map<Expression, Value *, ExpressionHash> loads;
  std::vector<Expression> inserted;

  for (auto *op : block->insts) {
    process_op(op, loads, inserted);
  }

  for (auto *child : dom->get_children(block)) {
    visit(child);
  }

  for (const auto &expression : inserted) {
    available.erase(expression);
  }
}

auto GVN::process_op(
  Op *op,
  std::unordered_map<Expression, Value *, ExpressionHash> &loads,
  std::vector<Expression> &inserted
) -> void {
  if (op->code == OpCode::Load && op->result && !op->operands.empty()) {
    Expression expression{
      OpCode::Load, op->result->type.get(), {number_value(op->operands[0])}
    };
    auto it = loads.find(expression);
    if (it != loads.end()) {
      rewriter.replace_all_uses_with(op->result, it->second);
      rewriter.eraseOp(op);
      value_numbers[op->result] = number_value(it->second);
      changed = true;
    } else {
      loads.emplace(expression, op->result);
      value_numbers[op->result] = next_number++;
    }
    return;
  }

  if (is_memory_barrier(op->code)) {
    loads.clear();
    return;
  }

  if (!op->result)
    return;

  // Loop phis may refer to values defined on a backedge.  Keep their number
  // unique in this one-pass GVN and only apply structural phi simplification
  // after CFG updates.
  if (op->code == OpCode::Phi) {
    number_value(op->result);
    return;
  }

  if (auto expression = build_expression(op)) {
    std::vector<ValueNumber> operands;
    operands.reserve(expression->operands.size());
    for (auto *operand : op->operands) {
      operands.push_back(number_value(operand));
    }

    if (auto *replacement = simplify(op, operands)) {
      rewriter.replace_all_uses_with(op->result, replacement);
      rewriter.eraseOp(op);
      value_numbers[op->result] = number_value(replacement);
      changed = true;
      return;
    }

    auto it = available.find(*expression);
    if (it != available.end()) {
      rewriter.replace_all_uses_with(op->result, it->second);
      rewriter.eraseOp(op);
      value_numbers[op->result] = number_value(it->second);
      changed = true;
      return;
    }

    available.emplace(*expression, op->result);
    inserted.push_back(*expression);
  }

  value_numbers[op->result] = next_number++;
}

auto GVN::number_value(Value *value) -> ValueNumber {
  if (!value)
    return 0;

  auto it = value_numbers.find(value);
  if (it != value_numbers.end())
    return it->second;

  if (value->kind == ValueKind::Constant) {
    auto key = value->type->to_string() + ":" + value->dump();
    auto constant_it = constant_numbers.find(key);
    if (constant_it != constant_numbers.end()) {
      value_numbers[value] = constant_it->second;
      return constant_it->second;
    }

    auto number = next_number++;
    constant_numbers.emplace(std::move(key), number);
    value_numbers[value] = number;
    return number;
  }

  auto number = next_number++;
  value_numbers[value] = number;
  return number;
}

auto GVN::build_expression(Op *op) -> std::optional<Expression> {
  if (!op->result)
    return std::nullopt;

  if (!is_pure_opcode(op->code))
    return std::nullopt;
  if (op->code == OpCode::GetPtr) {
    auto plan = ir::analyze_getptr(
      op->operands[0]->type, op->result->type, op->operands.size() - 1
    );
    if (plan.reads_memory)
      return std::nullopt;
  }

  Expression expression{op->code, op->result->type.get(), {}};
  expression.operands.reserve(op->operands.size());
  for (auto *operand : op->operands) {
    expression.operands.push_back(number_value(operand));
  }

  if (is_commutative(op->code) && expression.operands.size() == 2) {
    if (expression.operands[0] > expression.operands[1])
      std::swap(expression.operands[0], expression.operands[1]);
  }
  return expression;
}

auto GVN::simplify(Op *op, const std::vector<ValueNumber> &operands)
  -> Value * {
  if (operands.size() != 2 || operands[0] != operands[1])
    return nullptr;
  if (op->operands[0]->type->is_f32())
    return nullptr;

  switch (op->code) {
  case OpCode::Eq:
  case OpCode::Le:
  case OpCode::Ge:
    return module->ctx->make_const(Bool::get(), 1);
  case OpCode::Ne:
  case OpCode::Lt:
  case OpCode::Gt:
    return module->ctx->make_const(Bool::get(), 0);
  default:
    return nullptr;
  }
}

auto GVN::is_pure_opcode(OpCode code) -> bool {
  switch (code) {
  case OpCode::Add:
  case OpCode::Sub:
  case OpCode::Mul:
  case OpCode::Div:
  case OpCode::Mod:
  case OpCode::FAdd:
  case OpCode::FSub:
  case OpCode::FMul:
  case OpCode::FDiv:
  case OpCode::I2F:
  case OpCode::F2I:
  case OpCode::ZExt:
  case OpCode::Eq:
  case OpCode::Ne:
  case OpCode::Lt:
  case OpCode::Gt:
  case OpCode::Le:
  case OpCode::Ge:
  case OpCode::And:
  case OpCode::Or:
  case OpCode::Xor:
  case OpCode::Shl:
  case OpCode::Shr:
  case OpCode::GetPtr:
    return true;
  default:
    return false;
  }
}

auto GVN::is_memory_barrier(OpCode code) -> bool {
  return code == OpCode::Store || code == OpCode::Memset ||
         code == OpCode::Call;
}

auto GVN::is_commutative(OpCode code) -> bool {
  return code == OpCode::Add || code == OpCode::Mul || code == OpCode::Eq ||
         code == OpCode::Ne || code == OpCode::And || code == OpCode::Or ||
         code == OpCode::Xor;
}

} // namespace exodus::mid_ir::opt
