#include "peephole.hpp"

#include "instr.hpp"
#include <iterator>
#include <list>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace exodus::riscv {
namespace {

using low_ir::MachineBasicBlock;
using low_ir::MachineFunction;
using low_ir::MachineInst;
using low_ir::MachineOperand;

auto reg_at(const MachineInst &inst, size_t index) -> std::optional<int> {
  if (index >= inst.operands.size() || !inst.operands[index].is_reg())
    return std::nullopt;
  return inst.operands[index].get_reg();
}

auto imm_at(const MachineInst &inst, size_t index) -> std::optional<int> {
  if (
    index >= inst.operands.size() ||
    inst.operands[index].kind != MachineOperand::Imm
  )
    return std::nullopt;
  return std::get<int>(inst.operands[index].data);
}

auto is_virtual(int reg) -> bool { return reg >= 128; }

auto is_power_of_two(int value) -> bool {
  return value > 0 && (value & (value - 1)) == 0;
}

auto log2_power_of_two(int value) -> int {
  int shift = 0;
  while (value > 1) {
    value >>= 1;
    ++shift;
  }
  return shift;
}

auto count_uses(const MachineFunction &function)
  -> std::unordered_map<int, int> {
  std::unordered_map<int, int> uses;
  for (const auto &block : function.blocks) {
    for (const auto &inst : block->insts) {
      for (const auto &operand : inst.operands) {
        if (!operand.is_reg())
          continue;
        const auto &reg = std::get<MachineOperand::RegData>(operand.data);
        if (reg.is_use && is_virtual(reg.id))
          ++uses[reg.id];
      }
    }
  }
  return uses;
}

auto make_copy(int dst, int src) -> MachineInst {
  return MachineInst(COPY).add_reg(dst, true, false).add_reg(src);
}

auto make_li(int dst, int value) -> MachineInst {
  return MachineInst(LI).add_reg(dst, true, false).add_imm(value);
}

auto make_binary(int opcode, int dst, int lhs, int rhs) -> MachineInst {
  return MachineInst(opcode)
    .add_reg(dst, true, false)
    .add_reg(lhs)
    .add_reg(rhs);
}

auto make_unary_imm(int opcode, int dst, int src, int value) -> MachineInst {
  return MachineInst(opcode)
    .add_reg(dst, true, false)
    .add_reg(src)
    .add_imm(value);
}

auto replace_op(
  MachineBasicBlock &block,
  std::list<MachineInst>::iterator it,
  std::vector<MachineInst> replacement
) -> void {
  auto next = std::next(it);
  block.insts.erase(it);
  for (auto &inst : replacement)
    block.insts.insert(next, std::move(inst));
}

auto replace_constant_op(
  MachineBasicBlock &block,
  std::list<MachineInst>::iterator constant,
  std::list<MachineInst>::iterator op,
  std::vector<MachineInst> replacement
) -> void {
  auto next = std::next(op);
  block.insts.erase(constant);
  block.insts.erase(op);
  for (auto &inst : replacement)
    block.insts.insert(next, std::move(inst));
}

// Match operations with a one-use LI immediately before the operation.
// Materializing an arbitrary constant is more expensive than an immediate
// form, and this pattern is the canonical output of isel.
auto simplify_constant_operation(
  MachineFunction &function,
  MachineBasicBlock &block,
  std::list<MachineInst>::iterator li,
  std::list<MachineInst>::iterator op,
  const std::unordered_map<int, int> &uses
) -> bool {
  if (li->opcode != LI || li->operands.size() != 2)
    return false;
  auto constant_reg = reg_at(*li, 0);
  auto value = imm_at(*li, 1);
  if (!constant_reg || !value || !is_virtual(*constant_reg))
    return false;
  auto use_count = uses.find(*constant_reg);
  if (use_count == uses.end() || use_count->second != 1)
    return false;

  if (op->operands.size() != 3)
    return false;
  auto dst = reg_at(*op, 0);
  auto lhs = reg_at(*op, 1);
  auto rhs = reg_at(*op, 2);
  if (!dst || !lhs || !rhs)
    return false;

  auto source = *lhs == *constant_reg ? rhs : lhs;
  if (*lhs != *constant_reg && *rhs != *constant_reg)
    return false;

  std::vector<MachineInst> replacement;
  switch (op->opcode) {
  case ADD:
  case ADDW:
    if (*value == 0) {
      replacement.push_back(make_copy(*dst, *source));
    } else if (*value >= -2048 && *value <= 2047) {
      replacement.push_back(
        make_unary_imm(op->opcode == ADDW ? ADDIW : ADDI, *dst, *source, *value)
      );
    }
    break;
  case SUB:
  case SUBW:
    if (*rhs == *constant_reg && *value == 0) {
      replacement.push_back(make_copy(*dst, *lhs));
    } else if (*lhs == *constant_reg && *value == 0) {
      replacement.push_back(make_binary(op->opcode, *dst, ZERO, *rhs));
    }
    break;
  case OR:
  case XOR:
    if (*value == 0) {
      replacement.push_back(make_copy(*dst, *source));
    } else if (*value >= -2048 && *value <= 2047) {
      replacement.push_back(
        make_unary_imm(op->opcode == OR ? ORI : XORI, *dst, *source, *value)
      );
    }
    break;
  case AND:
    if (*value == 0) {
      replacement.push_back(make_copy(*dst, ZERO));
    } else if (*value >= -2048 && *value <= 2047) {
      replacement.push_back(make_unary_imm(ANDI, *dst, *source, *value));
    }
    break;
  case SLL:
  case SRL:
  case SRA:
    if (*value >= 0 && *value < 64) {
      auto immediate_opcode = *rhs == *constant_reg
                                ? (op->opcode == SLL   ? SLLI
                                   : op->opcode == SRL ? SRLI
                                                       : SRAI)
                                : 0;
      if (immediate_opcode != 0)
        replacement.push_back(
          make_unary_imm(immediate_opcode, *dst, *lhs, *value)
        );
    }
    break;
  case MULW:
  case MUL:
    if (*value == 0) {
      replacement.push_back(make_li(*dst, 0));
    } else if (*value == 1) {
      replacement.push_back(make_copy(*dst, *source));
    } else if (*value == -1) {
      replacement.push_back(
        make_binary(op->opcode == MULW ? SUBW : SUB, *dst, ZERO, *source)
      );
    } else if (*value == 2) {
      replacement.push_back(
        make_binary(op->opcode == MULW ? ADDW : ADD, *dst, *source, *source)
      );
    } else if (op->opcode == MUL && is_power_of_two(*value)) {
      replacement.push_back(
        make_unary_imm(SLLI, *dst, *source, log2_power_of_two(*value))
      );
    }
    break;
  case DIVW:
    if (*value == 1) {
      replacement.push_back(make_copy(*dst, *source));
    } else if (*value == -1) {
      replacement.push_back(make_binary(SUBW, *dst, ZERO, *source));
    } else if (is_power_of_two(*value) && *value <= (1 << 30)) {
      // Signed division truncates toward zero.  Add the sign-dependent bias
      // before the arithmetic shift to avoid rounding negative values down.
      // Reuse the result register for the short-lived sign, bias, and
      // adjusted values; this keeps the expansion from raising spill pressure.
      if (*dst == *source)
        break;
      auto shift = log2_power_of_two(*value);
      replacement.push_back(make_unary_imm(SRAI, *dst, *source, 31));
      if (*value - 1 <= 2047) {
        replacement.push_back(make_unary_imm(ANDI, *dst, *dst, *value - 1));
      } else {
        auto mask = function.new_vreg();
        replacement.push_back(make_li(mask, *value - 1));
        replacement.push_back(make_binary(AND, *dst, *dst, mask));
      }
      replacement.push_back(make_binary(ADDW, *dst, *source, *dst));
      replacement.push_back(make_unary_imm(SRAI, *dst, *dst, shift));
    }
    break;
  case REMW:
    if (*value == 1 || *value == -1)
      replacement.push_back(make_li(*dst, 0));
    break;
  default:
    break;
  }

  if (replacement.empty())
    return false;
  replace_constant_op(block, li, op, std::move(replacement));
  return true;
}

auto simplify_local_operation(
  MachineBasicBlock &block, std::list<MachineInst>::iterator it
) -> bool {
  auto &inst = *it;
  if (inst.operands.size() < 2)
    return false;

  auto dst = reg_at(inst, 0);
  auto lhs = reg_at(inst, 1);
  if (!dst || !lhs)
    return false;

  if (inst.opcode == COPY && *dst == *lhs) {
    block.insts.erase(it);
    return true;
  }

  if (inst.operands.size() == 3) {
    auto rhs = reg_at(inst, 2);
    if (!rhs)
      return false;
    if (
      (inst.opcode == ADD || inst.opcode == SUB || inst.opcode == OR ||
       inst.opcode == XOR || inst.opcode == SLL || inst.opcode == SRL ||
       inst.opcode == SRA) &&
      *rhs == ZERO
    ) {
      replace_op(block, it, {make_copy(*dst, *lhs)});
      return true;
    }
    if (inst.opcode == AND && (*lhs == ZERO || *rhs == ZERO)) {
      replace_op(block, it, {make_copy(*dst, ZERO)});
      return true;
    }
  }

  if (
    inst.operands.size() == 3 && inst.operands[2].kind == MachineOperand::Imm
  ) {
    auto imm = imm_at(inst, 2);
    if (!imm)
      return false;
    if (
      (inst.opcode == ADDI || inst.opcode == ORI || inst.opcode == XORI) &&
      *imm == 0
    ) {
      replace_op(block, it, {make_copy(*dst, *lhs)});
      return true;
    }
    if (inst.opcode == ANDI && *imm == 0) {
      replace_op(block, it, {make_copy(*dst, ZERO)});
      return true;
    }
  }
  return false;
}

} // namespace

auto run_peephole(MachineFunction &function) -> void {
  bool changed = true;
  while (changed) {
    changed = false;
    auto uses = count_uses(function);
    for (auto &block_ptr : function.blocks) {
      auto &block = *block_ptr;
      for (auto it = block.insts.begin(); it != block.insts.end();) {
        auto next = std::next(it);
        if (
          next != block.insts.end() &&
          simplify_constant_operation(function, block, it, next, uses)
        ) {
          changed = true;
          it = block.insts.begin();
          continue;
        }
        if (simplify_local_operation(block, it)) {
          changed = true;
          it = block.insts.begin();
          continue;
        }
        it = next;
      }
    }
  }
}

} // namespace exodus::riscv
