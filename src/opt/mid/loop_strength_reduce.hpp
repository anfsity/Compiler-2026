#pragma once

#include "../../mid/affine_loop.hpp"
#include "../../mid/rewriter.hpp"
#include <optional>
#include <unordered_map>

namespace exodus::mid_ir::opt {

class LoopStrengthReduce {
public:
  explicit LoopStrengthReduce(MidModule *m) : module(m) {}

  auto run(LinearFunction &func, exodus::opt::LinearFunctionAnalysisManager &am)
    -> exodus::opt::PreservedAnalysis;

private:
  MidModule *module;
  DomTree *dom = nullptr;
  AffineLoopInfo *affine_loops = nullptr;
  std::unordered_map<Op *, Block *> op_blocks;

  auto build_op_block_map(LinearFunction &func) -> void;
  auto
  linear_coefficient(Value *value, Value *induction, const Loop &loop) const
    -> std::optional<int>;
  auto matches_migrated_affine_shape(
    Value *value, Value *induction, const Loop &loop
  ) const -> bool;
  auto expression_cost(Value *value, Value *induction, const Loop &loop) const
    -> std::optional<unsigned>;
  auto clone_initial_expression(
    Value *value,
    Value *induction,
    Value *initial,
    const Loop &loop,
    Block *preheader,
    std::unordered_map<Value *, Value *> &cache
  ) -> Value *;
  auto can_clone_initial_expression(
    Value *value,
    Value *induction,
    Value *initial,
    const Loop &loop,
    Block *preheader
  ) const -> bool;
  auto reduce_getptrs(LinearFunction &func, const Loop &loop) -> bool;
  auto reduce_getptr(
    Op *getptr,
    const Loop &loop,
    const CountedLoopInfo &counted,
    MidIRRewriter &rewriter,
    std::unordered_map<Value *, Value *> &cache
  ) -> bool;

  auto is_clonable_expression(const Op &op, const Loop &loop) const -> bool;
};

} // namespace exodus::mid_ir::opt
