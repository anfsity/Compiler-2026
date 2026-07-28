#pragma once

#include "../../high/effects.hpp"
#include "../../high/ir.hpp"
#include "../../high/rewriter.hpp"
#include "../../high/visitor.hpp"
#include "../AnalysisManager.hpp"
#include <string>
#include <unordered_map>

namespace exodus::high_ir::opt {

struct InstSimplify : RecursiveOpVisitor<InstSimplify> {
  Module *module;
  IRRewriter rewriter;
  bool changed = false;
  std::unordered_map<std::string, Function *> functions;
  std::unordered_map<Function *, OpEffects> function_effects;

  using RecursiveOpVisitor<InstSimplify>::visit;

  explicit InstSimplify(Module *module_) : module(module_) {}

  auto run(Function &function, exodus::opt::FunctionAnalysisManager &)
    -> exodus::opt::PreservedAnalysis;
  auto visit(Op *op) -> void;

private:
  auto simplify_pure_calls(Region &region) -> void;
  auto is_readnone_scalar_call(Op *op) const -> bool;
  auto replace_with(Op *op, Value *value) -> void;
  auto replace_with_zero(Op *op) -> void;
  auto replace_with_bool(Op *op, bool value) -> void;
};

} // namespace exodus::high_ir::opt
