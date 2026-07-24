#pragma once

#include "../../mid/loop.hpp"
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
  struct Induction {
    Op *phi = nullptr;
    Value *initial = nullptr;
    Block *latch = nullptr;
    int step = 0;
  };

  struct AffineForm {
    int64_t coefficient = 0;
    int64_t offset = 0;
  };

  MidModule *module;
  std::unordered_map<Op *, Block *> op_blocks;

  auto build_op_block_map(LinearFunction &func) -> void;
  auto find_inductions(const Loop &loop) const -> std::vector<Induction>;
  auto
  linear_coefficient(Value *value, Value *induction, const Loop &loop) const
    -> std::optional<int>;
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
  auto reduce_getptrs(LinearFunction &func, const Loop &loop) -> bool;
  auto reduce_getptr(
    Op *getptr,
    const Loop &loop,
    const Induction &induction,
    MidIRRewriter &rewriter,
    std::unordered_map<Value *, Value *> &cache
  ) -> bool;

  static auto integer_constant(Value *value) -> std::optional<int>;
  auto affine_form(Value *value, Value *induction, const Loop &loop) const
    -> std::optional<AffineForm>;
  auto index_is_no_wrap(
    Value *index, const Loop &loop, const Induction &induction
  ) const -> bool;
  auto is_clonable_expression(const Op &op, const Loop &loop) const -> bool;
};

} // namespace exodus::mid_ir::opt
