#pragma once

#include "PassManager.hpp"
#include "high/constant_propagation.hpp"

namespace exodus::opt {

class PassBuilder {
  high_ir::Module *m;

public:
  PassBuilder(high_ir::Module *_m) : m(_m) {}

  auto buildFunctionPipeline() -> FunctionPassManager {
    FunctionPassManager fpm;

#define FUNCTION_PASS(NAME, ID, DESC) fpm.addPass(NAME(m)); // NOLINT
#include "Passes.def"

    return fpm;
  }

  auto buildModulePipeline() -> ModulePassManager {
    ModulePassManager mpm;

#define MODULE_PASS(NAME, ID, DESC) mpm.addPass(NAME(m)); // NOLINT
#include "Passes.def"

    return mpm;
  }
};

} // namespace exodus::opt
