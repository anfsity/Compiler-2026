#pragma once

#include "loop.hpp"
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace exodus::mid_ir {

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
  auto compute(LinearFunction &func, LoopInfo &loops) -> void;

  auto get_inductions(const Loop &loop) const -> std::vector<InductionInfo>;
  auto match_counted_loop(const Loop &loop) const
    -> std::optional<CountedLoopInfo>;
  auto exact_trip_count(const CountedLoopInfo &loop) const
    -> std::optional<uint64_t>;

  static auto integer_constant(Value *value) -> std::optional<int32_t>;

private:
  std::unordered_map<Op *, Block *> op_blocks;

  auto definition_block(Value *value) const -> Block *;
  auto loop_invariant(Value *value, const Loop &loop) const -> bool;
  auto normalized_condition(
    Op *compare, Value *induction, bool true_is_continue
  ) const -> std::optional<std::pair<OpCode, Value *>>;
};

struct AffineLoopAnalysis {
  using Result = AffineLoopInfo;

  auto run(LinearFunction &func, exodus::opt::LinearFunctionAnalysisManager &am)
    -> Result {
    auto &loops = am.get_result<LoopAnalysis>(func);
    AffineLoopInfo info;
    info.compute(func, loops);
    return info;
  }
};

} // namespace exodus::mid_ir
