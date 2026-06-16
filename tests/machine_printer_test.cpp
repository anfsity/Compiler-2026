#include "../3rd-party/fmt/core.h"
#include "../src/low/riscv/isel.hpp"
#include "../src/low/riscv/machine_printer.hpp"
#include <cstdlib>
#include <string>
#include <vector>

using namespace exodus;
using namespace exodus::ir;
using namespace exodus::low_ir;
using namespace exodus::mid_ir;
using namespace exodus::riscv;

#ifdef EXODUS_UNIT_TEST

auto require(bool condition) -> void {
  if (!condition) {
    std::abort();
  }
}

auto test_printer() -> void {
  IRContext ctx;
  MidModule module;
  module.ctx = &ctx;

  LinearFunction func;
  func.name = "test_func";
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
  auto text = MachinePrinter::to_string(*mf);
  require(text.find("function @test_func {") != std::string::npos);
  require(text.find("entry:") != std::string::npos);
  require(text.find("%v128 = COPY") != std::string::npos);
  require(text.find("a0 = COPY") != std::string::npos);
}

auto test_block_labels_and_frame_slots() -> void {
  MachineFunction mf;
  mf.name = "labels";
  auto *entry =
    mf.blocks.emplace_back(std::make_unique<MachineBasicBlock>(0, "entry"))
      .get();
  auto *exit =
    mf.blocks.emplace_back(std::make_unique<MachineBasicBlock>(1, "exit"))
      .get();
  entry->succs.push_back(exit);
  exit->preds.push_back(entry);

  auto local = mf.add_stack_slot(4);
  auto outgoing = mf.add_outgoing_arg_slot(8, 4);
  mf.stack_slots[local].offset = -4;
  mf.stack_slots[outgoing].offset = 0;

  entry->insts.emplace_back(BNE).add_reg(A0).add_reg(ZERO).add_mbb(exit);
  exit->insts.emplace_back(RET);

  auto text = MachinePrinter::to_string(mf);
  require(
    text.find("%stack.0 = local, size 4, align 4, offset -4") !=
    std::string::npos
  );
  require(
    text.find("%stack.1 = outgoing_arg, size 4, align 4, offset 0, arg 8") !=
    std::string::npos
  );
  require(text.find("entry:  # preds: [], succs: [exit]") != std::string::npos);
  require(text.find("BNE      a0, zero, exit") != std::string::npos);
  require(text.find("L1") == std::string::npos);
}

auto test_global_operand() -> void {
  MachineFunction mf;
  mf.name = "global";
  auto *entry =
    mf.blocks.emplace_back(std::make_unique<MachineBasicBlock>(0, "entry"))
      .get();
  entry->insts.emplace_back(LA)
    .add_reg(128, true, false)
    .add_operand(MachineOperand::global("g"));

  auto text = MachinePrinter::to_string(mf);
  require(text.find("%v128 = LA       @g") != std::string::npos);
}

auto test_module_printer() -> void {
  IRContext ctx;
  MidModule module;
  module.ctx = &ctx;

  auto *addr = ctx.make_value<GlobalAddr>(I32::get()->ptr_to(), "g");
  auto global = std::make_unique<GlobalVar>();
  global->name = "g";
  global->type = I32::get();
  global->init = InitVal{ZeroInit{}};
  global->addr = addr;
  module.globals.push_back(global.get());

  auto decl = std::make_unique<LinearFunction>();
  decl->name = "getint";
  decl->type = Func::get(I32::get(), {});
  decl->is_decl = true;
  module.functions.push_back(std::move(decl));

  auto function = std::make_unique<MachineFunction>();
  function->name = "main";
  function->blocks.emplace_back(
    std::make_unique<MachineBasicBlock>(0, "entry")
  );

  std::vector<std::unique_ptr<MachineFunction>> functions;
  functions.push_back(std::move(function));

  MachinePrinter printer;
  auto text = printer.to_string(module, functions);
  require(text.find("@g = global zeroinit : i32") != std::string::npos);
  require(text.find("decl @getint() : () -> i32") != std::string::npos);
  require(text.find("function @main {") != std::string::npos);
  require(text.find("entry:") != std::string::npos);
}

int main() {
  test_printer();
  test_block_labels_and_frame_slots();
  test_global_operand();
  test_module_printer();
  return 0;
}

#endif
