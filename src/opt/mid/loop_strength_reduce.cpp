#include "loop_strength_reduce.hpp"

#include "../../mid/getptr.hpp"
#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <unordered_set>

namespace exodus::mid_ir::opt {

auto LoopStrengthReduce::run(
  LinearFunction &func, exodus::opt::LinearFunctionAnalysisManager &am
) -> exodus::opt::PreservedAnalysis {
  if (func.blocks.empty())
    return exodus::opt::PreservedAnalysis::all();

  auto &loop_info = am.get_result<LoopAnalysis>(func);
  auto &dom_result = am.get_result<DominanceAnalysis>(func);
  dom = &dom_result;
  affine_loops = &am.get_result<AffineLoopAnalysis>(func);
  build_op_block_map(func);

  bool changed = false;
  for (auto *loop : loop_info.get_loops_innermost_first()) {
    // A recurrence in a non-innermost loop stays live across its complete
    // subloops.  That long live range can cost more spills than the occasional
    // outer-loop address calculation saves, so only form short-lived pointer
    // recurrences here.
    if (!loop->get_subloops().empty())
      continue;
    changed |= reduce_getptrs(func, *loop);
  }

  op_blocks.clear();
  affine_loops = nullptr;
  dom = nullptr;
  if (!changed)
    return exodus::opt::PreservedAnalysis::all();

  exodus::opt::PreservedAnalysis preserved;
  preserved.preserve<DominanceAnalysis>();
  preserved.preserve<LoopAnalysis>();
  return preserved;
}

auto LoopStrengthReduce::build_op_block_map(LinearFunction &func) -> void {
  op_blocks.clear();
  for (auto &block : func.blocks) {
    for (auto *op : block->insts)
      op_blocks[op] = block.get();
  }
}

auto LoopStrengthReduce::linear_coefficient(
  Value *value, Value *induction, const Loop &loop
) const -> std::optional<int> {
  if (value == induction)
    return 1;
  if (!value || value->kind != ValueKind::OpResult)
    return 0;

  auto *creator = static_cast<Op *>(static_cast<OpResult *>(value)->creator);
  auto block_it = op_blocks.find(creator);
  if (
    !creator || block_it == op_blocks.end() || !loop.contains(block_it->second)
  )
    return 0;
  if (!is_clonable_expression(*creator, loop))
    return std::nullopt;

  if (creator->code == OpCode::Load)
    return 0;
  if (creator->operands.size() != 2)
    return std::nullopt;

  auto lhs = linear_coefficient(creator->operands[0], induction, loop);
  auto rhs = linear_coefficient(creator->operands[1], induction, loop);
  if (!lhs || !rhs)
    return std::nullopt;
  if (creator->code != OpCode::Add && creator->code != OpCode::Sub)
    return *lhs == 0 && *rhs == 0 ? std::optional<int>{0} : std::nullopt;
  long long coefficient = creator->code == OpCode::Add
                            ? static_cast<long long>(*lhs) + *rhs
                            : static_cast<long long>(*lhs) - *rhs;
  if (
    coefficient < std::numeric_limits<int>::min() ||
    coefficient > std::numeric_limits<int>::max()
  ) {
    return std::nullopt;
  }
  return static_cast<int>(coefficient);
}

auto LoopStrengthReduce::matches_migrated_affine_shape(
  Value *value, Value *induction, const Loop &loop
) const -> bool {
  if (value == induction || AffineLoopInfo::integer_constant(value))
    return true;
  if (!value || value->kind != ValueKind::OpResult)
    return false;

  auto *creator = static_cast<Op *>(static_cast<OpResult *>(value)->creator);
  auto block = op_blocks.find(creator);
  if (
    !creator || block == op_blocks.end() || !loop.contains(block->second) ||
    creator->operands.size() != 2 ||
    (creator->code != OpCode::Add && creator->code != OpCode::Sub)
  ) {
    return false;
  }
  return matches_migrated_affine_shape(creator->operands[0], induction, loop) &&
         matches_migrated_affine_shape(creator->operands[1], induction, loop);
}

auto LoopStrengthReduce::expression_cost(
  Value *value, Value *induction, const Loop &loop
) const -> std::optional<unsigned> {
  if (value == induction || !value || value->kind != ValueKind::OpResult)
    return 0;
  auto *creator = static_cast<Op *>(static_cast<OpResult *>(value)->creator);
  auto block_it = op_blocks.find(creator);
  if (
    !creator || block_it == op_blocks.end() || !loop.contains(block_it->second)
  )
    return 0;
  if (!is_clonable_expression(*creator, loop))
    return std::nullopt;

  unsigned cost = creator->code == OpCode::Load ? 0u : 1u;
  for (auto *operand : creator->operands) {
    auto operand_cost = expression_cost(operand, induction, loop);
    if (!operand_cost)
      return std::nullopt;
    cost += *operand_cost;
  }
  return cost;
}

auto LoopStrengthReduce::is_clonable_expression(
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

auto LoopStrengthReduce::clone_initial_expression(
  Value *value,
  Value *induction,
  Value *initial,
  const Loop &loop,
  Block *preheader,
  std::unordered_map<Value *, Value *> &cache
) -> Value * {
  if (value == induction)
    return initial;
  if (!value || value->kind != ValueKind::OpResult)
    return value;
  if (auto it = cache.find(value); it != cache.end())
    return it->second;

  auto *creator = static_cast<Op *>(static_cast<OpResult *>(value)->creator);
  auto block_it = op_blocks.find(creator);
  if (
    !creator || block_it == op_blocks.end() || !loop.contains(block_it->second)
  ) {
    return affine_loops->is_available_at_preheader(value, preheader) ? value
                                                                     : nullptr;
  }
  if (!is_clonable_expression(*creator, loop) || !creator->result)
    return nullptr;

  auto *clone = module->make_op(creator->code, creator->payload);
  for (auto *operand : creator->operands) {
    auto *replacement = clone_initial_expression(
      operand, induction, initial, loop, preheader, cache
    );
    if (!replacement)
      return nullptr;
    clone->operands.push_back(replacement);
  }
  clone->result =
    module->ctx->make_value<OpResult>(creator->result->type, clone);
  for (auto *operand : clone->operands)
    operand->addUse(clone);
  preheader->insts.insert(std::prev(preheader->insts.end()), clone);
  op_blocks[clone] = preheader;
  cache[value] = clone->result;
  return clone->result;
}

auto LoopStrengthReduce::can_clone_initial_expression(
  Value *value,
  Value *induction,
  Value *initial,
  const Loop &loop,
  Block *preheader
) const -> bool {
  if (value == induction)
    return affine_loops->is_available_at_preheader(initial, preheader);
  if (!value || value->kind != ValueKind::OpResult)
    return value != nullptr;

  auto *creator = static_cast<Op *>(static_cast<OpResult *>(value)->creator);
  auto block_it = op_blocks.find(creator);
  if (!creator || block_it == op_blocks.end())
    return false;
  if (!loop.contains(block_it->second))
    return affine_loops->is_available_at_preheader(value, preheader);
  if (!creator->result || !is_clonable_expression(*creator, loop))
    return false;
  return std::all_of(
    creator->operands.begin(), creator->operands.end(), [&](Value *operand) {
      return can_clone_initial_expression(
        operand, induction, initial, loop, preheader
      );
    }
  );
}

auto LoopStrengthReduce::dynamic_guard_proves_no_wrap(
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
    auto block = op_blocks.find(creator);
    if (
      !creator || block == op_blocks.end() || !loop.contains(block->second) ||
      creator->operands.size() != 2 ||
      (creator->code != OpCode::Add && creator->code != OpCode::Sub)
    ) {
      active.erase(current);
      return false;
    }
    const bool operands_safe =
      prove(creator->operands[0]) && prove(creator->operands[1]);
    auto form = affine_loops->affine_form(current, counted, loop);
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

auto LoopStrengthReduce::constant_affine_difference(
  Value *value, Value *anchor, const Loop &loop
) const -> std::optional<int64_t> {
  if (value == anchor)
    return 0;

  auto value_constant = AffineLoopInfo::integer_constant(value);
  auto anchor_constant = AffineLoopInfo::integer_constant(anchor);
  if (value_constant && anchor_constant)
    return static_cast<int64_t>(*value_constant) - *anchor_constant;

  for (auto *scope = &loop; scope; scope = scope->get_parent()) {
    auto counted = affine_loops->match_counted_loop(*scope);
    if (!counted)
      continue;
    auto value_form = affine_loops->affine_form(value, *counted, *scope);
    auto anchor_form = affine_loops->affine_form(anchor, *counted, *scope);
    if (
      !value_form || !anchor_form ||
      value_form->coefficient != anchor_form->coefficient
    ) {
      continue;
    }
    if (
      !affine_loops->is_no_wrap(value, *counted, *scope) ||
      !affine_loops->is_no_wrap(anchor, *counted, *scope)
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

auto LoopStrengthReduce::reduce_getptrs(LinearFunction &func, const Loop &loop)
  -> bool {
  auto counted = affine_loops->match_counted_loop(loop, true);
  if (!counted)
    return false;

  std::vector<Op *> candidates;
  for (auto *block : loop.get_blocks()) {
    for (auto *op : block->insts) {
      if (
        op->code == OpCode::GetPtr && op->result && op->operands.size() >= 2 &&
        op->operands[0]->type->is_ptr() && op->result->type->is_ptr()
      ) {
        candidates.push_back(op);
      }
    }
  }

  MidIRRewriter rewriter;
  rewriter.set_scope(func);
  bool changed = false;
  std::unordered_map<Value *, Value *> cache;
  struct FormedRecurrence {
    std::vector<Value *> operands;
    std::shared_ptr<Type> result_type;
    std::shared_ptr<Type> layout_type;
    Value *value = nullptr;
  };
  std::vector<FormedRecurrence> formed;
  std::unordered_set<Op *> reduced;
  bool formed_implicit_slot_recurrence = false;
  for (auto *getptr : candidates) {
    auto duplicate = std::find_if(
      formed.begin(), formed.end(), [&](const FormedRecurrence &recurrence) {
        return recurrence.operands == getptr->operands &&
               recurrence.result_type == getptr->result->type &&
               recurrence.layout_type == getptr_layout_type(*getptr);
      }
    );
    if (duplicate != formed.end()) {
      rewriter.replace_all_uses_with(getptr->result, duplicate->value);
      rewriter.eraseOp(getptr);
      op_blocks.erase(getptr);
      changed = true;
      continue;
    }

    auto plan = mid_ir::analyze_getptr(*getptr);
    if (!plan.valid)
      continue;
    const bool reads_immutable_slot =
      plan.reads_memory &&
      immutable_local_pointer_slot(getptr->operands[0], loop.get_preheader());
    const bool has_identical_candidate =
      std::any_of(candidates.begin(), candidates.end(), [&](Op *other) {
        return other != getptr && other->result &&
               other->operands == getptr->operands &&
               other->result->type == getptr->result->type;
      });
    if (
      reads_immutable_slot &&
      (formed_implicit_slot_recurrence || !has_identical_candidate)
    )
      continue;

    Value *replacement = nullptr;
    if (!reduce_getptr(getptr, loop, *counted, rewriter, cache, &replacement)) {
      continue;
    }
    formed.push_back(
      {getptr->operands,
       getptr->result->type,
       getptr_layout_type(*getptr),
       replacement}
    );
    reduced.insert(getptr);
    formed_implicit_slot_recurrence |= reads_immutable_slot;
    changed = true;
  }

  // Rebase nearby addresses on an already-formed recurrence.  This shares one
  // loop-carried pointer across accesses such as a[i][j +/- 1][k] while
  // preserving the source's i32 index semantics: every non-identical index
  // must have a checked, no-wrap affine difference in this loop nest.
  for (auto *getptr : candidates) {
    if (reduced.count(getptr) || !getptr->result || !op_blocks.count(getptr))
      continue;
    auto plan = mid_ir::analyze_getptr(*getptr);
    if (!plan.valid || plan.reads_memory)
      continue;

    FormedRecurrence *best_anchor = nullptr;
    int64_t best_byte_offset = 0;
    for (auto &anchor : formed) {
      if (
        anchor.operands.size() != getptr->operands.size() ||
        anchor.operands.empty() || anchor.operands[0] != getptr->operands[0] ||
        anchor.result_type != getptr->result->type ||
        anchor.layout_type != getptr_layout_type(*getptr)
      ) {
        continue;
      }

      __int128 byte_offset = 0;
      bool compatible = true;
      for (const auto &step : plan.steps) {
        if (
          step.kind != ir::GetPtrStep::Kind::Index ||
          step.index_pos + 1 >= getptr->operands.size()
        ) {
          compatible = false;
          break;
        }
        auto difference = constant_affine_difference(
          getptr->operands[step.index_pos + 1],
          anchor.operands[step.index_pos + 1],
          loop
        );
        if (!difference) {
          compatible = false;
          break;
        }
        byte_offset += static_cast<__int128>(*difference) * step.scale;
      }
      if (
        !compatible || byte_offset < std::numeric_limits<int64_t>::min() ||
        byte_offset > std::numeric_limits<int64_t>::max()
      ) {
        continue;
      }

      auto candidate_offset = static_cast<int64_t>(byte_offset);
      // Only create a rebase that the target can absorb directly in a memory
      // operand.  Larger offsets need their own LI/ADD pair, so retaining a
      // separate short-lived recurrence is both cheaper and more stable under
      // the fixed-point pipeline.
      if (candidate_offset < -2048 || candidate_offset > 2047)
        continue;
      auto magnitude = [](int64_t value) -> uint64_t {
        return value < 0 ? uint64_t{0} - static_cast<uint64_t>(value)
                         : static_cast<uint64_t>(value);
      };
      if (
        !best_anchor ||
        magnitude(candidate_offset) < magnitude(best_byte_offset)
      ) {
        best_anchor = &anchor;
        best_byte_offset = candidate_offset;
      }
    }
    if (!best_anchor)
      continue;

    auto update_plan =
      ir::analyze_getptr(best_anchor->value->type, getptr->result->type, 1);
    if (
      update_plan.reads_memory || update_plan.steps.size() != 1 ||
      update_plan.steps.front().kind != ir::GetPtrStep::Kind::Index ||
      update_plan.steps.front().scale <= 0 ||
      best_byte_offset % update_plan.steps.front().scale != 0
    ) {
      continue;
    }
    auto pointer_offset = best_byte_offset / update_plan.steps.front().scale;
    if (
      pointer_offset < std::numeric_limits<int>::min() ||
      pointer_offset > std::numeric_limits<int>::max()
    ) {
      continue;
    }

    auto *rebased = module->make_op(OpCode::GetPtr);
    auto *offset =
      module->ctx->make_const(I32::get(), static_cast<int>(pointer_offset));
    rebased->operands = {best_anchor->value, offset};
    rebased->payload = default_getptr_payload(best_anchor->value);
    rebased->result =
      module->ctx->make_value<OpResult>(getptr->result->type, rebased);
    best_anchor->value->addUse(rebased);
    offset->addUse(rebased);

    auto block = op_blocks.find(getptr);
    if (block == op_blocks.end())
      continue;
    auto position = std::find(
      block->second->insts.begin(), block->second->insts.end(), getptr
    );
    if (position == block->second->insts.end())
      continue;
    block->second->insts.insert(position, rebased);
    op_blocks[rebased] = block->second;
    rewriter.replace_all_uses_with(getptr->result, rebased->result);
    rewriter.eraseOp(getptr);
    op_blocks.erase(getptr);
    changed = true;
  }
  rewriter.finalize(func);
  return changed;
}

auto LoopStrengthReduce::immutable_local_pointer_slot(
  Value *base, Block *preheader
) const -> bool {
  if (
    !base || base->kind != ValueKind::OpResult || !base->type->is_ptr() ||
    !preheader
  ) {
    return false;
  }

  auto *alloca = static_cast<Op *>(static_cast<OpResult *>(base)->creator);
  auto alloca_block = op_blocks.find(alloca);
  if (
    !alloca || alloca->code != OpCode::Alloca || !alloca->result ||
    alloca->result != base || alloca_block == op_blocks.end() || !dom ||
    !dom->dominate(alloca_block->second, preheader)
  ) {
    return false;
  }

  auto stored_type = std::static_pointer_cast<Ptr>(base->type)->target;
  if (!stored_type || !stored_type->is_ptr())
    return false;

  Op *initial_store = nullptr;
  for (const auto &[user, block] : op_blocks) {
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

  auto store_block = op_blocks.find(initial_store);
  return store_block != op_blocks.end() &&
         dom->dominate(store_block->second, preheader) &&
         affine_loops->is_available_at_preheader(
           initial_store->operands[0], preheader
         );
}

auto LoopStrengthReduce::reduce_getptr(
  Op *getptr,
  const Loop &loop,
  const CountedLoopInfo &counted,
  MidIRRewriter &rewriter,
  std::unordered_map<Value *, Value *> &cache,
  Value **replacement
) -> bool {
  if (replacement)
    *replacement = nullptr;
  const auto &induction = counted.induction;
  if (
    !getptr->result || getptr->operands.size() < 2 ||
    !getptr->operands[0]->type->is_ptr() || !getptr->result->type->is_ptr()
  ) {
    return false;
  }
  auto *preheader = loop.get_preheader();
  if (
    !preheader ||
    !affine_loops->is_available_at_preheader(getptr->operands[0], preheader)
  )
    return false;
  auto getptr_block = op_blocks.find(getptr);
  if (
    getptr_block == op_blocks.end() || !dom ||
    !dom->dominate(getptr_block->second, induction.latch)
  ) {
    return false;
  }

  auto index_count = getptr->operands.size() - 1;
  auto plan = mid_ir::analyze_getptr(*getptr);
  if (!plan.valid)
    return false;
  const bool reads_immutable_slot =
    plan.reads_memory &&
    immutable_local_pointer_slot(getptr->operands[0], preheader);
  bool has_non_store_use = false;
  if (reads_immutable_slot) {
    for (const auto &[user, block] : op_blocks) {
      (void)block;
      bool uses_result =
        std::find(
          user->operands.begin(), user->operands.end(), getptr->result
        ) != user->operands.end();
      if (!uses_result)
        continue;
      if (
        user->code != OpCode::Store || user->operands.size() != 2 ||
        user->operands[1] != getptr->result
      ) {
        has_non_store_use = true;
        break;
      }
    }
  }
  if (
    (plan.reads_memory && !reads_immutable_slot) ||
    (reads_immutable_slot && !has_non_store_use) ||
    std::count_if(
      plan.steps.begin(),
      plan.steps.end(),
      [](const auto &step) { return step.kind == ir::GetPtrStep::Kind::Index; }
    ) != static_cast<std::ptrdiff_t>(index_count) ||
    std::count_if(
      plan.steps.begin(),
      plan.steps.end(),
      [](const auto &step) {
        return step.kind == ir::GetPtrStep::Kind::ImplicitLoad;
      }
    ) != static_cast<std::ptrdiff_t>(reads_immutable_slot ? 1 : 0) ||
    (reads_immutable_slot &&
     (plan.steps.empty() ||
      plan.steps.front().kind != ir::GetPtrStep::Kind::ImplicitLoad))
  ) {
    return false;
  }

  size_t varying_index = index_count;
  int varying_coefficient = 0;
  int varying_scale = 0;
  unsigned original_cost = 0;
  for (const auto &step : plan.steps) {
    if (step.kind == ir::GetPtrStep::Kind::ImplicitLoad) {
      // The immutable pointer slot is loaded once in the recurrence preheader
      // instead of once per source GetPtr execution.
      ++original_cost;
      continue;
    }
    if (step.index_pos >= index_count || step.scale <= 0)
      return false;
    auto *index = getptr->operands[step.index_pos + 1];
    auto coefficient = linear_coefficient(index, induction.phi->result, loop);
    if (!coefficient)
      return false;
    if (*coefficient != 0) {
      if (
        varying_index != index_count ||
        (*coefficient != 1 && *coefficient != -1)
      ) {
        return false;
      }
      varying_index = step.index_pos;
      varying_coefficient = *coefficient;
      varying_scale = step.scale;
      auto expression = expression_cost(index, induction.phi->result, loop);
      if (!expression)
        return false;
      original_cost += *expression;
    } else if (!affine_loops->is_available_at_preheader(index, preheader)) {
      return false;
    }

    // Every pure index step needs an address add, and a non-unit scale needs
    // at least one additional instruction in the current RISC-V lowering.
    original_cost += 1 + static_cast<unsigned>(step.scale != 1);
  }
  if (varying_index == index_count)
    return false;

  auto *varying_value = getptr->operands[varying_index + 1];
  if (!matches_migrated_affine_shape(
        varying_value, induction.phi->result, loop
      )) {
    return false;
  }
  auto form = affine_loops->affine_form(varying_value, counted, loop);
  if (!form || (form->coefficient != 1 && form->coefficient != -1))
    return false;
  const bool regular_no_wrap =
    affine_loops->is_no_wrap(varying_value, counted, loop);
  const bool guard_no_wrap =
    !regular_no_wrap &&
    dynamic_guard_proves_no_wrap(varying_value, counted, loop);
  if (!regular_no_wrap && !guard_no_wrap)
    return false;
  // For a dynamic guarded IV + offset, do not speculate the source i32
  // arithmetic in the preheader: a zero-trip loop need not evaluate it.  Seed
  // the address with the raw IV and apply the proven constant displacement in
  // pointer width instead.
  const bool seed_with_raw_induction = guard_no_wrap && form->offset != 0;
  auto *initial_expression =
    seed_with_raw_induction ? induction.phi->result : varying_value;
  if (!can_clone_initial_expression(
        initial_expression,
        induction.phi->result,
        induction.initial,
        loop,
        preheader
      ))
    return false;

  auto update_plan =
    ir::analyze_getptr(getptr->result->type, getptr->result->type, 1);
  if (
    update_plan.reads_memory || update_plan.steps.size() != 1 ||
    update_plan.steps.front().kind != ir::GetPtrStep::Kind::Index ||
    update_plan.steps.front().scale <= 0
  ) {
    return false;
  }

  auto byte_step = static_cast<int64_t>(varying_coefficient) *
                   static_cast<int64_t>(induction.step) * varying_scale;
  auto update_scale = update_plan.steps.front().scale;
  if (byte_step % update_scale != 0)
    return false;
  auto pointer_step = byte_step / update_scale;
  if (
    pointer_step < std::numeric_limits<int>::min() ||
    pointer_step > std::numeric_limits<int>::max()
  ) {
    return false;
  }

  std::optional<int> seed_pointer_offset;
  if (seed_with_raw_induction) {
    auto initial_byte_offset =
      static_cast<__int128>(form->offset) * varying_scale;
    if (
      initial_byte_offset < std::numeric_limits<int64_t>::min() ||
      initial_byte_offset > std::numeric_limits<int64_t>::max() ||
      initial_byte_offset % update_scale != 0
    ) {
      return false;
    }
    auto initial_offset = initial_byte_offset / update_scale;
    if (
      initial_offset < std::numeric_limits<int>::min() ||
      initial_offset > std::numeric_limits<int>::max()
    ) {
      return false;
    }
    seed_pointer_offset = static_cast<int>(initial_offset);
  }

  unsigned update_cost = 0;
  if (byte_step >= -2048 && byte_step <= 2047) {
    update_cost = 1;
  } else if (
    byte_step >= std::numeric_limits<int>::min() &&
    byte_step <= std::numeric_limits<int>::max()
  ) {
    update_cost = 2;
  } else {
    update_cost = 2 + static_cast<unsigned>(update_scale != 1);
  }
  // Reserve one instruction for a possible backedge Phi copy.  Simple
  // one-index addressing therefore stays unchanged unless the recurrence has
  // a clear instruction-count advantage.
  if (original_cost <= update_cost + 1)
    return false;

  auto *initial_index = clone_initial_expression(
    initial_expression,
    induction.phi->result,
    induction.initial,
    loop,
    preheader,
    cache
  );
  if (!initial_index)
    return false;

  auto *initial_pointer = module->make_op(OpCode::GetPtr);
  initial_pointer->payload = getptr->payload;
  initial_pointer->operands = getptr->operands;
  initial_pointer->operands[varying_index + 1] = initial_index;
  initial_pointer->result =
    module->ctx->make_value<OpResult>(getptr->result->type, initial_pointer);
  for (auto *operand : initial_pointer->operands)
    operand->addUse(initial_pointer);
  preheader->insts.insert(std::prev(preheader->insts.end()), initial_pointer);
  op_blocks[initial_pointer] = preheader;

  Value *recurrence_initial = initial_pointer->result;
  if (seed_pointer_offset) {
    auto *adjusted = module->make_op(OpCode::GetPtr);
    auto *offset = module->ctx->make_const(I32::get(), *seed_pointer_offset);
    adjusted->operands = {initial_pointer->result, offset};
    adjusted->payload = default_getptr_payload(initial_pointer->result);
    adjusted->result =
      module->ctx->make_value<OpResult>(getptr->result->type, adjusted);
    initial_pointer->result->addUse(adjusted);
    offset->addUse(adjusted);
    preheader->insts.insert(std::prev(preheader->insts.end()), adjusted);
    op_blocks[adjusted] = preheader;
    recurrence_initial = adjusted->result;
  }

  auto *pointer_phi = module->make_op(OpCode::Phi, PhiPayload{});
  pointer_phi->result =
    module->ctx->make_value<OpResult>(getptr->result->type, pointer_phi);

  auto *next_pointer = module->make_op(OpCode::GetPtr);
  auto *step_value =
    module->ctx->make_const(I32::get(), static_cast<int>(pointer_step));
  next_pointer->operands = {pointer_phi->result, step_value};
  next_pointer->payload = default_getptr_payload(pointer_phi->result);
  next_pointer->result =
    module->ctx->make_value<OpResult>(getptr->result->type, next_pointer);
  for (auto *operand : next_pointer->operands)
    operand->addUse(next_pointer);
  induction.latch->insts.insert(
    std::prev(induction.latch->insts.end()), next_pointer
  );
  op_blocks[next_pointer] = induction.latch;

  auto &incoming = std::get<PhiPayload>(pointer_phi->payload).incoming;
  incoming = {
    {preheader, recurrence_initial},
    {induction.latch, next_pointer->result},
  };
  recurrence_initial->addUse(pointer_phi);
  next_pointer->result->addUse(pointer_phi);

  auto insert_pos = loop.get_header()->insts.begin();
  while (insert_pos != loop.get_header()->insts.end() &&
         (*insert_pos)->code == OpCode::Phi) {
    ++insert_pos;
  }
  loop.get_header()->insts.insert(insert_pos, pointer_phi);
  op_blocks[pointer_phi] = loop.get_header();

  rewriter.replace_all_uses_with(getptr->result, pointer_phi->result);
  rewriter.eraseOp(getptr);
  op_blocks.erase(getptr);
  if (replacement)
    *replacement = pointer_phi->result;
  return true;
}

} // namespace exodus::mid_ir::opt
