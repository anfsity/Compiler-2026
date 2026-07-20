#include "tail_recursion_elim.hpp"

#include <iterator>
#include <unordered_set>

namespace exodus::mid_ir::opt {

auto TailRecursionElim::collect_tail_calls(LinearFunction &func)
  -> std::vector<TailCall> {
  std::vector<TailCall> tail_calls;

  for (auto &block_ptr : func.blocks) {
    auto &insts = block_ptr->insts;
    if (insts.size() < 2)
      continue;

    auto ret_it = std::prev(insts.end());
    auto call_it = std::prev(ret_it);
    Op *ret = *ret_it;
    Op *call = *call_it;

    if (ret->code != OpCode::Ret || call->code != OpCode::Call)
      continue;

    const auto &payload = std::get<CallPayload>(call->payload);
    if (payload.func_name != func.name)
      continue;

    if (call->operands.size() != func.args.size())
      continue;

    bool returns_call = ret->operands.size() == 1 && call->result &&
                        ret->operands.front() == call->result;
    bool returns_void = ret->operands.empty();
    if (!returns_call && !returns_void)
      continue;

    // A removed call must not have any users other than the adjacent return.
    if (call->result) {
      std::unordered_set<OpBase *> scope;
      for (auto &scope_block : func.blocks) {
        for (auto *scope_op : scope_block->insts) {
          scope.insert(scope_op);
        }
      }

      bool has_unrelated_user = false;
      for (auto *user_base : call->result->users) {
        // Flattening reuses Value objects, so stale High IR users may still
        // be present in the use list. Only users in the current Mid IR scope
        // are relevant to this transformation.
        if (!scope.count(user_base))
          continue;

        auto *user = static_cast<Op *>(user_base);
        if (user && user != ret) {
          has_unrelated_user = true;
          break;
        }
      }
      if (has_unrelated_user)
        continue;
    }

    tail_calls.push_back({block_ptr.get(), call, ret});
  }

  return tail_calls;
}

auto TailRecursionElim::renumber_blocks(LinearFunction &func) -> void {
  int id = 0;
  for (auto &block : func.blocks) {
    block->id = id++;
  }
}

auto TailRecursionElim::rebuild_cfg(LinearFunction &func) -> void {
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

auto TailRecursionElim::run(
  LinearFunction &func, exodus::opt::LinearFunctionAnalysisManager & /* am */
) -> exodus::opt::PreservedAnalysis {
  if (func.is_decl || func.blocks.empty())
    return exodus::opt::PreservedAnalysis::all();

  auto tail_calls = collect_tail_calls(func);
  if (tail_calls.empty())
    return exodus::opt::PreservedAnalysis::all();

  Block *entry = func.blocks.front().get();

  // The original entry block becomes the loop header. A preheader supplies
  // the initial values for the loop-carried arguments, while every tail call
  // supplies one back-edge incoming value.
  int new_block_id = static_cast<int>(func.blocks.size());
  auto preheader_ptr =
    std::make_unique<Block>(new_block_id, func.name + "_tail_preheader");
  Block *preheader = preheader_ptr.get();

  func.blocks.push_front(std::move(preheader_ptr));
  renumber_blocks(func);

  std::vector<Op *> phis;
  phis.reserve(func.args.size());
  auto insert_pos = entry->insts.begin();
  for (auto *arg : func.args) {
    auto *phi = module->make_op(OpCode::Phi, PhiPayload{});
    phi->result = module->ctx->make_value<OpResult>(arg->type, phi);
    entry->insts.insert(insert_pos, phi);
    phis.push_back(phi);
  }

  MidIRRewriter rewriter;
  rewriter.set_scope(func);

  // Replace argument uses before adding initial Phi incoming values. This
  // prevents the replacement from turning those incoming values into a
  // self-reference. Recursive call operands are rewritten to the current
  // iteration's Phi values as well.
  for (size_t i = 0; i < func.args.size(); ++i) {
    rewriter.replace_all_uses_with(func.args[i], phis[i]->result);
  }

  for (size_t i = 0; i < func.args.size(); ++i) {
    auto &payload = std::get<PhiPayload>(phis[i]->payload);
    payload.incoming.push_back({preheader, func.args[i]});
    func.args[i]->addUse(phis[i]);
  }

  auto *preheader_jump = module->make_op(OpCode::Jump);
  preheader_jump->successors.push_back(entry);
  preheader->insts.push_back(preheader_jump);

  for (const auto &tail_call : tail_calls) {
    for (size_t i = 0; i < phis.size(); ++i) {
      auto &payload = std::get<PhiPayload>(phis[i]->payload);
      Value *next_value = tail_call.call->operands[i];
      payload.incoming.push_back({tail_call.block, next_value});
      next_value->addUse(phis[i]);
    }

    rewriter.eraseOp(tail_call.call);
    rewriter.eraseOp(tail_call.ret);

    auto *backedge = module->make_op(OpCode::Jump);
    backedge->successors.push_back(entry);
    tail_call.block->insts.push_back(backedge);
  }

  rewriter.finalize(func);
  rebuild_cfg(func);

  return exodus::opt::PreservedAnalysis::none();
}

} // namespace exodus::mid_ir::opt
