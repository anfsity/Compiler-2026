#include "machine_printer.hpp"
#include "../../../3rd-party/fmt/format.h"
#include "../../../3rd-party/fmt/ranges.h"
#include "instr.hpp"
#include <string_view>
#include <unordered_map>
#include <vector>

namespace exodus::riscv {

namespace {

using namespace std::string_view_literals;
using namespace exodus::low_ir;

auto get_op_name(int op) -> std::string {
  static const std::unordered_map<int, std::string_view> names = {
    {PHI, "PHI"sv},
    {COPY, "COPY"sv},
    {ADD, "ADD"sv},
    {ADDW, "ADDW"sv},
    {ADDI, "ADDI"sv},
    {SUB, "SUB"sv},
    {SUBW, "SUBW"sv},
    {LUI, "LUI"sv},
    {AUIPC, "AUIPC"sv},
    {SLL, "SLL"sv},
    {SLLI, "SLLI"sv},
    {SRL, "SRL"sv},
    {SRLI, "SRLI"sv},
    {SRA, "SRA"sv},
    {SRAI, "SRAI"sv},
    {AND, "AND"sv},
    {ANDI, "ANDI"sv},
    {OR, "OR"sv},
    {ORI, "ORI"sv},
    {XOR, "XOR"sv},
    {XORI, "XORI"sv},
    {SLT, "SLT"sv},
    {SLTI, "SLTI"sv},
    {SLTU, "SLTU"sv},
    {SLTIU, "SLTIU"sv},
    {LW, "LW"sv},
    {LD, "LD"sv},
    {LH, "LH"sv},
    {LB, "LB"sv},
    {LHU, "LHU"sv},
    {LBU, "LBU"sv},
    {SW, "SW"sv},
    {SD, "SD"sv},
    {SH, "SH"sv},
    {SB, "SB"sv},
    {BEQ, "BEQ"sv},
    {BNE, "BNE"sv},
    {BLT, "BLT"sv},
    {BGE, "BGE"sv},
    {BLTU, "BLTU"sv},
    {BGEU, "BGEU"sv},
    {JAL, "JAL"sv},
    {JALR, "JALR"sv},
    {MUL, "MUL"sv},
    {MULW, "MULW"sv},
    {MULH, "MULH"sv},
    {MULHSU, "MULHSU"sv},
    {MULHU, "MULHU"sv},
    {DIV, "DIV"sv},
    {DIVW, "DIVW"sv},
    {DIVU, "DIVU"sv},
    {REM, "REM"sv},
    {REMW, "REMW"sv},
    {REMU, "REMU"sv},
    {FLW, "FLW"sv},
    {FSW, "FSW"sv},
    {FADD_S, "FADD.S"sv},
    {FSUB_S, "FSUB.S"sv},
    {FMUL_S, "FMUL.S"sv},
    {FDIV_S, "FDIV.S"sv},
    {FSQRT_S, "FSQRT.S"sv},
    {FSGNJ_S, "FSGNJ.S"sv},
    {FSGNJN_S, "FSGNJN.S"sv},
    {FSGNJX_S, "FSGNJX.S"sv},
    {FEQ_S, "FEQ.S"sv},
    {FLT_S, "FLT.S"sv},
    {FLE_S, "FLE.S"sv},
    {FCVT_W_S, "FCVT.W.S"sv},
    {FCVT_WU_S, "FCVT.WU.S"sv},
    {FCVT_S_W, "FCVT.S.W"sv},
    {FCVT_S_WU, "FCVT.S.WU"sv},
    {FMV_X_W, "FMV.X.W"sv},
    {FMV_W_X, "FMV.W.X"sv},
    {LI, "LI"sv},
    {LA, "LA"sv},
    {CALL, "CALL"sv},
    {RET, "RET"sv},
    {ADJSTACKDOWN, "ADJSTACKDOWN"sv},
    {ADJSTACKUP, "ADJSTACKUP"sv}
  };
  if (auto it = names.find(op); it != names.end())
    return std::string(it->second);
  return "OP_" + std::to_string(op);
}

auto block_label(const MachineBasicBlock &mbb) -> std::string {
  return mbb.name.empty() ? "L" + std::to_string(mbb.id) : mbb.name;
}

auto print_operand(const MachineOperand &mo) -> std::string {
  switch (mo.kind) {
  case MachineOperand::Reg: {
    int id = mo.get_reg();
    if (id < 128)
      return get_reg_name(id);
    return "%v" + std::to_string(id);
  }
  case MachineOperand::Imm:
    return std::to_string(std::get<int>(mo.data));
  case MachineOperand::FImm:
    return std::to_string(std::get<float>(mo.data));
  case MachineOperand::FrameIdx:
    return fmt::format("%stack.{}", std::get<int>(mo.data));
  case MachineOperand::MBB:
    return block_label(*std::get<MachineBasicBlock *>(mo.data));
  case MachineOperand::Global:
    return "@" + std::get<std::string>(mo.data);
  case MachineOperand::Symbol:
    return std::get<std::string>(mo.data);
  }
  return "?";
}

auto print_inst(const MachineInst &mi) -> std::string {
  std::vector<std::string> defs, uses;
  for (const auto &mo : mi.operands) {
    if (mo.is_def()) {
      defs.push_back(print_operand(mo));
    } else {
      uses.push_back(print_operand(mo));
    }
  }

  std::string line = "    ";
  if (!defs.empty()) {
    line += fmt::format("{} = ", fmt::join(defs, ", "));
  }
  line += fmt::format("{:<8}", get_op_name(mi.opcode));
  if (!uses.empty()) {
    line += fmt::format(" {}", fmt::join(uses, ", "));
  }
  line += "\n";
  return line;
}

auto print_block(const MachineBasicBlock &mbb) -> std::string {
  std::string res = fmt::format("{}:", block_label(mbb));

  if (!mbb.preds.empty() || !mbb.succs.empty()) {
    std::vector<std::string> ps, ss;
    for (auto *p : mbb.preds) {
      ps.push_back(block_label(*p));
    }
    for (auto *s : mbb.succs) {
      ss.push_back(block_label(*s));
    }
    res += fmt::format(
      "  # preds: [{}], succs: [{}]", fmt::join(ps, ", "), fmt::join(ss, ", ")
    );
  }
  res += "\n";

  for (const auto &mi : mbb.insts) {
    res += print_inst(mi);
  }
  return res;
}

static auto frame_slot_kind_name(MachineFunction::FrameSlotKind kind)
  -> std::string_view {
  switch (kind) {
  case MachineFunction::FrameSlotKind::Local:
    return "local"sv;
  case MachineFunction::FrameSlotKind::Spill:
    return "spill"sv;
  case MachineFunction::FrameSlotKind::OutgoingArg:
    return "outgoing_arg"sv;
  case MachineFunction::FrameSlotKind::IncomingArg:
    return "incoming_arg"sv;
  }
  return "unknown"sv;
}

} // namespace

auto MachinePrinter::to_string(
  const mid_ir::MidModule &module,
  const std::vector<std::unique_ptr<MachineFunction>> &functions
) -> std::string {
  std::string res;

  for (auto *global : module.globals) {
    res += dump_global(global->addr, global->init, global->type);
  }

  for (const auto &function : module.functions) {
    if (!function->is_decl) {
      continue;
    }

    reset_context();
    std::string args;
    for (size_t i = 0; i < function->args.size(); ++i) {
      args += get_value_name(function->args[i]);
      if (i + 1 < function->args.size()) {
        args += ", ";
      }
    }
    res += fmt::format(
      "decl @{}({}) : {}\n", function->name, args, function->type->to_string()
    );
  }

  for (const auto &function : functions) {
    res += MachinePrinter::to_string(*function);
  }

  return res;
}

auto MachinePrinter::to_string(const MachineFunction &mf) -> std::string {
  std::string res = fmt::format("function @{} {{\n", mf.name);

  for (const auto &slot : mf.stack_slots) {
    res += fmt::format(
      "  %stack.{} = {}, size {}, align {}, offset {}",
      slot.id,
      frame_slot_kind_name(slot.kind),
      slot.size,
      slot.align,
      slot.offset
    );
    if (slot.arg_index >= 0) {
      res += fmt::format(", arg {}", slot.arg_index);
    }
    res += "\n";
  }

  for (const auto &mbb : mf.blocks) {
    res += print_block(*mbb);
  }

  res += "}\n";
  return res;
}

} // namespace exodus::riscv
