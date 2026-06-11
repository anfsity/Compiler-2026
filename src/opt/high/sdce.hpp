#pragma once

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

  static auto isIntrinsicallyLive(Op *op) -> bool;

  auto buildParentMap(Region &r, Op *parent = nullptr) -> void;
  auto mark(Op *op) -> void;
  auto initialMark(Region &r) -> void;
  auto collectDead(Region &r) -> void;

  auto run(Function &f, FunctionAnalysisManager & /* FAM */)
    -> PreservedAnalysis;
};

} // namespace exodus::high_ir::opt
