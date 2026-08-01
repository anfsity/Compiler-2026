#pragma once

#include "../helper/log.hpp"
#include "AnalysisManager.hpp"
#include "PassContext.hpp"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace exodus::opt {

struct PassRunResult {
  bool changed = false;
  PreservedAnalysis preserved = PreservedAnalysis::all();

  auto all_preserved() const -> bool { return preserved.all_preserved(); }
};

struct FixedPointResult {
  bool changed_this_iteration = false;
  bool changed_any = false;
  bool stable = true;
  bool reached_iteration_limit = false;
  size_t iterations = 0;
  PreservedAnalysis preserved = PreservedAnalysis::all();

  auto all_preserved() const -> bool {
    return stable && !changed_any && preserved.all_preserved();
  }
};

class FixedPointContext {
public:
  explicit FixedPointContext(size_t max_iterations)
      : iteration_limit(max_iterations) {
    if (max_iterations == 0) {
      result.stable = false;
      result.reached_iteration_limit = true;
    }
  }

  auto can_run_iteration() const -> bool {
    return result.iterations < iteration_limit;
  }

  auto record_iteration(PassRunResult run) -> void {
    ++result.iterations;
    result.changed_this_iteration = run.changed;
    result.preserved.intersect(run.preserved);
    if (!run.changed)
      return;

    result.changed_any = true;
    result.preserved = PreservedAnalysis::none();
    if (result.iterations == iteration_limit) {
      result.stable = false;
      result.reached_iteration_limit = true;
    }
  }

  auto current_result() const -> FixedPointResult { return result; }

private:
  size_t iteration_limit = 0;
  FixedPointResult result;
};

template <typename IRUnitT>
class Pass {
  struct Concept { // NOLINT
    virtual ~Concept() = default;
    virtual auto run(IRUnitT &ir, AnalysisManager<IRUnitT> &am)
      -> PreservedAnalysis = 0;
    virtual auto name() const -> std::string = 0;
    virtual auto desc() const -> std::string = 0;
  };

  template <typename PassT>
  struct Model : Concept {
    PassT pass;
    std::string _name;
    std::string _desc;

    Model(PassT p, std::string n, std::string d)
        : pass(std::move(p)), _name(std::move(n)), _desc(std::move(d)) {}

    auto run(IRUnitT &ir, AnalysisManager<IRUnitT> &am)
      -> PreservedAnalysis override {
      return pass.run(ir, am);
    }

    auto name() const -> std::string override { return _name; }
    auto desc() const -> std::string override { return _desc; }
  };

  // type erase
  std::unique_ptr<Concept> self;

public:
  template <typename PassT>
  Pass(PassT p, std::string name, std::string desc)
      : self(
          std::make_unique<Model<PassT>>(
            std::move(p), std::move(name), std::move(desc)
          )
        ) {}

  auto run(IRUnitT &ir, AnalysisManager<IRUnitT> &am) -> PreservedAnalysis {
    return self->run(ir, am);
  }

  auto run(PassContext<IRUnitT> &context) -> PreservedAnalysis {
    auto preserved = run(context.ir(), context.analysis());
    context.after_pass(name());
    context.invalidate(preserved);
    context.verify(name());
    return preserved;
  }

  auto name() const -> std::string { return self->name(); }
  auto desc() const -> std::string { return self->desc(); }
};

template <typename IRUnitT>
class PassManager {
  std::vector<Pass<IRUnitT>> pipeline;
  std::function<void(const std::string &, IRUnitT &)> after_pass_cb;

public:
  template <typename PassT>
  auto add_pass(PassT p, std::string name, std::string desc) -> void {
    pipeline.emplace_back(std::move(p), std::move(name), std::move(desc));
  }

  auto size() const -> size_t { return pipeline.size(); }

  auto set_after_pass_callback(
    std::function<void(const std::string &, IRUnitT &)> cb
  ) -> void {
    after_pass_cb = std::move(cb);
  }

  auto run(IRUnitT &ir, AnalysisManager<IRUnitT> &am) -> PreservedAnalysis {
    auto context = make_context(ir, am);
    return run(context);
  }

  auto run(PassContext<IRUnitT> &context) -> PreservedAnalysis {
    PreservedAnalysis combined_pa = PreservedAnalysis::all();
    for (auto &pass : pipeline) {
      ::exodus::Log::log_info("pass name: {}", pass.name());

      PreservedAnalysis pa = pass.run(context.ir(), context.analysis());

      context.after_pass(pass.name());
      context.invalidate(pa);
      context.verify(pass.name());

      combined_pa.intersect(pa);
    }
    return combined_pa;
  }

  auto run_with_result(IRUnitT &ir, AnalysisManager<IRUnitT> &am)
    -> PassRunResult {
    auto preserved = run(ir, am);
    return {!preserved.all_preserved(), preserved};
  }

  auto run_with_result(PassContext<IRUnitT> &context) -> PassRunResult {
    auto preserved = run(context);
    return {!preserved.all_preserved(), preserved};
  }

  auto run_to_fixed_point(
    IRUnitT &ir, AnalysisManager<IRUnitT> &am, size_t max_iterations = 8
  ) -> FixedPointResult {
    FixedPointContext fixed_point(max_iterations);
    while (fixed_point.can_run_iteration()) {
      auto pa = run(ir, am);
      PassRunResult iteration_result{!pa.all_preserved(), pa};
      fixed_point.record_iteration(iteration_result);
      if (!iteration_result.changed)
        break;
    }
    return fixed_point.current_result();
  }

  auto
  run_to_fixed_point(PassContext<IRUnitT> &context, size_t max_iterations = 8)
    -> FixedPointResult {
    FixedPointContext fixed_point(max_iterations);
    while (fixed_point.can_run_iteration()) {
      auto result = run_with_result(context);
      fixed_point.record_iteration(result);
      if (!result.changed)
        break;
    }
    return fixed_point.current_result();
  }

private:
  auto make_context(IRUnitT &ir, AnalysisManager<IRUnitT> &am)
    -> PassContext<IRUnitT> {
    return PassContext<IRUnitT>(ir, am, {}, after_pass_cb);
  }
};

using ModulePassManager = PassManager<high_ir::Module>;
using FunctionPassManager = PassManager<high_ir::Function>;
using LinearFunctionPassManager = PassManager<::exodus::mid_ir::LinearFunction>;
using MidModulePassManager = PassManager<::exodus::mid_ir::MidModule>;

} // namespace exodus::opt
