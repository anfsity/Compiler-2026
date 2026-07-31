#include "inliner.hpp"

namespace exodus::high_ir::opt {

Inliner::Inliner(Module *_m) : m(_m) {
  for (auto &f : m->functions) {
    func_map[f->name] = f.get();
  }
}

auto Inliner::run(Module &module, ModuleAnalysisManager &mam)
  -> PreservedAnalysis {
  call_graph.build(*m);
  mam.register_pass<FunctionEffectsAnalysis>();
  function_effects = mam.get_result<FunctionEffectsAnalysis>(module);
  call_counts.clear();
  for (const auto &[caller, node] : call_graph.getNodes()) {
    (void)caller;
    for (auto *callee : node.callees)
      ++call_counts[callee];
  }

  bool changed = false;
  // Bottom-up processing: Leaf functions (those that call no one or only
  // inlined ones) first. Reverse topological order of SCCs.
  const auto &sccs = call_graph.getSCCs();
  for (auto it = sccs.rbegin(); it != sccs.rend(); ++it) {
    for (auto *func : *it) {
      if (func->is_decl)
        continue;
      changed |= runOnFunction(*func);
    }
  }

  return changed ? PreservedAnalysis::none() : PreservedAnalysis::all();
}

auto Inliner::getRetStatus(const Region &r) -> RetStatus {
  for (auto it = r.begin(); it != r.end(); ++it) {
    Op *op = *it;
    RetStatus status = RetStatus::None;

    if (op->code == OpCode::Ret) {
      status = RetStatus::Safe;
    } else if (op->code == OpCode::If) {
      auto &p = std::get<IfPayload>(op->payload);
      RetStatus s1 = getRetStatus(*p.then_region);
      RetStatus s2 =
        p.else_region ? getRetStatus(*p.else_region) : RetStatus::None;

      if (s1 == RetStatus::Unsafe || s2 == RetStatus::Unsafe)
        status = RetStatus::Unsafe;
      else if (s1 == RetStatus::Safe || s2 == RetStatus::Safe)
        status = RetStatus::Safe;
    } else if (op->code == OpCode::While) {
      auto &p = std::get<WhilePayload>(op->payload);
      // Returns inside loops are always unsafe for simple inlining/flattening
      if (getRetStatus(*p.loop_region) != RetStatus::None)
        status = RetStatus::Unsafe;
    }

    if (status != RetStatus::None) {
      if (status == RetStatus::Unsafe || std::next(it) != r.end())
        return RetStatus::Unsafe;
      return RetStatus::Safe;
    }
  }
  return RetStatus::None;
}

auto Inliner::runOnFunction(Function &f) -> bool {
  bool changed = false;
  bool local_changed = false;
  do { // NOLINT
    local_changed = tryInlineInRegion(f.body, f, 0);
    changed |= local_changed;
  } while (local_changed);
  return changed;
}

auto Inliner::tryInlineInRegion(Region &r, Function &caller, int depth)
  -> bool {
  for (auto it = r.begin(); it != r.end(); ++it) {
    Op *op = *it;
    if (op->code == OpCode::Call) {
      auto &p = std::get<CallPayload>(op->payload);
      if (func_map.count(p.func_name)) {
        Function *callee = func_map[p.func_name];
        if (shouldInline(*op, *callee, caller, depth)) {
          if (inlineCall(r, it, op, *callee))
            return true;
        }
      }
    } else if (op->code == OpCode::If) {
      auto &p = std::get<IfPayload>(op->payload);
      if (tryInlineInRegion(*p.then_region, caller, depth))
        return true;
      if (p.else_region && tryInlineInRegion(*p.else_region, caller, depth))
        return true;
    } else if (op->code == OpCode::While) {
      auto &p = std::get<WhilePayload>(op->payload);
      if (tryInlineInRegion(*p.cond_region, caller, depth + 1))
        return true;
      if (tryInlineInRegion(*p.loop_region, caller, depth + 1))
        return true;
    }
  }
  return false;
}

auto Inliner::shouldInline(
  Op &call_op, Function &callee, Function &caller, int depth
) -> bool {
  if (callee.is_decl || callee.no_inline)
    return false;
  if (
    &callee == &caller || call_graph.isRecursive(&callee) ||
    call_op.operands.size() != callee.args.size()
  )
    return false;

  int cost = CostModel::calculate(callee, &call_op.operands);
  if (cost < 30)
    return true; // Wrapper or very small function heuristic

  int threshold = 180 - (depth * 40);
  auto effect_it = function_effects.find(&callee);
  if (effect_it != function_effects.end()) {
    const auto &effects = effect_it->second;
    if (effects.has_unknown_effect)
      threshold -= 70;
    else if (effects.writes.empty())
      threshold += effects.reads.empty() ? 70 : 35;
  }
  for (auto *operand : call_op.operands) {
    if (operand && operand->kind == ValueKind::Constant)
      threshold += 10;
  }
  if (call_counts[&callee] > 4)
    threshold -= 40;
  return cost < threshold;
}

auto Inliner::inlineCall(
  Region &r, Region::iterator it, Op *call_op, Function &callee
) -> bool {
  if (getRetStatus(callee.body) == RetStatus::Unsafe)
    return false;

  IRCloner cloner(&m->ctx);
  for (size_t i = 0; i < callee.args.size(); ++i) {
    cloner.value_map[callee.args[i]] = call_op->operands[i];
  }

  auto func_type = std::static_pointer_cast<Func>(callee.type);
  Region cloned_body = cloner.cloneRegion(callee.body);

  // The common single-return form can be inlined directly.  This avoids the
  // temporary alloca/load pair and immediately exposes constants to users.
  if (!cloned_body.empty() && cloned_body.back()->code == OpCode::Ret) {
    Op *ret_op = cloned_body.back();
    Value *return_value =
      ret_op->operands.empty() ? nullptr : ret_op->operands.front();
    for (auto *operand : ret_op->operands)
      operand->rmUse(ret_op);
    cloned_body.pop_back();
    if (call_op->result && return_value) {
      IRRewriter rewriter;
      rewriter.replace_all_uses_with(call_op->result, return_value);
    }
    r.insert(it, cloned_body.begin(), cloned_body.end());
    for (auto *operand : call_op->operands)
      operand->rmUse(call_op);
    r.erase(it);
    return true;
  }

  Value *ret_alloca = nullptr;
  if (!func_type->ret_type->is_void()) {
    Op *alloca_op = m->ctx.make_op(OpCode::Alloca);
    alloca_op->result =
      m->ctx.make_value<OpResult>(func_type->ret_type->ptr_to(), alloca_op);
    ret_alloca = alloca_op->result;
    r.insert(it, alloca_op);
  }

  struct RetToStore : RecursiveOpVisitor<RetToStore> {
    Value *ret_alloca;
    std::vector<Op *> to_replace;
    RetToStore(Value *a) : ret_alloca(a) {}
    using RecursiveOpVisitor<RetToStore>::visit;
    auto visit(Op *op, OpTag<OpCode::Ret>) -> void { to_replace.push_back(op); }
  };
  RetToStore rts(ret_alloca);
  rts.visit(cloned_body);

  IRRewriter ret_rewriter;
  for (auto *ret_op : rts.to_replace) {
    if (!ret_op->operands.empty() && ret_alloca) {
      ret_op->code = OpCode::Store;
      ret_op->operands.push_back(ret_alloca);
      ret_alloca->addUse(ret_op);
    } else {
      ret_rewriter.eraseOp(ret_op);
    }
  }
  ret_rewriter.finalize(cloned_body);

  if (call_op->result && ret_alloca) {
    Op *load_op = m->ctx.make_op(OpCode::Load);
    load_op->operands = {ret_alloca};
    ret_alloca->addUse(load_op);
    load_op->result = m->ctx.make_value<OpResult>(func_type->ret_type, load_op);

    IRRewriter rewriter;
    rewriter.replace_all_uses_with(call_op->result, load_op->result);
    r.insert(it, cloned_body.begin(), cloned_body.end());
    r.insert(it, load_op);
  } else {
    r.insert(it, cloned_body.begin(), cloned_body.end());
  }

  for (auto *operand : call_op->operands)
    operand->rmUse(call_op);
  r.erase(it);
  return true;
}

} // namespace exodus::high_ir::opt
