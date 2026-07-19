#include "../PassRegistry.hpp"
#include "constant_propagation.hpp"
#include "inliner.hpp"
#include "inst_simplify.hpp"
#include "ipcp.hpp"
#include "region_simplify.hpp"
#include "return_insertion.hpp"
#include "sdce.hpp"

namespace exodus::high_ir::opt {

static exodus::opt::RegisterModulePass<IPCP>
  reg_ipcp("ipcp", "interprocedural constant propagation");

static exodus::opt::RegisterModulePass<Inliner>
  reg_inliner("inliner", "function inlining");

static exodus::opt::RegisterFunctionPass<ReturnInsertion> reg_return_insertion(
  "return_insertion", "insert missing return instructions"
);

static exodus::opt::RegisterFunctionPass<CP>
  reg_const_prop("const_prop", "constant propagation");

static exodus::opt::RegisterFunctionPass<InstSimplify>
  reg_inst_simplify("inst_simplify", "instruction simplification");

static exodus::opt::RegisterFunctionPass<RegionSimplify>
  reg_region_simplify("region_simplify", "structured region simplification");

static exodus::opt::RegisterFunctionPass<SimpleDCE>
  reg_simple_dce("simple_dce", "simple dead code elimination");

auto register_passes() -> void {}

} // namespace exodus::high_ir::opt
