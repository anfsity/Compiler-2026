#pragma once

#include "../../mid/ir.hpp"
#include "../../mid/rewriter.hpp"
#include "../AnalysisManager.hpp"
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
  static auto
  has_scoped_users(const Op *op, const std::unordered_set<OpBase *> &scope)
    -> bool;
};

} // namespace exodus::mid_ir::opt
