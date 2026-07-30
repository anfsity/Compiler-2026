#include "gvn.hpp"

#include "../../mid/getptr.hpp"
#include <algorithm>
#include <unordered_set>

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

  visit(func.blocks.front().get(), {});
  rewriter.finalize(func);
  dom = nullptr;

  return changed ? exodus::opt::PreservedAnalysis::none()
                 : exodus::opt::PreservedAnalysis::all();
}

auto GVN::visit(Block *block, MemoryState state) -> void {
  prepare_inherited_state(block, state);

  std::vector<Expression> inserted;

  for (auto *op : block->insts) {
    process_op(op, state, inserted);
  }

  // A store in one arm does not make a dominating store dead on sibling arms.
  // Only carry dead-store candidates across a single-successor edge.
  if (block->succs.size() != 1)
    state.pending_stores.clear();

  for (auto *child : dom->get_children(block)) {
    visit(child, state);
  }

  for (const auto &expression : inserted) {
    available.erase(expression);
  }
}

auto GVN::prepare_inherited_state(Block *block, MemoryState &state) -> void {
  if (block->preds.size() <= 1)
    return;

  auto *idom = dom->get_idom(block);
  if (!idom) {
    state.clear();
    return;
  }

  bool has_load = false;
  bool has_store = false;
  std::vector<Block *> worklist(block->preds.begin(), block->preds.end());
  std::unordered_set<Block *> visited;
  while (!worklist.empty()) {
    auto *current = worklist.back();
    worklist.pop_back();
    if (current == idom || !visited.insert(current).second)
      continue;
    if (!dom->dominate(idom, current)) {
      state.clear();
      return;
    }

    for (auto *op : current->insts) {
      if (op->code == OpCode::Load) {
        has_load = true;
        continue;
      }
      if (op->code == OpCode::Store) {
        has_store = true;
        invalidate_for_write(state, alias_analysis.get_location(*op));
        continue;
      }
      if (is_memory_barrier(op)) {
        state.clear();
        return;
      }
    }
    worklist.insert(
      worklist.end(), current->preds.begin(), current->preds.end()
    );
  }

  // Path-local reads or writes make stored-value forwarding ambiguous at the
  // join.  Dominating loads survive only when every path write is NoAlias.
  if (has_load || has_store)
    state.stored_values.clear();
  state.pending_stores.clear();
}

auto GVN::process_op(
  Op *op, MemoryState &state, std::vector<Expression> &inserted
) -> void {
  if (op->code == OpCode::Load && op->result && !op->operands.empty()) {
    auto address_number = number_value(op->operands[0]);
    if (
      auto stored = state.stored_values.find(address_number);
      stored != state.stored_values.end()
    ) {
      rewriter.replace_all_uses_with(op->result, stored->second.value);
      rewriter.eraseOp(op);
      value_numbers[op->result] = number_value(stored->second.value);
      changed = true;
      return;
    }

    auto location = alias_analysis.get_location(*op);
    observe_read(state, location);
    Expression expression{
      OpCode::Load, op->result->type.get(), {address_number}
    };
    auto it = state.loads.find(expression);
    if (it != state.loads.end()) {
      rewriter.replace_all_uses_with(op->result, it->second.value);
      rewriter.eraseOp(op);
      value_numbers[op->result] = number_value(it->second.value);
      changed = true;
    } else {
      state.loads.emplace(
        expression,
        MemoryState::LoadFact{
          op->result,
          location.value_or(
            MemoryLocation{op->operands[0], nullptr, std::nullopt, 0}
          ),
        }
      );
      value_numbers[op->result] = next_number++;
    }
    return;
  }

  if (op->code == OpCode::Store && op->operands.size() >= 2) {
    auto address_number = number_value(op->operands[1]);
    if (
      auto previous = state.pending_stores.find(address_number);
      previous != state.pending_stores.end()
    ) {
      rewriter.eraseOp(previous->second.operation);
      changed = true;
    }
    auto location = alias_analysis.get_location(*op);
    invalidate_for_write(state, location);
    auto memory_location = location.value_or(
      MemoryLocation{op->operands[1], nullptr, std::nullopt, 0}
    );
    state.stored_values[address_number] = {op->operands[0], memory_location};
    state.pending_stores[address_number] = {op, memory_location};
    return;
  }

  if (is_memory_barrier(op)) {
    state.clear();
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

auto GVN::invalidate_for_write(
  MemoryState &state, const std::optional<MemoryLocation> &location
) -> void {
  auto may_alias = [&](const MemoryLocation &candidate) {
    return !location || alias_analysis.may_alias(candidate, *location);
  };
  for (auto it = state.loads.begin(); it != state.loads.end();) {
    if (may_alias(it->second.location))
      it = state.loads.erase(it);
    else
      ++it;
  }
  for (auto it = state.stored_values.begin();
       it != state.stored_values.end();) {
    if (may_alias(it->second.location))
      it = state.stored_values.erase(it);
    else
      ++it;
  }
}

auto GVN::observe_read(
  MemoryState &state, const std::optional<MemoryLocation> &location
) -> void {
  for (auto it = state.pending_stores.begin();
       it != state.pending_stores.end();) {
    if (!location || alias_analysis.may_alias(it->second.location, *location)) {
      it = state.pending_stores.erase(it);
    } else {
      ++it;
    }
  }
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
  if (reads_memory_through_getptr(op))
    return std::nullopt;

  Expression expression{op->code, op->result->type.get(), {}};
  if (op->code == OpCode::GetPtr)
    expression.getptr_layout_type = getptr_layout_type(*op).get();
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

auto GVN::reads_memory_through_getptr(const Op *op) -> bool {
  if (
    !op || op->code != OpCode::GetPtr || op->operands.empty() || !op->result ||
    !op->operands[0]->type->is_ptr() || !op->result->type->is_ptr()
  ) {
    return false;
  }
  auto plan = mid_ir::analyze_getptr(*op);
  return !plan.valid || plan.reads_memory;
}

auto GVN::is_memory_barrier(const Op *op) -> bool {
  return op && (op->code == OpCode::Memset || op->code == OpCode::Call ||
                reads_memory_through_getptr(op));
}

auto GVN::is_commutative(OpCode code) -> bool {
  return code == OpCode::Add || code == OpCode::Mul || code == OpCode::Eq ||
         code == OpCode::Ne || code == OpCode::And || code == OpCode::Or ||
         code == OpCode::Xor;
}

} // namespace exodus::mid_ir::opt
