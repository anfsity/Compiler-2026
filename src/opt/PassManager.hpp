#pragma once

#include "../helper/log.hpp"
#include "AnalysisManager.hpp"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace exodus::opt {

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

  auto set_after_pass_callback(
    std::function<void(const std::string &, IRUnitT &)> cb
  ) -> void {
    after_pass_cb = std::move(cb);
  }

  auto run(IRUnitT &ir, AnalysisManager<IRUnitT> &am) -> PreservedAnalysis {
    PreservedAnalysis combined_pa = PreservedAnalysis::all();
    for (auto &pass : pipeline) {
      ::exodus::Log::log_info("pass name: {}", pass.name());

      PreservedAnalysis pa = pass.run(ir, am);

      if (after_pass_cb) {
        after_pass_cb(pass.name(), ir);
      }

      am.invalidate(ir, pa);

      if (!pa.all_preserved()) {
        combined_pa = PreservedAnalysis::none();
      }
    }
    return combined_pa;
  }

  auto run_to_fixed_point(
    IRUnitT &ir, AnalysisManager<IRUnitT> &am, size_t max_iterations = 8
  ) -> PreservedAnalysis {
    bool changed = false;
    for (size_t iteration = 0; iteration < max_iterations; ++iteration) {
      auto pa = run(ir, am);
      if (pa.all_preserved())
        break;
      changed = true;
    }
    return changed ? PreservedAnalysis::none() : PreservedAnalysis::all();
  }
};

using ModulePassManager = PassManager<high_ir::Module>;
using FunctionPassManager = PassManager<high_ir::Function>;
using LinearFunctionPassManager = PassManager<::exodus::mid_ir::LinearFunction>;

} // namespace exodus::opt
