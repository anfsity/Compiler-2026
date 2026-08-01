#ifdef EXODUS_UNIT_TEST
#include "../src/mid/cfg_editor.hpp"
#include <cassert>
#include <iostream>

using namespace exodus::mid_ir;
using namespace exodus::ir;

auto add_jump(MidModule &module, Block *from, Block *to) -> void {
  auto *jump = module.make_op(OpCode::Jump);
  jump->successors.push_back(to);
  from->insts.push_back(jump);
}

auto test_redirect_updates_cfg_and_phi() -> void {
  IRContext ctx;
  MidModule module;
  module.ctx = &ctx;
  LinearFunction func;
  func.next_block_id = 1000;

  auto entry = std::make_unique<Block>(101, "entry");
  auto left = std::make_unique<Block>(205, "left");
  auto right = std::make_unique<Block>(309, "right");
  auto merge = std::make_unique<Block>(711, "merge");
  auto *p_entry = entry.get();
  auto *p_left = left.get();
  auto *p_right = right.get();
  auto *p_merge = merge.get();

  auto i32 = exodus::I32::get();
  auto *cond = ctx.make_const(exodus::Bool::get(), 1);
  auto *branch = module.make_op(OpCode::Branch);
  branch->operands.push_back(cond);
  cond->addUse(branch);
  branch->successors = {p_left, p_right};
  p_entry->insts.push_back(branch);
  add_jump(module, p_left, p_merge);
  add_jump(module, p_right, p_merge);

  auto *phi = module.make_op(OpCode::Phi, PhiPayload{});
  phi->result = ctx.make_value<OpResult>(i32, phi);
  auto *c1 = ctx.make_const(i32, 1);
  auto *c2 = ctx.make_const(i32, 2);
  std::get<PhiPayload>(phi->payload).incoming = {{p_left, c1}, {p_right, c2}};
  c1->addUse(phi);
  c2->addUse(phi);
  p_merge->insts.push_back(phi);
  auto *ret = module.make_op(OpCode::Ret);
  ret->operands.push_back(phi->result);
  phi->result->addUse(ret);
  p_merge->insts.push_back(ret);

  func.blocks.push_back(std::move(entry));
  func.blocks.push_back(std::move(left));
  func.blocks.push_back(std::move(right));
  func.blocks.push_back(std::move(merge));

  CFGEditor cfg(module, func);
  cfg.synchronize();
  assert(cfg.redirect_edge(p_left, p_merge, p_right));
  assert(cfg.check_local_consistency());

  assert(p_left->succs.size() == 1 && p_left->succs.front() == p_right);
  assert(p_merge->preds.size() == 1 && p_merge->preds.front() == p_right);
  const auto &incoming = std::get<PhiPayload>(phi->payload).incoming;
  assert(incoming.size() == 1);
  assert(incoming.front().first == p_right);
  assert(incoming.front().second == c2);
  assert(c1->users.empty());
  std::cout << "test_redirect_updates_cfg_and_phi passed!\n";
}

auto test_transaction_rolls_back_failed_commit() -> void {
  IRContext ctx;
  MidModule module;
  module.ctx = &ctx;
  LinearFunction func;
  func.next_block_id = 50;

  auto entry = std::make_unique<Block>(10, "entry");
  auto *p_entry = entry.get();
  auto *ret = module.make_op(OpCode::Ret);
  p_entry->insts.push_back(ret);
  func.blocks.push_back(std::move(entry));

  CFGEditor cfg(module, func);
  {
    auto tx = cfg.begin_transaction();
    auto *bad = cfg.create_block("bad");
    assert(bad);
    auto *phi = module.make_op(OpCode::Phi, PhiPayload{});
    phi->result = ctx.make_value<OpResult>(exodus::I32::get(), phi);
    bad->insts.push_back(phi);
    auto *zero = ctx.make_const(exodus::I32::get(), 0);
    assert(cfg.set_phi_incoming(phi, {{p_entry, zero}}));
    assert(!tx.commit());
  }

  assert(func.blocks.size() == 1);
  assert(func.blocks.front().get() == p_entry);
  assert(func.next_block_id == 50);
  assert(cfg.check_local_consistency());
  std::cout << "test_transaction_rolls_back_failed_commit passed!\n";
}

auto test_consistency_rejects_stale_preds_and_incomplete_phi() -> void {
  IRContext ctx;
  MidModule module;
  module.ctx = &ctx;
  LinearFunction func;
  func.next_block_id = 3;

  auto entry = std::make_unique<Block>(0, "entry");
  auto merge = std::make_unique<Block>(1, "merge");
  auto orphan = std::make_unique<Block>(2, "orphan");
  auto *p_entry = entry.get();
  auto *p_merge = merge.get();
  auto *p_orphan = orphan.get();

  add_jump(module, p_entry, p_merge);
  auto *ret = module.make_op(OpCode::Ret);
  p_merge->insts.push_back(ret);

  func.blocks.push_back(std::move(entry));
  func.blocks.push_back(std::move(merge));
  func.blocks.push_back(std::move(orphan));

  CFGEditor cfg(module, func);
  cfg.synchronize();
  assert(cfg.check_local_consistency());

  p_merge->preds.push_back(p_orphan);
  std::string reason;
  assert(!cfg.check_local_consistency(&reason));
  assert(reason == "pred list does not match terminators");

  cfg.synchronize();
  auto *phi = module.make_op(OpCode::Phi, PhiPayload{});
  phi->result = ctx.make_value<OpResult>(exodus::I32::get(), phi);
  p_merge->insts.push_front(phi);
  assert(!cfg.check_local_consistency(&reason));
  assert(reason == "Phi incoming does not match preds");

  std::cout
    << "test_consistency_rejects_stale_preds_and_incomplete_phi passed!\n";
}

auto test_merge_and_stable_ids() -> void {
  IRContext ctx;
  MidModule module;
  module.ctx = &ctx;
  LinearFunction func;
  func.next_block_id = 100;

  auto entry = std::make_unique<Block>(11, "entry");
  auto exit = std::make_unique<Block>(73, "exit");
  auto *p_entry = entry.get();
  auto *p_exit = exit.get();

  auto *add = module.make_op(OpCode::Add);
  p_entry->insts.push_back(add);
  add_jump(module, p_entry, p_exit);
  p_exit->insts.push_back(module.make_op(OpCode::Ret));
  func.blocks.push_back(std::move(entry));
  func.blocks.push_back(std::move(exit));

  CFGEditor cfg(module, func);
  cfg.synchronize();
  auto *middle = cfg.create_block("middle", p_exit);
  assert(middle);
  assert(middle->id >= 100);
  assert(p_entry->id == 11 && p_exit->id == 73);

  assert(cfg.redirect_edge(p_entry, p_exit, middle));
  auto *middle_jump = module.make_op(OpCode::Jump);
  middle_jump->successors.push_back(p_exit);
  assert(cfg.set_terminator(middle, middle_jump));
  assert((p_entry->succs == std::vector<Block *>{middle}));
  assert((middle->succs == std::vector<Block *>{p_exit}));
  assert(cfg.check_local_consistency());

  assert(cfg.merge_blocks(p_entry, middle));
  assert(func.blocks.size() == 2);
  assert(p_entry->id == 11 && p_exit->id == 73);
  assert(cfg.check_local_consistency());
  std::cout << "test_merge_and_stable_ids passed!\n";
}

auto test_transaction_restores_new_value_users() -> void {
  IRContext ctx;
  MidModule module;
  module.ctx = &ctx;
  LinearFunction func;
  auto entry = std::make_unique<Block>(0, "entry");
  auto *p_entry = entry.get();
  auto *ret = module.make_op(OpCode::Ret);
  p_entry->insts.push_back(ret);
  func.blocks.push_back(std::move(entry));

  CFGEditor cfg(module, func);
  cfg.synchronize();
  Value *temporary = nullptr;
  {
    auto tx = cfg.begin_transaction();
    auto *bad = cfg.create_block("bad");
    auto *add = module.make_op(OpCode::Add);
    temporary = module.ctx->make_value<OpResult>(exodus::I32::get(), add);
    add->result = static_cast<OpResult *>(temporary);
    bad->insts.push_back(add);
    ret->operands.push_back(temporary);
    temporary->addUse(ret);
    assert(!tx.commit());
  }

  assert(func.blocks.size() == 1);
  assert(ret->operands.empty());
  assert(temporary->users.empty());
  assert(cfg.check_local_consistency());
  std::cout << "test_transaction_restores_new_value_users passed!\n";
}

auto main() -> int {
  test_redirect_updates_cfg_and_phi();
  test_transaction_rolls_back_failed_commit();
  test_consistency_rejects_stale_preds_and_incomplete_phi();
  test_merge_and_stable_ids();
  test_transaction_restores_new_value_users();
  return 0;
}
#endif
