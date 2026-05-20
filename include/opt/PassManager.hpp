#pragma once

#include "AnalysisManager.hpp"
#include <memory>
#include <vector>

namespace exodus::opt {

template <typename IRUnitT>
class Pass {
  struct Concept { // NOLINT
    virtual ~Concept() = default;
    virtual PreservedAnalysis
    run(IRUnitT &ir, AnalysisManager<IRUnitT> &am) = 0;
  };

  template <typename PassT>
  struct Model : Concept {
    PassT pass;
    Model(PassT p) : pass(std::move(p)) {}
    PreservedAnalysis run(IRUnitT &ir, AnalysisManager<IRUnitT> &am) override {
      return pass.run(ir, am);
    }
  };

  // type erase
  std::unique_ptr<Concept> self;

public:
  template <typename PassT>
  Pass(PassT p) : self(std::make_unique<Model<PassT>>(std::move(p))) {}

  PreservedAnalysis run(IRUnitT &ir, AnalysisManager<IRUnitT> &am) {
    return self->run(ir, am);
  }
};

template <typename IRUnitT>
class PassManager {
  std::vector<Pass<IRUnitT>> pipeline;

public:
  template <typename PassT>
  void addPass(PassT p) {
    pipeline.emplace_back(std::move(p));
  }

  PreservedAnalysis run(IRUnitT &ir, AnalysisManager<IRUnitT> &am) {
    PreservedAnalysis combined_pa = PreservedAnalysis::all();
    for (auto &pass : pipeline) {
      PreservedAnalysis pa = pass.run(ir, am);
      am.invalidate(ir, pa);

      if (!pa.all_preserved()) {
        combined_pa = PreservedAnalysis::none();
      }
    }
    return combined_pa;
  }
};

using ModulePassManager = PassManager<high_ir::Module>;
using FunctionPassManager = PassManager<high_ir::Function>;

} // namespace exodus::opt
