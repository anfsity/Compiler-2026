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

auto TailRecursionElim::run(
  LinearFunction &func, exodus::opt::LinearFunctionAnalysisManager & /* am */
) -> exodus::opt::PreservedAnalysis {
  if (func.is_decl || func.blocks.empty())
    return exodus::opt::PreservedAnalysis::all();

  auto tail_calls = collect_tail_calls(func);
  if (tail_calls.empty())
    return exodus::opt::PreservedAnalysis::all();

  Block *entry = func.blocks.front().get();
  CFGEditor cfg(*module, func);
  cfg.synchronize();
  auto tx = cfg.begin_transaction();

  // The original entry block becomes the loop header. A preheader supplies
  // the initial values for the loop-carried arguments, while every tail call
  // supplies one back-edge incoming value.
  Block *preheader = cfg.create_block(func.name + "_tail_preheader", entry);
  if (!preheader)
    return exodus::opt::PreservedAnalysis::all();

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
    if (!cfg.add_phi_incoming(phis[i], preheader, func.args[i]))
      return exodus::opt::PreservedAnalysis::all();
  }

  auto *preheader_jump = module->make_op(OpCode::Jump);
  preheader_jump->successors.push_back(entry);

  for (const auto &tail_call : tail_calls) {
    for (size_t i = 0; i < phis.size(); ++i) {
      Value *next_value = tail_call.call->operands[i];
      if (!cfg.add_phi_incoming(phis[i], tail_call.block, next_value))
        return exodus::opt::PreservedAnalysis::all();
    }

    rewriter.eraseOp(tail_call.call);
    rewriter.eraseOp(tail_call.ret);

    auto *backedge = module->make_op(OpCode::Jump);
    backedge->successors.push_back(entry);
    if (!cfg.set_terminator(tail_call.block, backedge))
      return exodus::opt::PreservedAnalysis::all();
  }

  rewriter.finalize(func);
  if (!cfg.set_terminator(preheader, preheader_jump) || !tx.commit())
    return exodus::opt::PreservedAnalysis::all();

  func.tail_recursion_eliminated = true;
  return exodus::opt::PreservedAnalysis::none();
}

} // namespace exodus::mid_ir::opt
