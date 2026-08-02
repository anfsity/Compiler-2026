#include "loop_unroll.hpp"

#include <algorithm>
#include <unordered_set>
#include <variant>

namespace exodus::mid_ir::opt {

auto LoopUnroll::run(
  LinearFunction &func, exodus::opt::LinearFunctionAnalysisManager &am
) -> exodus::opt::PreservedAnalysis {
  if (!module || !module->ctx || func.blocks.empty())
    return exodus::opt::PreservedAnalysis::all();

  CFGEditor cfg(*module, func);
  cfg.synchronize();
  auto &loops = am.get_result<LoopAnalysis>(func);
  auto &affine = am.get_result<AffineLoopAnalysis>(func);
  build_op_block_map(func);

  // Removing a loop invalidates LoopInfo pointers for its parents and siblings.
  // Transform at most one loop per pass invocation; the surrounding fixed point
  // rebuilds all analyses before considering the next candidate.
  for (auto *loop : loops.get_loops_innermost_first()) {
    auto counted = affine.match_counted_loop(*loop);
    if (!counted)
      continue;
    auto trip_count = affine.exact_trip_count(*counted);
    if (!trip_count || *trip_count > max_trip_count)
      continue;
    if (try_unroll(cfg, func, *loop, *counted, *trip_count)) {
      op_blocks.clear();
      return exodus::opt::PreservedAnalysis::none();
    }
  }

  op_blocks.clear();
  return exodus::opt::PreservedAnalysis::all();
}

auto LoopUnroll::build_op_block_map(LinearFunction &func) -> void {
  op_blocks.clear();
  for (auto &block : func.blocks) {
    for (auto *op : block->insts)
      op_blocks[op] = block.get();
  }
}

auto LoopUnroll::validate_shape(
  LinearFunction &func,
  const Loop &loop,
  const CountedLoopInfo &counted,
  uint64_t trip_count,
  std::vector<Op *> &phis,
  std::vector<Op *> &body
) const -> bool {
  if (
    loop.get_blocks().size() != 2 || counted.continuation != counted.latch ||
    counted.header == counted.latch || counted.preheader->insts.empty() ||
    counted.header->insts.empty() || counted.latch->insts.empty() ||
    (!counted.exit->insts.empty() &&
     counted.exit->insts.front()->code == OpCode::Phi)
  ) {
    return false;
  }

  auto *preheader_terminator = counted.preheader->insts.back();
  auto *header_terminator = counted.header->insts.back();
  auto *latch_terminator = counted.latch->insts.back();
  if (
    preheader_terminator->code != OpCode::Jump ||
    preheader_terminator->successors.size() != 1 ||
    preheader_terminator->successors.front() != counted.header ||
    header_terminator->code != OpCode::Branch ||
    latch_terminator->code != OpCode::Jump ||
    latch_terminator->successors.size() != 1 ||
    latch_terminator->successors.front() != counted.header
  ) {
    return false;
  }

  bool saw_non_phi = false;
  for (auto *op : counted.header->insts) {
    if (op->code == OpCode::Phi && !saw_non_phi) {
      if (!op->result)
        return false;
      phis.push_back(op);
      continue;
    }
    saw_non_phi = true;
    if (op != counted.compare && op != header_terminator)
      return false;
  }
  if (
    phis.empty() || !counted.compare ||
    std::find(
      counted.header->insts.begin(),
      counted.header->insts.end(),
      counted.compare
    ) == counted.header->insts.end()
  ) {
    return false;
  }

  for (auto *phi : phis) {
    size_t preheader_incoming = 0;
    size_t latch_incoming = 0;
    for (const auto &[pred, value] :
         std::get<PhiPayload>(phi->payload).incoming) {
      if (!value)
        return false;
      preheader_incoming += pred == counted.preheader;
      latch_incoming += pred == counted.latch;
    }
    if (
      preheader_incoming != 1 || latch_incoming != 1 ||
      std::get<PhiPayload>(phi->payload).incoming.size() != 2
    ) {
      return false;
    }
  }

  for (auto *op : counted.latch->insts) {
    if (op == latch_terminator)
      continue;
    switch (op->code) {
    case OpCode::Phi:
    case OpCode::Alloca:
    case OpCode::Ret:
    case OpCode::Jump:
    case OpCode::Branch:
    case OpCode::Memset:
      return false;
    case OpCode::Call:
      // An exact single iteration moves the call without duplicating or
      // speculating it: the canonical header is proven true once and false on
      // the backedge. Multi-iteration unrolling still rejects calls to avoid
      // code growth and changed call scheduling.
      if (trip_count != 1)
        return false;
      break;
    default:
      break;
    }
    if (
      !std::holds_alternative<EmptyPayload>(op->payload) &&
      !(trip_count == 1 && op->code == OpCode::Call &&
        std::holds_alternative<CallPayload>(op->payload)) &&
      !(op->code == OpCode::GetPtr &&
        std::holds_alternative<GetPtrPayload>(op->payload))
    )
      return false;
    body.push_back(op);
  }

  if (trip_count > 1) {
    auto cloned_ops = body.size() * static_cast<size_t>(trip_count);
    auto growth_ops = body.size() * static_cast<size_t>(trip_count - 1);
    if (cloned_ops > max_cloned_ops || growth_ops > max_growth_ops)
      return false;
  }

  std::unordered_set<Value *> loop_results;
  std::unordered_set<Value *> phi_results;
  for (auto *block : loop.get_blocks()) {
    for (auto *op : block->insts) {
      if (op->result)
        loop_results.insert(op->result);
    }
  }
  for (auto *phi : phis)
    phi_results.insert(phi->result);

  // Only loop-carried header values have a well-defined value after all
  // iterations in this first implementation.  Reject direct live-outs from a
  // body instruction instead of guessing that the last iteration dominates.
  for (auto &block : func.blocks) {
    if (loop.contains(block.get()))
      continue;
    for (auto *op : block->insts) {
      for (auto *operand : op->operands) {
        if (loop_results.count(operand) && !phi_results.count(operand))
          return false;
      }
      if (op->code != OpCode::Phi)
        continue;
      for (const auto &[pred, value] :
           std::get<PhiPayload>(op->payload).incoming) {
        (void)pred;
        if (loop_results.count(value) && !phi_results.count(value))
          return false;
      }
    }
  }
  return true;
}

auto LoopUnroll::clone_body(
  Block *destination,
  const std::vector<Op *> &body,
  const std::vector<Op *> &phis,
  const CountedLoopInfo &counted,
  uint64_t trip_count,
  std::unordered_map<Value *, Value *> &final_values
) -> bool {
  for (auto *phi : phis) {
    Value *initial = nullptr;
    for (const auto &[pred, value] :
         std::get<PhiPayload>(phi->payload).incoming) {
      if (pred == counted.preheader)
        initial = value;
    }
    if (!initial)
      return false;
    final_values[phi->result] = initial;
  }

  std::vector<Op *> clones;
  for (uint64_t iteration = 0; iteration < trip_count; ++iteration) {
    std::unordered_map<Value *, Value *> iteration_values = final_values;
    for (auto *source : body) {
      auto *clone = module->make_op(source->code, source->payload);
      clone->operands.reserve(source->operands.size());
      for (auto *operand : source->operands) {
        auto replacement = iteration_values.find(operand);
        if (replacement != iteration_values.end()) {
          clone->operands.push_back(replacement->second);
          continue;
        }
        if (operand && operand->kind == ValueKind::OpResult) {
          auto *definition =
            static_cast<Op *>(static_cast<OpResult *>(operand)->creator);
          auto block = op_blocks.find(definition);
          if (
            block != op_blocks.end() &&
            (block->second == counted.header || block->second == counted.latch)
          ) {
            return false;
          }
        }
        clone->operands.push_back(operand);
      }
      if (source->result) {
        clone->result =
          module->ctx->make_value<OpResult>(source->result->type, clone);
        iteration_values[source->result] = clone->result;
      }
      clones.push_back(clone);
    }

    std::unordered_map<Value *, Value *> next_values;
    for (auto *phi : phis) {
      Value *backedge = nullptr;
      for (const auto &[pred, value] :
           std::get<PhiPayload>(phi->payload).incoming) {
        if (pred == counted.latch)
          backedge = value;
      }
      auto replacement = iteration_values.find(backedge);
      next_values[phi->result] =
        replacement == iteration_values.end() ? backedge : replacement->second;
      if (!next_values[phi->result])
        return false;
    }
    final_values = std::move(next_values);
  }

  auto insertion_point = std::prev(destination->insts.end());
  for (auto *clone : clones) {
    for (auto *operand : clone->operands) {
      if (operand)
        operand->addUse(clone);
    }
    destination->insts.insert(insertion_point, clone);
  }
  return true;
}

auto LoopUnroll::remove_loop_blocks(
  CFGEditor &cfg, const CountedLoopInfo &counted
) -> bool {
  if (!cfg.set_successors(counted.preheader, {counted.exit}))
    return false;

  std::unordered_set<Block *> removed_blocks{counted.header, counted.latch};
  std::unordered_set<Op *> removed_ops;
  std::unordered_set<Value *> removed_results;
  for (auto *block : removed_blocks) {
    for (auto *op : block->insts) {
      removed_ops.insert(op);
      if (op->result)
        removed_results.insert(op->result);
    }
  }

  for (const auto &block : cfg.function().blocks) {
    if (removed_blocks.count(block.get()))
      continue;
    for (auto *op : block->insts) {
      for (auto *operand : op->operands) {
        if (removed_results.count(operand))
          return false;
      }
      if (op->code != OpCode::Phi)
        continue;
      for (const auto &[_, value] :
           std::get<PhiPayload>(op->payload).incoming) {
        (void)_;
        if (removed_results.count(value))
          return false;
      }
    }
  }

  for (auto *op : removed_ops) {
    for (auto *operand : op->operands) {
      if (operand)
        operand->rmUse(op);
    }
    if (op->code == OpCode::Phi) {
      for (auto &[_, value] : std::get<PhiPayload>(op->payload).incoming) {
        (void)_;
        if (value)
          value->rmUse(op);
      }
    }
    if (op->result)
      op->result->users.clear();
    op->operands.clear();
    op->successors.clear();
  }

  for (auto it = cfg.function().blocks.begin();
       it != cfg.function().blocks.end();) {
    if (removed_blocks.count(it->get()))
      it = cfg.function().blocks.erase(it);
    else
      ++it;
  }
  cfg.synchronize();
  return true;
}

auto LoopUnroll::try_unroll(
  CFGEditor &cfg,
  LinearFunction &func,
  const Loop &loop,
  const CountedLoopInfo &counted,
  uint64_t trip_count
) -> bool {
  std::vector<Op *> phis;
  std::vector<Op *> body;
  if (!validate_shape(func, loop, counted, trip_count, phis, body))
    return false;

  auto tx = cfg.begin_transaction();
  std::unordered_map<Value *, Value *> final_values;
  if (!clone_body(
        counted.preheader, body, phis, counted, trip_count, final_values
      )) {
    return false;
  }

  MidIRRewriter rewriter;
  rewriter.set_scope(func);
  for (auto *phi : phis) {
    auto replacement = final_values.find(phi->result);
    if (replacement == final_values.end() || !replacement->second)
      return false;
    rewriter.replace_all_uses_with(phi->result, replacement->second);
    rewriter.eraseOp(phi);
  }
  rewriter.finalize(func);

  if (!remove_loop_blocks(cfg, counted))
    return false;
  cfg.synchronize();
  return tx.commit();
}

} // namespace exodus::mid_ir::opt
