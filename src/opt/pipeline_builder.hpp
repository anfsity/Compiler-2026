#pragma once

#include "PassBuilder.hpp"
#include <string>
#include <vector>

namespace exodus::opt {

// Builds explicit optimization pipelines from registered pass descriptors.
// PassBuilder remains the pass factory; pipeline order and validation live
// here.
class PipelineBuilder {
  PassBuilder pass_builder;

public:
  PipelineBuilder(
    high_ir::Module *module, ::exodus::mid_ir::MidModule *mid_module = nullptr
  )
      : pass_builder(module, mid_module) {}

  auto build_function_pipeline() -> FunctionPassManager;
  auto build_function_pipeline(unsigned opt_level) -> FunctionPassManager;
  auto build_function_pipeline(
    const std::vector<std::string> &pipeline, unsigned opt_level = 2
  ) -> FunctionPassManager;

  auto build_module_pipeline() -> ModulePassManager;
  auto build_module_pipeline(unsigned opt_level) -> ModulePassManager;
  auto build_module_pipeline(
    const std::vector<std::string> &pipeline, unsigned opt_level = 2
  ) -> ModulePassManager;

  auto build_linear_function_pipeline() -> LinearFunctionPassManager;
  auto build_linear_function_pipeline(unsigned opt_level)
    -> LinearFunctionPassManager;
  auto build_linear_function_pipeline(
    const std::vector<std::string> &pipeline, unsigned opt_level = 2
  ) -> LinearFunctionPassManager;

  auto build_mid_module_pipeline() -> MidModulePassManager;
  auto build_mid_module_pipeline(unsigned opt_level) -> MidModulePassManager;
  auto build_mid_module_pipeline(
    const std::vector<std::string> &pipeline, unsigned opt_level = 2
  ) -> MidModulePassManager;
};

} // namespace exodus::opt
