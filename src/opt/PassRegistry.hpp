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
  using LinearFunctionPassCreator = std::function<
    Pass<::exodus::mid_ir::LinearFunction>(::exodus::mid_ir::MidModule *)>;

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

  auto register_function_pass(
    std::string &id, std::string desc, FunctionPassCreator creator
  ) -> void {
    if (!function_passes.count(id)) {
      function_order.push_back(id);
    }
    function_passes[id] =
      PassInfo<FunctionPassCreator>{std::move(desc), std::move(creator)};
  }

  auto register_module_pass(
    std::string &id, std::string desc, ModulePassCreator creator
  ) -> void {
    if (!module_passes.count(id)) {
      module_order.push_back(id);
    }
    module_passes[id] =
      PassInfo<ModulePassCreator>{std::move(desc), std::move(creator)};
  }

  auto register_linear_function_pass(
    std::string &id, std::string desc, LinearFunctionPassCreator creator
  ) -> void {
    if (!linear_function_passes.count(id)) {
      linear_function_order.push_back(id);
    }
    linear_function_passes[id] =
      PassInfo<LinearFunctionPassCreator>{std::move(desc), std::move(creator)};
  }

  auto get_function_passes() const
    -> const std::map<std::string, PassInfo<FunctionPassCreator>> & {
    return function_passes;
  }

  auto get_module_passes() const
    -> const std::map<std::string, PassInfo<ModulePassCreator>> & {
    return module_passes;
  }

  auto get_linear_function_passes() const
    -> const std::map<std::string, PassInfo<LinearFunctionPassCreator>> & {
    return linear_function_passes;
  }

  auto get_function_order() const -> const std::vector<std::string> & {
    return function_order;
  }

  auto get_module_order() const -> const std::vector<std::string> & {
    return module_order;
  }

  auto get_linear_function_order() const -> const std::vector<std::string> & {
    return linear_function_order;
  }

private:
  PassRegistry() = default;

  std::map<std::string, PassInfo<FunctionPassCreator>> function_passes;
  std::map<std::string, PassInfo<ModulePassCreator>> module_passes;
  std::map<std::string, PassInfo<LinearFunctionPassCreator>>
    linear_function_passes;
  std::vector<std::string> function_order;
  std::vector<std::string> module_order;
  std::vector<std::string> linear_function_order;
};

template <typename PassT>
struct RegisterFunctionPass {
  RegisterFunctionPass(std::string id, std::string desc) {
    auto pass_id = std::move(id);
    auto pass_desc = std::move(desc);
    PassRegistry::instance().register_function_pass(
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
    PassRegistry::instance().register_module_pass(
      pass_id, pass_desc, [pass_id, pass_desc](high_ir::Module *m) {
        return Pass<high_ir::Module>(PassT(m), pass_id, pass_desc);
      }
    );
  }
};

template <typename PassT>
struct RegisterLinearFunctionPass {
  RegisterLinearFunctionPass(std::string id, std::string desc) {
    auto pass_id = std::move(id);
    auto pass_desc = std::move(desc);
    PassRegistry::instance().register_linear_function_pass(
      pass_id, pass_desc, [pass_id, pass_desc](exodus::mid_ir::MidModule *m) {
        return Pass<::exodus::mid_ir::LinearFunction>(
          PassT(m), pass_id, pass_desc
        );
      }
    );
  }
};

} // namespace exodus::opt
