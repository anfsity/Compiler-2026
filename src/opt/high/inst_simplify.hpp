#pragma once

#include "../../high/ir.hpp"
#include "../../high/rewriter.hpp"
#include "../../high/visitor.hpp"
#include "../AnalysisManager.hpp"

namespace exodus::high_ir::opt {

struct InstSimplify : RecursiveOpVisitor<InstSimplify> {
  Module *module;
  IRRewriter rewriter;
  bool changed = false;

  using RecursiveOpVisitor<InstSimplify>::visit;

  explicit InstSimplify(Module *module_) : module(module_) {}

  auto run(Function &function, exodus::opt::FunctionAnalysisManager &)
    -> exodus::opt::PreservedAnalysis;
  auto visit(Op *op) -> void;

private:
  auto replace_with(Op *op, Value *value) -> void;
  auto replace_with_zero(Op *op) -> void;
  auto replace_with_bool(Op *op, bool value) -> void;
};

} // namespace exodus::high_ir::opt
