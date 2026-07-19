#ifdef EXODUS_UNIT_TEST
#include "../src/high/effects.hpp"
#include "../src/opt/AnalysisManager.hpp"
#include "../src/opt/high/inst_simplify.hpp"
#include "../src/opt/high/region_simplify.hpp"
#include "../src/type.hpp"
#include <cassert>

using namespace exodus;
using namespace exodus::high_ir;
using namespace exodus::high_ir::opt;
using namespace exodus::opt;

namespace {

auto add_operand(Op *op, Value *value) -> void {
  op->operands.push_back(value);
  value->addUse(op);
}

auto make_store(high_ir::IRContext &ctx, Value *value, Value *address) -> Op * {
  auto *store = ctx.make_op(OpCode::Store);
  add_operand(store, value);
  add_operand(store, address);
  return store;
}

auto test_op_effects() -> void {
  high_ir::IRContext ctx;

  auto *add = ctx.make_op(OpCode::Add);
  add_operand(add, ctx.make_const(I32::get(), 1));
  add_operand(add, ctx.make_const(I32::get(), 2));
  add->result = ctx.make_value<OpResult>(I32::get(), add);
  auto add_effects = get_op_effects(*add);
  assert(add_effects.cseable);
  assert(add_effects.speculatable);
  assert(!add_effects.has_observable_effect());

  auto i32 = I32::get();
  auto array = Array::get(i32, 3);
  auto array_ptr = Ptr::get(array);
  auto slot_type = Ptr::get(array_ptr);
  auto *slot = ctx.make_op(OpCode::Alloca);
  slot->result = ctx.make_value<OpResult>(slot_type, slot);

  auto *getptr = ctx.make_op(OpCode::GetPtr);
  add_operand(getptr, slot->result);
  add_operand(getptr, ctx.make_const(i32, 0));
  add_operand(getptr, ctx.make_const(i32, 0));
  getptr->result = ctx.make_value<OpResult>(Ptr::get(i32), getptr);
  auto getptr_effects = get_op_effects(*getptr);
  assert(getptr_effects.reads_memory());
  assert(getptr_effects.reads.count(slot->result));
  assert(!getptr_effects.cseable);

  auto *indirect_store =
    make_store(ctx, ctx.make_const(i32, 4), getptr->result);
  auto indirect_store_effects = get_op_effects(*indirect_store);
  assert(indirect_store_effects.writes.count(getptr->result));
  assert(!indirect_store_effects.writes.count(slot->result));

  auto *global = ctx.make_value<GlobalAddr>(i32->ptr_to(), "g");
  auto then_region = std::make_unique<Region>();
  then_region->push_back(make_store(ctx, ctx.make_const(i32, 3), global));
  auto *if_op = ctx.make_op(OpCode::If);
  add_operand(if_op, ctx.make_const(Bool::get(), 1));
  if_op->payload = IfPayload{std::move(then_region), std::nullopt};
  assert(get_op_effects(*if_op).writes.empty());

  Region region;
  region.push_back(if_op);
  auto region_effects = get_region_effects(region);
  assert(region_effects.writes.count(global));
  assert(region_effects.has_observable_effect());

  auto *call = ctx.make_op(OpCode::Call, CallPayload{"external"});
  add_operand(call, global);
  auto call_effects = get_op_effects(*call);
  assert(call_effects.has_unknown_effect);
  assert(call_effects.reads.count(global));
  assert(call_effects.writes.count(global));
}

auto test_inst_simplify() -> void {
  Module module;
  Function function;
  function.name = "inst_simplify";
  function.type = Func::get(Bool::get(), {I32::get()});
  auto *argument = module.ctx.make_value<Argument>(I32::get(), 0);
  function.args.push_back(argument);

  auto *add = module.ctx.make_op(OpCode::Add);
  add_operand(add, argument);
  add_operand(add, module.ctx.make_const(I32::get(), 0));
  add->result = module.ctx.make_value<OpResult>(I32::get(), add);
  function.body.push_back(add);

  auto *mul = module.ctx.make_op(OpCode::Mul);
  add_operand(mul, add->result);
  add_operand(mul, module.ctx.make_const(I32::get(), 1));
  mul->result = module.ctx.make_value<OpResult>(I32::get(), mul);
  function.body.push_back(mul);

  auto *equal = module.ctx.make_op(OpCode::Eq);
  add_operand(equal, mul->result);
  add_operand(equal, mul->result);
  equal->result = module.ctx.make_value<OpResult>(Bool::get(), equal);
  function.body.push_back(equal);

  auto *ret = module.ctx.make_op(OpCode::Ret);
  add_operand(ret, equal->result);
  function.body.push_back(ret);

  FunctionAnalysisManager fam;
  InstSimplify simplify(&module);
  auto result = simplify.run(function, fam);
  assert(!result.all_preserved());
  assert(function.body.size() == 1);
  assert(function.body.front() == ret);
  assert(ret->operands[0]->kind == ValueKind::Constant);
  assert(std::get<int>(static_cast<Constant *>(ret->operands[0])->val) == 1);
}

auto test_region_simplify() -> void {
  Module module;
  Function function;
  function.name = "region_simplify";
  function.type = Func::get(Void::get(), {});
  auto *global =
    module.ctx.make_value<GlobalAddr>(I32::get()->ptr_to(), "selected");

  auto then_region = std::make_unique<Region>();
  auto *selected =
    make_store(module.ctx, module.ctx.make_const(I32::get(), 1), global);
  then_region->push_back(selected);
  Region else_region;
  else_region.push_back(
    make_store(module.ctx, module.ctx.make_const(I32::get(), 2), global)
  );
  auto *if_op = module.ctx.make_op(OpCode::If);
  add_operand(if_op, module.ctx.make_const(Bool::get(), 1));
  if_op->payload = IfPayload{std::move(then_region), std::move(else_region)};
  function.body.push_back(if_op);

  auto condition_region = std::make_unique<Region>();
  auto *call =
    module.ctx.make_op(OpCode::Call, CallPayload{"condition_effect"});
  condition_region->push_back(call);
  auto *condition = module.ctx.make_op(OpCode::Condition);
  add_operand(condition, module.ctx.make_const(Bool::get(), 0));
  condition_region->push_back(condition);

  auto loop_region = std::make_unique<Region>();
  loop_region->push_back(
    make_store(module.ctx, module.ctx.make_const(I32::get(), 3), global)
  );
  auto *while_op = module.ctx.make_op(OpCode::While);
  while_op->payload =
    WhilePayload{std::move(condition_region), std::move(loop_region)};
  function.body.push_back(while_op);

  auto empty_then = std::make_unique<Region>();
  auto *empty_if = module.ctx.make_op(OpCode::If);
  add_operand(empty_if, module.ctx.make_value<Argument>(Bool::get(), 0));
  empty_if->payload = IfPayload{std::move(empty_then), std::nullopt};
  function.body.push_back(empty_if);

  FunctionAnalysisManager fam;
  RegionSimplify simplify(&module);
  auto result = simplify.run(function, fam);
  assert(!result.all_preserved());
  assert(function.body.size() == 2);
  auto it = function.body.begin();
  assert(*it++ == selected);
  assert(*it == call);
}

} // namespace

auto main() -> int {
  test_op_effects();
  test_inst_simplify();
  test_region_simplify();
  return 0;
}

#endif
