#pragma once

#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "PassManager.hpp"

namespace exodus::opt {

class PassRegistry {
public:
  enum class Scope {
    HighFunction,
    HighModule,
    MidFunction,
    MidModule,
  };

  enum class Stage {
    Canonicalize,
    Interprocedural,
    Scalar,
    Loop,
    CFG,
    Cleanup,
  };

  struct PassDescriptor {
    std::string id;
    std::string desc;
    Scope scope = Scope::HighFunction;
    Stage stage = Stage::Scalar;
    unsigned min_opt_level = 1;
    std::vector<std::string> dependencies;
    std::vector<std::string> required_analyses;
  };

  using FunctionPassCreator =
    std::function<Pass<high_ir::Function>(high_ir::Module *)>;
  using ModulePassCreator =
    std::function<Pass<high_ir::Module>(high_ir::Module *)>;
  using LinearFunctionPassCreator = std::function<
    Pass<::exodus::mid_ir::LinearFunction>(::exodus::mid_ir::MidModule *)>;
  using MidModulePassCreator = std::function<
    Pass<::exodus::mid_ir::MidModule>(::exodus::mid_ir::MidModule *)>;

  template <typename CreatorT>
  struct PassInfo {
    PassDescriptor descriptor;
    CreatorT creator;

    auto desc() const -> const std::string & { return descriptor.desc; }
  };

public:
  static auto instance() -> PassRegistry & {
    static PassRegistry inst;
    return inst;
  }

  auto register_function_pass(
    std::string id,
    std::string desc,
    FunctionPassCreator creator,
    Stage stage = Stage::Scalar,
    unsigned min_opt_level = 1,
    std::vector<std::string> dependencies = {},
    std::vector<std::string> required_analyses = {}
  ) -> void {
    auto key = id;
    function_passes[key] = PassInfo<FunctionPassCreator>{
      {std::move(id),
       std::move(desc),
       Scope::HighFunction,
       stage,
       min_opt_level,
       std::move(dependencies),
       std::move(required_analyses)},
      std::move(creator)
    };
  }

  auto register_module_pass(
    std::string id,
    std::string desc,
    ModulePassCreator creator,
    Stage stage = Stage::Interprocedural,
    unsigned min_opt_level = 1,
    std::vector<std::string> dependencies = {},
    std::vector<std::string> required_analyses = {}
  ) -> void {
    auto key = id;
    module_passes[key] = PassInfo<ModulePassCreator>{
      {std::move(id),
       std::move(desc),
       Scope::HighModule,
       stage,
       min_opt_level,
       std::move(dependencies),
       std::move(required_analyses)},
      std::move(creator)
    };
  }

  auto register_linear_function_pass(
    std::string id,
    std::string desc,
    LinearFunctionPassCreator creator,
    Stage stage = Stage::Scalar,
    unsigned min_opt_level = 1,
    std::vector<std::string> dependencies = {},
    std::vector<std::string> required_analyses = {}
  ) -> void {
    auto key = id;
    linear_function_passes[key] = PassInfo<LinearFunctionPassCreator>{
      {std::move(id),
       std::move(desc),
       Scope::MidFunction,
       stage,
       min_opt_level,
       std::move(dependencies),
       std::move(required_analyses)},
      std::move(creator)
    };
  }

  auto register_mid_module_pass(
    std::string id,
    std::string desc,
    MidModulePassCreator creator,
    Stage stage = Stage::Interprocedural,
    unsigned min_opt_level = 1,
    std::vector<std::string> dependencies = {},
    std::vector<std::string> required_analyses = {}
  ) -> void {
    auto key = id;
    mid_module_passes[key] = PassInfo<MidModulePassCreator>{
      {std::move(id),
       std::move(desc),
       Scope::MidModule,
       stage,
       min_opt_level,
       std::move(dependencies),
       std::move(required_analyses)},
      std::move(creator)
    };
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

  auto get_mid_module_passes() const
    -> const std::map<std::string, PassInfo<MidModulePassCreator>> & {
    return mid_module_passes;
  }

private:
  PassRegistry() = default;

  std::map<std::string, PassInfo<FunctionPassCreator>> function_passes;
  std::map<std::string, PassInfo<ModulePassCreator>> module_passes;
  std::map<std::string, PassInfo<LinearFunctionPassCreator>>
    linear_function_passes;
  std::map<std::string, PassInfo<MidModulePassCreator>> mid_module_passes;
};

template <typename PassT>
struct RegisterFunctionPass {
  RegisterFunctionPass(
    std::string id,
    std::string desc,
    PassRegistry::Stage stage = PassRegistry::Stage::Scalar,
    unsigned min_opt_level = 1,
    std::vector<std::string> dependencies = {},
    std::vector<std::string> required_analyses = {}
  ) {
    auto pass_id = std::move(id);
    auto pass_desc = std::move(desc);
    PassRegistry::instance().register_function_pass(
      pass_id,
      pass_desc,
      [pass_id, pass_desc](high_ir::Module *m) {
        return Pass<high_ir::Function>(PassT(m), pass_id, pass_desc);
      },
      stage,
      min_opt_level,
      std::move(dependencies),
      std::move(required_analyses)
    );
  }
};

template <typename PassT>
struct RegisterModulePass {
  RegisterModulePass(
    std::string id,
    std::string desc,
    PassRegistry::Stage stage = PassRegistry::Stage::Interprocedural,
    unsigned min_opt_level = 1,
    std::vector<std::string> dependencies = {},
    std::vector<std::string> required_analyses = {}
  ) {
    auto pass_id = std::move(id);
    auto pass_desc = std::move(desc);
    PassRegistry::instance().register_module_pass(
      pass_id,
      pass_desc,
      [pass_id, pass_desc](high_ir::Module *m) {
        return Pass<high_ir::Module>(PassT(m), pass_id, pass_desc);
      },
      stage,
      min_opt_level,
      std::move(dependencies),
      std::move(required_analyses)
    );
  }
};

template <typename PassT>
struct RegisterLinearFunctionPass {
  RegisterLinearFunctionPass(
    std::string id,
    std::string desc,
    PassRegistry::Stage stage = PassRegistry::Stage::Scalar,
    unsigned min_opt_level = 1,
    std::vector<std::string> dependencies = {},
    std::vector<std::string> required_analyses = {}
  ) {
    auto pass_id = std::move(id);
    auto pass_desc = std::move(desc);
    PassRegistry::instance().register_linear_function_pass(
      pass_id,
      pass_desc,
      [pass_id, pass_desc](exodus::mid_ir::MidModule *m) {
        return Pass<::exodus::mid_ir::LinearFunction>(
          PassT(m), pass_id, pass_desc
        );
      },
      stage,
      min_opt_level,
      std::move(dependencies),
      std::move(required_analyses)
    );
  }
};

template <typename PassT>
struct RegisterMidModulePass {
  RegisterMidModulePass(
    std::string id,
    std::string desc,
    PassRegistry::Stage stage = PassRegistry::Stage::Interprocedural,
    unsigned min_opt_level = 1,
    std::vector<std::string> dependencies = {},
    std::vector<std::string> required_analyses = {}
  ) {
    auto pass_id = std::move(id);
    auto pass_desc = std::move(desc);
    PassRegistry::instance().register_mid_module_pass(
      pass_id,
      pass_desc,
      [pass_id, pass_desc](exodus::mid_ir::MidModule *m) {
        return Pass<::exodus::mid_ir::MidModule>(PassT(m), pass_id, pass_desc);
      },
      stage,
      min_opt_level,
      std::move(dependencies),
      std::move(required_analyses)
    );
  }
};

} // namespace exodus::opt
