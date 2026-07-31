#include "scalar_evolution.hpp"

#include <algorithm>

namespace exodus::mid_ir {

auto ScalarEvolution::compute(
  LinearFunction &func,
  LoopInfo &loop_info,
  AffineLoopInfo &affine_info,
  DomTree &dom_tree
) -> void {
  loops = &loop_info;
  affine = &affine_info;
  dom = &dom_tree;
  op_blocks.clear();
  for (auto &block : func.blocks) {
    for (auto *op : block->insts)
      op_blocks[op] = block.get();
  }
}

auto ScalarEvolution::definition_block(Value *value) const -> Block * {
  if (!value || value->kind != ValueKind::OpResult)
    return nullptr;
  auto *op = static_cast<Op *>(static_cast<OpResult *>(value)->creator);
  auto it = op_blocks.find(op);
  return it == op_blocks.end() ? nullptr : it->second;
}

auto ScalarEvolution::get_add_recurrence(const Loop &loop) const
  -> std::optional<SCEVAddRecurrence> {
  if (!affine)
    return std::nullopt;
  auto counted = affine->match_counted_loop(loop);
  if (!counted)
    return std::nullopt;

  auto initial = get_affine(counted->induction.initial, {});
  if (!initial)
    return std::nullopt;
  return SCEVAddRecurrence{
    &loop,
    *counted,
    *initial,
    counted->induction.step,
    affine->is_no_wrap(counted->induction.phi->result, *counted, loop),
  };
}

auto ScalarEvolution::get_affine(
  Value *value, const std::vector<CountedLoopInfo> &iteration_space
) const -> std::optional<SCEVAffineExpr> {
  std::unordered_set<Value *> active;
  return get_affine_impl(value, iteration_space, active);
}

auto ScalarEvolution::get_affine_impl(
  Value *value,
  const std::vector<CountedLoopInfo> &iteration_space,
  std::unordered_set<Value *> &active
) const -> std::optional<SCEVAffineExpr> {
  if (!value || !value->type->is_i32())
    return std::nullopt;
  if (auto constant = AffineLoopInfo::integer_constant(value))
    return SCEVAffineExpr{*constant, {}, true};

  for (const auto &counted : iteration_space) {
    if (value != counted.induction.phi->result)
      continue;
    auto *loop = loops ? loops->get_loop_for(counted.header) : nullptr;
    const bool no_wrap =
      loop && affine && affine->is_no_wrap(value, counted, *loop);
    return SCEVAffineExpr{0, {{value, 1}}, no_wrap};
  }

  if (!active.insert(value).second)
    return std::nullopt;
  auto finish = [&active, value](std::optional<SCEVAffineExpr> result) {
    active.erase(value);
    return result;
  };

  auto *scope =
    iteration_space.empty() ? nullptr : iteration_space.front().header;
  auto *scope_loop = scope && loops ? loops->get_loop_for(scope) : nullptr;
  auto *block = definition_block(value);
  if (
    value->kind != ValueKind::OpResult || !block || !scope_loop ||
    !scope_loop->contains(block)
  ) {
    // SSA values defined before the SCoP are symbolic parameters.  Their
    // runtime value is fixed for every modeled statement instance.
    if (
      !scope_loop || value->kind != ValueKind::OpResult ||
      (block && dom && dom->dominate(block, iteration_space.front().preheader))
    ) {
      return finish(SCEVAffineExpr{0, {{value, 1}}, true});
    }
    return finish(std::nullopt);
  }

  auto *creator = static_cast<Op *>(static_cast<OpResult *>(value)->creator);
  if (
    !creator || creator->operands.size() != 2 ||
    (creator->code != OpCode::Add && creator->code != OpCode::Sub &&
     creator->code != OpCode::Mul)
  ) {
    return finish(std::nullopt);
  }

  auto lhs = get_affine_impl(creator->operands[0], iteration_space, active);
  auto rhs = get_affine_impl(creator->operands[1], iteration_space, active);
  if (!lhs || !rhs)
    return finish(std::nullopt);

  std::optional<SCEVAffineExpr> result;
  if (creator->code == OpCode::Add) {
    result = combine_affine_expressions(*lhs, *rhs, 1);
  } else if (creator->code == OpCode::Sub) {
    result = combine_affine_expressions(*lhs, *rhs, -1);
  } else if (lhs->is_constant()) {
    result = scale_affine_expression(*rhs, lhs->constant);
  } else if (rhs->is_constant()) {
    result = scale_affine_expression(*lhs, rhs->constant);
  }
  if (!result)
    return finish(std::nullopt);

  auto range = expression_range(*result, iteration_space);
  result->no_wrap =
    lhs->no_wrap && rhs->no_wrap && range && affine_range_fits_i32(*range);
  return finish(result);
}

auto ScalarEvolution::expression_range(
  const SCEVAffineExpr &expr,
  const std::vector<CountedLoopInfo> &iteration_space
) const -> std::optional<IntegerRange> {
  std::unordered_map<Value *, IntegerRange> symbol_ranges;
  for (const auto &[symbol, coefficient] : expr.coefficients) {
    (void)coefficient;
    auto it = std::find_if(
      iteration_space.begin(),
      iteration_space.end(),
      [symbol](const CountedLoopInfo &counted) {
        return counted.induction.phi->result == symbol;
      }
    );
    if (it == iteration_space.end() || !affine)
      return std::nullopt;
    auto range = affine->induction_range(*it);
    if (!range)
      return std::nullopt;
    symbol_ranges.emplace(symbol, *range);
  }
  return affine_expression_range(expr, symbol_ranges);
}

} // namespace exodus::mid_ir
