#include "loop_strength_reduce.hpp"

#include "lsr_context.hpp"
#include "lsr_recurrence.hpp"

namespace exodus::mid_ir::opt {

auto LoopStrengthReduce::run(
  LinearFunction &func, exodus::opt::LinearFunctionAnalysisManager &am
) -> exodus::opt::PreservedAnalysis {
  if (func.blocks.empty())
    return exodus::opt::PreservedAnalysis::all();

  auto &loop_info = am.get_result<LoopAnalysis>(func);
  auto &dom_result = am.get_result<DominanceAnalysis>(func);
  auto &affine_result = am.get_result<AffineLoopAnalysis>(func);
  LSRContext context{module, &dom_result, &affine_result, {}};
  build_lsr_op_block_map(context, func);

  GetPtrRecurrencePlanner planner(context, func);
  bool changed = false;
  for (auto *loop : loop_info.get_loops_innermost_first()) {
    // Outer-loop address recurrences may stay live across a child loop, but
    // the same affine/no-wrap and dominance proofs still apply.  Keeping the
    // decision in the planner lets it reject expressions whose update would
    // not repay the longer live range while enabling row/column pointers in
    // nested dynamic-programming loops.
    changed |= planner.reduce(*loop);
  }

  if (!changed)
    return exodus::opt::PreservedAnalysis::all();

  exodus::opt::PreservedAnalysis preserved;
  preserved.preserve<DominanceAnalysis>();
  preserved.preserve<LoopAnalysis>();
  return preserved;
}

} // namespace exodus::mid_ir::opt
