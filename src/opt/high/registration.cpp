#include "../PassRegistry.hpp"
#include "constant_propagation.hpp"
#include "inliner.hpp"
#include "return_insertion.hpp"
#include "sdce.hpp"

namespace exodus::high_ir::opt {

static exodus::opt::RegisterModulePass<Inliner>
  reg_inliner("inliner", "function inlining");

static exodus::opt::RegisterFunctionPass<ReturnInsertion> reg_return_insertion(
  "return_insertion", "insert missing return instructions"
);

static exodus::opt::RegisterFunctionPass<CP>
  reg_const_prop("const_prop", "constant propagation");

static exodus::opt::RegisterFunctionPass<SimpleDCE>
  reg_simple_dce("simple_dce", "simple dead code elimination");

auto registerPasses() -> void {}

} // namespace exodus::high_ir::opt
