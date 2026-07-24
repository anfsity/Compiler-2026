#pragma once

#include "../../mid/ir.hpp"
#include "../../mid/rewriter.hpp"
#include "../AnalysisManager.hpp"
#include <deque>
#include <unordered_set>

namespace exodus::mid_ir::opt {

class DCE {
  MidIRRewriter rewriter;

public:
  explicit DCE(MidModule *) {}

  auto run(LinearFunction &func, exodus::opt::LinearFunctionAnalysisManager &am)
    -> exodus::opt::PreservedAnalysis;

private:
  static auto has_observable_effect(const Op *op) -> bool;
  static auto mark_definition(
    Value *value,
    const std::unordered_set<Op *> &scope,
    std::unordered_set<Op *> &live,
    std::deque<Op *> &worklist
  ) -> void;
};

} // namespace exodus::mid_ir::opt
