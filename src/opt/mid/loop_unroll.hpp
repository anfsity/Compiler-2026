#pragma once

#include "../../mid/affine_loop.hpp"
#include "../../mid/rewriter.hpp"
#include <unordered_map>
#include <unordered_set>

namespace exodus::mid_ir::opt {

class LoopUnroll {
public:
  explicit LoopUnroll(MidModule *m) : module(m) {}

  auto run(LinearFunction &func, exodus::opt::LinearFunctionAnalysisManager &am)
    -> exodus::opt::PreservedAnalysis;

private:
  static constexpr uint64_t max_trip_count = 8;
  static constexpr size_t max_cloned_ops = 48;
  static constexpr size_t max_growth_ops = 32;

  MidModule *module;
  std::unordered_map<Op *, Block *> op_blocks;

  auto build_op_block_map(LinearFunction &func) -> void;
  auto try_unroll(
    LinearFunction &func,
    const Loop &loop,
    const CountedLoopInfo &counted,
    uint64_t trip_count
  ) -> bool;
  auto validate_shape(
    LinearFunction &func,
    const Loop &loop,
    const CountedLoopInfo &counted,
    uint64_t trip_count,
    std::vector<Op *> &phis,
    std::vector<Op *> &body
  ) const -> bool;
  auto clone_body(
    Block *destination,
    const std::vector<Op *> &body,
    const std::vector<Op *> &phis,
    const CountedLoopInfo &counted,
    uint64_t trip_count,
    std::unordered_map<Value *, Value *> &final_values
  ) -> bool;
  auto remove_loop_blocks(LinearFunction &func, const CountedLoopInfo &counted)
    -> void;

  static auto renumber_blocks(LinearFunction &func) -> void;
};

} // namespace exodus::mid_ir::opt
