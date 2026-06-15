#include "../../mid/mem2reg.hpp"
#include "../PassRegistry.hpp"

namespace exodus::mid_ir::opt {

static exodus::opt::RegisterLinearFunctionPass<Mem2Reg>
  reg_mem2reg("mem2reg", "memory to register promotion");

auto register_passes() -> void {}

} // namespace exodus::mid_ir::opt
