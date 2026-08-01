#include "monotonic_guard_tighten.hpp"

#include <algorithm>

namespace exodus::mid_ir::opt {
namespace {

auto jump_target(Block *block) -> Block * {
  if (!block || block->insts.empty())
    return nullptr;
  auto *jump = block->insts.back();
  if (jump->code != OpCode::Jump || jump->successors.size() != 1)
    return nullptr;
  return jump->successors.front();
}

auto unique_predecessors_are(
  Block *block, Block *first, Block *second = nullptr
) -> bool {
  if (!block)
    return false;
  std::vector<Block *> unique;
  for (auto *pred : block->preds) {
    if (std::find(unique.begin(), unique.end(), pred) == unique.end())
      unique.push_back(pred);
  }
  if (!second)
    return unique.size() == 1 && unique.front() == first;
  return unique.size() == 2 &&
         std::find(unique.begin(), unique.end(), first) != unique.end() &&
         std::find(unique.begin(), unique.end(), second) != unique.end();
}

auto i32_constant_is(Value *value, int expected) -> bool {
  auto constant = AffineLoopInfo::integer_constant(value);
  return constant && *constant == expected;
}

auto reset_operands(Op *op, std::vector<Value *> operands) -> void {
  for (auto *operand : op->operands) {
    if (operand)
      operand->rmUse(op);
  }
  op->operands = std::move(operands);
  for (auto *operand : op->operands) {
    if (operand)
      operand->addUse(op);
  }
}

} // namespace

auto MonotonicGuardTighten::run(
  LinearFunction &func, exodus::opt::LinearFunctionAnalysisManager &am
) -> exodus::opt::PreservedAnalysis {
  if (func.blocks.empty())
    return exodus::opt::PreservedAnalysis::all();

  CFGEditor cfg(*module, func);
  cfg.synchronize();
  build_op_block_map(func);
  auto &loops = am.get_result<LoopAnalysis>(func);
  for (auto *loop : loops.get_loops_innermost_first()) {
    if (try_tighten(cfg, func, *loop))
      return exodus::opt::PreservedAnalysis::none();
  }
  return exodus::opt::PreservedAnalysis::all();
}

auto MonotonicGuardTighten::build_op_block_map(LinearFunction &func) -> void {
  op_blocks.clear();
  for (auto &block : func.blocks) {
    for (auto *op : block->insts)
      op_blocks[op] = block.get();
  }
}

auto MonotonicGuardTighten::is_loop_invariant(
  Value *value, const Loop &loop
) const -> bool {
  if (!value)
    return false;
  if (value->kind != ValueKind::OpResult)
    return true;
  auto *definition = static_cast<Op *>(static_cast<OpResult *>(value)->creator);
  auto it = op_blocks.find(definition);
  return definition && it != op_blocks.end() && !loop.contains(it->second);
}

auto MonotonicGuardTighten::is_increment_by_one(Op *op, Value *induction) const
  -> bool {
  if (
    !op || op->code != OpCode::Add || !op->result || op->operands.size() != 2
  ) {
    return false;
  }
  return (op->operands[0] == induction &&
          i32_constant_is(op->operands[1], 1)) ||
         (op->operands[1] == induction && i32_constant_is(op->operands[0], 1));
}

auto MonotonicGuardTighten::has_live_out_use(
  const LinearFunction &func, Value *value, const Loop &loop
) const -> bool {
  if (!value)
    return false;

  for (const auto &block : func.blocks) {
    if (loop.contains(block.get()))
      continue;

    for (auto *op : block->insts) {
      if (
        std::find(op->operands.begin(), op->operands.end(), value) !=
        op->operands.end()
      ) {
        return true;
      }
      if (op->code != OpCode::Phi)
        continue;

      const auto &incoming = std::get<PhiPayload>(op->payload).incoming;
      if (
        std::any_of(
          incoming.begin(), incoming.end(), [value](const auto &entry) {
            return entry.second == value;
          }
        )
      ) {
        return true;
      }
    }
  }
  return false;
}

auto MonotonicGuardTighten::has_only_current_user(
  const LinearFunction &func, Value *value, Op *expected
) const -> bool {
  if (!value || !expected)
    return false;
  size_t uses = 0;
  for (const auto &block : func.blocks) {
    for (auto *op : block->insts) {
      size_t op_uses = static_cast<size_t>(
        std::count(op->operands.begin(), op->operands.end(), value)
      );
      if (op->code == OpCode::Phi) {
        const auto &incoming = std::get<PhiPayload>(op->payload).incoming;
        op_uses += static_cast<size_t>(std::count_if(
          incoming.begin(), incoming.end(), [value](const auto &entry) {
            return entry.second == value;
          }
        ));
      }
      if (op != expected && op_uses != 0)
        return false;
      uses += op_uses;
    }
  }
  if (uses != 1)
    return false;

  const auto &incoming = std::get<PhiPayload>(expected->payload).incoming;
  return std::count_if(
           incoming.begin(), incoming.end(), [value](const auto &entry) {
             return entry.second == value;
           }
         ) == 1;
}

auto MonotonicGuardTighten::try_tighten(
  CFGEditor &cfg, LinearFunction &func, const Loop &loop
) -> bool {
  if (
    !loop.get_preheader() || loop.get_back_edges().size() != 1 ||
    loop.get_exiting_blocks().size() != 1 || loop.get_exit_blocks().size() != 1
  ) {
    return false;
  }

  auto *header = loop.get_header();
  auto *latch = loop.get_back_edges().front().first;
  if (
    !header || !latch || loop.get_exiting_blocks().front() != header ||
    header->insts.size() != 3 || latch->insts.size() != 2 ||
    jump_target(latch) != header
  ) {
    return false;
  }

  auto *induction_phi = header->insts.front();
  auto *header_compare = *std::next(header->insts.begin());
  auto *header_branch = header->insts.back();
  if (
    induction_phi->code != OpCode::Phi || !induction_phi->result ||
    !induction_phi->result->type->is_i32() ||
    header_compare->code != OpCode::Lt || !header_compare->result ||
    header_compare->operands.size() != 2 ||
    header_compare->operands[0] != induction_phi->result ||
    !is_loop_invariant(header_compare->operands[1], loop) ||
    header_branch->code != OpCode::Branch ||
    header_branch->operands.size() != 1 ||
    header_branch->operands[0] != header_compare->result ||
    header_branch->successors.size() != 2 ||
    header_branch->successors[1] != loop.get_exit_blocks().front()
  ) {
    return false;
  }

  auto *guard = header_branch->successors[0];
  if (
    !guard || !loop.contains(guard) || guard->insts.size() != 2 ||
    !unique_predecessors_are(guard, header)
  ) {
    return false;
  }
  auto *guard_compare = guard->insts.front();
  auto *guard_branch = guard->insts.back();
  if (
    !guard_compare->result || guard_compare->operands.size() != 2 ||
    guard_branch->code != OpCode::Branch ||
    guard_branch->operands.size() != 1 ||
    guard_branch->operands[0] != guard_compare->result ||
    guard_branch->successors.size() != 2
  ) {
    return false;
  }

  Value *threshold = nullptr;
  bool true_is_skip = true;
  if (
    guard_compare->code == OpCode::Lt &&
    guard_compare->operands[1] == induction_phi->result
  ) {
    threshold = guard_compare->operands[0];
  } else if (
    guard_compare->code == OpCode::Gt &&
    guard_compare->operands[0] == induction_phi->result
  ) {
    threshold = guard_compare->operands[1];
  } else if (
    guard_compare->code == OpCode::Le &&
    guard_compare->operands[0] == induction_phi->result
  ) {
    threshold = guard_compare->operands[1];
    true_is_skip = false;
  } else if (
    guard_compare->code == OpCode::Ge &&
    guard_compare->operands[1] == induction_phi->result
  ) {
    threshold = guard_compare->operands[0];
    true_is_skip = false;
  } else {
    return false;
  }
  if (
    !threshold || !threshold->type || !threshold->type->is_i32() ||
    !is_loop_invariant(threshold, loop)
  ) {
    return false;
  }

  auto *skip = guard_branch->successors[true_is_skip ? 0 : 1];
  auto *active = guard_branch->successors[true_is_skip ? 1 : 0];
  if (
    !skip || !active || skip == active || !loop.contains(skip) ||
    !loop.contains(active) || skip->insts.size() != 2 ||
    active->insts.size() < 2 || jump_target(skip) != latch ||
    jump_target(active) != latch || !unique_predecessors_are(skip, guard) ||
    !unique_predecessors_are(active, guard) ||
    !unique_predecessors_are(latch, skip, active)
  ) {
    return false;
  }

  auto *skip_update = skip->insts.front();
  auto *active_update = *std::prev(active->insts.end(), 2);
  auto *latch_phi = latch->insts.front();
  if (
    !is_increment_by_one(skip_update, induction_phi->result) ||
    !is_increment_by_one(active_update, induction_phi->result) ||
    latch_phi->code != OpCode::Phi || !latch_phi->result ||
    latch_phi->result->type != induction_phi->result->type
  ) {
    return false;
  }

  const auto &latch_incoming =
    std::get<PhiPayload>(latch_phi->payload).incoming;
  if (latch_incoming.size() != 2)
    return false;
  auto incoming_matches = [&](Block *block, Value *value) {
    return std::count_if(
             latch_incoming.begin(),
             latch_incoming.end(),
             [block, value](const auto &entry) {
               return entry.first == block && entry.second == value;
             }
           ) == 1;
  };
  if (
    !incoming_matches(skip, skip_update->result) ||
    !incoming_matches(active, active_update->result) ||
    !has_only_current_user(func, skip_update->result, latch_phi) ||
    !has_only_current_user(func, active_update->result, latch_phi)
  ) {
    return false;
  }

  const auto &induction_incoming =
    std::get<PhiPayload>(induction_phi->payload).incoming;
  if (induction_incoming.size() != 2)
    return false;
  auto latch_value = std::find_if(
    induction_incoming.begin(),
    induction_incoming.end(),
    [latch](const auto &entry) { return entry.first == latch; }
  );
  auto initial_value = std::find_if(
    induction_incoming.begin(),
    induction_incoming.end(),
    [preheader = loop.get_preheader()](const auto &entry) {
      return entry.first == preheader;
    }
  );
  if (
    latch_value == induction_incoming.end() ||
    initial_value == induction_incoming.end() ||
    latch_value->second != latch_phi->result
  ) {
    return false;
  }
  if (has_live_out_use(func, induction_phi->result, loop))
    return false;

  auto tx = cfg.begin_transaction();

  // j < bound implies j <= INT_MAX - 1, so both original j + 1 updates are
  // non-wrapping.  Once invariant threshold < j becomes true, incrementing j
  // cannot make it false before the original signed bound exits the loop.
  auto *within_threshold = module->make_op(OpCode::Le);
  within_threshold->operands = {induction_phi->result, threshold};
  for (auto *operand : within_threshold->operands)
    operand->addUse(within_threshold);
  within_threshold->result = module->ctx->make_value<OpResult>(
    header_compare->result->type, within_threshold
  );

  auto *tightened_condition = module->make_op(OpCode::And);
  tightened_condition->operands = {
    header_compare->result, within_threshold->result
  };
  for (auto *operand : tightened_condition->operands)
    operand->addUse(tightened_condition);
  tightened_condition->result = module->ctx->make_value<OpResult>(
    header_compare->result->type, tightened_condition
  );

  auto branch_position = std::prev(header->insts.end());
  header->insts.insert(branch_position, within_threshold);
  header->insts.insert(branch_position, tightened_condition);
  reset_operands(header_branch, {tightened_condition->result});

  auto *guard_jump = module->make_op(OpCode::Jump);
  guard_jump->successors = {active};
  if (!cfg.set_terminator(guard, guard_jump))
    return false;

  rewriter.set_scope(func);
  rewriter.replace_all_uses_with(latch_phi->result, active_update->result);
  rewriter.eraseOp(latch_phi);

  auto active_update_position =
    std::find(active->insts.begin(), active->insts.end(), active_update);
  auto latch_jump_position = std::prev(latch->insts.end());
  latch->insts.splice(
    latch_jump_position, active->insts, active_update_position
  );
  rewriter.finalize(func);
  cfg.synchronize();
  return tx.commit();
}

} // namespace exodus::mid_ir::opt
