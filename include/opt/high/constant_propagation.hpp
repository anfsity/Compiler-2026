#pragma once

#include "../../high/ir.hpp"
#include "../../high/rewriter.hpp"
#include "../../high/visitor.hpp"
#include "../AnalysisManager.hpp"
#include <unordered_map>
#include <unordered_set>

namespace exodus::opt {

using namespace exodus::high_ir;

struct CP : RecursiveOpVisitor<CP> {
  std::unordered_map<Value *, Value *> env;
  std::unordered_set<Value *> safe_allocas;
  std::unordered_set<Value *> safe_globals;
  IRRewriter rewriter;
  IRContext *ctx;
  Module *m;
  bool changed = false;

  using RecursiveOpVisitor<CP>::visit;

  CP(Module *_m) : ctx(&_m->ctx), m(_m) {}

  auto run(Function &f, FunctionAnalysisManager & /* FAM */)
    -> PreservedAnalysis {
    env.clear();
    safe_allocas.clear();
    safe_globals.clear();
    rewriter.clear();
    changed = false;

    for (auto &g : m->globals) {
      if (!g->is_const && (g->type->is_i32() || g->type->is_f32())) {
        safe_globals.insert(g->addr);
      }
    }

    visit(f.body);
    rewriter.finalize(f);

    return changed ? PreservedAnalysis::none() : PreservedAnalysis::all();
  }

  void visit(Op *op) {
    RecursiveOpVisitor<CP>::visit(op);

    if (op->result && !op->operands.empty()) {
      try_fold(op);
    }
  }

  void visit(Op *op, OpTag<OpCode::Alloca>) {
    auto ptr_type = std::static_pointer_cast<exodus::Ptr>(op->result->type);
    if (ptr_type->target->is_i32() || ptr_type->target->is_f32()) {
      safe_allocas.insert(op->result);
    }
  }

  void visit(Op *op, OpTag<OpCode::Load>) {
    Value *ptr = op->operands[0];
    if (env.count(ptr)) {
      rewriter.replaceOp(op, env[ptr]);
      changed = true;
    }
  }

  void visit(Op *op, OpTag<OpCode::Store>) {
    Value *val = op->operands[0];
    Value *ptr = op->operands[1];

    if (safe_allocas.count(ptr) || safe_globals.count(ptr)) {
      env[ptr] = val;
    }
  }

  void visit(Op *op, OpTag<OpCode::Call>) {
    RecursiveOpVisitor<CP>::visit(op, OpTag<OpCode::Call>{});
    clear_global_env();
  }

  void visit(Op *op, OpTag<OpCode::If>) {
    RecursiveOpVisitor<CP>::visit(op, OpTag<OpCode::If>{});
    clear_all_env();
  }

  void visit(Op *op, OpTag<OpCode::While>) {
    RecursiveOpVisitor<CP>::visit(op, OpTag<OpCode::While>{});
    clear_all_env();
  }

  void clear_global_env() {
    for (auto it = env.begin(); it != env.end();) {
      Value *addr = it->first;
      if (addr->kind == ValueKind::GlobalVar) {
        it = env.erase(it);
      } else {
        ++it;
      }
    }
  }

  void clear_all_env() { env.clear(); }

  template <typename T>
  auto fold_arith(OpCode code, T l, T r) -> std::optional<Constant::Data> {
    switch (code) {
      // clang-format off
    case OpCode::Add:  case OpCode::FAdd: return l + r;
    case OpCode::Sub:  case OpCode::FSub: return l - r;
    case OpCode::Mul:  case OpCode::FMul: return l * r;
    case OpCode::Div:  case OpCode::FDiv: { 
      return r != 0 ? std::optional<Constant::Data>(l / r) : std::nullopt; 
    }
    case OpCode::Mod:  if constexpr (std::is_integral_v<T>) { 
      return r != 0 ? std::optional<Constant::Data>(l % r) : std::nullopt; 
    }
    case OpCode::Eq:   return (int)(l == r);
    case OpCode::Ne:   return (int)(l != r);
    case OpCode::Lt:   return (int)(l < r);
    case OpCode::Gt:   return (int)(l > r);
    case OpCode::Le:   return (int)(l <= r);
    case OpCode::Ge:   return (int)(l >= r);
    default: return std::nullopt;
      // clang-format on
    }
  }

  void try_fold(Op *op) {
    for (auto *v : op->operands)
      if (v->kind != ValueKind::Constant)
        return;

    std::optional<Constant::Data> res;
    if (op->operands.size() == 2u) {
      auto *l = static_cast<Constant *>(op->operands[0]);
      auto *r = static_cast<Constant *>(op->operands[1]);
      if (std::holds_alternative<int>(l->val))
        res =
          fold_arith(op->code, std::get<int>(l->val), std::get<int>(r->val));
      else
        res = fold_arith(
          op->code, std::get<float>(l->val), std::get<float>(r->val)
        );

    } else if (op->operands.size() == 1u) {
      auto *v = static_cast<Constant *>(op->operands[0]);
      if (op->code == OpCode::I2F)
        res = (float)std::get<int>(v->val);
      else if (op->code == OpCode::F2I)
        res = (int)std::get<float>(v->val);
      else if (op->code == OpCode::ZExt)
        res = std::get<int>(v->val);
    }

    if (res) {
      rewriter.replaceOp(op, ctx->make_value<Constant>(op->result->type, *res));
      changed = true;
    }
  }
};

} // namespace exodus::opt
