#include "ra_printer.hpp"

#include "../../../3rd-party/fmt/core.h"
#include "reg_alloca.hpp"

namespace exodus::riscv {
namespace {

auto opcode_name(int opcode) -> std::string_view {
  switch (opcode) {
    // clang-format off
    case PHI: return "PHI";
    case COPY: return "COPY";
    case ADD: return "ADD";
    case ADDW: return "ADDW";
    case ADDI: return "ADDI";
    case SUB: return "SUB";
    case SUBW: return "SUBW";
    case LUI: return "LUI";
    case AUIPC: return "AUIPC";
    case SLL: return "SLL";
    case SLLI: return "SLLI";
    case SRL: return "SRL";
    case SRLI: return "SRLI";
    case SRA: return "SRA";
    case SRAI: return "SRAI";
    case AND: return "AND";
    case ANDI: return "ANDI";
    case OR: return "OR";
    case ORI: return "ORI";
    case XOR: return "XOR";
    case XORI: return "XORI";
    case SLT: return "SLT";
    case SLTI: return "SLTI";
    case SLTU: return "SLTU";
    case SLTIU: return "SLTIU";
    case LW: return "LW";
    case LD: return "LD";
    case LH: return "LH";
    case LB: return "LB";
    case LHU: return "LHU";
    case LBU: return "LBU";
    case SW: return "SW";
    case SD: return "SD";
    case SH: return "SH";
    case SB: return "SB";
    case BEQ: return "BEQ";
    case BNE: return "BNE";
    case BLT: return "BLT";
    case BGE: return "BGE";
    case BLTU: return "BLTU";
    case BGEU: return "BGEU";
    case JAL: return "JAL";
    case JALR: return "JALR";
    case MUL: return "MUL";
    case MULW: return "MULW";
    case MULH: return "MULH";
    case MULHSU: return "MULHSU";
    case MULHU: return "MULHU";
    case DIV: return "DIV";
    case DIVW: return "DIVW";
    case DIVU: return "DIVU";
    case REM: return "REM";
    case REMW: return "REMW";
    case REMU: return "REMU";
    case FLW: return "FLW";
    case FSW: return "FSW";
    case FADD_S: return "FADD.S";
    case FSUB_S: return "FSUB.S";
    case FMUL_S: return "FMUL.S";
    case FDIV_S: return "FDIV.S";
    case FSQRT_S: return "FSQRT.S";
    case FSGNJ_S: return "FSGNJ.S";
    case FSGNJN_S: return "FSGNJN.S";
    case FSGNJX_S: return "FSGNJX.S";
    case FEQ_S: return "FEQ.S";
    case FLT_S: return "FLT.S";
    case FLE_S: return "FLE.S";
    case FCVT_W_S: return "FCVT.W.S";
    case FCVT_WU_S: return "FCVT.WU.S";
    case FCVT_S_W: return "FCVT.S.W";
    case FCVT_S_WU: return "FCVT.S.WU";
    case FMV_X_W: return "FMV.X.W";
    case FMV_W_X: return "FMV.W.X";
    case LI: return "LI";
    case LA: return "LA";
    case CALL: return "CALL";
    case RET: return "RET";
    case RET_NOFRAME: return "RET_NOFRAME";
    case PROLOGUE: return "PROLOGUE";
    case ADJSTACKDOWN: return "ADJSTACKDOWN";
    case ADJSTACKUP: return "ADJSTACKUP";
    default: return "OP";
    // clang-format on
  }
}

auto block_label(const low_ir::MachineBasicBlock &block) -> std::string {
  return block.name.empty() ? "L" + std::to_string(block.id) : block.name;
}

auto interval_start(const LiveInterval &interval) -> int {
  return interval.segments.empty() ? 0 : interval.segments.front().start;
}

auto interval_end(const LiveInterval &interval) -> int {
  return interval.segments.empty() ? 0 : interval.segments.back().end;
}

auto reg_name_for_dump(int reg) -> std::string {
  if (reg < 0)
    return "-";
  return get_reg_name(reg);
}

auto split_kind_name(SplitPoint::Kind kind) -> std::string_view {
  switch (kind) {
  case SplitPoint::Kind::Conflict:
    return "conflict";
  case SplitPoint::Kind::BlockBoundary:
    return "block";
  case SplitPoint::Kind::LoopBoundary:
    return "loop";
  }
  return "unknown";
}

} // namespace

auto RegAllocPrinter::to_string(const low_ir::MachineFunction &function) const
  -> std::string {
  std::string res;
  res += fmt::format("\n--- RA machine IR @{} ---\n", function.name);
  for (const auto &mbb : function.blocks) {
    res += fmt::format("{}:\n", block_label(*mbb));
    for (const auto &mi : mbb->insts) {
      res += "    ";
      if (
        !mi.operands.empty() && mi.operands[0].is_reg() &&
        mi.operands[0].is_def()
      ) {
        res += fmt::format("{} = ", get_reg_name(mi.operands[0].get_reg()));
      }
      res += fmt::format("{:<8}", opcode_name(mi.opcode));
      bool first = true;
      for (const auto &op : mi.operands) {
        if (op.is_def())
          continue;
        res += first ? " " : ", ";
        first = false;
        if (op.kind == low_ir::MachineOperand::Reg) {
          res += get_reg_name(op.get_reg());
        } else if (op.kind == low_ir::MachineOperand::Imm) {
          res += std::to_string(std::get<int>(op.data));
        } else if (op.kind == low_ir::MachineOperand::FrameIdx) {
          res += fmt::format("%stack.{}", std::get<int>(op.data));
        } else if (op.kind == low_ir::MachineOperand::MBB) {
          auto *target = std::get<low_ir::MachineBasicBlock *>(op.data);
          res += block_label(*target);
        } else if (op.kind == low_ir::MachineOperand::Global) {
          res += "@" + std::get<std::string>(op.data);
        } else if (op.kind == low_ir::MachineOperand::Symbol) {
          res += std::get<std::string>(op.data);
        }
      }
      res += "\n";
    }
  }
  return res;
}

auto RegAllocPrinter::dump_function(
  const low_ir::MachineFunction &function
) const -> void {
  if (!enabled_)
    return;
  fmt::print(out_, "{}", to_string(function));
}

auto RegAllocPrinter::intervals_to_string(
  const low_ir::MachineFunction &function,
  std::string_view stage,
  const std::vector<LiveInterval> &intervals
) const -> std::string {
  std::string res;
  res += fmt::format("\n--- RA {} @{} ---\n", stage, function.name);
  for (const auto &interval : intervals) {
    res += fmt::format(
      "v{} [{}..{}) assigned={} spilled={} spill_slot={} cost={{uses:{}, "
      "defs:{}, span:{}, score:{}}}\n",
      interval.vreg,
      interval_start(interval),
      interval_end(interval),
      reg_name_for_dump(interval.assigned_reg),
      interval.spilled ? "yes" : "no",
      interval.spill_slot,
      interval.spill_cost.use_count,
      interval.spill_cost.def_count,
      interval.spill_cost.span,
      interval.spill_cost.score
    );

    res += "  segments:";
    for (const auto &segment : interval.segments) {
      res += fmt::format(" [{}..{})", segment.start, segment.end);
    }
    res += "\n";

    res += "  defs:";
    for (auto pos : interval.def_positions) {
      res += fmt::format(" {}", pos);
    }
    res += "\n";

    res += "  uses:";
    for (auto pos : interval.use_positions) {
      res += fmt::format(" {}", pos);
    }
    res += "\n";

    res += "  hints:";
    for (const auto &hint : interval.hints) {
      res += fmt::format(" {}({})", reg_name_for_dump(hint.reg), hint.score);
    }
    res += "\n";

    if (interval.split_plan.should_split) {
      res += fmt::format(
        "  split: {} at {}\n",
        split_kind_name(interval.split_plan.point.kind),
        interval.split_plan.point.pos
      );
    } else {
      res += "  split: none\n";
    }
  }
  return res;
}

auto RegAllocPrinter::dump_intervals(
  const low_ir::MachineFunction &function,
  std::string_view stage,
  const std::vector<LiveInterval> &intervals
) const -> void {
  if (!enabled_)
    return;
  fmt::print(out_, "{}", intervals_to_string(function, stage, intervals));
}

} // namespace exodus::riscv
