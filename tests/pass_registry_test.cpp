#ifdef EXODUS_UNIT_TEST
#include "../src/opt/PassBuilder.hpp"
#include "../src/opt/pipeline_builder.hpp"
#include <cassert>
#include <iostream>
#include <stdexcept>

namespace exodus::mid_ir::opt {
auto register_passes() -> void;
}
namespace exodus::high_ir::opt {
auto register_passes() -> void;
}

using namespace exodus::mid_ir;
using namespace exodus::ir;

namespace {

struct NoopMidFunctionPass {
  auto run(LinearFunction &, exodus::opt::LinearFunctionAnalysisManager &)
    -> exodus::opt::PreservedAnalysis {
    return exodus::opt::PreservedAnalysis::all();
  }
};

auto make_noop_mid_function_pass(std::string id, std::string desc) {
  return [id = std::move(id), desc = std::move(desc)](MidModule *) {
    return exodus::opt::Pass<LinearFunction>(NoopMidFunctionPass{}, id, desc);
  };
}

} // namespace

auto make_function(std::string name, bool is_decl = false)
  -> std::unique_ptr<LinearFunction> {
  auto function = std::make_unique<LinearFunction>();
  function->name = std::move(name);
  function->is_decl = is_decl;
  return function;
}

auto test_mid_dead_functions_registered_as_module_pass() -> void {
  exodus::mid_ir::opt::register_passes();

  IRContext ctx;
  MidModule module;
  module.ctx = &ctx;

  auto main = make_function("main");
  auto live = make_function("live");
  auto dead = make_function("dead");
  auto *p_live = live.get();

  auto entry = std::make_unique<Block>(0, "entry");
  auto *call = module.make_op(OpCode::Call, CallPayload{"live"});
  entry->insts.push_back(call);
  auto *ret = module.make_op(OpCode::Ret);
  entry->insts.push_back(ret);
  main->blocks.push_back(std::move(entry));

  module.functions.push_back(std::move(main));
  module.functions.push_back(std::move(live));
  module.functions.push_back(std::move(dead));

  exodus::opt::PassBuilder builder(nullptr, &module);
  assert(builder.is_mid_module_pass("dead_functions"));
  auto pass = builder.create_mid_module_pass("dead_functions");
  exodus::opt::MidModuleAnalysisManager am;
  auto preserved = pass.run(module, am);

  assert(!preserved.all_preserved());
  assert(module.functions.size() == 2);
  assert(module.functions[1].get() == p_live);
  std::cout << "test_mid_dead_functions_registered_as_module_pass passed!\n";
}

auto test_pipeline_dependency_validation() -> void {
  auto &registry = exodus::opt::PassRegistry::instance();
  registry.register_linear_function_pass(
    "__test_dependency",
    "dependency",
    make_noop_mid_function_pass("__test_dependency", "dependency")
  );
  registry.register_linear_function_pass(
    "__test_needs_dependency",
    "needs dependency",
    make_noop_mid_function_pass("__test_needs_dependency", "needs dependency"),
    exodus::opt::PassRegistry::Stage::Scalar,
    1,
    {"__test_dependency"}
  );

  MidModule module;
  exodus::opt::PipelineBuilder builder(nullptr, &module);
  builder.build_linear_function_pipeline(
    {"__test_dependency", "__test_needs_dependency"}
  );

  bool missing_dependency = false;
  try {
    builder.build_linear_function_pipeline({"__test_needs_dependency"});
  } catch (const std::runtime_error &) {
    missing_dependency = true;
  }
  assert(missing_dependency);

  registry.register_linear_function_pass(
    "__test_cycle_a",
    "cycle a",
    make_noop_mid_function_pass("__test_cycle_a", "cycle a"),
    exodus::opt::PassRegistry::Stage::Scalar,
    1,
    {"__test_cycle_b"}
  );
  registry.register_linear_function_pass(
    "__test_cycle_b",
    "cycle b",
    make_noop_mid_function_pass("__test_cycle_b", "cycle b"),
    exodus::opt::PassRegistry::Stage::Scalar,
    1,
    {"__test_cycle_a"}
  );

  bool dependency_cycle = false;
  try {
    builder.build_linear_function_pipeline(
      {"__test_cycle_a", "__test_cycle_b"}
    );
  } catch (const std::runtime_error &) {
    dependency_cycle = true;
  }
  assert(dependency_cycle);

  std::cout << "test_pipeline_dependency_validation passed!\n";
}

auto test_descriptor_optimization_level_validation() -> void {
  auto &registry = exodus::opt::PassRegistry::instance();
  registry.register_linear_function_pass(
    "__test_o2_only",
    "optimization-level descriptor",
    make_noop_mid_function_pass(
      "__test_o2_only", "optimization-level descriptor"
    ),
    exodus::opt::PassRegistry::Stage::Loop,
    2,
    {},
    {"LoopAnalysis"}
  );

  MidModule module;
  exodus::opt::PipelineBuilder builder(nullptr, &module);
  bool rejected = false;
  try {
    builder.build_linear_function_pipeline({"__test_o2_only"}, 1);
  } catch (const std::runtime_error &) {
    rejected = true;
  }
  assert(rejected);

  auto it = registry.get_linear_function_passes().find("__test_o2_only");
  assert(it != registry.get_linear_function_passes().end());
  assert(it->second.descriptor.stage == exodus::opt::PassRegistry::Stage::Loop);
  assert(it->second.descriptor.required_analyses.size() == 1);
  builder.build_linear_function_pipeline({"__test_o2_only"}, 2);
  std::cout << "test_descriptor_optimization_level_validation passed!\n";
}

auto test_default_pipeline_respects_optimization_level() -> void {
  exodus::high_ir::opt::register_passes();
  exodus::mid_ir::opt::register_passes();

  IRContext ctx;
  MidModule module;
  module.ctx = &ctx;
  exodus::high_ir::Module high_module;
  exodus::opt::PipelineBuilder builder(&high_module, &module);

  assert(builder.build_function_pipeline(0).size() == 0);
  assert(builder.build_module_pipeline(0).size() == 0);
  assert(builder.build_linear_function_pipeline(0).size() == 0);
  assert(builder.build_mid_module_pipeline(0).size() == 0);

  assert(builder.build_function_pipeline(1).size() > 0);
  assert(builder.build_module_pipeline(1).size() > 0);
  assert(builder.build_linear_function_pipeline(1).size() > 0);
  assert(builder.build_mid_module_pipeline(1).size() > 0);
  std::cout << "test_default_pipeline_respects_optimization_level passed!\n";
}

auto main() -> int {
  test_mid_dead_functions_registered_as_module_pass();
  test_pipeline_dependency_validation();
  test_descriptor_optimization_level_validation();
  test_default_pipeline_respects_optimization_level();
  return 0;
}
#endif
