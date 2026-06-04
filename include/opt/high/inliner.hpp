#pragma once

#include "../../high/cloner.hpp"
#include "../../high/ir.hpp"
#include "../../high/rewriter.hpp"
#include "../../high/scc.hpp"
#include "../../high/visitor.hpp"
#include "../AnalysisManager.hpp"
#include "cost_model.hpp"
#include <cstdint>
#include <iterator>
#include <unordered_map>

namespace exodus::opt {

using namespace exodus::high_ir;

// --- Inliner Pass ---
class Inliner {
  Module *m;
  std::unordered_map<std::string, Function *> func_map;
  CallGraph call_graph;

public:
  Inliner(Module *_m) : m(_m) {
    for (auto &f : m->functions) {
      func_map[f->name] = f.get();
    }
  }

  PreservedAnalysis run(Module &, ModuleAnalysisManager &) {
    call_graph.build(*m);

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

private:
  enum class RetStatus : uint8_t { None, Safe, Unsafe };

  static RetStatus getRetStatus(const Region &r) {
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

  bool runOnFunction(Function &f) {
    bool changed = false;
    bool local_changed = false;
    do { // NOLINT
      local_changed = tryInlineInRegion(f.body, f, 0);
      changed |= local_changed;
    } while (local_changed);
    return changed;
  }

  bool tryInlineInRegion(Region &r, Function &caller, int depth) {
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

  bool shouldInline(Op &call_op, Function &callee, Function &, int depth) {
    if (callee.is_decl)
      return false;
    // TODO: 对广义编译期函数进行 inline 优化。
    if (call_graph.isRecursive(&callee))
      return false;

    int cost = CostModel::calculate(callee, &call_op.operands);
    if (cost < 15)
      return true; // Wrapper or very small function heuristic

    int threshold = 225 + (depth * 150);
    return cost < threshold;
  }

  bool
  inlineCall(Region &r, Region::iterator it, Op *call_op, Function &callee) {
    if (getRetStatus(callee.body) == RetStatus::Unsafe)
      return false;

    IRCloner cloner(&m->ctx);
    for (size_t i = 0; i < callee.args.size(); ++i) {
      cloner.value_map[callee.args[i]] = call_op->operands[i];
    }

    auto func_type = std::static_pointer_cast<Func>(callee.type);
    Value *ret_alloca = nullptr;
    if (!func_type->ret_type->is_void()) {
      Op *alloca_op = m->ctx.make_op(OpCode::Alloca);
      alloca_op->result =
        m->ctx.make_value<OpResult>(func_type->ret_type->ptr_to(), alloca_op);
      ret_alloca = alloca_op->result;
      r.insert(it, alloca_op);
    }

    Region cloned_body = cloner.cloneRegion(callee.body);

    struct RetToStore : RecursiveOpVisitor<RetToStore> {
      Value *ret_alloca;
      std::vector<Op *> to_replace;
      RetToStore(Value *a) : ret_alloca(a) {}
      using RecursiveOpVisitor<RetToStore>::visit;
      void visit(Op *op, OpTag<OpCode::Ret>) { to_replace.push_back(op); }
    };
    RetToStore rts(ret_alloca);
    rts.visit(cloned_body);

    for (auto *ret_op : rts.to_replace) {
      if (!ret_op->operands.empty() && ret_alloca) {
        ret_op->code = OpCode::Store;
        ret_op->operands.push_back(ret_alloca);
        ret_alloca->addUse(ret_op);
      } else {
        ret_op->code = OpCode::Add; // Dummy op code
        ret_op->operands.clear();
      }
    }

    if (call_op->result && ret_alloca) {
      Op *load_op = m->ctx.make_op(OpCode::Load);
      load_op->operands = {ret_alloca};
      ret_alloca->addUse(load_op);
      load_op->result =
        m->ctx.make_value<OpResult>(func_type->ret_type, load_op);

      IRRewriter rewriter;
      rewriter.replaceAllUsesWith(call_op->result, load_op->result);
      r.insert(it, cloned_body.begin(), cloned_body.end());
      r.insert(it, load_op);
    } else {
      r.insert(it, cloned_body.begin(), cloned_body.end());
    }

    r.erase(it);
    return true;
  }
};

} // namespace exodus::opt
