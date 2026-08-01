#include "loop_simplify.hpp"

#include <algorithm>

namespace exodus::mid_ir::opt {

auto LoopSimplify::run(
  LinearFunction &func, exodus::opt::LinearFunctionAnalysisManager &am
) -> exodus::opt::PreservedAnalysis {
  if (func.blocks.empty())
    return exodus::opt::PreservedAnalysis::all();

  CFGEditor cfg(*module, func);
  cfg.synchronize();
  auto &loop_info = am.get_result<LoopAnalysis>(func);
  bool changed = false;
  for (auto *loop : loop_info.get_loops_innermost_first()) {
    if (!loop->get_preheader()) {
      changed = create_preheader(cfg, *loop);
      if (changed)
        break;
    }
  }

  if (!changed) {
    for (auto *loop : loop_info.get_loops_innermost_first()) {
      if (loop->get_back_edges().size() > 1) {
        changed = create_single_latch(cfg, *loop);
        break;
      }
    }
  }

  if (!changed)
    return exodus::opt::PreservedAnalysis::all();

  cfg.synchronize();
  return exodus::opt::PreservedAnalysis::none();
}

auto LoopSimplify::create_single_latch(CFGEditor &cfg, Loop &loop) -> bool {
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

  if (!cfg.owns(header))
    return false;

  std::unordered_set<Block *> backedge_set(
    backedge_blocks.begin(), backedge_blocks.end()
  );
  for (auto *block : backedge_blocks) {
    if (block->insts.empty())
      return false;
    auto *terminator = block->insts.back();
    if (terminator->code != OpCode::Jump && terminator->code != OpCode::Branch)
      return false;
    if (
      std::find(
        terminator->successors.begin(), terminator->successors.end(), header
      ) == terminator->successors.end()
    )
      return false;
  }

  std::vector<std::pair<Op *, std::vector<std::pair<Block *, Value *>>>>
    rewritten_header_phis;
  struct LatchPhiPlan {
    Op *header_phi = nullptr;
    std::vector<std::pair<Block *, Value *>> backedge_incoming;
  };
  std::vector<LatchPhiPlan> latch_phi_plans;
  struct ClonedUpdatePlan {
    Op *header_phi = nullptr;
    Op *representative = nullptr;
    std::vector<std::pair<Block *, Value *>> header_incoming;
  };
  std::vector<ClonedUpdatePlan> cloned_update_plans;

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
      if (auto *representative = identical_binary_update(backedge_incoming)) {
        cloned_update_plans.push_back({phi, representative, header_incoming});
      } else {
        latch_phi_plans.push_back({phi, backedge_incoming});
      }
      continue;
    }
    header_incoming.push_back({nullptr, latch_value});
    rewritten_header_phis.push_back({phi, std::move(header_incoming)});
  }

  auto tx = cfg.begin_transaction();
  auto *latch = cfg.create_block(header->name + "_latch", header);
  if (!latch)
    return false;

  for (auto &[phi, incoming] : rewritten_header_phis) {
    for (auto &[pred, _] : incoming) {
      if (pred == nullptr)
        pred = latch;
    }
    if (!cfg.set_phi_incoming(phi, std::move(incoming)))
      return false;
  }

  for (auto &plan : cloned_update_plans) {
    auto *update = module->make_op(plan.representative->code);
    update->operands = plan.representative->operands;
    for (auto *operand : update->operands)
      operand->addUse(update);
    update->result = module->ctx->make_value<OpResult>(
      plan.representative->result->type, update
    );
    latch->insts.push_back(update);
    plan.header_incoming.push_back({latch, update->result});
    if (!cfg.set_phi_incoming(plan.header_phi, std::move(plan.header_incoming)))
      return false;
  }

  for (auto &plan : latch_phi_plans) {
    auto *latch_phi = module->make_op(OpCode::Phi, PhiPayload{});
    latch_phi->result = module->ctx->make_value<OpResult>(
      plan.header_phi->result->type, latch_phi
    );
    latch->insts.push_back(latch_phi);
    if (!cfg.set_phi_incoming(latch_phi, plan.backedge_incoming))
      return false;

    const auto original_incoming =
      std::get<PhiPayload>(plan.header_phi->payload).incoming;
    std::vector<std::pair<Block *, Value *>> header_incoming;
    for (const auto &incoming : original_incoming) {
      if (!backedge_set.count(incoming.first))
        header_incoming.push_back(incoming);
    }
    header_incoming.push_back({latch, latch_phi->result});
    if (!cfg.set_phi_incoming(plan.header_phi, std::move(header_incoming)))
      return false;
  }

  for (auto *block : backedge_blocks) {
    if (!cfg.redirect_edge(block, header, latch))
      return false;
  }

  auto *jump = module->make_op(OpCode::Jump);
  jump->successors.push_back(header);
  if (!cfg.set_terminator(latch, jump))
    return false;
  return tx.commit();
}

auto LoopSimplify::create_preheader(CFGEditor &cfg, Loop &loop) -> bool {
  Block *header = loop.get_header();
  if (!header || !cfg.owns(header))
    return false;
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

  if (outside_preds.empty() && header != cfg.function().blocks.front().get())
    return false;
  if (
    outside_preds.empty() && !header->insts.empty() &&
    header->insts.front()->code == OpCode::Phi
  ) {
    // There is no valid initial value to attach to the new entry edge.
    return false;
  }

  auto tx = cfg.begin_transaction();
  Block *preheader = cfg.create_block(header->name + "_preheader", header);
  if (!preheader)
    return false;

  std::unordered_set<Block *> outside_set(
    outside_preds.begin(), outside_preds.end()
  );
  rewrite_header_phis(cfg, header, preheader, outside_set);

  for (auto *pred : outside_preds) {
    if (!cfg.redirect_edge(pred, header, preheader))
      return false;
  }

  auto *jump = module->make_op(OpCode::Jump);
  jump->successors.push_back(header);
  if (!cfg.set_terminator(preheader, jump))
    return false;

  return tx.commit();
}

auto LoopSimplify::rewrite_header_phis(
  CFGEditor &cfg,
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
      preheader->insts.push_front(preheader_phi);
      cfg.set_phi_incoming(preheader_phi, outside_incoming);
      preheader_value = preheader_phi->result;
    }

    header_incoming.push_back({preheader, preheader_value});
    cfg.set_phi_incoming(phi, std::move(header_incoming));
  }
}

auto LoopSimplify::identical_binary_update(
  const std::vector<std::pair<Block *, Value *>> &incoming
) -> Op * {
  Op *representative = nullptr;
  for (const auto &[_, value] : incoming) {
    if (!value || value->kind != ValueKind::OpResult)
      return nullptr;
    auto *candidate =
      static_cast<Op *>(static_cast<OpResult *>(value)->creator);
    if (
      !candidate || !candidate->result || candidate->operands.size() != 2 ||
      (candidate->code != OpCode::Add && candidate->code != OpCode::Sub)
    ) {
      return nullptr;
    }
    if (!representative) {
      representative = candidate;
      continue;
    }
    if (
      candidate->code != representative->code ||
      candidate->operands != representative->operands
    ) {
      return nullptr;
    }
  }
  return representative;
}

} // namespace exodus::mid_ir::opt
