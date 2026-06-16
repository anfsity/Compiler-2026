#include "../src/low/riscv/isel.hpp"
#include <cstdlib>
#include <iostream>

using namespace exodus;
using namespace exodus::ir;
using namespace exodus::low_ir;
using namespace exodus::mid_ir;
using namespace exodus::riscv;

auto require(bool condition) -> void {
  if (!condition) {
    std::abort();
  }
}

auto test_select_add_and_ret() -> void {
  IRContext ctx;
  MidModule module;
  module.ctx = &ctx;

  LinearFunction func;
  func.name = "add";
  func.type = Func::get(I32::get(), {I32::get(), I32::get()});
  auto *lhs = ctx.make_value<Argument>(I32::get(), 0);
  auto *rhs = ctx.make_value<Argument>(I32::get(), 1);
  func.args = {lhs, rhs};

  auto entry = std::make_unique<Block>(0, "entry");
  auto *add = module.make_op(OpCode::Add);
  add->operands = {lhs, rhs};
  add->result = ctx.make_value<OpResult>(I32::get(), add);
  entry->insts.push_back(add);

  auto *ret = module.make_op(OpCode::Ret);
  ret->operands = {add->result};
  entry->insts.push_back(ret);
  func.blocks.push_back(std::move(entry));

  auto mf = lower_function(func);
  auto &insts = mf->blocks.front()->insts;
  require(insts.size() == 5);

  auto it = insts.begin();
  require((it++)->opcode == COPY);
  require((it++)->opcode == COPY);
  require((it++)->opcode == ADD);
  require((it++)->opcode == COPY);
  require((it++)->opcode == RET);

  std::cout << "test_select_add_and_ret passed!\n";
}

auto get_imm(const MachineOperand &operand) -> int {
  require(operand.kind == MachineOperand::Imm);
  return std::get<int>(operand.data);
}

auto get_fi(const MachineOperand &operand) -> int {
  require(operand.kind == MachineOperand::FrameIdx);
  return std::get<int>(operand.data);
}

auto get_symbol(const MachineOperand &operand) -> std::string {
  require(operand.kind == MachineOperand::Symbol);
  return std::get<std::string>(operand.data);
}

auto test_select_multidim_getptr_stride() -> void {
  IRContext ctx;
  MidModule module;
  module.ctx = &ctx;

  auto i32 = I32::get();
  auto array3 = Array::get(i32, 3);
  auto array2x3 = Array::get(array3, 2);
  auto ptr_array = Ptr::get(array2x3);

  LinearFunction func;
  func.name = "addr";
  func.type = Func::get(Ptr::get(i32), {ptr_array});
  auto *base = ctx.make_value<Argument>(ptr_array, 0);
  func.args = {base};

  auto entry = std::make_unique<Block>(0, "entry");
  auto *idx1 = ctx.make_const(i32, 1);
  auto *idx2 = ctx.make_const(i32, 2);

  auto *getptr = module.make_op(OpCode::GetPtr);
  getptr->operands = {base, idx1, idx2};
  getptr->result = ctx.make_value<OpResult>(Ptr::get(i32), getptr);
  entry->insts.push_back(getptr);

  auto *ret = module.make_op(OpCode::Ret);
  ret->operands = {getptr->result};
  entry->insts.push_back(ret);
  func.blocks.push_back(std::move(entry));

  auto mf = lower_function(func);
  auto &insts = mf->blocks.front()->insts;

  bool saw_first_dim_scale = false;
  bool saw_second_dim_shift = false;
  for (const auto &mi : insts) {
    if (
      mi.opcode == LI && mi.operands.size() == 2 &&
      get_imm(mi.operands[1]) == 12
    ) {
      saw_first_dim_scale = true;
    }
    if (
      mi.opcode == SLLI && mi.operands.size() == 3 &&
      get_imm(mi.operands[2]) == 2
    ) {
      saw_second_dim_shift = true;
    }
  }

  require(saw_first_dim_scale);
  require(saw_second_dim_shift);

  std::cout << "test_select_multidim_getptr_stride passed!\n";
}

auto test_select_stack_passed_args() -> void {
  IRContext ctx;
  MidModule module;
  module.ctx = &ctx;

  std::vector<std::shared_ptr<Type>> params(10, I32::get());
  LinearFunction func;
  func.name = "stack_args";
  func.type = Func::get(I32::get(), params);

  for (int i = 0; i < 10; ++i) {
    func.args.push_back(ctx.make_value<Argument>(I32::get(), i));
  }

  auto entry = std::make_unique<Block>(0, "entry");
  auto *call = module.make_op(OpCode::Call, CallPayload{"callee"});
  call->operands.insert(
    call->operands.end(), func.args.begin(), func.args.end()
  );
  call->result = ctx.make_value<OpResult>(I32::get(), call);
  entry->insts.push_back(call);

  auto *ret = module.make_op(OpCode::Ret);
  ret->operands = {call->result};
  entry->insts.push_back(ret);
  func.blocks.push_back(std::move(entry));

  auto mf = lower_function(func);

  int incoming = 0;
  int outgoing = 0;
  for (const auto &slot : mf->stack_slots) {
    if (slot.kind == MachineFunction::FrameSlotKind::IncomingArg) {
      require(slot.arg_index == 8 || slot.arg_index == 9);
      ++incoming;
    }
    if (slot.kind == MachineFunction::FrameSlotKind::OutgoingArg) {
      require(slot.arg_index == 8 || slot.arg_index == 9);
      ++outgoing;
    }
  }
  require(incoming == 2);
  require(outgoing == 2);

  bool saw_incoming_load = false;
  bool saw_outgoing_store = false;
  for (const auto &mi : mf->blocks.front()->insts) {
    if (mi.opcode == LW && mi.operands.size() == 3) {
      auto slot = mf->stack_slots[get_fi(mi.operands[1])];
      saw_incoming_load |=
        slot.kind == MachineFunction::FrameSlotKind::IncomingArg;
    }
    if (mi.opcode == SW && mi.operands.size() == 3) {
      auto slot = mf->stack_slots[get_fi(mi.operands[1])];
      saw_outgoing_store |=
        slot.kind == MachineFunction::FrameSlotKind::OutgoingArg;
    }
  }
  require(saw_incoming_load);
  require(saw_outgoing_store);

  std::cout << "test_select_stack_passed_args passed!\n";
}

auto test_select_global_addr() -> void {
  IRContext ctx;
  MidModule module;
  module.ctx = &ctx;

  LinearFunction func;
  func.name = "global";
  func.type = Func::get(I32::get(), {});

  auto *global = ctx.make_value<GlobalAddr>(Ptr::get(I32::get()), "g");
  auto entry = std::make_unique<Block>(0, "entry");

  auto *load = module.make_op(OpCode::Load);
  load->operands = {global};
  load->result = ctx.make_value<OpResult>(I32::get(), load);
  entry->insts.push_back(load);

  auto *ret = module.make_op(OpCode::Ret);
  ret->operands = {load->result};
  entry->insts.push_back(ret);
  func.blocks.push_back(std::move(entry));

  auto mf = lower_function(func);
  auto it = mf->blocks.front()->insts.begin();
  require(it->opcode == LA);
  require(it->operands.size() == 2);
  require(get_symbol(it->operands[1]) == "g");

  std::cout << "test_select_global_addr passed!\n";
}

auto test_select_getptr_implicit_load() -> void {
  IRContext ctx;
  MidModule module;
  module.ctx = &ctx;

  auto i32 = I32::get();
  auto arr3 = Array::get(i32, 3);
  auto arr3_ptr = Ptr::get(arr3);
  auto arr3_ptr_ptr = Ptr::get(arr3_ptr);

  LinearFunction func;
  func.name = "implicit_getptr";
  func.type = Func::get(Ptr::get(i32), {arr3_ptr_ptr});
  auto *base = ctx.make_value<Argument>(arr3_ptr_ptr, 0);
  func.args = {base};

  auto entry = std::make_unique<Block>(0, "entry");
  auto *idx0 = ctx.make_const(i32, 0);

  auto *getptr = module.make_op(OpCode::GetPtr);
  getptr->operands = {base, idx0, idx0};
  getptr->result = ctx.make_value<OpResult>(Ptr::get(i32), getptr);
  entry->insts.push_back(getptr);

  auto *ret = module.make_op(OpCode::Ret);
  ret->operands = {getptr->result};
  entry->insts.push_back(ret);
  func.blocks.push_back(std::move(entry));

  auto mf = lower_function(func);

  bool saw_implicit_load = false;
  for (const auto &mi : mf->blocks.front()->insts) {
    if (mi.opcode == LW && mi.operands.size() == 3) {
      saw_implicit_load = true;
    }
  }
  require(saw_implicit_load);

  std::cout << "test_select_getptr_implicit_load passed!\n";
}

#ifdef EXODUS_UNIT_TEST
int main() {
  test_select_add_and_ret();
  test_select_multidim_getptr_stride();
  test_select_stack_passed_args();
  test_select_global_addr();
  test_select_getptr_implicit_load();
  return 0;
}
#endif
