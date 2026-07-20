#ifdef EXODUS_UNIT_TEST
#include "../src/mid/dom.hpp"
#include "../src/opt/mid/mem2reg.hpp"
#include <cassert>
#include <iostream>

using namespace exodus::mid_ir;
using namespace exodus::mid_ir::opt;
using namespace exodus::ir;

auto test_basic_mem2reg() -> void {
  IRContext ctx;
  MidModule module;
  module.ctx = &ctx;

  LinearFunction func;
  auto entry = std::make_unique<Block>(0, "entry");
  Block *p_entry = entry.get();

  auto i32 = exodus::I32::get();
  auto p_i32 = exodus::Ptr::get(i32);

  // alloca %1 = i32
  auto *alloca = module.make_op(OpCode::Alloca);
  alloca->result = ctx.make_value<OpResult>(p_i32, alloca);
  p_entry->insts.push_back(alloca);

  // store 10, %1
  auto *c10 = ctx.make_const(i32, 10);
  auto *store = module.make_op(OpCode::Store);
  store->operands.push_back(c10);
  store->operands.push_back(alloca->result);
  c10->addUse(store);
  alloca->result->addUse(store);
  p_entry->insts.push_back(store);

  // %2 = load %1
  auto *load = module.make_op(OpCode::Load);
  load->operands.push_back(alloca->result);
  load->result = ctx.make_value<OpResult>(i32, load);
  alloca->result->addUse(load);
  p_entry->insts.push_back(load);

  // ret %2
  auto *ret = module.make_op(OpCode::Ret);
  ret->operands.push_back(load->result);
  load->result->addUse(ret);
  p_entry->insts.push_back(ret);

  func.blocks.push_back(std::move(entry));

  exodus::opt::LinearFunctionAnalysisManager am;
  am.register_pass<DominanceAnalysis>();

  Mem2Reg m2r(&module);
  m2r.run(func, am);

  // After mem2reg:
  // entry:
  //   ret 10

  assert(p_entry->insts.size() == 1);
  auto *final_op = p_entry->insts.front();
  assert(final_op->code == OpCode::Ret);
  assert(final_op->operands[0]->kind == ValueKind::Constant);
  assert(
    std::get<int>(static_cast<Constant *>(final_op->operands[0])->val) == 10
  );

  // Check users
  Value *val = final_op->operands[0];
  assert(val->users.size() == 1);
  assert(val->users.front() == static_cast<exodus::ir::OpBase *>(final_op));

  std::cout << "test_basic_mem2reg passed!\n";
}

auto test_diamond_mem2reg() -> void {
  IRContext ctx;
  MidModule module;
  module.ctx = &ctx;

  auto i32 = exodus::I32::get();
  auto p_i32 = exodus::Ptr::get(i32);

  LinearFunction func;
  auto entry = std::make_unique<Block>(0, "entry");
  auto then_bb = std::make_unique<Block>(1, "then");
  auto else_bb = std::make_unique<Block>(2, "else");
  auto merge = std::make_unique<Block>(3, "merge");

  Block *p_entry = entry.get();
  Block *p_then = then_bb.get();
  Block *p_else = else_bb.get();
  Block *p_merge = merge.get();

  p_entry->succs = {p_then, p_else};
  p_then->preds = {p_entry};
  p_then->succs = {p_merge};
  p_else->preds = {p_entry};
  p_else->succs = {p_merge};
  p_merge->preds = {p_then, p_else};

  // entry:
  //   %a = alloca i32
  auto *alloca = module.make_op(OpCode::Alloca);
  alloca->result = ctx.make_value<OpResult>(p_i32, alloca);
  p_entry->insts.push_back(alloca);

  // then:
  //   store 1, %a
  auto *c1 = ctx.make_const(i32, 1);
  auto *store1 = module.make_op(OpCode::Store);
  store1->operands.push_back(c1);
  store1->operands.push_back(alloca->result);
  c1->addUse(store1);
  alloca->result->addUse(store1);
  p_then->insts.push_back(store1);

  // else:
  //   store 2, %a
  auto *c2 = ctx.make_const(i32, 2);
  auto *store2 = module.make_op(OpCode::Store);
  store2->operands.push_back(c2);
  store2->operands.push_back(alloca->result);
  c2->addUse(store2);
  alloca->result->addUse(store2);
  p_else->insts.push_back(store2);

  // merge:
  //   %v = load %a
  //   ret %v
  auto *load = module.make_op(OpCode::Load);
  load->operands.push_back(alloca->result);
  load->result = ctx.make_value<OpResult>(i32, load);
  alloca->result->addUse(load);
  p_merge->insts.push_back(load);

  auto *ret = module.make_op(OpCode::Ret);
  ret->operands.push_back(load->result);
  load->result->addUse(ret);
  p_merge->insts.push_back(ret);

  func.blocks.push_back(std::move(entry));
  func.blocks.push_back(std::move(then_bb));
  func.blocks.push_back(std::move(else_bb));
  func.blocks.push_back(std::move(merge));

  exodus::opt::LinearFunctionAnalysisManager am;
  am.register_pass<DominanceAnalysis>();

  Mem2Reg m2r(&module);
  m2r.run(func, am);

  // After mem2reg:
  // merge should have a Phi
  assert(!p_merge->insts.empty());
  auto *phi = p_merge->insts.front();
  assert(phi->code == OpCode::Phi);

  auto &payload = std::get<PhiPayload>(phi->payload);
  assert(payload.incoming.size() == 2);

  // Check if ret uses phi result
  auto *final_ret = p_merge->insts.back();
  assert(final_ret->code == OpCode::Ret);
  assert(final_ret->operands[0] == phi->result);

  std::cout << "test_diamond_mem2reg passed!\n";
}

auto test_mem2reg_after_flattened_creator_update() -> void {
  IRContext ctx;
  MidModule module;
  module.ctx = &ctx;

  LinearFunction func;
  auto entry = std::make_unique<Block>(0, "entry");
  Block *p_entry = entry.get();

  auto i32 = exodus::I32::get();
  auto p_i32 = exodus::Ptr::get(i32);

  auto *old_alloca_owner = module.make_op(OpCode::Alloca);
  auto *alloca_res = ctx.make_value<OpResult>(p_i32, old_alloca_owner);

  auto *alloca = module.make_op(OpCode::Alloca);
  alloca->result = alloca_res;
  alloca_res->creator = alloca;
  p_entry->insts.push_back(alloca);

  auto *c42 = ctx.make_const(i32, 42);
  auto *store = module.make_op(OpCode::Store);
  store->operands.push_back(c42);
  store->operands.push_back(alloca->result);
  c42->addUse(store);
  alloca->result->addUse(store);
  p_entry->insts.push_back(store);

  auto *load = module.make_op(OpCode::Load);
  load->operands.push_back(alloca->result);
  load->result = ctx.make_value<OpResult>(i32, load);
  alloca->result->addUse(load);
  p_entry->insts.push_back(load);

  auto *ret = module.make_op(OpCode::Ret);
  ret->operands.push_back(load->result);
  load->result->addUse(ret);
  p_entry->insts.push_back(ret);

  func.blocks.push_back(std::move(entry));

  exodus::opt::LinearFunctionAnalysisManager am;
  am.register_pass<DominanceAnalysis>();

  Mem2Reg m2r(&module);
  m2r.run(func, am);

  assert(p_entry->insts.size() == 1);
  auto *final_ret = p_entry->insts.front();
  assert(final_ret->code == OpCode::Ret);
  assert(final_ret->operands[0]->kind == ValueKind::Constant);
  assert(
    std::get<int>(static_cast<Constant *>(final_ret->operands[0])->val) == 42
  );

  std::cout << "test_mem2reg_after_flattened_creator_update passed!\n";
}

int main() {
  test_basic_mem2reg();
  test_diamond_mem2reg();
  test_mem2reg_after_flattened_creator_update();
  return 0;
}
#endif
