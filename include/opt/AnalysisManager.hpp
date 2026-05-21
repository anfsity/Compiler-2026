#pragma once

#include "../high/ir.hpp"
#include <functional>
#include <memory>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>

namespace exodus::opt {

struct PreservedAnalysis {
  std::unordered_set<std::type_index> presvd_ids;
  bool all_presvd = false;

  static auto all() -> PreservedAnalysis {
    PreservedAnalysis pa;
    pa.all_presvd = true;
    return pa;
  }

  static auto none() -> PreservedAnalysis { return {}; }

  template <typename AnalysisT>
  void preserve() {
    presvd_ids.insert(typeid(AnalysisT));
  }

  bool is_presvd(std::type_index id) const {
    return all_presvd || presvd_ids.count(id);
  }

  bool all_preserved() const { return all_presvd; }
};

struct AnalysisResult {
  template <typename PassT>
  AnalysisResult(PassT, typename PassT::Result &&res)
      : self(std::make_unique<Model<PassT>>(std::move(res))) {}

  bool invalidate(void *ir, const PreservedAnalysis &pa) {
    return self->invalidate(ir, pa);
  }

  template <typename PassT>
  auto get() -> typename PassT::Result & {
    return static_cast<Model<PassT> *>(self.get())->result;
  }

private:
  struct Concept { // NOLINT
    virtual ~Concept() = default;
    virtual bool invalidate(void *ir, const PreservedAnalysis &pa) = 0;
  };

  template <typename PassT>
  struct Model : Concept {
    typename PassT::Result result;
    Model(typename PassT::Result &&res) : result(std::move(res)) {}

    bool invalidate(void *, const PreservedAnalysis &pa) override {
      return !pa.is_presvd(typeid(PassT));
    }
  };

  std::unique_ptr<Concept> self;
};

template <typename IRUnitT>
struct AnalysisManager {
  template <typename PassT>
  void registerPass() {
    generators[typeid(PassT)] = [](IRUnitT &ir, AnalysisManager &am) {
      return AnalysisResult(PassT{}, PassT{}.run(ir, am));
    };
  }

  template <typename PassT>
  auto getResult(IRUnitT &ir) -> typename PassT::Result & {
    auto it = cache.find(typeid(PassT));
    if (it == cache.end()) {
      it = cache.emplace(typeid(PassT), generators.at(typeid(PassT))(ir, *this))
             .first;
    }
    return it->second.template get<PassT>();
  }

  void invalidate(IRUnitT &ir, const PreservedAnalysis &pa) {
    if (pa.all_preserved())
      return;
    auto it = cache.begin();
    while (it != cache.end()) {
      if (it->second.invalidate(&ir, pa)) {
        it = cache.erase(it);
      } else {
        ++it;
      }
    }
  }

  void clear() { cache.clear(); }

private:
  using Generator = std::function<AnalysisResult(IRUnitT &, AnalysisManager &)>;
  std::unordered_map<std::type_index, Generator> generators;
  std::unordered_map<std::type_index, AnalysisResult> cache;
};

using FunctionAnalysisManager = AnalysisManager<high_ir::Function>;
using ModuleAnalysisManager = AnalysisManager<high_ir::Module>;

} // namespace exodus::opt
