#pragma once

#include "../../mid/cfg_editor.hpp"
#include "../../mid/ir.hpp"
#include "../../mid/rewriter.hpp"
#include "../AnalysisManager.hpp"
#include <vector>

namespace exodus::mid_ir::opt {

class TailRecursionElim {
  struct TailCall {
    Block *block = nullptr;
    Op *call = nullptr;
    Op *ret = nullptr;
  };

  MidModule *module;

public:
  explicit TailRecursionElim(MidModule *m) : module(m) {}

  auto run(LinearFunction &func, exodus::opt::LinearFunctionAnalysisManager &am)
    -> exodus::opt::PreservedAnalysis;

private:
  auto collect_tail_calls(LinearFunction &func) -> std::vector<TailCall>;
};

} // namespace exodus::mid_ir::opt
