#pragma once

#include "../../mid/cfg_editor.hpp"
#include "../AnalysisManager.hpp"
#include "polyhedral.hpp"
#include <unordered_set>

namespace exodus::mid_ir::opt {

class PolyhedralOpt {
public:
  explicit PolyhedralOpt(MidModule *module) : module(module) {}

  auto run(LinearFunction &func, exodus::opt::LinearFunctionAnalysisManager &am)
    -> exodus::opt::PreservedAnalysis;

private:
  MidModule *module;
  std::unordered_set<Block *> excluded_fallback_headers;

  auto prepare_reduction(
    CFGEditor &cfg,
    LinearFunction &func,
    exodus::opt::LinearFunctionAnalysisManager &am
  ) -> bool;
  static auto
  interchange(CFGEditor &cfg, LinearFunction &func, const PolyhedralScop &scop)
    -> bool;
};

} // namespace exodus::mid_ir::opt
