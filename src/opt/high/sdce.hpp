#pragma once

#include "../../helper/log.hpp"
#include "../../high/effects.hpp"
#include "../../high/ir.hpp"
#include "../../high/rewriter.hpp"
#include "../../high/visitor.hpp"
#include "../AnalysisManager.hpp"
#include <deque>
#include <unordered_map>
#include <unordered_set>

namespace exodus::high_ir::opt {

using namespace exodus::high_ir;
using namespace exodus::opt;

struct SimpleDCE {
  IRRewriter rewriter;
  std::unordered_map<Op *, Op *> parents;
  std::unordered_set<Op *> liveset;
  std::unordered_set<Value *> escaped_allocas;
  std::deque<Op *> worklist;

  SimpleDCE(Module * /* m */);

  static auto is_intrinsically_live(Op *op) -> bool;

  auto mark_stores_to(Value *ptr) -> void;
  auto collect_escaped_allocas() -> void;
  auto mark_memory_dependencies(Op *op) -> void;

  auto build_parent_map(Region &r, Op *parent = nullptr) -> void;
  auto mark(Op *op) -> void;
  auto initial_mark(Region &r) -> void;
  auto collect_dead(Region &r) -> void;

  auto run(Function &f, FunctionAnalysisManager & /* FAM */)
    -> PreservedAnalysis;
};

} // namespace exodus::high_ir::opt
