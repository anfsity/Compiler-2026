#include "loop_strength_reduce.hpp"

#include "../../mid/getptr.hpp"
#include <algorithm>
#include <cstdint>
#include <limits>

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

auto LoopStrengthReduce::reduce_getptrs(LinearFunction &func, const Loop &loop)
  -> bool {
  auto counted = affine_loops->match_counted_loop(loop);
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
    formed_implicit_slot_recurrence |= reads_immutable_slot;
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
  // Keep the migration candidate-neutral.  The former LSR proved dynamic
  // bounds only for the direct unit induction; checked offset ranges are a
  // new capability and need a separate profitability/validation patch.
  if (
    !AffineLoopInfo::integer_constant(counted.induction.bound) &&
    (form->coefficient != 1 || form->offset != 0)
  ) {
    return false;
  }
  if (!affine_loops->is_no_wrap(varying_value, counted, loop))
    return false;
  if (!can_clone_initial_expression(
        varying_value, induction.phi->result, induction.initial, loop, preheader
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
    varying_value,
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
    {preheader, initial_pointer->result},
    {induction.latch, next_pointer->result},
  };
  initial_pointer->result->addUse(pointer_phi);
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
