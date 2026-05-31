#pragma once

#include "PassManager.hpp"
#include "high/constant_propagation.hpp"
#include "high/inliner.hpp"
#include "high/return_insertion.hpp"
#include "high/sdce.hpp"
#include <functional>
#include <map>

namespace exodus::opt {

class PassBuilder {
  high_ir::Module *m;

  using FunctionPassCreator = std::function<Pass<high_ir::Function>()>;
  using ModulePassCreator = std::function<Pass<high_ir::Module>()>;

  std::map<std::string, FunctionPassCreator> function_passes;
  std::map<std::string, ModulePassCreator> module_passes;

public:
  PassBuilder(high_ir::Module *_m) : m(_m) { registerPasses(); }

  void registerPasses() {
#define FUNCTION_PASS(NAME, ID, DESC) /* NOLINT */                             \
  function_passes[ID] = [this]() {                                             \
    return Pass<high_ir::Function>(NAME(m), ID, DESC);                         \
  };
#define MODULE_PASS(NAME, ID, DESC) /* NOLINT */                               \
  module_passes[ID] = [this]() {                                               \
    return Pass<high_ir::Module>(NAME(m), ID, DESC);                           \
  };
#include "Passes.def"
  }

  bool isFunctionPass(const std::string &name) const {
    return function_passes.count(name);
  }

  bool isModulePass(const std::string &name) const {
    return module_passes.count(name);
  }

  auto createFunctionPass(const std::string &name) -> Pass<high_ir::Function> {
    return function_passes.at(name)();
  }

  auto createModulePass(const std::string &name) -> Pass<high_ir::Module> {
    return module_passes.at(name)();
  }

  auto buildFunctionPipeline() -> FunctionPassManager {
    FunctionPassManager fpm;
#define FUNCTION_PASS(NAME, ID, DESC) fpm.addPass(NAME(m), ID, DESC); // NOLINT
#include "Passes.def"
    return fpm;
  }

  auto buildModulePipeline() -> ModulePassManager {
    ModulePassManager mpm;
#define MODULE_PASS(NAME, ID, DESC) mpm.addPass(NAME(m), ID, DESC); // NOLINT
#include "Passes.def"
    return mpm;
  }
};

} // namespace exodus::opt
