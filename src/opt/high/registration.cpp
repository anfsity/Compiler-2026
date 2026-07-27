#include "../PassRegistry.hpp"
#include "constant_propagation.hpp"
#include "dead_function_elimination.hpp"
#include "global_init_promotion.hpp"
#include "idempotent_loop_simplify.hpp"
#include "inliner.hpp"
#include "inst_simplify.hpp"
#include "ipcp.hpp"
#include "local_array_loop_specialize.hpp"
#include "loop_recurrence_simplify.hpp"
#include "loop_unswitch.hpp"
#include "memoization.hpp"
#include "region_simplify.hpp"
#include "return_insertion.hpp"
#include "sdce.hpp"

namespace exodus::high_ir::opt {

static exodus::opt::RegisterModulePass<IPCP>
  reg_ipcp("ipcp", "interprocedural constant propagation");

static exodus::opt::RegisterModulePass<GlobalInitPromotion>
  reg_global_init_promotion(
    "global_init_promotion", "promote proven constant global initialization"
  );

static exodus::opt::RegisterModulePass<IdempotentLoopSimplify>
  reg_idempotent_loop_simplify(
    "idempotent_loop_simplify", "collapse proven idempotent counted loops"
  );

static exodus::opt::RegisterModulePass<Inliner>
  reg_inliner("inliner", "function inlining");

static exodus::opt::RegisterModulePass<DeadFunctionElimination>
  reg_dead_function_elimination(
    "dead_function_elimination", "remove functions unreachable from main"
  );

static exodus::opt::RegisterModulePass<Memoization>
  reg_memoization("memoization", "memoize pure functions");

static exodus::opt::RegisterFunctionPass<ReturnInsertion> reg_return_insertion(
  "return_insertion", "insert missing return instructions"
);

static exodus::opt::RegisterFunctionPass<LoopUnswitch> reg_loop_unswitch(
  "loop_unswitch", "unswitch proven invariant branches in small loops"
);

static exodus::opt::RegisterFunctionPass<LocalArrayLoopSpecialize>
  reg_local_array_loop_specialize(
    "local_array_loop_specialize",
    "specialize counted loops from proven local-array values"
  );

static exodus::opt::RegisterFunctionPass<CP>
  reg_const_prop("const_prop", "constant propagation");

static exodus::opt::RegisterFunctionPass<InstSimplify>
  reg_inst_simplify("inst_simplify", "instruction simplification");

static exodus::opt::RegisterFunctionPass<LoopRecurrenceSimplify>
  reg_loop_recurrence_simplify(
    "loop_recurrence_simplify", "simplify proven closed-form recurrences"
  );

static exodus::opt::RegisterFunctionPass<RegionSimplify>
  reg_region_simplify("region_simplify", "structured region simplification");

static exodus::opt::RegisterFunctionPass<SimpleDCE>
  reg_simple_dce("simple_dce", "simple dead code elimination");

auto register_passes() -> void {}

} // namespace exodus::high_ir::opt
