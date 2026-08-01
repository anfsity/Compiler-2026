#ifdef EXODUS_UNIT_TEST
#include "../src/mid/affine_loop.hpp"
#include "../src/opt/mid/scalar_evolution.hpp"
#include <cassert>
#include <memory>

using namespace exodus;
using namespace exodus::mid_ir;

auto main() -> int {
  IRContext context;
  MidModule module;
  module.ctx = &context;

  LinearFunction function;
  function.name = "counted";

  auto entry = std::make_unique<Block>(0, "entry");
  auto header = std::make_unique<Block>(1, "header");
  auto body = std::make_unique<Block>(2, "body");
  auto exit = std::make_unique<Block>(3, "exit");
  auto *entry_ptr = entry.get();
  auto *header_ptr = header.get();
  auto *body_ptr = body.get();
  auto *exit_ptr = exit.get();

  entry_ptr->succs = {header_ptr};
  header_ptr->preds = {entry_ptr, body_ptr};
  header_ptr->succs = {body_ptr, exit_ptr};
  body_ptr->preds = {header_ptr};
  body_ptr->succs = {header_ptr};
  exit_ptr->preds = {header_ptr};

  auto *zero = context.make_const(I32::get(), 0);
  auto *one = context.make_const(I32::get(), 1);
  auto *bound = context.make_const(I32::get(), 10);

  auto *phi = module.make_op(OpCode::Phi, PhiPayload{});
  phi->result = context.make_value<OpResult>(I32::get(), phi);
  auto *update = module.make_op(OpCode::Add);
  update->operands = {phi->result, one};
  update->result = context.make_value<OpResult>(I32::get(), update);
  std::get<PhiPayload>(phi->payload).incoming = {
    {entry_ptr, zero},
    {body_ptr, update->result},
  };
  auto *compare = module.make_op(OpCode::Lt);
  compare->operands = {phi->result, bound};
  compare->result = context.make_value<OpResult>(Bool::get(), compare);
  auto *header_branch = module.make_op(OpCode::Branch);
  header_branch->operands = {compare->result};
  header_branch->successors = {body_ptr, exit_ptr};
  auto *body_jump = module.make_op(OpCode::Jump);
  body_jump->successors = {header_ptr};
  auto *entry_jump = module.make_op(OpCode::Jump);
  entry_jump->successors = {header_ptr};
  auto *ret = module.make_op(OpCode::Ret);

  phi->result->addUse(update);
  phi->result->addUse(compare);
  one->addUse(update);
  bound->addUse(compare);
  compare->result->addUse(header_branch);
  zero->addUse(phi);
  update->result->addUse(phi);

  entry_ptr->insts.push_back(entry_jump);
  header_ptr->insts.push_back(phi);
  header_ptr->insts.push_back(compare);
  header_ptr->insts.push_back(header_branch);
  body_ptr->insts.push_back(update);
  body_ptr->insts.push_back(body_jump);
  exit_ptr->insts.push_back(ret);
  function.blocks.push_back(std::move(entry));
  function.blocks.push_back(std::move(header));
  function.blocks.push_back(std::move(body));
  function.blocks.push_back(std::move(exit));

  DomTree dom;
  dom.compute(function);
  LoopInfo loops;
  loops.compute(function, dom);
  auto loop_list = loops.get_loops();
  assert(loop_list.size() == 1);

  AffineLoopInfo affine;
  affine.compute(function, dom);
  auto counted = affine.match_counted_loop(*loop_list.front());
  assert(counted);
  auto update_form =
    affine.affine_form(update->result, *counted, *loop_list.front());
  assert(update_form);
  assert(update_form->coefficient == 1);
  assert(update_form->offset == 1);
  assert(affine.is_no_wrap(update->result, *counted, *loop_list.front()));

  ScalarEvolution scalar;
  scalar.compute(function, loops, affine, dom);
  auto expression = scalar.get_affine(update->result, {*counted});
  assert(expression);
  assert(expression->coefficient(phi->result) == 1);
  assert(expression->constant == 1);
  assert(expression->no_wrap);
  return 0;
}
#endif
