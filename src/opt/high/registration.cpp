#include "../PassRegistry.hpp"
#include "array_loop_specialize.hpp"
#include "const_prop.hpp"
#include "dead_functions.hpp"
#include "global_init_promotion.hpp"
#include "guarded_bitwise_idiom.hpp"
#include "idempotent_loops.hpp"
#include "inliner.hpp"
#include "inst_simplify.hpp"
#include "ipcp.hpp"
#include "loop_unswitch.hpp"
#include "memoization.hpp"
#include "pure_call_loop_sink.hpp"
#include "recurrence_simplify.hpp"
#include "region_simplify.hpp"
#include "return_insertion.hpp"
#include "sdce.hpp"

namespace exodus::high_ir::opt {

static exodus::opt::RegisterModulePass<GuardedBitwiseIdiom>
  reg_guarded_bitwise_idiom(
    "guarded_bitwise_idiom",
    "specialize proven bit-accumulation loops for non-negative operands"
  );

static exodus::opt::RegisterModulePass<IPCP>
  reg_ipcp("ipcp", "interprocedural constant propagation");

static exodus::opt::RegisterModulePass<GlobalInitPromotion>
  reg_global_init_promotion(
    "global_init_promotion", "promote proven constant global initialization"
  );

static exodus::opt::RegisterModulePass<IdempotentLoops> reg_idempotent_loops(
  "idempotent_loops", "collapse proven idempotent counted loops"
);

static exodus::opt::RegisterModulePass<PureCallLoopSink>
  reg_pure_call_loop_sink(
    "pure_call_loop_sink",
    "sink pure last-iteration calls out of guarded countdown loops"
  );

static exodus::opt::RegisterModulePass<Inliner>
  reg_inliner("inliner", "function inlining");

static exodus::opt::RegisterModulePass<DeadFunctions> reg_dead_functions(
  "dead_functions", "remove functions unreachable from main"
);

static exodus::opt::RegisterModulePass<Memoization>
  reg_memoization("memoization", "memoize pure functions");

static exodus::opt::RegisterFunctionPass<ReturnInsertion> reg_return_insertion(
  "return_insertion", "insert missing return instructions"
);

static exodus::opt::RegisterFunctionPass<LoopUnswitch> reg_loop_unswitch(
  "loop_unswitch", "unswitch proven invariant branches in small loops"
);

static exodus::opt::RegisterFunctionPass<ArrayLoopSpecialize>
  reg_array_loop_specialize(
    "array_loop_specialize",
    "specialize counted loops from proven local-array values"
  );

static exodus::opt::RegisterFunctionPass<ConstProp>
  reg_const_prop("const_prop", "constant propagation");

static exodus::opt::RegisterFunctionPass<InstSimplify>
  reg_inst_simplify("inst_simplify", "instruction simplification");

static exodus::opt::RegisterFunctionPass<RecurrenceSimplify>
  reg_recurrence_simplify(
    "recurrence_simplify", "simplify proven closed-form recurrences"
  );

static exodus::opt::RegisterFunctionPass<RegionSimplify>
  reg_region_simplify("region_simplify", "structured region simplification");

static exodus::opt::RegisterFunctionPass<SimpleDCE>
  reg_simple_dce("simple_dce", "simple dead code elimination");

auto register_passes() -> void {}

} // namespace exodus::high_ir::opt
