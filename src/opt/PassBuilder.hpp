#pragma once

#include "PassManager.hpp"
#include "PassRegistry.hpp"
#include <stdexcept>
#include <string>

namespace exodus::opt {

class PassBuilder {
  high_ir::Module *m;
  exodus::mid_ir::MidModule *mid_m;

public:
  PassBuilder(high_ir::Module *_m, exodus::mid_ir::MidModule *_mid_m = nullptr)
      : m(_m), mid_m(_mid_m) {}

  auto isFunctionPass(const std::string &name) const -> bool {
    return PassRegistry::instance().getFunctionPasses().count(name);
  }

  auto isModulePass(const std::string &name) const -> bool {
    return PassRegistry::instance().getModulePasses().count(name);
  }

  auto isLinearFunctionPass(const std::string &name) const -> bool {
    return PassRegistry::instance().getLinearFunctionPasses().count(name);
  }

  auto createFunctionPass(const std::string &name) -> Pass<high_ir::Function> {
    const auto &passes = PassRegistry::instance().getFunctionPasses();
    auto it = passes.find(name);
    if (it == passes.end()) {
      throw std::runtime_error("Function pass not found: " + name);
    }
    return it->second.creator(m);
  }

  auto createModulePass(const std::string &name) -> Pass<high_ir::Module> {
    const auto &passes = PassRegistry::instance().getModulePasses();
    auto it = passes.find(name);
    if (it == passes.end()) {
      throw std::runtime_error("Module pass not found: " + name);
    }
    return it->second.creator(m);
  }

  auto createLinearFunctionPass(const std::string &name)
    -> Pass<::exodus::mid_ir::LinearFunction> {
    const auto &passes = PassRegistry::instance().getLinearFunctionPasses();
    auto it = passes.find(name);
    if (it == passes.end()) {
      throw std::runtime_error("LinearFunction pass not found: " + name);
    }
    return it->second.creator(mid_m);
  }

  auto buildFunctionPipeline() -> FunctionPassManager {
    FunctionPassManager fpm;
    const auto &registry = PassRegistry::instance();
    const auto &passes = registry.getFunctionPasses();
    for (const auto &id : registry.getFunctionOrder()) {
      const auto &info = passes.at(id);
      fpm.addPass(info.creator(m), id, info.desc);
    }
    return fpm;
  }

  auto buildModulePipeline() -> ModulePassManager {
    ModulePassManager mpm;
    const auto &registry = PassRegistry::instance();
    const auto &passes = registry.getModulePasses();
    for (const auto &id : registry.getModuleOrder()) {
      const auto &info = passes.at(id);
      mpm.addPass(info.creator(m), id, info.desc);
    }
    return mpm;
  }

  auto buildLinearFunctionPipeline() -> LinearFunctionPassManager {
    LinearFunctionPassManager lfpm;
    const auto &registry = PassRegistry::instance();
    const auto &passes = registry.getLinearFunctionPasses();
    for (const auto &id : registry.getLinearFunctionOrder()) {
      const auto &info = passes.at(id);
      lfpm.addPass(info.creator(mid_m), id, info.desc);
    }
    return lfpm;
  }
};

} // namespace exodus::opt
