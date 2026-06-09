#include "../../../include/opt/PassRegistry.hpp"
#include "../../../include/opt/high/constant_propagation.hpp"
#include "../../../include/opt/high/inliner.hpp"
#include "../../../include/opt/high/return_insertion.hpp"
#include "../../../include/opt/high/sdce.hpp"

namespace exodus::high_ir::opt {

[[maybe_unused]] static exodus::opt::RegisterModulePass<Inliner>
  reg_inliner("inliner", "function inlining");

[[maybe_unused]] static exodus::opt::RegisterFunctionPass<ReturnInsertion>
  reg_return_insertion(
    "return_insertion", "insert missing return instructions"
  );

[[maybe_unused]] static exodus::opt::RegisterFunctionPass<CP>
  reg_const_prop("const_prop", "constant propagation");

[[maybe_unused]] static exodus::opt::RegisterFunctionPass<SimpleDCE>
  reg_simple_dce("simple_dce", "simple dead code elimination");

} // namespace exodus::high_ir::opt
