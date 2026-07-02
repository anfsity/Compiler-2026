#include "../src/opt/AnalysisManager.hpp"
#include "../src/opt/high/constant_propagation.hpp"
#include "../src/type.hpp"
#include <cassert>
#include <memory>

using namespace exodus;
using namespace exodus::high_ir;
using namespace exodus::high_ir::opt;
using namespace exodus::opt;

#ifdef EXODUS_UNIT_TEST

auto test_mutable_global_load_is_not_folded() -> void {
  Module module;

  auto global = std::make_unique<GlobalVar>();
  global->name = "g";
  global->type = I32::get();
  global->init = InitVal{0};
  global->is_const = false;
  global->addr = module.ctx.make_value<GlobalAddr>(Ptr::get(I32::get()), "g");
  auto *global_addr = global->addr;
  module.globals.push_back(std::move(global));

  Function function;
  function.name = "load_global";
  function.type = Func::get(I32::get(), {});

  auto *load = module.ctx.make_op(OpCode::Load);
  load->operands = {global_addr};
  global_addr->addUse(load);
  load->result = module.ctx.make_value<OpResult>(I32::get(), load);
  function.body.push_back(load);

  auto *ret = module.ctx.make_op(OpCode::Ret);
  ret->operands = {load->result};
  load->result->addUse(ret);
  function.body.push_back(ret);

  FunctionAnalysisManager fam;
  CP cp(&module);
  cp.run(function, fam);

  assert(function.body.front()->code == OpCode::Load);
}

int main() {
  test_mutable_global_load_is_not_folded();
  return 0;
}

#endif
