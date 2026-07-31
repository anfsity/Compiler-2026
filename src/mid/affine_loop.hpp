#pragma once

#include "affine_expr.hpp"
#include "loop.hpp"
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace exodus::mid_ir {

struct AffineForm {
  // Mathematical form only.  Consumers that change i32 evaluation must also
  // call is_no_wrap() to prove every source operation over the loop range.
  int64_t coefficient = 0;
  int64_t offset = 0;
};

struct InductionInfo {
  Op *phi = nullptr;
  Value *initial = nullptr;
  Value *bound = nullptr;
  Op *update = nullptr;
  Block *latch = nullptr;
  int64_t step = 0;
  OpCode predicate = OpCode::Eq;
};

struct CountedLoopInfo {
  Block *preheader = nullptr;
  Block *header = nullptr;
  Block *latch = nullptr;
  Block *continuation = nullptr;
  Block *exit = nullptr;
  Op *compare = nullptr;
  bool continue_on_true = false;
  bool induction_on_lhs = false;
  InductionInfo induction;
};

class AffineLoopInfo {
public:
  auto compute(LinearFunction &func, DomTree &dom) -> void;

  auto
  get_inductions(const Loop &loop, bool allow_dominating_update = false) const
    -> std::vector<InductionInfo>;
  auto match_counted_loop(
    const Loop &loop, bool allow_dominating_update = false
  ) const -> std::optional<CountedLoopInfo>;
  auto exact_trip_count(const CountedLoopInfo &loop) const
    -> std::optional<uint64_t>;
  auto affine_form(
    Value *value, const CountedLoopInfo &counted, const Loop &loop
  ) const -> std::optional<AffineForm>;
  auto induction_range(const CountedLoopInfo &loop) const
    -> std::optional<IntegerRange>;
  auto is_no_wrap(
    Value *value, const CountedLoopInfo &counted, const Loop &loop
  ) const -> bool;
  auto is_non_negative(
    Value *value, const CountedLoopInfo &counted, const Loop &loop
  ) const -> bool;
  auto is_available_at_preheader(Value *value, Block *preheader) const -> bool;

  static auto integer_constant(Value *value) -> std::optional<int32_t>;

private:
  std::unordered_map<Op *, Block *> op_blocks;
  DomTree *dom = nullptr;

  auto definition_block(Value *value) const -> Block *;
  auto affine_expression(
    Value *value, const CountedLoopInfo &counted, const Loop &loop
  ) const -> std::optional<AffineExpression>;
  auto loop_invariant(Value *value, const Loop &loop) const -> bool;
  auto normalized_condition(
    Op *compare, Value *induction, bool true_is_continue
  ) const -> std::optional<std::pair<OpCode, Value *>>;
  auto expression_is_no_wrap(
    Value *value,
    const CountedLoopInfo &counted,
    const Loop &loop,
    const IntegerRange &range
  ) const -> bool;
  auto induction_update_is_no_wrap(
    const CountedLoopInfo &counted, const IntegerRange &range
  ) const -> bool;
};

struct AffineLoopAnalysis {
  using Result = AffineLoopInfo;

  auto run(LinearFunction &func, exodus::opt::LinearFunctionAnalysisManager &am)
    -> Result {
    auto &dom = am.get_result<DominanceAnalysis>(func);
    AffineLoopInfo info;
    info.compute(func, dom);
    return info;
  }
};

} // namespace exodus::mid_ir
