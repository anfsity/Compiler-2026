#include "loop_strength_reduce_recurrence.hpp"

#include "../../mid/getptr.hpp"
#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>

namespace exodus::mid_ir::opt {

auto PointerRecurrenceBuilder::make_getptr(
  LSRContext &context,
  std::vector<Value *> operands,
  Op::Payload payload,
  const std::shared_ptr<Type> &result_type
) -> Op * {
  auto *getptr = context.module->make_op(OpCode::GetPtr, std::move(payload));
  getptr->operands = std::move(operands);
  getptr->result =
    context.module->ctx->make_value<OpResult>(result_type, getptr);
  for (auto *operand : getptr->operands)
    operand->addUse(getptr);
  return getptr;
}

auto PointerRecurrenceBuilder::clone_initial_expression(
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
  auto block_it = context.op_blocks.find(creator);
  if (
    !creator || block_it == context.op_blocks.end() ||
    !loop.contains(block_it->second)
  ) {
    return context.affine_loops->is_available_at_preheader(value, preheader)
             ? value
             : nullptr;
  }
  auto *result = creator->result;
  if (!result)
    return nullptr;

  // The matcher has already established that this is a clonable expression.
  // Build operands before allocating/inserting the clone so a failed lookup
  // cannot leave a partially formed operation in the module context.
  std::vector<Value *> operands;
  operands.reserve(creator->operands.size());
  for (auto *operand : creator->operands) {
    auto *replacement = clone_initial_expression(
      operand, induction, initial, loop, preheader, cache
    );
    if (!replacement)
      return nullptr;
    operands.push_back(replacement);
  }

  auto *clone = context.module->make_op(creator->code, creator->payload);
  clone->operands = std::move(operands);
  clone->result =
    context.module->ctx->make_value<OpResult>(result->type, clone);
  for (auto *operand : clone->operands)
    operand->addUse(clone);
  preheader->insts.insert(std::prev(preheader->insts.end()), clone);
  context.op_blocks[clone] = preheader;
  cache[value] = clone->result;
  return clone->result;
}

auto PointerRecurrenceBuilder::materialize(
  Op *getptr,
  const Loop &loop,
  const CountedLoopInfo &counted,
  const GetPtrReductionPlan &plan,
  MidIRRewriter &rewriter,
  std::unordered_map<Value *, Value *> &cache,
  Value **replacement
) -> bool {
  if (replacement)
    *replacement = nullptr;
  const auto &induction = counted.induction;
  auto *preheader = loop.get_preheader();
  auto *initial_index = clone_initial_expression(
    plan.initial_expression,
    induction.phi->result,
    induction.initial,
    loop,
    preheader,
    cache
  );
  if (!initial_index)
    return false;

  auto initial_operands = getptr->operands;
  initial_operands[plan.varying_index + 1] = initial_index;
  auto *initial_pointer = make_getptr(
    context, std::move(initial_operands), getptr->payload, getptr->result->type
  );
  preheader->insts.insert(std::prev(preheader->insts.end()), initial_pointer);
  context.op_blocks[initial_pointer] = preheader;

  Value *recurrence_initial = initial_pointer->result;
  if (plan.seed_pointer_offset) {
    auto *offset =
      context.module->ctx->make_const(I32::get(), *plan.seed_pointer_offset);
    auto *adjusted = make_getptr(
      context,
      {initial_pointer->result, offset},
      default_getptr_payload(initial_pointer->result),
      getptr->result->type
    );
    preheader->insts.insert(std::prev(preheader->insts.end()), adjusted);
    context.op_blocks[adjusted] = preheader;
    recurrence_initial = adjusted->result;
  }

  auto *pointer_phi = context.module->make_op(OpCode::Phi, PhiPayload{});
  pointer_phi->result = context.module->ctx->make_value<OpResult>(
    getptr->result->type, pointer_phi
  );

  auto *step_value =
    context.module->ctx->make_const(I32::get(), plan.pointer_step);
  auto *next_pointer = make_getptr(
    context,
    {pointer_phi->result, step_value},
    default_getptr_payload(pointer_phi->result),
    getptr->result->type
  );
  induction.latch->insts.insert(
    std::prev(induction.latch->insts.end()), next_pointer
  );
  context.op_blocks[next_pointer] = induction.latch;

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
  context.op_blocks[pointer_phi] = loop.get_header();

  rewriter.replace_all_uses_with(getptr->result, pointer_phi->result);
  rewriter.eraseOp(getptr);
  if (replacement)
    *replacement = pointer_phi->result;
  return true;
}

auto GetPtrRebaser::find_best_anchor(
  Op *getptr,
  const Loop &loop,
  const std::vector<FormedGetPtrRecurrence> &formed
) const -> std::optional<std::pair<Value *, int64_t>> {
  auto plan = mid_ir::analyze_getptr(*getptr);
  if (!plan.valid || plan.reads_memory)
    return std::nullopt;

  Value *best_anchor = nullptr;
  int64_t best_byte_offset = 0;
  for (const auto &anchor : formed) {
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
      auto difference = expressions.constant_affine_difference(
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
      !best_anchor || magnitude(candidate_offset) < magnitude(best_byte_offset)
    ) {
      best_anchor = anchor.value;
      best_byte_offset = candidate_offset;
    }
  }
  if (!best_anchor)
    return std::nullopt;
  return std::pair<Value *, int64_t>{best_anchor, best_byte_offset};
}

auto GetPtrRebaser::rebase(
  const Loop &loop,
  const std::vector<Op *> &candidates,
  const std::vector<FormedGetPtrRecurrence> &formed,
  const std::unordered_set<Op *> &reduced,
  MidIRRewriter &rewriter
) -> bool {
  bool changed = false;
  for (auto *getptr : candidates) {
    if (
      reduced.count(getptr) || !getptr->result ||
      !context.op_blocks.count(getptr)
    ) {
      continue;
    }

    auto anchor = find_best_anchor(getptr, loop, formed);
    if (!anchor)
      continue;

    auto *anchor_value = anchor->first;
    auto byte_offset = anchor->second;
    auto update_plan =
      ir::analyze_getptr(anchor_value->type, getptr->result->type, 1);
    if (
      update_plan.reads_memory || update_plan.steps.size() != 1 ||
      update_plan.steps.front().kind != ir::GetPtrStep::Kind::Index ||
      update_plan.steps.front().scale <= 0 ||
      byte_offset % update_plan.steps.front().scale != 0
    ) {
      continue;
    }
    auto pointer_offset = byte_offset / update_plan.steps.front().scale;
    if (
      pointer_offset < std::numeric_limits<int>::min() ||
      pointer_offset > std::numeric_limits<int>::max()
    ) {
      continue;
    }

    auto *offset = context.module->ctx->make_const(
      I32::get(), static_cast<int>(pointer_offset)
    );
    auto *rebased = PointerRecurrenceBuilder::make_getptr(
      context,
      {anchor_value, offset},
      default_getptr_payload(anchor_value),
      getptr->result->type
    );

    auto block = context.op_blocks.find(getptr);
    if (block == context.op_blocks.end())
      continue;
    auto position = std::find(
      block->second->insts.begin(), block->second->insts.end(), getptr
    );
    if (position == block->second->insts.end())
      continue;
    block->second->insts.insert(position, rebased);
    context.op_blocks[rebased] = block->second;
    rewriter.replace_all_uses_with(getptr->result, rebased->result);
    rewriter.eraseOp(getptr);
    context.op_blocks.erase(getptr);
    changed = true;
  }
  return changed;
}

auto GetPtrRecurrencePlanner::analyze(
  Op *getptr, const Loop &loop, const CountedLoopInfo &counted
) const -> std::optional<GetPtrReductionPlan> {
  const auto &induction = counted.induction;
  if (
    !getptr->result || getptr->operands.size() < 2 ||
    !getptr->operands[0]->type->is_ptr() || !getptr->result->type->is_ptr()
  ) {
    return std::nullopt;
  }
  auto *preheader = loop.get_preheader();
  if (
    !preheader || !context.affine_loops->is_available_at_preheader(
                    getptr->operands[0], preheader
                  )
  ) {
    return std::nullopt;
  }
  auto getptr_block = context.op_blocks.find(getptr);
  if (
    getptr_block == context.op_blocks.end() || !context.dom ||
    !context.dom->dominate(getptr_block->second, induction.latch)
  ) {
    return std::nullopt;
  }

  auto index_count = getptr->operands.size() - 1;
  auto getptr_plan = mid_ir::analyze_getptr(*getptr);
  if (!getptr_plan.valid)
    return std::nullopt;
  const bool reads_immutable_slot =
    getptr_plan.reads_memory &&
    immutable_slots.is_immutable_local_slot(getptr->operands[0], preheader);
  bool has_non_store_use = false;
  if (reads_immutable_slot) {
    for (const auto &[user, block] : context.op_blocks) {
      (void)block;
      const bool uses_result =
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
    (getptr_plan.reads_memory && !reads_immutable_slot) ||
    (reads_immutable_slot && !has_non_store_use) ||
    std::count_if(
      getptr_plan.steps.begin(),
      getptr_plan.steps.end(),
      [](const auto &step) { return step.kind == ir::GetPtrStep::Kind::Index; }
    ) != static_cast<std::ptrdiff_t>(index_count) ||
    std::count_if(
      getptr_plan.steps.begin(),
      getptr_plan.steps.end(),
      [](const auto &step) {
        return step.kind == ir::GetPtrStep::Kind::ImplicitLoad;
      }
    ) != static_cast<std::ptrdiff_t>(reads_immutable_slot ? 1 : 0) ||
    (reads_immutable_slot &&
     (getptr_plan.steps.empty() ||
      getptr_plan.steps.front().kind != ir::GetPtrStep::Kind::ImplicitLoad))
  ) {
    return std::nullopt;
  }

  size_t varying_index = index_count;
  int varying_coefficient = 0;
  int varying_scale = 0;
  unsigned original_cost = 0;
  for (const auto &step : getptr_plan.steps) {
    if (step.kind == ir::GetPtrStep::Kind::ImplicitLoad) {
      // The immutable pointer slot is loaded once in the recurrence preheader
      // instead of once per source GetPtr execution.
      ++original_cost;
      continue;
    }
    if (step.index_pos >= index_count || step.scale <= 0)
      return std::nullopt;
    auto *index = getptr->operands[step.index_pos + 1];
    auto expression = expressions.analyze(index, induction.phi->result, loop);
    if (!expression)
      return std::nullopt;
    if (expression->coefficient != 0) {
      if (
        varying_index != index_count ||
        (expression->coefficient != 1 && expression->coefficient != -1)
      ) {
        return std::nullopt;
      }
      varying_index = step.index_pos;
      varying_coefficient = expression->coefficient;
      varying_scale = step.scale;
      original_cost += expression->cost;
    } else if (!context.affine_loops->is_available_at_preheader(
                 index, preheader
               )) {
      return std::nullopt;
    }

    // Every pure index step needs an address add, and a non-unit scale needs
    // at least one additional instruction in the current RISC-V lowering.
    original_cost += 1 + static_cast<unsigned>(step.scale != 1);
  }
  if (varying_index == index_count)
    return std::nullopt;

  auto *varying_value = getptr->operands[varying_index + 1];
  auto varying_expression =
    expressions.analyze(varying_value, induction.phi->result, loop);
  if (!varying_expression || !varying_expression->migrated_shape)
    return std::nullopt;
  auto form = context.affine_loops->affine_form(varying_value, counted, loop);
  if (!form || (form->coefficient != 1 && form->coefficient != -1))
    return std::nullopt;
  const bool regular_no_wrap =
    context.affine_loops->is_no_wrap(varying_value, counted, loop);
  const bool guard_no_wrap =
    !regular_no_wrap &&
    expressions.dynamic_guard_proves_no_wrap(varying_value, counted, loop);
  if (!regular_no_wrap && !guard_no_wrap)
    return std::nullopt;

  // For a dynamic guarded IV + offset, do not speculate the source i32
  // arithmetic in the preheader: a zero-trip loop need not evaluate it.  Seed
  // the address with the raw IV and apply the proven constant displacement in
  // pointer width instead.
  const bool seed_with_raw_induction = guard_no_wrap && form->offset != 0;
  auto *initial_expression =
    seed_with_raw_induction ? induction.phi->result : varying_value;
  if (!expressions.can_clone_initial_expression(
        initial_expression,
        induction.phi->result,
        induction.initial,
        loop,
        preheader
      )) {
    return std::nullopt;
  }

  auto update_plan =
    ir::analyze_getptr(getptr->result->type, getptr->result->type, 1);
  if (
    update_plan.reads_memory || update_plan.steps.size() != 1 ||
    update_plan.steps.front().kind != ir::GetPtrStep::Kind::Index ||
    update_plan.steps.front().scale <= 0
  ) {
    return std::nullopt;
  }

  auto update_scale = update_plan.steps.front().scale;
  const auto byte_step_128 =
    static_cast<__int128>(varying_coefficient) * induction.step * varying_scale;
  if (
    byte_step_128 < std::numeric_limits<int64_t>::min() ||
    byte_step_128 > std::numeric_limits<int64_t>::max()
  ) {
    return std::nullopt;
  }
  auto byte_step = static_cast<int64_t>(byte_step_128);
  if (byte_step % update_scale != 0)
    return std::nullopt;
  auto pointer_step = byte_step / update_scale;
  if (
    pointer_step < std::numeric_limits<int>::min() ||
    pointer_step > std::numeric_limits<int>::max()
  ) {
    return std::nullopt;
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
      return std::nullopt;
    }
    auto initial_offset = initial_byte_offset / update_scale;
    if (
      initial_offset < std::numeric_limits<int>::min() ||
      initial_offset > std::numeric_limits<int>::max()
    ) {
      return std::nullopt;
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
    return std::nullopt;

  return GetPtrReductionPlan{
    initial_expression,
    varying_index,
    static_cast<int>(pointer_step),
    seed_pointer_offset,
  };
}

auto GetPtrRecurrencePlanner::reduce(const Loop &loop) -> bool {
  auto counted = context.affine_loops->match_counted_loop(loop, true);
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
  rewriter.set_scope(function);
  bool changed = false;
  std::unordered_map<Value *, Value *> cache;
  std::vector<FormedGetPtrRecurrence> formed;
  std::unordered_set<Op *> reduced;
  bool formed_implicit_slot_recurrence = false;
  for (auto *getptr : candidates) {
    auto duplicate = std::find_if(
      formed.begin(),
      formed.end(),
      [&](const FormedGetPtrRecurrence &recurrence) {
        return recurrence.operands == getptr->operands &&
               recurrence.result_type == getptr->result->type &&
               recurrence.layout_type == getptr_layout_type(*getptr);
      }
    );
    if (duplicate != formed.end()) {
      rewriter.replace_all_uses_with(getptr->result, duplicate->value);
      rewriter.eraseOp(getptr);
      context.op_blocks.erase(getptr);
      changed = true;
      continue;
    }

    auto source_plan = mid_ir::analyze_getptr(*getptr);
    if (!source_plan.valid)
      continue;
    const bool reads_immutable_slot =
      source_plan.reads_memory && immutable_slots.is_immutable_local_slot(
                                    getptr->operands[0], loop.get_preheader()
                                  );
    const bool has_identical_candidate =
      std::any_of(candidates.begin(), candidates.end(), [&](Op *other) {
        return other != getptr && other->result &&
               other->operands == getptr->operands &&
               other->result->type == getptr->result->type;
      });
    if (
      reads_immutable_slot &&
      (formed_implicit_slot_recurrence || !has_identical_candidate)
    ) {
      continue;
    }

    auto plan = analyze(getptr, loop, *counted);
    if (!plan)
      continue;
    Value *replacement = nullptr;
    if (!builder.materialize(
          getptr, loop, *counted, *plan, rewriter, cache, &replacement
        )) {
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

  changed |= rebaser.rebase(loop, candidates, formed, reduced, rewriter);
  rewriter.finalize(function);
  return changed;
}

} // namespace exodus::mid_ir::opt
