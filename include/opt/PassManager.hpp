#pragma once

#include "AnalysisManager.hpp"
#include "helper/log.hpp"
#include <memory>
#include <string>
#include <vector>

namespace exodus::opt {

template <typename IRUnitT>
class Pass {
  struct Concept { // NOLINT
    virtual ~Concept() = default;
    virtual PreservedAnalysis
    run(IRUnitT &ir, AnalysisManager<IRUnitT> &am) = 0;
    virtual std::string name() const = 0;
    virtual std::string desc() const = 0;
  };

  template <typename PassT>
  struct Model : Concept {
    PassT pass;
    std::string _name;
    std::string _desc;

    Model(PassT p, std::string n, std::string d)
        : pass(std::move(p)), _name(std::move(n)), _desc(std::move(d)) {}

    PreservedAnalysis run(IRUnitT &ir, AnalysisManager<IRUnitT> &am) override {
      return pass.run(ir, am);
    }

    std::string name() const override { return _name; }
    std::string desc() const override { return _desc; }
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

  PreservedAnalysis run(IRUnitT &ir, AnalysisManager<IRUnitT> &am) {
    return self->run(ir, am);
  }

  std::string name() const { return self->name(); }
  std::string desc() const { return self->desc(); }
};

template <typename IRUnitT>
class PassManager {
  std::vector<Pass<IRUnitT>> pipeline;

public:
  template <typename PassT>
  void addPass(PassT p, std::string name, std::string desc) {
    pipeline.emplace_back(std::move(p), std::move(name), std::move(desc));
  }

  PreservedAnalysis run(IRUnitT &ir, AnalysisManager<IRUnitT> &am) {
    PreservedAnalysis combined_pa = PreservedAnalysis::all();
    for (auto &pass : pipeline) {

      Log::log_info("pass name: {}", pass.name());

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
