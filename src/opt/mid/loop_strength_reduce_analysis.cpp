#include "loop_strength_reduce_analysis.hpp"

#include <algorithm>
#include <functional>
#include <limits>

namespace exodus::mid_ir::opt {

auto AffineExpressionMatcher::analyze(
  Value *value, Value *induction, const Loop &loop
) const -> std::optional<AffineExpressionInfo> {
  std::unordered_set<Value *> active;
  return analyze_impl(value, induction, loop, active);
}

auto AffineExpressionMatcher::analyze_impl(
  Value *value,
  Value *induction,
  const Loop &loop,
  std::unordered_set<Value *> &active
) const -> std::optional<AffineExpressionInfo> {
  if (value == induction)
    return AffineExpressionInfo{1, 0, true};
  if (!value)
    return AffineExpressionInfo{0, 0, false};
  if (AffineLoopInfo::integer_constant(value))
    return AffineExpressionInfo{0, 0, true};
  if (value->kind != ValueKind::OpResult)
    return AffineExpressionInfo{0, 0, false};
  if (!active.insert(value).second)
    return std::nullopt;

  auto *creator = static_cast<Op *>(static_cast<OpResult *>(value)->creator);
  auto block_it = context.op_blocks.find(creator);
  if (
    !creator || block_it == context.op_blocks.end() ||
    !loop.contains(block_it->second)
  ) {
    active.erase(value);
    return AffineExpressionInfo{0, 0, false};
  }
  if (!is_clonable_expression(*creator, loop)) {
    active.erase(value);
    return std::nullopt;
  }

  if (creator->code == OpCode::Load) {
    active.erase(value);
    return AffineExpressionInfo{0, 0, false};
  }
  if (creator->operands.size() != 2) {
    active.erase(value);
    return std::nullopt;
  }

  auto lhs = analyze_impl(creator->operands[0], induction, loop, active);
  auto rhs = analyze_impl(creator->operands[1], induction, loop, active);
  if (!lhs || !rhs) {
    active.erase(value);
    return std::nullopt;
  }

  int coefficient = 0;
  if (creator->code == OpCode::Add || creator->code == OpCode::Sub) {
    const auto signed_rhs =
      creator->code == OpCode::Add ? rhs->coefficient : -rhs->coefficient;
    const auto combined = static_cast<long long>(lhs->coefficient) +
                          static_cast<long long>(signed_rhs);
    if (
      combined < std::numeric_limits<int>::min() ||
      combined > std::numeric_limits<int>::max()
    ) {
      active.erase(value);
      return std::nullopt;
    }
    coefficient = static_cast<int>(combined);
  } else if (lhs->coefficient != 0 || rhs->coefficient != 0) {
    // LSR deliberately only migrates add/sub expression trees.  Keeping this
    // rejection here preserves the existing proof boundary for Mul/Shl while
    // still allowing constant subexpressions to contribute to cost analysis.
    active.erase(value);
    return std::nullopt;
  }

  AffineExpressionInfo result;
  result.coefficient = coefficient;
  result.cost = 1 + lhs->cost + rhs->cost;
  result.migrated_shape =
    (creator->code == OpCode::Add || creator->code == OpCode::Sub) &&
    lhs->migrated_shape && rhs->migrated_shape;
  active.erase(value);
  return result;
}

auto AffineExpressionMatcher::is_clonable_expression(
  const Op &op, const Loop &loop
) const -> bool {
  if (
    op.code == OpCode::Add || op.code == OpCode::Sub ||
    op.code == OpCode::Mul || op.code == OpCode::Shl
  ) {
    return true;
  }
  if (
    op.code != OpCode::Load || op.operands.size() != 1 || !op.operands[0] ||
    op.operands[0]->kind != ValueKind::GlobalVar ||
    !op.operands[0]->type->is_ptr()
  ) {
    return false;
  }
  auto target = std::static_pointer_cast<Ptr>(op.operands[0]->type)->target;
  if (target->is_array())
    return false;
  for (auto *block : loop.get_blocks()) {
    for (auto *candidate : block->insts) {
      if (
        candidate->code == OpCode::Call || candidate->code == OpCode::Store ||
        candidate->code == OpCode::Memset
      ) {
        return false;
      }
    }
  }
  return true;
}

auto AffineExpressionMatcher::can_clone_initial_expression(
  Value *value,
  Value *induction,
  Value *initial,
  const Loop &loop,
  Block *preheader
) const -> bool {
  std::unordered_set<Value *> active;
  std::function<bool(Value *)> can_clone = [&](Value *current) -> bool {
    if (current == induction)
      return context.affine_loops->is_available_at_preheader(
        initial, preheader
      );
    if (!current || current->kind != ValueKind::OpResult)
      return current != nullptr;
    if (!active.insert(current).second)
      return false;

    auto *creator =
      static_cast<Op *>(static_cast<OpResult *>(current)->creator);
    auto block_it = context.op_blocks.find(creator);
    if (!creator || block_it == context.op_blocks.end()) {
      active.erase(current);
      return false;
    }
    if (!loop.contains(block_it->second)) {
      auto result =
        context.affine_loops->is_available_at_preheader(current, preheader);
      active.erase(current);
      return result;
    }
    if (!creator->result || !is_clonable_expression(*creator, loop)) {
      active.erase(current);
      return false;
    }
    const bool result = std::all_of(
      creator->operands.begin(), creator->operands.end(), can_clone
    );
    active.erase(current);
    return result;
  };
  return can_clone(value);
}

auto AffineExpressionMatcher::dynamic_guard_proves_no_wrap(
  Value *value, const CountedLoopInfo &counted, const Loop &loop
) const -> bool {
  // A strict signed unit-step guard itself supplies one useful endpoint even
  // when the initial value and bound are dynamic:
  //
  //   iv < bound  => iv <= INT_MAX - 1 => iv + 1 does not wrap
  //   iv > bound  => iv >= INT_MIN + 1 => iv - 1 does not wrap
  //
  // Check every source Add/Sub in the migrated expression, not just its final
  // affine form.  This prevents an expression such as (iv + C) - C from being
  // accepted when the intermediate iv + C can overflow.
  const bool increasing =
    counted.induction.step == 1 && counted.induction.predicate == OpCode::Lt;
  const bool decreasing =
    counted.induction.step == -1 && counted.induction.predicate == OpCode::Gt;
  if (!increasing && !decreasing)
    return false;

  std::unordered_set<Value *> active;
  std::function<bool(Value *)> prove = [&](Value *current) -> bool {
    if (!current)
      return false;
    if (
      current == counted.induction.phi->result ||
      AffineLoopInfo::integer_constant(current)
    ) {
      return true;
    }
    if (!active.insert(current).second || current->kind != ValueKind::OpResult)
      return false;

    auto *creator =
      static_cast<Op *>(static_cast<OpResult *>(current)->creator);
    auto block = context.op_blocks.find(creator);
    if (
      !creator || block == context.op_blocks.end() ||
      !loop.contains(block->second) || creator->operands.size() != 2 ||
      (creator->code != OpCode::Add && creator->code != OpCode::Sub)
    ) {
      active.erase(current);
      return false;
    }
    const bool operands_safe =
      prove(creator->operands[0]) && prove(creator->operands[1]);
    auto form = context.affine_loops->affine_form(current, counted, loop);
    active.erase(current);
    if (!operands_safe || !form)
      return false;
    if (form->coefficient == 0) {
      return form->offset >= std::numeric_limits<int32_t>::min() &&
             form->offset <= std::numeric_limits<int32_t>::max();
    }
    if (form->coefficient != 1)
      return false;
    return increasing ? (form->offset >= 0 && form->offset <= 1)
                      : (form->offset >= -1 && form->offset <= 0);
  };
  return prove(value);
}

auto AffineExpressionMatcher::constant_affine_difference(
  Value *value, Value *anchor, const Loop &loop
) const -> std::optional<int64_t> {
  if (value == anchor)
    return 0;

  auto value_constant = AffineLoopInfo::integer_constant(value);
  auto anchor_constant = AffineLoopInfo::integer_constant(anchor);
  if (value_constant && anchor_constant)
    return static_cast<int64_t>(*value_constant) - *anchor_constant;

  for (auto *scope = &loop; scope; scope = scope->get_parent()) {
    auto counted = context.affine_loops->match_counted_loop(*scope);
    if (!counted)
      continue;
    auto value_form =
      context.affine_loops->affine_form(value, *counted, *scope);
    auto anchor_form =
      context.affine_loops->affine_form(anchor, *counted, *scope);
    if (
      !value_form || !anchor_form ||
      value_form->coefficient != anchor_form->coefficient
    ) {
      continue;
    }
    if (
      !context.affine_loops->is_no_wrap(value, *counted, *scope) ||
      !context.affine_loops->is_no_wrap(anchor, *counted, *scope)
    ) {
      return std::nullopt;
    }
    auto difference = static_cast<__int128>(value_form->offset) -
                      static_cast<__int128>(anchor_form->offset);
    if (
      difference < std::numeric_limits<int64_t>::min() ||
      difference > std::numeric_limits<int64_t>::max()
    ) {
      return std::nullopt;
    }
    return static_cast<int64_t>(difference);
  }
  return std::nullopt;
}

auto ImmutablePointerSlotAnalysis::is_immutable_local_slot(
  Value *base, Block *preheader
) const -> bool {
  if (
    !base || base->kind != ValueKind::OpResult || !base->type->is_ptr() ||
    !preheader
  ) {
    return false;
  }

  auto *alloca = static_cast<Op *>(static_cast<OpResult *>(base)->creator);
  auto alloca_block = context.op_blocks.find(alloca);
  if (
    !alloca || alloca->code != OpCode::Alloca || !alloca->result ||
    alloca->result != base || alloca_block == context.op_blocks.end() ||
    !context.dom || !context.dom->dominate(alloca_block->second, preheader)
  ) {
    return false;
  }

  auto stored_type = std::static_pointer_cast<Ptr>(base->type)->target;
  if (!stored_type || !stored_type->is_ptr())
    return false;

  Op *initial_store = nullptr;
  for (const auto &[user, block] : context.op_blocks) {
    (void)block;
    if (user->code == OpCode::Phi) {
      const auto &incoming = std::get<PhiPayload>(user->payload).incoming;
      if (std::any_of(incoming.begin(), incoming.end(), [&](const auto &edge) {
            return edge.second == base;
          })) {
        return false;
      }
      continue;
    }
    const bool uses_base =
      std::find(user->operands.begin(), user->operands.end(), base) !=
      user->operands.end();
    if (!uses_base)
      continue;
    if (
      user->code == OpCode::Store && user->operands.size() == 2 &&
      user->operands[1] == base
    ) {
      if (
        initial_store || !user->operands[0] ||
        user->operands[0]->type != stored_type
      ) {
        return false;
      }
      initial_store = user;
      continue;
    }
    if (
      user->code != OpCode::GetPtr || user->operands.empty() ||
      user->operands[0] != base
    ) {
      return false;
    }
  }
  if (!initial_store)
    return false;

  auto store_block = context.op_blocks.find(initial_store);
  return store_block != context.op_blocks.end() &&
         context.dom->dominate(store_block->second, preheader) &&
         context.affine_loops->is_available_at_preheader(
           initial_store->operands[0], preheader
         );
}

} // namespace exodus::mid_ir::opt
