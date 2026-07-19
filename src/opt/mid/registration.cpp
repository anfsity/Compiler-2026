#include "../../mid/cfg_simplify.hpp"
#include "../../mid/dce.hpp"
#include "../../mid/gvn.hpp"
#include "../../mid/mem2reg.hpp"
#include "../PassRegistry.hpp"

namespace exodus::mid_ir::opt {

static exodus::opt::RegisterLinearFunctionPass<Mem2Reg>
  reg_mem2reg("mem2reg", "memory to register promotion");

static exodus::opt::RegisterLinearFunctionPass<GVN>
  reg_gvn("gvn", "global value numbering");

static exodus::opt::RegisterLinearFunctionPass<CFGSimplify>
  reg_cfg_simplify("cfg_simplify", "control-flow graph simplification");

static exodus::opt::RegisterLinearFunctionPass<DCE>
  reg_dce("dce", "dead code elimination");

auto register_passes() -> void {}

} // namespace exodus::mid_ir::opt
