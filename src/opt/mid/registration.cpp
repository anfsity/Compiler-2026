#include "../PassRegistry.hpp"
#include "cfg_simplify.hpp"
#include "dce.hpp"
#include "gvn.hpp"
#include "mem2reg.hpp"
#include "tail_recursion_elim.hpp"

namespace exodus::mid_ir::opt {

static exodus::opt::RegisterLinearFunctionPass<Mem2Reg>
  reg_mem2reg("mem2reg", "memory to register promotion");

static exodus::opt::RegisterLinearFunctionPass<TailRecursionElim>
  reg_tail_recursion_elim("tail_recursion_elim", "tail recursion elimination");

static exodus::opt::RegisterLinearFunctionPass<GVN>
  reg_gvn("gvn", "global value numbering");

static exodus::opt::RegisterLinearFunctionPass<CFGSimplify>
  reg_cfg_simplify("cfg_simplify", "control-flow graph simplification");

static exodus::opt::RegisterLinearFunctionPass<DCE>
  reg_dce("dce", "dead code elimination");

auto register_passes() -> void {}

} // namespace exodus::mid_ir::opt
