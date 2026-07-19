#pragma once

#include "../../high/effects.hpp"
#include "../../high/ir.hpp"
#include "../../high/rewriter.hpp"
#include "../../high/visitor.hpp"
#include "../AnalysisManager.hpp"
#include <unordered_map>
#include <unordered_set>

namespace exodus::high_ir::opt {

using namespace exodus::high_ir;
using namespace exodus::opt;

struct CP : RecursiveOpVisitor<CP> {
  std::unordered_map<Value *, Value *> env;
  std::unordered_set<Value *> safe_allocas;
  std::unordered_set<Value *> safe_globals;
  IRRewriter rewriter;
  IRContext *ctx;
  Module *m;
  std::unordered_map<std::string, Function *> functions;
  std::unordered_map<Function *, OpEffects> function_effects;
  bool changed = false;

  using RecursiveOpVisitor<CP>::visit;

  CP(Module *_m);

  auto run(Function &f, FunctionAnalysisManager & /* FAM */)
    -> PreservedAnalysis;

  auto visit(Op *op) -> void;

  auto visit(Op *op, OpTag<OpCode::Alloca>) -> void;
  auto visit(Op *op, OpTag<OpCode::Load>) -> void;
  auto visit(Op *op, OpTag<OpCode::Store>) -> void;
  auto visit(Op *op, OpTag<OpCode::Call>) -> void;

  // 为了更好的进行优化，在 if 语句内，现对 if 语句里面进行试探
  // 在 if () { A  } else { B } 中，A 和 B 都是可以基于之前的结果进行 CP 的
  // 但是如果在 A/B 中进行了 store，由于分支语句的不确定性，我们无法进行良好假设
  // 所以在后面会从集合中移除他们。
  auto visit(Op *op, OpTag<OpCode::If>) -> void;
  auto visit(Op *op, OpTag<OpCode::While>) -> void;

  auto invalidate_writes(const OpEffects &effects) -> void;
  auto resolved_effects(const Region &region) const -> OpEffects;
  auto clear_global_env() -> void;

  template <typename T>
  auto fold_arith(OpCode code, T l, T r) -> std::optional<Constant::Data>;

  auto try_fold(Op *op) -> void;
};

} // namespace exodus::high_ir::opt
