#pragma once

#include "../../base/getptr.hpp"
#include "../../helper/log.hpp"
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
  std::deque<Op *> worklist;

  SimpleDCE(Module * /* m */);

  static auto is_intrinsically_live(Op *op) -> bool;

  static auto get_addr_root(Value *v) -> Value *;
  auto mark_stores_to(Value *ptr) -> void;
  auto mark_implicit_get_ptr_stores(Op *op) -> void;

  auto build_parent_map(Region &r, Op *parent = nullptr) -> void;
  auto mark(Op *op) -> void;
  auto initial_mark(Region &r) -> void;
  auto collect_dead(Region &r) -> void;

  auto run(Function &f, FunctionAnalysisManager & /* FAM */)
    -> PreservedAnalysis;
};

} // namespace exodus::high_ir::opt
