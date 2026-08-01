#include "pipeline_builder.hpp"
#include <functional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace {

using exodus::opt::PassRegistry;

template <typename PassMapT>
auto filter_pipeline(
  const std::vector<std::string> &pipeline,
  const PassMapT &passes,
  unsigned opt_level
) -> std::vector<std::string> {
  std::vector<std::string> filtered;
  filtered.reserve(pipeline.size());
  for (const auto &id : pipeline) {
    auto it = passes.find(id);
    if (it == passes.end())
      throw std::runtime_error("unregistered default pipeline pass: " + id);
    if (it->second.descriptor.min_opt_level <= opt_level)
      filtered.push_back(id);
  }
  return filtered;
}

template <typename PassMapT>
auto validate_pipeline(
  const std::vector<std::string> &pipeline,
  const PassMapT &passes,
  const char *scope,
  unsigned opt_level
) -> void {
  std::unordered_set<std::string> in_pipeline;
  std::unordered_map<std::string, size_t> position;
  for (size_t index = 0; index < pipeline.size(); ++index) {
    const auto &id = pipeline[index];
    if (!in_pipeline.insert(id).second)
      throw std::runtime_error(
        std::string("duplicate ") + scope + " pass in pipeline: " + id
      );
    position[id] = index;
    if (!passes.count(id))
      throw std::runtime_error(
        std::string("unregistered ") + scope + " pass in pipeline: " + id
      );
    if (passes.at(id).descriptor.min_opt_level > opt_level)
      throw std::runtime_error(
        std::string("pass requires a higher optimization level: ") + id
      );
  }

  for (const auto &id : pipeline) {
    for (const auto &dependency : passes.at(id).descriptor.dependencies) {
      if (!passes.count(dependency))
        throw std::runtime_error(
          std::string("unregistered dependency for ") + id + ": " + dependency
        );
      if (!in_pipeline.count(dependency))
        throw std::runtime_error(
          std::string("missing dependency for ") + id + ": " + dependency
        );
    }
  }

  std::unordered_set<std::string> visiting;
  std::unordered_set<std::string> visited;
  std::function<void(const std::string &)> visit = [&](const std::string &id) {
    if (visited.count(id))
      return;
    if (!visiting.insert(id).second)
      throw std::runtime_error(
        std::string("dependency cycle in ") + scope + " pipeline at: " + id
      );
    for (const auto &dependency : passes.at(id).descriptor.dependencies) {
      if (in_pipeline.count(dependency))
        visit(dependency);
    }
    visiting.erase(id);
    visited.insert(id);
  };
  for (const auto &id : pipeline)
    visit(id);

  for (const auto &id : pipeline) {
    for (const auto &dependency : passes.at(id).descriptor.dependencies) {
      if (position.at(dependency) > position.at(id))
        throw std::runtime_error(
          std::string("dependency appears after ") + id + ": " + dependency
        );
    }
  }
}

auto default_high_module_pipeline() -> const std::vector<std::string> & {
  static const std::vector<std::string> pipeline{
    "guarded_bitwise_idiom",
    "ipcp",
    "global_init_promotion",
    "idempotent_loop_simplify",
    "pure_call_loop_sink",
    "inliner",
    "dead_function_elimination",
    "memoization",
  };
  return pipeline;
}

auto default_high_function_pipeline() -> const std::vector<std::string> & {
  static const std::vector<std::string> pipeline{
    "return_insertion",
    "loop_unswitch",
    "local_array_loop_specialize",
    "const_prop",
    "inst_simplify",
    "loop_recurrence_simplify",
    "region_simplify",
    "simple_dce",
  };
  return pipeline;
}

auto default_mid_function_pipeline() -> const std::vector<std::string> & {
  static const std::vector<std::string> pipeline{
    "immutable_pointer_slot_canonicalize",
    "mem2reg",
    "tail_recursion_elim",
    "mid_inliner",
    "instcombine",
    "loop_simplify",
    "licm",
    "gvn",
    "early_dce",
    "monotonic_guard_tighten",
    "polyhedral_opt",
    "loop_strength_reduce",
    "loop_idiom_recognize",
    "loop_unroll",
    "cfg_simplify",
    "dce",
  };
  return pipeline;
}

auto default_mid_module_pipeline() -> const std::vector<std::string> & {
  static const std::vector<std::string> pipeline{"dead_functions"};
  return pipeline;
}

} // namespace

namespace exodus::opt {

auto PipelineBuilder::build_function_pipeline() -> FunctionPassManager {
  return build_function_pipeline(2);
}

auto PipelineBuilder::build_function_pipeline(unsigned opt_level)
  -> FunctionPassManager {
  const auto &passes = PassRegistry::instance().get_function_passes();
  return build_function_pipeline(
    filter_pipeline(default_high_function_pipeline(), passes, opt_level),
    opt_level
  );
}

auto PipelineBuilder::build_function_pipeline(
  const std::vector<std::string> &pipeline, unsigned opt_level
) -> FunctionPassManager {
  FunctionPassManager fpm;
  const auto &passes = PassRegistry::instance().get_function_passes();
  validate_pipeline(pipeline, passes, "high function", opt_level);
  for (const auto &id : pipeline) {
    const auto &info = passes.at(id);
    fpm.add_pass(pass_builder.create_function_pass(id), id, info.desc());
  }
  return fpm;
}

auto PipelineBuilder::build_module_pipeline() -> ModulePassManager {
  return build_module_pipeline(2);
}

auto PipelineBuilder::build_module_pipeline(unsigned opt_level)
  -> ModulePassManager {
  const auto &passes = PassRegistry::instance().get_module_passes();
  return build_module_pipeline(
    filter_pipeline(default_high_module_pipeline(), passes, opt_level),
    opt_level
  );
}

auto PipelineBuilder::build_module_pipeline(
  const std::vector<std::string> &pipeline, unsigned opt_level
) -> ModulePassManager {
  ModulePassManager mpm;
  const auto &passes = PassRegistry::instance().get_module_passes();
  validate_pipeline(pipeline, passes, "high module", opt_level);
  for (const auto &id : pipeline) {
    const auto &info = passes.at(id);
    mpm.add_pass(pass_builder.create_module_pass(id), id, info.desc());
  }
  return mpm;
}

auto PipelineBuilder::build_linear_function_pipeline()
  -> LinearFunctionPassManager {
  return build_linear_function_pipeline(2);
}

auto PipelineBuilder::build_linear_function_pipeline(unsigned opt_level)
  -> LinearFunctionPassManager {
  const auto &passes = PassRegistry::instance().get_linear_function_passes();
  return build_linear_function_pipeline(
    filter_pipeline(default_mid_function_pipeline(), passes, opt_level),
    opt_level
  );
}

auto PipelineBuilder::build_linear_function_pipeline(
  const std::vector<std::string> &pipeline, unsigned opt_level
) -> LinearFunctionPassManager {
  LinearFunctionPassManager lfpm;
  const auto &passes = PassRegistry::instance().get_linear_function_passes();
  validate_pipeline(pipeline, passes, "mid function", opt_level);
  for (const auto &id : pipeline) {
    const auto &info = passes.at(id);
    lfpm.add_pass(
      pass_builder.create_linear_function_pass(id), id, info.desc()
    );
  }
  return lfpm;
}

auto PipelineBuilder::build_mid_module_pipeline() -> MidModulePassManager {
  return build_mid_module_pipeline(2);
}

auto PipelineBuilder::build_mid_module_pipeline(unsigned opt_level)
  -> MidModulePassManager {
  const auto &passes = PassRegistry::instance().get_mid_module_passes();
  return build_mid_module_pipeline(
    filter_pipeline(default_mid_module_pipeline(), passes, opt_level), opt_level
  );
}

auto PipelineBuilder::build_mid_module_pipeline(
  const std::vector<std::string> &pipeline, unsigned opt_level
) -> MidModulePassManager {
  MidModulePassManager mpm;
  const auto &passes = PassRegistry::instance().get_mid_module_passes();
  validate_pipeline(pipeline, passes, "mid module", opt_level);
  for (const auto &id : pipeline) {
    const auto &info = passes.at(id);
    mpm.add_pass(pass_builder.create_mid_module_pass(id), id, info.desc());
  }
  return mpm;
}

} // namespace exodus::opt
