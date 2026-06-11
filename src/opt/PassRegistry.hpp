#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "PassManager.hpp"

namespace exodus::opt {

class PassRegistry {
public:
  using FunctionPassCreator =
    std::function<Pass<high_ir::Function>(high_ir::Module *)>;
  using ModulePassCreator =
    std::function<Pass<high_ir::Module>(high_ir::Module *)>;

  template <typename CreatorT>
  struct PassInfo {
    std::string desc;
    CreatorT creator;
  };

public:
  static auto instance() -> PassRegistry & {
    static PassRegistry inst;
    return inst;
  }

  auto registerFunctionPass(
    std::string id, std::string desc, FunctionPassCreator creator
  ) -> void {
    if (!function_passes.count(id)) {
      function_order.push_back(id);
    }
    function_passes[std::move(id)] =
      PassInfo<FunctionPassCreator>{std::move(desc), std::move(creator)};
  }

  auto registerModulePass(
    std::string id, std::string desc, ModulePassCreator creator
  ) -> void {
    if (!module_passes.count(id)) {
      module_order.push_back(id);
    }
    module_passes[std::move(id)] =
      PassInfo<ModulePassCreator>{std::move(desc), std::move(creator)};
  }

  auto getFunctionPasses() const
    -> const std::map<std::string, PassInfo<FunctionPassCreator>> & {
    return function_passes;
  }

  auto getModulePasses() const
    -> const std::map<std::string, PassInfo<ModulePassCreator>> & {
    return module_passes;
  }

  auto getFunctionOrder() const -> const std::vector<std::string> & {
    return function_order;
  }

  auto getModuleOrder() const -> const std::vector<std::string> & {
    return module_order;
  }

private:
  PassRegistry() = default;

  std::map<std::string, PassInfo<FunctionPassCreator>> function_passes;
  std::map<std::string, PassInfo<ModulePassCreator>> module_passes;
  std::vector<std::string> function_order;
  std::vector<std::string> module_order;
};

template <typename PassT>
struct RegisterFunctionPass {
  RegisterFunctionPass(std::string id, std::string desc) {
    auto pass_id = std::move(id);
    auto pass_desc = std::move(desc);
    PassRegistry::instance().registerFunctionPass(
      pass_id, pass_desc, [pass_id, pass_desc](high_ir::Module *m) {
        return Pass<high_ir::Function>(PassT(m), pass_id, pass_desc);
      }
    );
  }
};

template <typename PassT>
struct RegisterModulePass {
  RegisterModulePass(std::string id, std::string desc) {
    auto pass_id = std::move(id);
    auto pass_desc = std::move(desc);
    PassRegistry::instance().registerModulePass(
      pass_id, pass_desc, [pass_id, pass_desc](high_ir::Module *m) {
        return Pass<high_ir::Module>(PassT(m), pass_id, pass_desc);
      }
    );
  }
};

} // namespace exodus::opt
