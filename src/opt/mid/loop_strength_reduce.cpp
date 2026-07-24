#include "loop_strength_reduce.hpp"

#include "../../base/getptr.hpp"
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
  build_op_block_map(func);

  bool changed = false;
  for (auto *loop : loop_info.get_loops_innermost_first())
    changed |= reduce_getptrs(func, *loop);

  op_blocks.clear();
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

auto LoopStrengthReduce::integer_constant(Value *value) -> std::optional<int> {
  if (!value || value->kind != ValueKind::Constant)
    return std::nullopt;
  auto *constant = static_cast<Constant *>(value);
  if (!std::holds_alternative<int>(constant->val))
    return std::nullopt;
  return std::get<int>(constant->val);
}

auto LoopStrengthReduce::find_inductions(const Loop &loop) const
  -> std::vector<Induction> {
  std::vector<Induction> result;
  if (!loop.get_preheader() || loop.get_back_edges().size() != 1)
    return result;

  Block *latch = loop.get_back_edges().front().first;
  for (auto *phi : loop.get_header()->insts) {
    if (phi->code != OpCode::Phi)
      break;
    if (!phi->result || !phi->result->type->is_i32())
      continue;

    Value *initial = nullptr;
    Value *backedge = nullptr;
    for (const auto &[pred, value] :
         std::get<PhiPayload>(phi->payload).incoming) {
      if (pred == loop.get_preheader())
        initial = value;
      else if (pred == latch)
        backedge = value;
    }
    if (!initial || !backedge || backedge->kind != ValueKind::OpResult)
      continue;

    auto *update =
      static_cast<Op *>(static_cast<OpResult *>(backedge)->creator);
    auto block_it = op_blocks.find(update);
    if (
      !update || block_it == op_blocks.end() || block_it->second != latch ||
      update->operands.size() != 2
    ) {
      continue;
    }

    std::optional<int> step;
    if (update->code == OpCode::Add) {
      if (update->operands[0] == phi->result)
        step = integer_constant(update->operands[1]);
      else if (update->operands[1] == phi->result)
        step = integer_constant(update->operands[0]);
    } else if (
      update->code == OpCode::Sub && update->operands[0] == phi->result
    ) {
      if (auto amount = integer_constant(update->operands[1])) {
        auto negated = -static_cast<int64_t>(*amount);
        if (
          negated >= std::numeric_limits<int>::min() &&
          negated <= std::numeric_limits<int>::max()
        ) {
          step = static_cast<int>(negated);
        }
      }
    }
    if (!step || *step == 0)
      continue;
    result.push_back({phi, initial, latch, *step});
  }
  return result;
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

auto LoopStrengthReduce::affine_form(
  Value *value, Value *induction, const Loop &loop
) const -> std::optional<AffineForm> {
  if (!value)
    return std::nullopt;
  if (value == induction)
    return AffineForm{1, 0};
  if (value->kind == ValueKind::Constant) {
    auto constant = integer_constant(value);
    return constant ? std::optional<AffineForm>{{0, *constant}} : std::nullopt;
  }
  if (value->kind != ValueKind::OpResult)
    return std::nullopt;

  auto *creator = static_cast<Op *>(static_cast<OpResult *>(value)->creator);
  auto block_it = op_blocks.find(creator);
  if (
    !creator || block_it == op_blocks.end() ||
    !loop.contains(block_it->second) || creator->operands.size() != 2 ||
    (creator->code != OpCode::Add && creator->code != OpCode::Sub)
  ) {
    return std::nullopt;
  }

  auto lhs = affine_form(creator->operands[0], induction, loop);
  auto rhs = affine_form(creator->operands[1], induction, loop);
  if (!lhs || !rhs)
    return std::nullopt;
  auto checked_add = [](int64_t lhs, int64_t rhs) -> std::optional<int64_t> {
    if (
      (rhs > 0 && lhs > std::numeric_limits<int64_t>::max() - rhs) ||
      (rhs < 0 && lhs < std::numeric_limits<int64_t>::min() - rhs)
    ) {
      return std::nullopt;
    }
    return lhs + rhs;
  };
  auto checked_sub =
    [&checked_add](int64_t lhs, int64_t rhs) -> std::optional<int64_t> {
    if (rhs == std::numeric_limits<int64_t>::min()) {
      if (lhs >= 0)
        return std::nullopt;
      auto partial = checked_add(lhs, std::numeric_limits<int64_t>::max());
      return partial ? checked_add(*partial, 1) : std::nullopt;
    }
    return checked_add(lhs, -rhs);
  };
  auto coefficient = creator->code == OpCode::Add
                       ? checked_add(lhs->coefficient, rhs->coefficient)
                       : checked_sub(lhs->coefficient, rhs->coefficient);
  auto offset = creator->code == OpCode::Add
                  ? checked_add(lhs->offset, rhs->offset)
                  : checked_sub(lhs->offset, rhs->offset);
  if (!coefficient || !offset)
    return std::nullopt;
  return AffineForm{*coefficient, *offset};
}

auto LoopStrengthReduce::index_is_no_wrap(
  Value *index, const Loop &loop, const Induction &induction
) const -> bool {
  auto form = affine_form(index, induction.phi->result, loop);
  auto initial = integer_constant(induction.initial);
  if (!form || !initial || (form->coefficient != 1 && form->coefficient != -1))
    return false;

  auto *header = loop.get_header();
  if (header->insts.empty() || header->insts.back()->code != OpCode::Branch)
    return false;
  auto *branch = header->insts.back();
  if (
    branch->operands.size() != 1 || branch->successors.size() != 2 ||
    (loop.contains(branch->successors[0]) ==
     loop.contains(branch->successors[1]))
  ) {
    return false;
  }
  auto *condition = branch->operands[0];
  if (!condition || condition->kind != ValueKind::OpResult)
    return false;
  auto *compare =
    static_cast<Op *>(static_cast<OpResult *>(condition)->creator);
  auto compare_block = op_blocks.find(compare);
  if (
    !compare || compare_block == op_blocks.end() ||
    compare_block->second != header || compare->operands.size() != 2
  ) {
    return false;
  }

  auto predicate = compare->code;
  Value *bound_value = nullptr;
  if (compare->operands[0] == induction.phi->result) {
    bound_value = compare->operands[1];
  } else if (compare->operands[1] == induction.phi->result) {
    bound_value = compare->operands[0];
    switch (predicate) {
    case OpCode::Lt:
      predicate = OpCode::Gt;
      break;
    case OpCode::Le:
      predicate = OpCode::Ge;
      break;
    case OpCode::Gt:
      predicate = OpCode::Lt;
      break;
    case OpCode::Ge:
      predicate = OpCode::Le;
      break;
    default:
      break;
    }
  } else {
    return false;
  }
  auto bound = integer_constant(bound_value);
  if (!bound)
    return false;

  bool true_is_continue = loop.contains(branch->successors[0]);
  if (!true_is_continue) {
    switch (predicate) {
    case OpCode::Lt:
      predicate = OpCode::Ge;
      break;
    case OpCode::Le:
      predicate = OpCode::Gt;
      break;
    case OpCode::Gt:
      predicate = OpCode::Le;
      break;
    case OpCode::Ge:
      predicate = OpCode::Lt;
      break;
    default:
      break;
    }
  }

  int64_t iv_min = 0;
  int64_t iv_max = 0;
  if (induction.step > 0) {
    if (predicate != OpCode::Lt && predicate != OpCode::Le)
      return false;
    if (*initial > *bound)
      return true;
    iv_min = *initial;
    iv_max = predicate == OpCode::Lt ? static_cast<int64_t>(*bound) - 1
                                     : static_cast<int64_t>(*bound);
    if (iv_max < iv_min)
      return true;
    if (iv_max + induction.step > std::numeric_limits<int32_t>::max())
      return false;
  } else {
    if (predicate != OpCode::Gt && predicate != OpCode::Ge)
      return false;
    if (*initial < *bound)
      return true;
    iv_max = *initial;
    iv_min = predicate == OpCode::Gt ? static_cast<int64_t>(*bound) + 1
                                     : static_cast<int64_t>(*bound);
    if (iv_min > iv_max)
      return true;
    if (iv_min + induction.step < std::numeric_limits<int32_t>::min())
      return false;
  }

  auto index_at = [form](int64_t iv) {
    return form->coefficient * iv + form->offset;
  };
  auto min_index = std::min(index_at(iv_min), index_at(iv_max));
  auto max_index = std::max(index_at(iv_min), index_at(iv_max));
  return min_index >= std::numeric_limits<int32_t>::min() &&
         max_index <= std::numeric_limits<int32_t>::max();
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
  )
    return value;
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

auto LoopStrengthReduce::reduce_getptrs(LinearFunction &func, const Loop &loop)
  -> bool {
  auto inductions = find_inductions(loop);
  if (inductions.empty())
    return false;

  std::vector<Op *> candidates;
  for (auto *block : loop.get_blocks()) {
    for (auto *op : block->insts) {
      if (op->code == OpCode::GetPtr)
        candidates.push_back(op);
    }
  }

  MidIRRewriter rewriter;
  rewriter.set_scope(func);
  bool changed = false;
  std::unordered_map<Op *, std::unordered_map<Value *, Value *>> caches;
  for (auto *getptr : candidates) {
    for (const auto &induction : inductions) {
      if (
        reduce_getptr(getptr, loop, induction, rewriter, caches[induction.phi])
      ) {
        changed = true;
        break;
      }
    }
  }
  rewriter.finalize(func);
  return changed;
}

auto LoopStrengthReduce::reduce_getptr(
  Op *getptr,
  const Loop &loop,
  const Induction &induction,
  MidIRRewriter &rewriter,
  std::unordered_map<Value *, Value *> &cache
) -> bool {
  if (
    !getptr->result || getptr->operands.size() != 2 ||
    !getptr->operands[0]->type->is_ptr() || !getptr->result->type->is_ptr()
  ) {
    return false;
  }
  if (getptr->operands[0]->kind == ValueKind::OpResult) {
    auto *base_creator =
      static_cast<Op *>(static_cast<OpResult *>(getptr->operands[0])->creator);
    auto base_block = op_blocks.find(base_creator);
    if (
      base_creator && base_block != op_blocks.end() &&
      loop.contains(base_block->second)
    ) {
      return false;
    }
  }
  auto plan =
    ir::analyze_getptr(getptr->operands[0]->type, getptr->result->type, 1);
  if (
    plan.reads_memory || plan.steps.size() != 1 ||
    plan.steps.front().kind != ir::GetPtrStep::Kind::Index
  ) {
    return false;
  }

  auto coefficient =
    linear_coefficient(getptr->operands[1], induction.phi->result, loop);
  if (!coefficient || (*coefficient != 1 && *coefficient != -1))
    return false;
  auto cost = expression_cost(getptr->operands[1], induction.phi->result, loop);
  if (!cost || *cost == 0)
    return false;
  if (!index_is_no_wrap(getptr->operands[1], loop, induction))
    return false;

  long long pointer_step =
    static_cast<long long>(*coefficient) * induction.step;
  if (
    pointer_step < std::numeric_limits<int>::min() ||
    pointer_step > std::numeric_limits<int>::max()
  ) {
    return false;
  }

  auto *initial_index = clone_initial_expression(
    getptr->operands[1],
    induction.phi->result,
    induction.initial,
    loop,
    loop.get_preheader(),
    cache
  );
  if (!initial_index)
    return false;

  auto *initial_pointer = module->make_op(OpCode::GetPtr);
  initial_pointer->operands = {getptr->operands[0], initial_index};
  initial_pointer->result =
    module->ctx->make_value<OpResult>(getptr->result->type, initial_pointer);
  for (auto *operand : initial_pointer->operands)
    operand->addUse(initial_pointer);
  loop.get_preheader()->insts.insert(
    std::prev(loop.get_preheader()->insts.end()), initial_pointer
  );
  op_blocks[initial_pointer] = loop.get_preheader();

  auto *pointer_phi = module->make_op(OpCode::Phi, PhiPayload{});
  pointer_phi->result =
    module->ctx->make_value<OpResult>(getptr->result->type, pointer_phi);

  auto *next_pointer = module->make_op(OpCode::GetPtr);
  auto *step_value =
    module->ctx->make_const(I32::get(), static_cast<int>(pointer_step));
  next_pointer->operands = {pointer_phi->result, step_value};
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
    {loop.get_preheader(), initial_pointer->result},
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
  return true;
}

} // namespace exodus::mid_ir::opt
