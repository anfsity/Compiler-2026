#pragma once

#include "../../mid/affine_loop.hpp"
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace exodus::mid_ir {

struct SCEVAffineExpr {
  int64_t constant = 0;
  std::unordered_map<Value *, int64_t> coefficients;
  bool no_wrap = false;

  auto coefficient(Value *symbol) const -> int64_t {
    auto it = coefficients.find(symbol);
    return it == coefficients.end() ? 0 : it->second;
  }
  auto is_constant() const -> bool { return coefficients.empty(); }
};

struct SCEVAddRecurrence {
  const Loop *loop = nullptr;
  CountedLoopInfo counted;
  SCEVAffineExpr initial;
  int64_t step = 0;
  bool no_wrap = false;
};

class ScalarEvolution {
public:
  auto compute(
    LinearFunction &func, LoopInfo &loops, AffineLoopInfo &affine, DomTree &dom
  ) -> void;

  auto get_add_recurrence(const Loop &loop) const
    -> std::optional<SCEVAddRecurrence>;
  auto get_affine(
    Value *value, const std::vector<CountedLoopInfo> &iteration_space
  ) const -> std::optional<SCEVAffineExpr>;

private:
  std::unordered_map<Op *, Block *> op_blocks;
  LoopInfo *loops = nullptr;
  AffineLoopInfo *affine = nullptr;
  DomTree *dom = nullptr;

  auto get_affine_impl(
    Value *value,
    const std::vector<CountedLoopInfo> &iteration_space,
    std::unordered_set<Value *> &active
  ) const -> std::optional<SCEVAffineExpr>;
  auto expression_range(
    const SCEVAffineExpr &expr,
    const std::vector<CountedLoopInfo> &iteration_space
  ) const -> std::optional<IntegerRange>;
  auto definition_block(Value *value) const -> Block *;
};

struct ScalarEvolutionAnalysis {
  using Result = ScalarEvolution;

  auto run(LinearFunction &func, exodus::opt::LinearFunctionAnalysisManager &am)
    -> Result {
    auto &loops = am.get_result<LoopAnalysis>(func);
    auto &affine = am.get_result<AffineLoopAnalysis>(func);
    auto &dom = am.get_result<DominanceAnalysis>(func);
    ScalarEvolution result;
    result.compute(func, loops, affine, dom);
    return result;
  }
};

} // namespace exodus::mid_ir
