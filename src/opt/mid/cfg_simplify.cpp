#include "cfg_simplify.hpp"

#include <algorithm>
#include <unordered_set>
#include <vector>

namespace exodus::mid_ir::opt {
namespace {

auto has_pred(const Block *block, const Block *pred) -> bool {
  return std::find(block->preds.begin(), block->preds.end(), pred) !=
         block->preds.end();
}

auto starts_with_phi(const Block *block) -> bool {
  return !block->insts.empty() && block->insts.front()->code == OpCode::Phi;
}

} // namespace

auto CFGSimplify::run(
  LinearFunction &func, exodus::opt::LinearFunctionAnalysisManager & /* am */
) -> exodus::opt::PreservedAnalysis {
  if (func.blocks.empty())
    return exodus::opt::PreservedAnalysis::all();

  changed = false;
  CFGEditor cfg(*module, func);
  cfg.synchronize();
  rewriter.set_scope(func);

  fold_constant_branches(cfg);
  remove_unreachable_blocks(cfg);
  simplify_phis(func);
  rewriter.finalize(func);
  cfg.synchronize();

  merge_linear_blocks(cfg);
  rewriter.set_scope(func);
  simplify_phis(func);
  rewriter.finalize(func);
  cfg.synchronize();

  return changed ? exodus::opt::PreservedAnalysis::none()
                 : exodus::opt::PreservedAnalysis::all();
}

auto CFGSimplify::fold_constant_branches(CFGEditor &cfg) -> void {
  for (auto &block : cfg.function().blocks) {
    if (block->insts.empty())
      continue;

    Op *branch = block->insts.back();
    if (
      branch->code != OpCode::Branch || branch->operands.size() != 1 ||
      branch->successors.size() != 2 ||
      branch->operands[0]->kind != ValueKind::Constant
    )
      continue;

    auto *condition = static_cast<Constant *>(branch->operands[0]);
    if (!std::holds_alternative<int>(condition->val))
      continue;

    auto selected = std::get<int>(condition->val) != 0 ? 0u : 1u;
    Block *target = branch->successors[selected];
    auto *jump = cfg.module().make_op(OpCode::Jump);
    jump->successors.push_back(target);
    if (cfg.set_terminator(block.get(), jump)) {
      changed = true;
    }
  }
}

auto CFGSimplify::remove_unreachable_blocks(CFGEditor &cfg) -> void {
  std::unordered_set<Block *> reachable;
  std::vector<Block *> worklist{cfg.function().blocks.front().get()};
  while (!worklist.empty()) {
    Block *block = worklist.back();
    worklist.pop_back();
    if (!reachable.insert(block).second)
      continue;
    worklist.insert(worklist.end(), block->succs.begin(), block->succs.end());
  }

  for (auto it = cfg.function().blocks.begin();
       it != cfg.function().blocks.end();) {
    Block *block = it->get();
    ++it;
    if (reachable.count(block)) {
      continue;
    }
    if (cfg.remove_block(block))
      changed = true;
  }
}

auto CFGSimplify::simplify_phis(LinearFunction &func) -> void {
  for (auto &block : func.blocks) {
    for (auto *phi : block->insts) {
      if (phi->code != OpCode::Phi)
        break;

      auto &payload = std::get<PhiPayload>(phi->payload);
      for (auto incoming = payload.incoming.begin();
           incoming != payload.incoming.end();) {
        if (!has_pred(block.get(), incoming->first)) {
          incoming->second->rmUse(phi);
          incoming = payload.incoming.erase(incoming);
          changed = true;
        } else {
          ++incoming;
        }
      }

      Value *replacement = nullptr;
      if (payload.incoming.size() == 1) {
        replacement = payload.incoming.front().second;
      } else if (!payload.incoming.empty()) {
        replacement = payload.incoming.front().second;
        for (auto &[_, value] : payload.incoming) {
          if (value != replacement) {
            replacement = nullptr;
            break;
          }
        }
      }

      if (replacement && replacement != phi->result) {
        rewriter.replace_all_uses_with(phi->result, replacement);
        rewriter.eraseOp(phi);
        changed = true;
      }
    }
  }
}

auto CFGSimplify::merge_linear_blocks(CFGEditor &cfg) -> void {
  bool merged = false;
  do { // NOLINT
    merged = false;
    cfg.synchronize();
    Block *entry = cfg.function().blocks.front().get();

    for (auto source_it = cfg.function().blocks.begin();
         source_it != cfg.function().blocks.end();
         ++source_it) {
      Block *source = source_it->get();
      if (
        source->insts.empty() || source->insts.back()->code != OpCode::Jump ||
        source->succs.size() != 1
      )
        continue;

      Block *target = source->succs.front();
      if (
        target == source || target == entry || target->preds.size() != 1 ||
        starts_with_phi(target)
      )
        continue;
      if (cfg.merge_blocks(source, target)) {
        changed = true;
        merged = true;
        break;
      }
    }
  } while (merged);
}

} // namespace exodus::mid_ir::opt
