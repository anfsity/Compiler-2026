#include "asm_printer.hpp"

#include "../../../3rd-party/fmt/format.h"
#include "instr.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <unordered_map>
#include <variant>

namespace exodus::riscv {
namespace {

using exodus::low_ir::MachineBasicBlock;
using exodus::low_ir::MachineFunction;
using exodus::low_ir::MachineInst;
using exodus::low_ir::MachineOperand;

auto align_to(int value, int align) -> int {
  return ((value + align - 1) / align) * align;
}

auto block_label(
  const MachineFunction &function, const MachineBasicBlock &block
) -> std::string {
  auto suffix =
    block.name.empty() ? "bb" + std::to_string(block.id) : block.name;
  return ".L" + function.name + "_" + suffix;
}

auto public_function_label(const MachineFunction &function) -> std::string {
  return function.name;
}

auto reg_name(const MachineOperand &operand) -> std::string {
  return get_reg_name(operand.get_reg());
}

auto is_float_reg(int reg) -> bool {
  return reg >= static_cast<int>(F0) && reg <= static_cast<int>(F31);
}

auto saved_reg_slot_size(int reg) -> int { return is_float_reg(reg) ? 4 : 8; }

auto imm_value(const MachineOperand &operand) -> int {
  return std::get<int>(operand.data);
}

auto symbol_name(const MachineOperand &operand) -> std::string {
  return std::get<std::string>(operand.data);
}

auto target_label(
  const MachineFunction &function, const MachineOperand &operand
) -> std::string {
  return block_label(function, *std::get<MachineBasicBlock *>(operand.data));
}

struct FrameLayout {
  int frame_bytes = 0;
  int ra_offset = 0;
  std::unordered_map<int, int> offsets;
  std::unordered_map<int, int> saved_reg_offsets;
};

auto is_callee_saved_reg(int reg) -> bool {
  return reg == static_cast<int>(S1) ||
         (reg >= static_cast<int>(S2) && reg <= static_cast<int>(S11)) ||
         reg == static_cast<int>(FS0) || reg == static_cast<int>(FS1) ||
         (reg >= static_cast<int>(FS2) && reg <= static_cast<int>(FS11));
}

auto collect_used_callee_saved_regs(const MachineFunction &function)
  -> std::vector<int> {
  std::vector<int> regs;
  for (const auto &block : function.blocks) {
    for (const auto &inst : block->insts) {
      for (const auto &operand : inst.operands) {
        if (!operand.is_reg())
          continue;
        auto reg = operand.get_reg();
        if (
          is_callee_saved_reg(reg) &&
          std::find(regs.begin(), regs.end(), reg) == regs.end()
        ) {
          regs.push_back(reg);
        }
      }
    }
  }
  std::sort(regs.begin(), regs.end());
  return regs;
}

auto compute_frame_layout(const MachineFunction &function) -> FrameLayout {
  FrameLayout layout;
  int outgoing_extent = 0;
  auto saved_regs = collect_used_callee_saved_regs(function);

  for (const auto &slot : function.stack_slots) {
    auto size = align_to(slot.size, slot.align);
    switch (slot.kind) {
    case MachineFunction::FrameSlotKind::OutgoingArg:
      layout.offsets[slot.id] = (slot.arg_index - 8) * size;
      outgoing_extent =
        std::max(outgoing_extent, layout.offsets[slot.id] + size);
      break;
    case MachineFunction::FrameSlotKind::Local:
    case MachineFunction::FrameSlotKind::Spill:
    case MachineFunction::FrameSlotKind::IncomingArg:
      break;
    }
  }

  int local_offset = outgoing_extent;
  for (const auto &slot : function.stack_slots) {
    auto size = align_to(slot.size, slot.align);
    switch (slot.kind) {
    case MachineFunction::FrameSlotKind::Local:
    case MachineFunction::FrameSlotKind::Spill:
      local_offset = align_to(local_offset, slot.align);
      layout.offsets[slot.id] = local_offset;
      local_offset += size;
      break;
    case MachineFunction::FrameSlotKind::OutgoingArg:
    case MachineFunction::FrameSlotKind::IncomingArg:
      break;
    }
  }

  auto save_offset = local_offset;
  for (auto reg : saved_regs) {
    auto slot_size = saved_reg_slot_size(reg);
    save_offset = align_to(save_offset, slot_size);
    layout.saved_reg_offsets[reg] = save_offset;
    save_offset += slot_size;
  }
  save_offset = align_to(save_offset, 8);
  layout.ra_offset = save_offset;
  save_offset += 8;

  layout.frame_bytes = align_to(save_offset, 16);
  for (const auto &slot : function.stack_slots) {
    if (slot.kind == MachineFunction::FrameSlotKind::IncomingArg) {
      layout.offsets[slot.id] =
        layout.frame_bytes +
        (slot.arg_index - 8) * align_to(slot.size, slot.align);
    }
  }
  return layout;
}

auto frame_offset(const FrameLayout &layout, const MachineOperand &operand)
  -> int {
  auto slot = std::get<int>(operand.data);
  return layout.offsets.at(slot);
}

auto append_load_address(
  std::string &out, int tmp_reg, std::string_view base, int offset
) -> void {
  out += fmt::format("    li {}, {}\n", get_reg_name(tmp_reg), offset);
  out += fmt::format(
    "    add {}, {}, {}\n", get_reg_name(tmp_reg), base, get_reg_name(tmp_reg)
  );
}

auto choose_scratch_reg(std::initializer_list<std::string_view> conflicts)
  -> int {
  for (auto candidate : {static_cast<int>(T4), static_cast<int>(T3)}) {
    auto candidate_name = get_reg_name(candidate);
    if (
      std::find(conflicts.begin(), conflicts.end(), candidate_name) ==
      conflicts.end()
    ) {
      return candidate;
    }
  }
  return T4;
}

auto append_mem_inst(
  std::string &out,
  std::string_view opcode,
  const std::string &reg,
  int offset,
  const std::string &base,
  int tmp_reg = T4
) -> void {
  if (offset >= -2048 && offset <= 2047) {
    out += fmt::format("    {} {}, {}({})\n", opcode, reg, offset, base);
    return;
  }

  auto addr_tmp = tmp_reg;
  if (reg == get_reg_name(addr_tmp) || base == get_reg_name(addr_tmp)) {
    addr_tmp = choose_scratch_reg({reg, base, get_reg_name(tmp_reg)});
  }
  append_load_address(out, addr_tmp, base, offset);
  out += fmt::format("    {} {}, 0({})\n", opcode, reg, get_reg_name(addr_tmp));
}

auto append_stack_adjust(std::string &out, int amount) -> void {
  if (amount == 0)
    return;
  if (amount >= -2048 && amount <= 2047) {
    out += fmt::format("    addi sp, sp, {}\n", amount);
    return;
  }
  out += fmt::format("    li t6, {}\n", amount);
  out += "    add sp, sp, t6\n";
}

auto append_addi_or_li_add(
  std::string &out, const std::string &dst, const std::string &src, int offset
) -> void {
  if (offset >= -2048 && offset <= 2047) {
    out += fmt::format("    addi {}, {}, {}\n", dst, src, offset);
    return;
  }
  auto tmp = dst == src ? get_reg_name(T4) : dst;
  out += fmt::format("    li {}, {}\n", tmp, offset);
  out += fmt::format("    add {}, {}, {}\n", dst, src, tmp);
}

auto global_words(const ir::InitVal &init, const std::shared_ptr<Type> &type)
  -> std::vector<int> {
  if (std::holds_alternative<int>(init.data)) {
    return {std::get<int>(init.data)};
  }
  if (std::holds_alternative<float>(init.data)) {
    int bits = 0;
    auto value = std::get<float>(init.data);
    std::memcpy(&bits, &value, sizeof(float));
    return {bits};
  }
  if (std::holds_alternative<ir::ZeroInit>(init.data)) {
    return std::vector<int>(type->byte_size() / 4, 0);
  }

  std::vector<int> words;
  for (const auto &element : std::get<ir::InitList>(init.data).values) {
    auto nested = global_words(element, I32::get());
    words.insert(words.end(), nested.begin(), nested.end());
  }
  return words;
}

auto print_global(const ir::GlobalVar &global) -> std::string {
  auto words = global_words(global.init, global.type);
  std::string out;
  out += fmt::format("    .globl {}\n", global.name);
  out += fmt::format("{}:\n", global.name);

  auto all_zero =
    std::all_of(words.begin(), words.end(), [](int word) { return word == 0; });
  if (all_zero) {
    out += fmt::format("    .zero {}\n", global.type->byte_size());
    return out;
  }

  for (auto word : words) {
    out += fmt::format("    .word {}\n", word);
  }
  return out;
}

auto emit_inst(
  const MachineInst &inst,
  const MachineFunction &function,
  const FrameLayout &layout
) -> std::string {
  std::string out;
  auto &ops = inst.operands;

  switch (inst.opcode) {
  case COPY:
    if (ops[0].get_reg() == ops[1].get_reg())
      return out;
    out += fmt::format(
      "    {} {}, {}\n",
      is_float_reg(ops[0].get_reg()) || is_float_reg(ops[1].get_reg()) ? "fmv.s"
                                                                       : "mv",
      reg_name(ops[0]),
      reg_name(ops[1])
    );
    break;
  case ADD:
  case ADDW:
  case SUB:
  case SUBW:
  case MUL:
  case MULW:
  case DIV:
  case DIVW:
  case REM:
  case REMW:
  case AND:
  case OR:
  case XOR:
  case SLL:
  case SRA: {
    const char *name = inst.opcode == ADD    ? "add"
                       : inst.opcode == ADDW ? "addw"
                       : inst.opcode == SUB  ? "sub"
                       : inst.opcode == SUBW ? "subw"
                       : inst.opcode == MUL  ? "mul"
                       : inst.opcode == MULW ? "mulw"
                       : inst.opcode == DIV  ? "div"
                       : inst.opcode == DIVW ? "divw"
                       : inst.opcode == REM  ? "rem"
                       : inst.opcode == REMW ? "remw"
                       : inst.opcode == AND  ? "and"
                       : inst.opcode == OR   ? "or"
                       : inst.opcode == XOR  ? "xor"
                       : inst.opcode == SLL  ? "sll"
                                             : "sra";
    out += fmt::format(
      "    {} {}, {}, {}\n",
      name,
      reg_name(ops[0]),
      reg_name(ops[1]),
      reg_name(ops[2])
    );
    break;
  }
  case ADDI:
    if (ops[2].kind == MachineOperand::FrameIdx) {
      append_addi_or_li_add(
        out, reg_name(ops[0]), reg_name(ops[1]), frame_offset(layout, ops[2])
      );
    } else {
      append_addi_or_li_add(
        out, reg_name(ops[0]), reg_name(ops[1]), imm_value(ops[2])
      );
    }
    break;
  case SLLI:
  case SRLI:
  case SRAI:
  case ANDI:
  case ORI:
  case XORI:
  case SLTIU: {
    const char *name = inst.opcode == SLLI   ? "slli"
                       : inst.opcode == SRLI ? "srli"
                       : inst.opcode == SRAI ? "srai"
                       : inst.opcode == ANDI ? "andi"
                       : inst.opcode == ORI  ? "ori"
                       : inst.opcode == XORI ? "xori"
                                             : "sltiu";
    out += fmt::format(
      "    {} {}, {}, {}\n",
      name,
      reg_name(ops[0]),
      reg_name(ops[1]),
      imm_value(ops[2])
    );
    break;
  }
  case SLT:
  case SLTU:
    out += fmt::format(
      "    {} {}, {}, {}\n",
      inst.opcode == SLT ? "slt" : "sltu",
      reg_name(ops[0]),
      reg_name(ops[1]),
      reg_name(ops[2])
    );
    break;
  case LW:
  case LD:
  case FLW: {
    auto offset = ops[1].kind == MachineOperand::FrameIdx
                    ? frame_offset(layout, ops[1])
                    : imm_value(ops[2]);
    auto base =
      ops[1].kind == MachineOperand::FrameIdx ? "sp" : reg_name(ops[1]);
    auto opcode = inst.opcode == FLW ? "flw" : inst.opcode == LD ? "ld" : "lw";
    append_mem_inst(out, opcode, reg_name(ops[0]), offset, base);
    break;
  }
  case SW:
  case SD:
  case FSW: {
    auto offset = ops[1].kind == MachineOperand::FrameIdx
                    ? frame_offset(layout, ops[1])
                    : imm_value(ops[2]);
    auto base =
      ops[1].kind == MachineOperand::FrameIdx ? "sp" : reg_name(ops[1]);
    auto opcode = inst.opcode == FSW ? "fsw" : inst.opcode == SD ? "sd" : "sw";
    append_mem_inst(out, opcode, reg_name(ops[0]), offset, base);
    break;
  }
  case BEQ:
  case BNE:
  case BLT:
  case BGE:
  case BLTU:
  case BGEU: {
    const char *name = inst.opcode == BEQ    ? "beq"
                       : inst.opcode == BNE  ? "bne"
                       : inst.opcode == BLT  ? "blt"
                       : inst.opcode == BGE  ? "bge"
                       : inst.opcode == BLTU ? "bltu"
                                             : "bgeu";
    out += fmt::format(
      "    {} {}, {}, {}\n",
      name,
      reg_name(ops[0]),
      reg_name(ops[1]),
      target_label(function, ops[2])
    );
    break;
  }
  case JAL:
    if (ops[0].is_reg() && ops[0].get_reg() == ZERO) {
      out += fmt::format("    j {}\n", target_label(function, ops[1]));
    } else {
      out += fmt::format(
        "    jal {}, {}\n", reg_name(ops[0]), target_label(function, ops[1])
      );
    }
    break;
  case JALR:
    out += fmt::format("    jalr {}\n", reg_name(ops[0]));
    break;
  case FADD_S:
  case FSUB_S:
  case FMUL_S:
  case FDIV_S: {
    const char *name = inst.opcode == FADD_S   ? "fadd.s"
                       : inst.opcode == FSUB_S ? "fsub.s"
                       : inst.opcode == FMUL_S ? "fmul.s"
                                               : "fdiv.s";
    out += fmt::format(
      "    {} {}, {}, {}\n",
      name,
      reg_name(ops[0]),
      reg_name(ops[1]),
      reg_name(ops[2])
    );
    break;
  }
  case FEQ_S:
  case FLT_S:
  case FLE_S: {
    const char *name = inst.opcode == FEQ_S   ? "feq.s"
                       : inst.opcode == FLT_S ? "flt.s"
                                              : "fle.s";
    out += fmt::format(
      "    {} {}, {}, {}\n",
      name,
      reg_name(ops[0]),
      reg_name(ops[1]),
      reg_name(ops[2])
    );
    break;
  }
  case FCVT_W_S:
    out += fmt::format(
      "    fcvt.w.s {}, {}, rtz\n", reg_name(ops[0]), reg_name(ops[1])
    );
    break;
  case FCVT_S_W:
    out +=
      fmt::format("    fcvt.s.w {}, {}\n", reg_name(ops[0]), reg_name(ops[1]));
    break;
  case FMV_W_X:
  case FMV_X_W:
    out += fmt::format(
      "    {} {}, {}\n",
      inst.opcode == FMV_W_X ? "fmv.w.x" : "fmv.x.w",
      reg_name(ops[0]),
      reg_name(ops[1])
    );
    break;
  case LI:
    out += fmt::format("    li {}, {}\n", reg_name(ops[0]), imm_value(ops[1]));
    break;
  case LA:
    out +=
      fmt::format("    la {}, {}\n", reg_name(ops[0]), symbol_name(ops[1]));
    break;
  case CALL:
    out += fmt::format("    call {}\n", symbol_name(ops[0]));
    break;
  case RET:
    for (const auto &[reg, offset] : layout.saved_reg_offsets) {
      append_mem_inst(
        out, is_float_reg(reg) ? "flw" : "ld", get_reg_name(reg), offset, "sp"
      );
    }
    append_mem_inst(out, "ld", "ra", layout.ra_offset, "sp");
    append_stack_adjust(out, layout.frame_bytes);
    out += "    ret\n";
    break;
  default:
    throw std::logic_error("unsupported RISC-V opcode in asm printer");
  }

  return out;
}

} // namespace

auto AsmPrinter::to_string(
  const mid_ir::MidModule &module,
  const std::vector<std::unique_ptr<MachineFunction>> &functions
) -> std::string {
  std::string out;
  if (!module.globals.empty()) {
    out += "    .data\n";
    for (auto *global : module.globals) {
      out += print_global(*global);
    }
    out += "\n";
  }

  out += "    .text\n";
  for (const auto &function : functions) {
    out += AsmPrinter::to_string(*function);
  }
  return out;
}

auto AsmPrinter::to_string(const MachineFunction &function) -> std::string {
  auto layout = compute_frame_layout(function);

  std::string out;
  out += fmt::format("    .globl {}\n", public_function_label(function));
  out += fmt::format("{}:\n", public_function_label(function));
  append_stack_adjust(out, -layout.frame_bytes);
  append_mem_inst(out, "sd", "ra", layout.ra_offset, "sp");
  for (const auto &[reg, offset] : layout.saved_reg_offsets) {
    append_mem_inst(
      out, is_float_reg(reg) ? "fsw" : "sd", get_reg_name(reg), offset, "sp"
    );
  }

  for (const auto &block : function.blocks) {
    out += fmt::format("{}:\n", block_label(function, *block));
    for (const auto &inst : block->insts) {
      out += emit_inst(inst, function, layout);
    }
  }
  return out;
}

} // namespace exodus::riscv
