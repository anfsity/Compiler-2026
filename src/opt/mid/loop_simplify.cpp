#include "loop_simplify.hpp"

#include <algorithm>

namespace exodus::mid_ir::opt {

auto LoopSimplify::run(
  LinearFunction &func, exodus::opt::LinearFunctionAnalysisManager &am
) -> exodus::opt::PreservedAnalysis {
  if (func.blocks.empty())
    return exodus::opt::PreservedAnalysis::all();

  auto &loop_info = am.get_result<LoopAnalysis>(func);
  bool changed = false;
  for (auto *loop : loop_info.get_loops_innermost_first()) {
    if (!loop->get_preheader())
      changed |= create_preheader(func, *loop);
  }

  if (!changed) {
    for (auto *loop : loop_info.get_loops_innermost_first()) {
      if (loop->get_back_edges().size() > 1) {
        changed = create_single_latch(func, *loop);
        break;
      }
    }
  }

  if (!changed)
    return exodus::opt::PreservedAnalysis::all();

  renumber_blocks(func);
  rebuild_cfg(func);
  return exodus::opt::PreservedAnalysis::none();
}

auto LoopSimplify::create_single_latch(LinearFunction &func, Loop &loop)
  -> bool {
  auto *header = loop.get_header();
  if (!header || loop.get_back_edges().size() < 2)
    return false;

  std::vector<Block *> backedge_blocks;
  for (const auto &[source, sink] : loop.get_back_edges()) {
    if (
      sink != header ||
      std::find(backedge_blocks.begin(), backedge_blocks.end(), source) !=
        backedge_blocks.end()
    ) {
      continue;
    }
    backedge_blocks.push_back(source);
  }
  if (backedge_blocks.size() < 2)
    return false;

  auto header_it = std::find_if(
    func.blocks.begin(),
    func.blocks.end(),
    [header](const std::unique_ptr<Block> &block) {
      return block.get() == header;
    }
  );
  if (header_it == func.blocks.end())
    return false;

  auto latch_ptr = std::make_unique<Block>(
    static_cast<int>(func.blocks.size()), header->name + "_latch"
  );
  auto *latch = latch_ptr.get();
  func.blocks.insert(header_it, std::move(latch_ptr));

  std::unordered_set<Block *> backedge_set(
    backedge_blocks.begin(), backedge_blocks.end()
  );
  for (auto *block : backedge_blocks) {
    if (block->insts.empty())
      return false;
    auto *terminator = block->insts.back();
    bool retargeted = false;
    for (auto *&successor : terminator->successors) {
      if (successor == header) {
        successor = latch;
        retargeted = true;
      }
    }
    if (!retargeted)
      return false;
  }

  for (auto *phi : header->insts) {
    if (phi->code != OpCode::Phi)
      break;
    const auto incoming = std::get<PhiPayload>(phi->payload).incoming;
    std::vector<std::pair<Block *, Value *>> backedge_incoming;
    std::vector<std::pair<Block *, Value *>> header_incoming;
    for (const auto &entry : incoming) {
      if (backedge_set.count(entry.first))
        backedge_incoming.push_back(entry);
      else
        header_incoming.push_back(entry);
    }
    if (backedge_incoming.size() != backedge_blocks.size())
      return false;

    Value *latch_value = backedge_incoming.front().second;
    const bool all_same = std::all_of(
      backedge_incoming.begin(),
      backedge_incoming.end(),
      [latch_value](const auto &entry) { return entry.second == latch_value; }
    );
    if (!all_same) {
      Op *representative = nullptr;
      bool identical_update = true;
      for (const auto &[block, value] : backedge_incoming) {
        (void)block;
        if (!value || value->kind != ValueKind::OpResult) {
          identical_update = false;
          break;
        }
        auto *candidate =
          static_cast<Op *>(static_cast<OpResult *>(value)->creator);
        if (
          !candidate || !candidate->result || candidate->operands.size() != 2 ||
          (candidate->code != OpCode::Add && candidate->code != OpCode::Sub)
        ) {
          identical_update = false;
          break;
        }
        if (!representative) {
          representative = candidate;
          continue;
        }
        if (
          candidate->code != representative->code ||
          candidate->operands != representative->operands
        ) {
          identical_update = false;
          break;
        }
      }

      if (identical_update) {
        auto *update = module->make_op(representative->code);
        update->operands = representative->operands;
        for (auto *operand : update->operands)
          operand->addUse(update);
        update->result = module->ctx->make_value<OpResult>(
          representative->result->type, update
        );
        latch->insts.push_back(update);
        latch_value = update->result;
      } else {
        auto *latch_phi = module->make_op(OpCode::Phi, PhiPayload{});
        latch_phi->result =
          module->ctx->make_value<OpResult>(phi->result->type, latch_phi);
        reset_phi_incoming(latch_phi, backedge_incoming);
        latch->insts.push_back(latch_phi);
        latch_value = latch_phi->result;
      }
    }
    header_incoming.push_back({latch, latch_value});
    reset_phi_incoming(phi, std::move(header_incoming));
  }

  auto *jump = module->make_op(OpCode::Jump);
  jump->successors.push_back(header);
  latch->insts.push_back(jump);
  return true;
}

auto LoopSimplify::create_preheader(LinearFunction &func, Loop &loop) -> bool {
  Block *header = loop.get_header();
  std::vector<Block *> outside_preds;
  for (auto *pred : header->preds) {
    if (
      !loop.contains(pred) &&
      std::find(outside_preds.begin(), outside_preds.end(), pred) ==
        outside_preds.end()
    ) {
      outside_preds.push_back(pred);
    }
  }

  if (outside_preds.empty() && header != func.blocks.front().get())
    return false;
  if (
    outside_preds.empty() && !header->insts.empty() &&
    header->insts.front()->code == OpCode::Phi
  ) {
    // There is no valid initial value to attach to the new entry edge.
    return false;
  }

  auto header_it = std::find_if(
    func.blocks.begin(),
    func.blocks.end(),
    [header](const std::unique_ptr<Block> &block) {
      return block.get() == header;
    }
  );
  if (header_it == func.blocks.end())
    return false;

  auto preheader_ptr = std::make_unique<Block>(
    static_cast<int>(func.blocks.size()), header->name + "_preheader"
  );
  Block *preheader = preheader_ptr.get();
  func.blocks.insert(header_it, std::move(preheader_ptr));

  for (auto *pred : outside_preds) {
    if (pred->insts.empty())
      continue;
    Op *terminator = pred->insts.back();
    for (auto *&successor : terminator->successors) {
      if (successor == header)
        successor = preheader;
    }
  }

  auto *jump = module->make_op(OpCode::Jump);
  jump->successors.push_back(header);
  preheader->insts.push_back(jump);

  std::unordered_set<Block *> outside_set(
    outside_preds.begin(), outside_preds.end()
  );
  rewrite_header_phis(header, preheader, outside_set);
  return true;
}

auto LoopSimplify::rewrite_header_phis(
  Block *header,
  Block *preheader,
  const std::unordered_set<Block *> &outside_preds
) -> void {
  for (auto *phi : header->insts) {
    if (phi->code != OpCode::Phi)
      break;

    const auto old_incoming = std::get<PhiPayload>(phi->payload).incoming;
    std::vector<std::pair<Block *, Value *>> outside_incoming;
    std::vector<std::pair<Block *, Value *>> header_incoming;

    for (const auto &incoming : old_incoming) {
      if (outside_preds.count(incoming.first)) {
        outside_incoming.push_back(incoming);
      } else {
        header_incoming.push_back(incoming);
      }
    }

    if (outside_incoming.empty())
      continue;

    Value *preheader_value = outside_incoming.front().second;
    bool all_same = std::all_of(
      outside_incoming.begin(),
      outside_incoming.end(),
      [preheader_value](const auto &incoming) {
        return incoming.second == preheader_value;
      }
    );

    if (!all_same) {
      auto *preheader_phi = module->make_op(OpCode::Phi, PhiPayload{});
      preheader_phi->result =
        module->ctx->make_value<OpResult>(phi->result->type, preheader_phi);
      reset_phi_incoming(preheader_phi, outside_incoming);
      preheader->insts.push_front(preheader_phi);
      preheader_value = preheader_phi->result;
    }

    header_incoming.push_back({preheader, preheader_value});
    reset_phi_incoming(phi, std::move(header_incoming));
  }
}

auto LoopSimplify::reset_phi_incoming(
  Op *phi, std::vector<std::pair<Block *, Value *>> incoming
) -> void {
  auto &payload = std::get<PhiPayload>(phi->payload);
  std::unordered_set<Value *> old_values;
  for (const auto &[block, value] : payload.incoming) {
    (void)block;
    if (value)
      old_values.insert(value);
  }
  for (auto *value : old_values)
    value->rmUse(phi);

  payload.incoming = std::move(incoming);
  for (const auto &[block, value] : payload.incoming) {
    (void)block;
    if (value)
      value->addUse(phi);
  }
}

auto LoopSimplify::rebuild_cfg(LinearFunction &func) -> void {
  for (auto &block : func.blocks) {
    block->preds.clear();
    block->succs.clear();
  }

  for (auto &block_ptr : func.blocks) {
    Block *block = block_ptr.get();
    if (block->insts.empty())
      continue;
    Op *terminator = block->insts.back();
    if (terminator->code != OpCode::Jump && terminator->code != OpCode::Branch)
      continue;
    for (auto *successor : terminator->successors) {
      block->succs.push_back(successor);
      successor->preds.push_back(block);
    }
  }
}

auto LoopSimplify::renumber_blocks(LinearFunction &func) -> void {
  int id = 0;
  for (auto &block : func.blocks)
    block->id = id++;
}

} // namespace exodus::mid_ir::opt
