#ifdef EXODUS_UNIT_TEST
#include "../src/low/riscv/asm_printer.hpp"
#include "../src/low/riscv/instr.hpp"
#include <cstdlib>
#include <string>

using namespace exodus::low_ir;
using namespace exodus::riscv;

auto require(bool condition) -> void {
  if (!condition) {
    std::abort();
  }
}

auto test_stack_frame_is_aligned_and_indexed_from_sp() -> void {
  MachineFunction mf;
  mf.name = "frame";
  auto *entry =
    mf.blocks.emplace_back(std::make_unique<MachineBasicBlock>(0, "entry"))
      .get();
  auto local = mf.add_stack_slot(4);

  entry->insts.emplace_back(PROLOGUE);
  entry->insts.emplace_back(ADDI)
    .add_reg(T0, true, false)
    .add_reg(SP)
    .add_fi(local);
  entry->insts.emplace_back(RET);

  auto text = AsmPrinter::to_string(mf);
  require(text.find("addi sp, sp, -16") != std::string::npos);
  require(text.find("sd ra, 8(sp)") != std::string::npos);
  require(text.find("addi t0, sp, 0") != std::string::npos);
  require(text.find("ld ra, 8(sp)") != std::string::npos);
  require(text.find("addi sp, sp, 16") != std::string::npos);
}

auto test_outgoing_and_incoming_arg_offsets() -> void {
  MachineFunction mf;
  mf.name = "args";
  auto *entry =
    mf.blocks.emplace_back(std::make_unique<MachineBasicBlock>(0, "entry"))
      .get();
  auto outgoing = mf.add_outgoing_arg_slot(8, 8, 8);
  auto incoming = mf.add_incoming_arg_slot(9, 8, 8);

  entry->insts.emplace_back(PROLOGUE);
  entry->insts.emplace_back(SW).add_reg(A0).add_fi(outgoing).add_imm(0);
  entry->insts.emplace_back(LW)
    .add_reg(A1, true, false)
    .add_fi(incoming)
    .add_imm(0);
  entry->insts.emplace_back(RET);

  auto text = AsmPrinter::to_string(mf);
  require(text.find("addi sp, sp, -16") != std::string::npos);
  require(text.find("sw a0, 0(sp)") != std::string::npos);
  require(text.find("lw a1, 24(sp)") != std::string::npos);
}

auto test_outgoing_arg_offsets_reuse_call_slot() -> void {
  MachineFunction mf;
  mf.name = "outgoing_reuse";
  auto *entry =
    mf.blocks.emplace_back(std::make_unique<MachineBasicBlock>(0, "entry"))
      .get();
  auto first_call_arg8 = mf.add_outgoing_arg_slot(8, 8, 8);
  auto first_call_arg9 = mf.add_outgoing_arg_slot(9, 8, 8);
  auto second_call_arg8 = mf.add_outgoing_arg_slot(8, 8, 8);

  entry->insts.emplace_back(PROLOGUE);
  entry->insts.emplace_back(SD).add_reg(A0).add_fi(first_call_arg8).add_imm(0);
  entry->insts.emplace_back(SD).add_reg(A1).add_fi(first_call_arg9).add_imm(0);
  entry->insts.emplace_back(SD).add_reg(A2).add_fi(second_call_arg8).add_imm(0);
  entry->insts.emplace_back(RET);

  auto text = AsmPrinter::to_string(mf);
  auto first = text.find("sd a0, 0(sp)");
  auto second = text.find("sd a1, 8(sp)");
  auto reused = text.find("sd a2, 0(sp)");
  require(first != std::string::npos);
  require(second != std::string::npos);
  require(reused != std::string::npos);
  require(text.find("sd a2, 16(sp)") == std::string::npos);
}

auto test_float_copy_prints_float_move() -> void {
  MachineFunction mf;
  mf.name = "float_copy";
  auto *entry =
    mf.blocks.emplace_back(std::make_unique<MachineBasicBlock>(0, "entry"))
      .get();
  entry->insts.emplace_back(COPY).add_reg(FT0, true, false).add_reg(FA0);
  entry->insts.emplace_back(RET_NOFRAME);

  auto text = AsmPrinter::to_string(mf);
  require(text.find("fmv.s ft0, fa0") != std::string::npos);
}

auto test_float_to_int_conversion_uses_truncation() -> void {
  MachineFunction mf;
  mf.name = "f2i";
  auto *entry =
    mf.blocks.emplace_back(std::make_unique<MachineBasicBlock>(0, "entry"))
      .get();
  entry->insts.emplace_back(FCVT_W_S).add_reg(A0, true, false).add_reg(FA0);
  entry->insts.emplace_back(RET_NOFRAME);

  auto text = AsmPrinter::to_string(mf);
  require(text.find("fcvt.w.s a0, fa0, rtz") != std::string::npos);
}

auto test_callee_saved_registers_are_saved() -> void {
  MachineFunction mf;
  mf.name = "saved";
  auto *entry =
    mf.blocks.emplace_back(std::make_unique<MachineBasicBlock>(0, "entry"))
      .get();

  entry->insts.emplace_back(PROLOGUE);
  entry->insts.emplace_back(COPY).add_reg(S1, true, false).add_reg(A0);
  entry->insts.emplace_back(RET);

  auto text = AsmPrinter::to_string(mf);
  require(text.find("sd s1, 0(sp)") != std::string::npos);
  require(text.find("ld s1, 0(sp)") != std::string::npos);
}

auto test_block_labels_are_function_scoped() -> void {
  MachineFunction first;
  first.name = "first";
  first.blocks.emplace_back(std::make_unique<MachineBasicBlock>(0, "entry"));

  MachineFunction second;
  second.name = "second";
  second.blocks.emplace_back(std::make_unique<MachineBasicBlock>(0, "entry"));

  auto first_text = AsmPrinter::to_string(first);
  auto second_text = AsmPrinter::to_string(second);

  require(first_text.find(".Lfirst_entry:") != std::string::npos);
  require(second_text.find(".Lsecond_entry:") != std::string::npos);
  require(first_text.find(".Lentry:") == std::string::npos);
  require(second_text.find(".Lentry:") == std::string::npos);
}

auto test_large_offset_does_not_clobber_memory_reg() -> void {
  MachineFunction mf;
  mf.name = "large_offset";
  auto *entry =
    mf.blocks.emplace_back(std::make_unique<MachineBasicBlock>(0, "entry"))
      .get();

  mf.add_stack_slot(4096, 8);
  auto large = mf.add_stack_slot(8, 8);
  entry->insts.emplace_back(PROLOGUE);
  entry->insts.emplace_back(SD).add_reg(T6).add_fi(large).add_imm(0);
  entry->insts.emplace_back(RET);

  auto text = AsmPrinter::to_string(mf);
  require(text.find("sd t6, 0(t6)") == std::string::npos);
  require(text.find("sd t6, 0(t5)") != std::string::npos);
}

auto test_large_offset_keeps_non_stack_base() -> void {
  MachineFunction mf;
  mf.name = "large_base";
  auto *entry =
    mf.blocks.emplace_back(std::make_unique<MachineBasicBlock>(0, "entry"))
      .get();

  entry->insts.emplace_back(LW)
    .add_reg(A0, true, false)
    .add_reg(S1)
    .add_imm(4096);
  entry->insts.emplace_back(RET_NOFRAME);

  auto text = AsmPrinter::to_string(mf);
  require(text.find("add t6, s1, t6") != std::string::npos);
  require(text.find("add t6, sp, t6") == std::string::npos);
}

int main() {
  test_stack_frame_is_aligned_and_indexed_from_sp();
  test_outgoing_and_incoming_arg_offsets();
  test_outgoing_arg_offsets_reuse_call_slot();
  test_float_copy_prints_float_move();
  test_float_to_int_conversion_uses_truncation();
  test_callee_saved_registers_are_saved();
  test_block_labels_are_function_scoped();
  test_large_offset_does_not_clobber_memory_reg();
  test_large_offset_keeps_non_stack_base();
  return 0;
}

#endif
