#pragma once

#include "../../high/effects.hpp"
#include "../AnalysisManager.hpp"

namespace exodus::high_ir::opt {

using namespace exodus::opt;

struct FunctionEffectsAnalysis {
  using Result = std::unordered_map<Function *, OpEffects>;

  auto run(Module &module, ModuleAnalysisManager &) -> Result {
    return get_function_effects(module);
  }
};

} // namespace exodus::high_ir::opt
