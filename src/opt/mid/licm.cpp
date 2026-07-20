#include "licm.hpp"

#include "../../base/getptr.hpp"
#include <algorithm>

namespace exodus::mid_ir::opt {

auto LICM::run(
  LinearFunction &func, exodus::opt::LinearFunctionAnalysisManager &am
) -> exodus::opt::PreservedAnalysis {
  if (func.blocks.empty())
    return exodus::opt::PreservedAnalysis::all();

  auto &loop_info = am.get_result<LoopAnalysis>(func);
  auto &dom_result = am.get_result<DominanceAnalysis>(func);

  dom = &dom_result;
  changed = false;
  build_op_block_map(func);

  for (auto *loop : loop_info.get_loops_innermost_first())
    process_loop(func, *loop);

  dom = nullptr;
  invariant_ops.clear();
  op_blocks.clear();

  if (!changed)
    return exodus::opt::PreservedAnalysis::all();

  exodus::opt::PreservedAnalysis preserved;
  preserved.preserve<DominanceAnalysis>();
  preserved.preserve<LoopAnalysis>();
  return preserved;
}

auto LICM::build_op_block_map(LinearFunction &func) -> void {
  op_blocks.clear();
  for (auto &block : func.blocks) {
    for (auto *op : block->insts)
      op_blocks[op] = block.get();
  }
}

auto LICM::process_loop(LinearFunction &func, Loop &loop) -> void {
  if (!loop.get_preheader())
    return;

  invariant_ops.clear();
  mark_and_hoist(func, loop);
  sink_stores(func, loop);
}

auto LICM::mark_and_hoist(LinearFunction &func, Loop &loop) -> void {
  std::vector<Op *> candidates;
  std::vector<Op *> hoist_order;
  std::unordered_map<Op *, size_t> remaining_dependencies;
  std::unordered_map<Op *, std::vector<Op *>> dependents;

  for (auto &block : func.blocks) {
    if (!loop.contains(block.get()))
      continue;
    for (auto *op : block->insts) {
      if (op->result && op->code != OpCode::Phi && op->code != OpCode::Alloca)
        candidates.push_back(op);
    }
  }

  for (auto *op : candidates) {
    std::unordered_set<Op *> dependencies;
    for (auto *operand : op->operands) {
      if (!operand || operand->kind != ValueKind::OpResult)
        continue;
      auto *creator =
        static_cast<Op *>(static_cast<OpResult *>(operand)->creator);
      auto block_it = op_blocks.find(creator);
      if (
        creator && block_it != op_blocks.end() &&
        loop.contains(block_it->second)
      ) {
        dependencies.insert(creator);
      }
    }

    remaining_dependencies[op] = dependencies.size();
    for (auto *dependency : dependencies)
      dependents[dependency].push_back(op);
  }

  std::deque<Op *> worklist;
  for (auto *op : candidates) {
    if (remaining_dependencies[op] == 0)
      worklist.push_back(op);
  }

  while (!worklist.empty()) {
    Op *op = worklist.front();
    worklist.pop_front();

    auto block_it = op_blocks.find(op);
    if (block_it == op_blocks.end() || !loop.contains(block_it->second))
      continue;
    Block *block = block_it->second;

    if (!operands_are_invariant(*op, loop) || !can_hoist(op, block, loop))
      continue;

    invariant_ops.insert(op);
    hoist_order.push_back(op);

    for (auto *dependent : dependents[op]) {
      auto it = remaining_dependencies.find(dependent);
      if (it == remaining_dependencies.end() || it->second == 0)
        continue;
      --it->second;
      if (it->second == 0)
        worklist.push_back(dependent);
    }
  }

  if (!hoist_set_fits_register_budget(loop)) {
    invariant_ops.clear();
    return;
  }

  for (auto *op : hoist_order) {
    auto block_it = op_blocks.find(op);
    if (block_it != op_blocks.end() && loop.contains(block_it->second))
      move_to_preheader(op, block_it->second, loop.get_preheader());
  }
}

auto LICM::can_hoist(Op *op, Block *block, const Loop &loop) const -> bool {
  if (!op->result)
    return false;

  switch (op->code) {
  case OpCode::Add:
  case OpCode::Sub:
  case OpCode::Mul:
  case OpCode::FAdd:
  case OpCode::FSub:
  case OpCode::FMul:
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
    return true;
  case OpCode::Div:
  case OpCode::Mod:
  case OpCode::FDiv:
    return block_dominates_all_exits(block, loop);
  case OpCode::Load: {
    auto location = alias_analysis.get_location(*op);
    return location &&
           (alias_analysis.is_dereferenceable(*location) ||
            block_dominates_all_exits(block, loop)) &&
           !has_aliasing_write(loop, *location);
  }
  case OpCode::GetPtr: {
    auto read_location = get_getptr_read_location(*op);
    if (!read_location) {
      if (!has_non_call_loop_user(op->result, loop))
        return false;
      return is_safe_to_speculate(*op);
    }
    return (alias_analysis.is_dereferenceable(*read_location) ||
            block_dominates_all_exits(block, loop)) &&
           !has_aliasing_write(loop, *read_location);
  }
  default:
    return false;
  }
}

auto LICM::operands_are_invariant(const Op &op, const Loop &loop) const
  -> bool {
  return std::all_of(
    op.operands.begin(), op.operands.end(), [this, &loop](Value *operand) {
      return value_is_invariant(operand, loop);
    }
  );
}

auto LICM::value_is_invariant(Value *value, const Loop &loop) const -> bool {
  if (!value || value->kind != ValueKind::OpResult)
    return true;

  auto *creator = static_cast<Op *>(static_cast<OpResult *>(value)->creator);
  if (!creator)
    return true;
  auto it = op_blocks.find(creator);
  if (it == op_blocks.end() || !loop.contains(it->second))
    return true;
  return invariant_ops.count(creator) != 0;
}

auto LICM::block_dominates_all_exits(Block *block, const Loop &loop) const
  -> bool {
  bool has_exit = false;
  for (auto *exiting : loop.get_exiting_blocks()) {
    has_exit = true;
    if (!dom->dominate(block, exiting))
      return false;
  }
  for (auto *return_block : loop.get_return_blocks()) {
    has_exit = true;
    if (!dom->dominate(block, return_block))
      return false;
  }
  return has_exit;
}

auto LICM::value_dominates_block(Value *value, Block *block) const -> bool {
  if (!value || value->kind != ValueKind::OpResult)
    return true;
  auto *creator = static_cast<Op *>(static_cast<OpResult *>(value)->creator);
  auto it = op_blocks.find(creator);
  if (it == op_blocks.end())
    return true;
  return dom->dominate(it->second, block);
}

auto LICM::has_aliasing_write(
  const Loop &loop, const MemoryLocation &location
) const -> bool {
  for (auto *block : loop.get_blocks()) {
    for (auto *op : block->insts) {
      if (op->code == OpCode::Call)
        return true;
      if (op->code != OpCode::Store && op->code != OpCode::Memset)
        continue;
      auto write_location = alias_analysis.get_location(*op);
      if (
        !write_location || alias_analysis.may_alias(location, *write_location)
      )
        return true;
    }
  }
  return false;
}

auto LICM::has_interfering_access(
  const Loop &loop, const Op &store, const MemoryLocation &location
) const -> bool {
  for (auto *block : loop.get_blocks()) {
    for (auto *op : block->insts) {
      if (op == &store)
        continue;
      if (op->code == OpCode::Call)
        return true;

      std::optional<MemoryLocation> access;
      if (
        op->code == OpCode::Load || op->code == OpCode::Store ||
        op->code == OpCode::Memset
      ) {
        access = alias_analysis.get_location(*op);
      } else if (op->code == OpCode::GetPtr) {
        access = get_getptr_read_location(*op);
      } else {
        continue;
      }

      if (!access || alias_analysis.may_alias(location, *access))
        return true;
    }
  }
  return false;
}

auto LICM::exits_are_dedicated(const Loop &loop) const -> bool {
  if (loop.get_exit_blocks().empty() || !loop.get_return_blocks().empty())
    return false;
  for (auto *exit : loop.get_exit_blocks()) {
    if (exit->preds.empty())
      return false;
    for (auto *pred : exit->preds) {
      if (!loop.contains(pred))
        return false;
    }
  }
  return true;
}

auto LICM::hoist_set_fits_register_budget(const Loop &loop) const -> bool {
  size_t integer_boundary = 0;
  size_t float_boundary = 0;
  for (auto *op : invariant_ops) {
    if (!op->result || !has_loop_user_outside_hoist_set(op->result, loop)) {
      continue;
    }
    if (op->result->type->is_f32())
      ++float_boundary;
    else
      ++integer_boundary;
  }

  bool has_call = loop_contains_call(loop);
  // Keep two registers free for instructions introduced while lowering the
  // loop body. The allocator exposes 5/11 integer and 10/12 float registers.
  size_t integer_budget = has_call ? 9u : 3u;
  size_t float_budget = has_call ? 10u : 8u;
  return live_in_pressure(loop, false) + integer_boundary <= integer_budget &&
         live_in_pressure(loop, true) + float_boundary <= float_budget;
}

auto LICM::loop_contains_call(const Loop &loop) const -> bool {
  for (auto *block : loop.get_blocks()) {
    for (auto *op : block->insts) {
      if (op->code == OpCode::Call)
        return true;
    }
  }
  return false;
}

auto LICM::live_in_pressure(const Loop &loop, bool floating) const -> size_t {
  std::unordered_set<Value *> live_ins;

  for (auto *phi : loop.get_header()->insts) {
    if (phi->code != OpCode::Phi)
      break;
    if (phi->result && phi->result->type->is_f32() == floating)
      live_ins.insert(phi->result);
  }

  for (auto *block : loop.get_blocks()) {
    for (auto *op : block->insts) {
      if (op->code == OpCode::Phi)
        continue;
      for (auto *operand : op->operands) {
        if (!operand || operand->type->is_f32() != floating)
          continue;
        if (operand->kind == ValueKind::Argument) {
          live_ins.insert(operand);
          continue;
        }
        if (operand->kind != ValueKind::OpResult)
          continue;
        auto *creator =
          static_cast<Op *>(static_cast<OpResult *>(operand)->creator);
        auto it = op_blocks.find(creator);
        if (it == op_blocks.end() || !loop.contains(it->second))
          live_ins.insert(operand);
      }
    }
  }

  return live_ins.size();
}

auto LICM::has_non_call_loop_user(Value *value, const Loop &loop) const
  -> bool {
  for (auto *block : loop.get_blocks()) {
    for (auto *op : block->insts) {
      if (
        op->code != OpCode::Call &&
        std::find(op->operands.begin(), op->operands.end(), value) !=
          op->operands.end()
      ) {
        return true;
      }
      if (op->code != OpCode::Phi)
        continue;
      const auto &incoming = std::get<PhiPayload>(op->payload).incoming;
      if (
        std::any_of(incoming.begin(), incoming.end(), [value](const auto &in) {
          return in.second == value;
        })
      )
        return true;
    }
  }
  return false;
}

auto LICM::has_loop_user_outside_hoist_set(Value *value, const Loop &loop) const
  -> bool {
  for (auto *block : loop.get_blocks()) {
    for (auto *op : block->insts) {
      bool uses_value =
        std::find(op->operands.begin(), op->operands.end(), value) !=
        op->operands.end();
      if (op->code == OpCode::Phi) {
        const auto &incoming = std::get<PhiPayload>(op->payload).incoming;
        uses_value |= std::any_of(
          incoming.begin(), incoming.end(), [value](const auto &in) {
            return in.second == value;
          }
        );
      }
      if (uses_value && !invariant_ops.count(op))
        return true;
    }
  }
  return false;
}

auto LICM::get_getptr_read_location(const Op &op) const
  -> std::optional<MemoryLocation> {
  if (
    op.code != OpCode::GetPtr || op.operands.empty() || !op.result ||
    !op.operands[0]->type->is_ptr() || !op.result->type->is_ptr()
  ) {
    return std::nullopt;
  }
  auto plan = ir::analyze_getptr(
    op.operands[0]->type, op.result->type, op.operands.size() - 1
  );
  if (!plan.reads_memory)
    return std::nullopt;
  return alias_analysis.get_location(op.operands[0], pointer_storage_size);
}

auto LICM::move_to_preheader(Op *op, Block *from, Block *preheader) -> void {
  auto from_it = std::find(from->insts.begin(), from->insts.end(), op);
  if (from_it == from->insts.end() || preheader->insts.empty())
    return;

  auto insert_pos = std::prev(preheader->insts.end());
  preheader->insts.splice(insert_pos, from->insts, from_it);
  op_blocks[op] = preheader;
  changed = true;
}

auto LICM::clone_store_to_exit(const Op &store, Block *exit) -> void {
  auto *clone = module->make_op(OpCode::Store);
  clone->operands = store.operands;
  for (auto *operand : clone->operands)
    operand->addUse(clone);

  auto insert_pos = exit->insts.begin();
  while (insert_pos != exit->insts.end() && (*insert_pos)->code == OpCode::Phi)
    ++insert_pos;
  exit->insts.insert(insert_pos, clone);
  op_blocks[clone] = exit;
}

auto LICM::sink_stores(LinearFunction &func, Loop &loop) -> void {
  if (!exits_are_dedicated(loop))
    return;

  std::vector<std::pair<Op *, Block *>> stores;
  for (auto &block : func.blocks) {
    if (!loop.contains(block.get()))
      continue;
    for (auto *op : block->insts) {
      if (op->code == OpCode::Store)
        stores.push_back({op, block.get()});
    }
  }

  MidIRRewriter rewriter;
  rewriter.set_scope(func);
  for (const auto &[store, block] : stores) {
    if (store->operands.size() < 2)
      continue;
    Value *value = store->operands[0];
    Value *address = store->operands[1];
    auto location = alias_analysis.get_location(*store);
    if (
      !location || !value_is_invariant(address, loop) ||
      !block_dominates_all_exits(block, loop) ||
      has_interfering_access(loop, *store, *location)
    ) {
      continue;
    }

    bool available_at_exits = std::all_of(
      loop.get_exit_blocks().begin(),
      loop.get_exit_blocks().end(),
      [this, value, address](Block *exit) {
        return value_dominates_block(value, exit) &&
               value_dominates_block(address, exit);
      }
    );
    if (!available_at_exits)
      continue;

    for (auto *exit : loop.get_exit_blocks())
      clone_store_to_exit(*store, exit);
    rewriter.eraseOp(store);
    op_blocks.erase(store);
    changed = true;
  }
  rewriter.finalize(func);
}

auto LICM::is_safe_to_speculate(const Op &op) -> bool {
  switch (op.code) {
  case OpCode::Add:
  case OpCode::Sub:
  case OpCode::Mul:
  case OpCode::FAdd:
  case OpCode::FSub:
  case OpCode::FMul:
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
    return true;
  case OpCode::GetPtr: {
    if (
      op.operands.empty() || !op.result || !op.operands[0]->type->is_ptr() ||
      !op.result->type->is_ptr()
    ) {
      return false;
    }
    return !ir::analyze_getptr(
              op.operands[0]->type, op.result->type, op.operands.size() - 1
    )
              .reads_memory;
  }
  default:
    return false;
  }
}

} // namespace exodus::mid_ir::opt
