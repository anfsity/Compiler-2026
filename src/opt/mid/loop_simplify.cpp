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

  if (!changed)
    return exodus::opt::PreservedAnalysis::all();

  renumber_blocks(func);
  rebuild_cfg(func);
  return exodus::opt::PreservedAnalysis::none();
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
