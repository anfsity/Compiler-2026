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

  auto intersect(const PreservedAnalysis &other) -> void {
    if (other.all_presvd)
      return;
    if (all_presvd) {
      *this = other;
      return;
    }
    for (auto it = presvd_ids.begin(); it != presvd_ids.end();) {
      if (!other.presvd_ids.count(*it))
        it = presvd_ids.erase(it);
      else
        ++it;
    }
  }
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
  auto register_pass() -> void {
    register_pass(PassT{});
  }

  template <typename PassT>
  auto register_pass(PassT pass) -> void {
    generators[typeid(PassT)] =
      [pass](IRUnitT &ir, AnalysisManager &am) mutable {
        auto result = pass.run(ir, am);
        return AnalysisResult(pass, std::move(result));
      };
  }

  template <typename PassT>
  auto get_result(IRUnitT &ir) -> typename PassT::Result & {
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

  // Parent/module passes use this bridge when their mutation can invalidate
  // analyses cached for child units.  The child manager owns the precise
  // cache keys, so clearing it is safer than trying to infer affected units.
  auto invalidate_children() -> void { cache.clear(); }

private:
  struct CacheKey { // NOLINT
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
using MidModuleAnalysisManager = AnalysisManager<::exodus::mid_ir::MidModule>;

} // namespace exodus::opt
