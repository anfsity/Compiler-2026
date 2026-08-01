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

  auto is_function_pass(const std::string &name) const -> bool {
    return PassRegistry::instance().get_function_passes().count(name);
  }

  auto is_module_pass(const std::string &name) const -> bool {
    return PassRegistry::instance().get_module_passes().count(name);
  }

  auto is_linear_function_pass(const std::string &name) const -> bool {
    return PassRegistry::instance().get_linear_function_passes().count(name);
  }

  auto is_mid_module_pass(const std::string &name) const -> bool {
    return PassRegistry::instance().get_mid_module_passes().count(name);
  }

  auto create_function_pass(const std::string &name)
    -> Pass<high_ir::Function> {
    const auto &passes = PassRegistry::instance().get_function_passes();
    auto it = passes.find(name);
    if (it == passes.end()) {
      throw std::runtime_error("Function pass not found: " + name);
    }
    return it->second.creator(m);
  }

  auto create_module_pass(const std::string &name) -> Pass<high_ir::Module> {
    const auto &passes = PassRegistry::instance().get_module_passes();
    auto it = passes.find(name);
    if (it == passes.end()) {
      throw std::runtime_error("Module pass not found: " + name);
    }
    return it->second.creator(m);
  }

  auto create_linear_function_pass(const std::string &name)
    -> Pass<::exodus::mid_ir::LinearFunction> {
    const auto &passes = PassRegistry::instance().get_linear_function_passes();
    auto it = passes.find(name);
    if (it == passes.end()) {
      throw std::runtime_error("LinearFunction pass not found: " + name);
    }
    return it->second.creator(mid_m);
  }

  auto create_mid_module_pass(const std::string &name)
    -> Pass<::exodus::mid_ir::MidModule> {
    const auto &passes = PassRegistry::instance().get_mid_module_passes();
    auto it = passes.find(name);
    if (it == passes.end()) {
      throw std::runtime_error("Mid module pass not found: " + name);
    }
    return it->second.creator(mid_m);
  }
};

} // namespace exodus::opt
