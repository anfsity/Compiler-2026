#pragma once

#include "../opt/AnalysisManager.hpp"
#include "ir.hpp"
#include "rewriter.hpp"
#include <unordered_set>

namespace exodus::mid_ir {

class DCE {
  MidIRRewriter rewriter;

public:
  explicit DCE(MidModule *) {}

  auto run(LinearFunction &func, exodus::opt::LinearFunctionAnalysisManager &am)
    -> exodus::opt::PreservedAnalysis;

private:
  static auto has_observable_effect(const Op *op) -> bool;
  static auto
  has_scoped_users(const Op *op, const std::unordered_set<Op *> &scope) -> bool;
};

} // namespace exodus::mid_ir
