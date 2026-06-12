#pragma once

#include "../high/ir.hpp"
#include "../mid/ir.hpp"
#include <functional>
#include <memory>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <utility>

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
  auto preserve() -> void {
    presvd_ids.insert(typeid(AnalysisT));
  }

  auto is_presvd(std::type_index id) const -> bool {
    return all_presvd || presvd_ids.count(id);
  }

  auto all_preserved() const -> bool { return all_presvd; }
};

struct AnalysisResult {
  template <typename PassT>
  AnalysisResult(PassT, typename PassT::Result &&res)
      : self(std::make_unique<Model<PassT>>(std::move(res))) {}

  auto invalidate(void *ir, const PreservedAnalysis &pa) -> bool {
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

    auto invalidate(void *, const PreservedAnalysis &pa) -> bool override {
      return !pa.is_presvd(typeid(PassT));
    }
  };

  std::unique_ptr<Concept> self;
};

template <typename IRUnitT>
struct AnalysisManager {
  template <typename PassT>
  auto registerPass() -> void {
    generators[typeid(PassT)] = [](IRUnitT &ir, AnalysisManager &am) {
      return AnalysisResult(PassT{}, PassT{}.run(ir, am));
    };
  }

  template <typename PassT>
  auto getResult(IRUnitT &ir) -> typename PassT::Result & {
    CacheKey key{&ir, typeid(PassT)};
    auto it = cache.find(key);
    if (it == cache.end()) {
      it = cache.emplace(key, generators.at(typeid(PassT))(ir, *this)).first;
    }
    return it->second.template get<PassT>();
  }

  auto invalidate(IRUnitT &ir, const PreservedAnalysis &pa) -> void {
    if (pa.all_preserved())
      return;
    auto it = cache.begin();
    while (it != cache.end()) {
      if (it->first.ir == &ir && it->second.invalidate(&ir, pa)) {
        it = cache.erase(it);
      } else {
        ++it;
      }
    }
  }

  auto clear() -> void { cache.clear(); }

private:
  struct CacheKey {
    IRUnitT *ir;
    std::type_index pass;

    auto operator==(const CacheKey &other) const -> bool {
      return ir == other.ir && pass == other.pass;
    }
  };

  struct CacheKeyHash {
    auto operator()(const CacheKey &key) const -> size_t {
      return std::hash<IRUnitT *>{}(key.ir) ^
             (std::hash<std::type_index>{}(key.pass) << 1);
    }
  };

  using Generator = std::function<AnalysisResult(IRUnitT &, AnalysisManager &)>;
  std::unordered_map<std::type_index, Generator> generators;
  std::unordered_map<CacheKey, AnalysisResult, CacheKeyHash> cache;
};

using FunctionAnalysisManager = AnalysisManager<high_ir::Function>;
using ModuleAnalysisManager = AnalysisManager<high_ir::Module>;

// mid-IR
using LinearFunctionAnalysisManager =
  AnalysisManager<::exodus::mid_ir::LinearFunction>;

} // namespace exodus::opt
