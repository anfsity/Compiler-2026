#include "cfg_simplify.hpp"

#include <algorithm>
#include <unordered_set>
#include <vector>

namespace exodus::mid_ir {
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
  rewriter.set_scope(func);

  fold_constant_branches(func);
  rebuild_cfg(func);
  remove_unreachable_blocks(func);
  rebuild_cfg(func);
  simplify_phis(func);
  rewriter.finalize(func);

  merge_linear_blocks(func);
  rebuild_cfg(func);
  rewriter.set_scope(func);
  simplify_phis(func);
  rewriter.finalize(func);

  return changed ? exodus::opt::PreservedAnalysis::none()
                 : exodus::opt::PreservedAnalysis::all();
}

auto CFGSimplify::fold_constant_branches(LinearFunction &func) -> void {
  for (auto &block : func.blocks) {
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
    condition->rmUse(branch);
    branch->operands.clear();
    branch->code = OpCode::Jump;
    branch->successors = {target};
    changed = true;
  }
}

auto CFGSimplify::rebuild_cfg(LinearFunction &func) -> void {
  for (auto &block : func.blocks) {
    block->preds.clear();
    block->succs.clear();
  }

  for (auto &block : func.blocks) {
    if (block->insts.empty())
      continue;
    Op *terminator = block->insts.back();
    if (terminator->code != OpCode::Jump && terminator->code != OpCode::Branch)
      continue;
    for (auto *successor : terminator->successors) {
      block->succs.push_back(successor);
      successor->preds.push_back(block.get());
    }
  }
}

auto CFGSimplify::remove_unreachable_blocks(LinearFunction &func) -> void {
  std::unordered_set<Block *> reachable;
  std::vector<Block *> worklist{func.blocks.front().get()};
  while (!worklist.empty()) {
    Block *block = worklist.back();
    worklist.pop_back();
    if (!reachable.insert(block).second)
      continue;
    worklist.insert(worklist.end(), block->succs.begin(), block->succs.end());
  }

  for (auto it = func.blocks.begin(); it != func.blocks.end();) {
    if (reachable.count(it->get())) {
      ++it;
      continue;
    }
    for (auto *op : (*it)->insts)
      rewriter.eraseOp(op);
    it = func.blocks.erase(it);
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
        for (auto &[pred, value] : payload.incoming) {
          (void)pred;
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

auto CFGSimplify::merge_linear_blocks(LinearFunction &func) -> void {
  bool merged = false;
  do { // NOLINT
    merged = false;
    rebuild_cfg(func);
    Block *entry = func.blocks.front().get();

    for (auto source_it = func.blocks.begin(); source_it != func.blocks.end();
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

      auto target_it = std::find_if(
        func.blocks.begin(),
        func.blocks.end(),
        [target](const std::unique_ptr<Block> &block) {
          return block.get() == target;
        }
      );
      if (target_it == func.blocks.end())
        continue;

      source->insts.pop_back();
      source->insts.splice(source->insts.end(), target->insts);

      for (auto &block : func.blocks) {
        for (auto *op : block->insts) {
          for (auto *&successor : op->successors) {
            if (successor == target)
              successor = source;
          }
          if (op->code == OpCode::Phi) {
            auto &payload = std::get<PhiPayload>(op->payload);
            for (auto &[pred, value] : payload.incoming) {
              (void)value;
              if (pred == target)
                pred = source;
            }
          }
        }
      }

      func.blocks.erase(target_it);
      changed = true;
      merged = true;
      break;
    }
  } while (merged);
}

} // namespace exodus::mid_ir
