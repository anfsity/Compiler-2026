#include "peephole.hpp"

#include "../cfg_analysis.hpp"
#include "instr.hpp"
#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>
#include <list>
#include <optional>
#include <unordered_map>
#include <unordered_set>
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

struct SignedDivisionMagic {
  int multiplier = 0;
  int shift = 0;
};

// Hacker's Delight, signed 32-bit invariant division.  The returned
// multiplier/shift pair is valid over the complete i32 domain; callers still
// handle 0, +/-1, powers of two, and INT_MIN separately.
auto signed_division_magic(int divisor) -> std::optional<SignedDivisionMagic> {
  if (
    divisor == 0 || divisor == 1 || divisor == -1 ||
    divisor == std::numeric_limits<int32_t>::min()
  ) {
    return std::nullopt;
  }

  auto absolute_divisor = divisor < 0 ? -static_cast<int64_t>(divisor)
                                      : static_cast<int64_t>(divisor);
  auto ad = static_cast<uint64_t>(absolute_divisor);
  constexpr uint64_t two31 = uint64_t{1} << 31;
  auto unsigned_divisor = static_cast<uint32_t>(divisor);
  auto t = two31 + (static_cast<uint64_t>(unsigned_divisor) >> 31);
  auto anc = t - 1 - t % ad;
  int p = 31;
  auto q1 = two31 / anc;
  auto r1 = two31 - q1 * anc;
  auto q2 = two31 / ad;
  auto r2 = two31 - q2 * ad;
  uint64_t delta = 0;
  do {
    ++p;
    q1 *= 2;
    r1 *= 2;
    if (r1 >= anc) {
      ++q1;
      r1 -= anc;
    }
    q2 *= 2;
    r2 *= 2;
    if (r2 >= ad) {
      ++q2;
      r2 -= ad;
    }
    delta = ad - r2;
  } while (q1 < delta || (q1 == delta && r1 == 0));

  auto multiplier = static_cast<int64_t>(q2 + 1);
  if (divisor < 0)
    multiplier = -multiplier;
  constexpr int64_t two32 = int64_t{1} << 32;
  if (multiplier > std::numeric_limits<int32_t>::max())
    multiplier -= two32;
  if (multiplier < std::numeric_limits<int32_t>::min())
    multiplier += two32;
  return SignedDivisionMagic{static_cast<int>(multiplier), p - 32};
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

auto compute_sign_extended_i32_regs(const MachineFunction &function)
  -> std::unordered_set<int> {
  std::unordered_set<int> proven;
  auto is_proven = [&](int reg) {
    return reg == ZERO || proven.count(reg) != 0;
  };

  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto &block : function.blocks) {
      for (const auto &inst : block->insts) {
        auto dst = reg_at(inst, 0);
        if (!dst || !is_virtual(*dst) || proven.count(*dst) != 0)
          continue;

        bool result_is_sign_extended = false;
        switch (inst.opcode) {
        case LI:
        case LW:
        case LH:
        case LB:
        case LHU:
        case LBU:
        case ADDW:
        case ADDIW:
        case SUBW:
        case MULW:
        case DIVW:
        case REMW:
        case SLT:
        case SLTI:
        case SLTU:
        case SLTIU:
        case FCVT_W_S:
        case FCVT_WU_S:
        case FMV_X_W:
          result_is_sign_extended = true;
          break;
        case COPY:
          if (auto source = reg_at(inst, 1))
            result_is_sign_extended = is_proven(*source);
          break;
        case PHI: {
          result_is_sign_extended = inst.operands.size() >= 3;
          for (size_t i = 1; i + 1 < inst.operands.size(); i += 2) {
            if (
              !inst.operands[i].is_reg() ||
              !is_proven(inst.operands[i].get_reg())
            ) {
              result_is_sign_extended = false;
              break;
            }
          }
          break;
        }
        case AND:
        case OR:
        case XOR:
          if (auto lhs = reg_at(inst, 1); lhs && is_proven(*lhs)) {
            if (auto rhs = reg_at(inst, 2))
              result_is_sign_extended = is_proven(*rhs);
          }
          break;
        case ANDI:
        case ORI:
        case XORI:
        case SRAI:
          if (auto source = reg_at(inst, 1))
            result_is_sign_extended = is_proven(*source);
          break;
        case SRA:
          if (auto source = reg_at(inst, 1))
            result_is_sign_extended = is_proven(*source);
          break;
        default:
          break;
        }

        if (result_is_sign_extended)
          changed |= proven.insert(*dst).second;
      }
    }
  }
  return proven;
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
  const std::unordered_map<int, int> &uses,
  const std::unordered_set<int> &sign_extended_i32_regs,
  bool in_hot_loop
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
    } else if (*dst != *source && *value > 2) {
      auto lower = static_cast<int64_t>(*value) - 1;
      auto upper = static_cast<int64_t>(*value) + 1;
      auto is_power_of_two_i64 = [](int64_t candidate) {
        return candidate > 0 && (candidate & (candidate - 1)) == 0;
      };
      auto log2_i64 = [](int64_t candidate) {
        int shift = 0;
        while (candidate > 1) {
          candidate >>= 1;
          ++shift;
        }
        return shift;
      };
      if (is_power_of_two_i64(lower)) {
        replacement.push_back(
          make_unary_imm(SLLI, *dst, *source, log2_i64(lower))
        );
        replacement.push_back(
          make_binary(op->opcode == MULW ? ADDW : ADD, *dst, *dst, *source)
        );
      } else if (is_power_of_two_i64(upper)) {
        replacement.push_back(
          make_unary_imm(SLLI, *dst, *source, log2_i64(upper))
        );
        replacement.push_back(
          make_binary(op->opcode == MULW ? SUBW : SUB, *dst, *dst, *source)
        );
      }
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
      // ADDIW first establishes the sign-extension that DIVW would apply to
      // an arbitrary i32 producer.  Plain RV64 bitwise operations do not
      // preserve that property, so using SRAI directly on source is unsound.
      // Reuse the result register for the normalized value, sign, bias, and
      // adjusted values to avoid raising spill pressure.
      if (*dst == *source)
        break;
      auto shift = log2_power_of_two(*value);
      if (sign_extended_i32_regs.count(*source) == 0) {
        replacement.push_back(make_unary_imm(ADDIW, *dst, *source, 0));
        replacement.push_back(make_unary_imm(SRAI, *dst, *dst, 31));
      } else {
        replacement.push_back(make_unary_imm(SRAI, *dst, *source, 31));
      }
      if (*value - 1 <= 2047) {
        replacement.push_back(make_unary_imm(ANDI, *dst, *dst, *value - 1));
      } else {
        auto mask = function.new_vreg();
        replacement.push_back(make_li(mask, *value - 1));
        replacement.push_back(make_binary(AND, *dst, *dst, mask));
      }
      replacement.push_back(make_binary(ADDW, *dst, *source, *dst));
      replacement.push_back(make_unary_imm(SRAI, *dst, *dst, shift));
    } else if (in_hot_loop && *dst != *source) {
      auto magic = signed_division_magic(*value);
      if (!magic)
        break;
      auto multiplier = function.new_vreg();
      auto correction = function.new_vreg();
      auto normalized = *source;
      replacement.push_back(make_li(multiplier, magic->multiplier));
      // DIVW interprets only the low i32 and sign-extends it.  Normalize the
      // dividend explicitly before the 64-bit multiply so this expansion is
      // valid for every machine producer, including AND/OR/XOR/SLL.
      if (sign_extended_i32_regs.count(*source) == 0) {
        replacement.push_back(make_unary_imm(ADDIW, correction, *source, 0));
        normalized = correction;
      }
      replacement.push_back(make_binary(MUL, *dst, normalized, multiplier));
      replacement.push_back(make_unary_imm(SRAI, *dst, *dst, 32));
      if (*value > 0 && magic->multiplier < 0) {
        replacement.push_back(make_binary(ADDW, *dst, *dst, normalized));
      } else if (*value < 0 && magic->multiplier > 0) {
        replacement.push_back(make_binary(SUBW, *dst, *dst, normalized));
      }
      if (magic->shift > 0)
        replacement.push_back(make_unary_imm(SRAI, *dst, *dst, magic->shift));
      replacement.push_back(make_unary_imm(SRLI, correction, *dst, 63));
      replacement.push_back(make_binary(ADDW, *dst, *dst, correction));
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
  low_ir::DominatorTree dom;
  dom.compute(function);
  low_ir::LoopInfo loop_info;
  loop_info.compute(function, dom);
  std::unordered_set<MachineBasicBlock *> hot_loop_blocks;
  const auto &loops = loop_info.get_loops();
  for (const auto &loop : loops) {
    auto has_subloop =
      std::any_of(loops.begin(), loops.end(), [&](const auto &other) {
        return &other != &loop && loop.blocks.count(other.header) != 0 &&
               other.blocks.size() < loop.blocks.size();
      });
    if (!has_subloop)
      hot_loop_blocks.insert(loop.blocks.begin(), loop.blocks.end());
  }

  bool changed = true;
  while (changed) {
    changed = false;
    auto uses = count_uses(function);
    auto sign_extended_i32_regs = compute_sign_extended_i32_regs(function);
    for (auto &block_ptr : function.blocks) {
      auto &block = *block_ptr;
      for (auto it = block.insts.begin(); it != block.insts.end();) {
        auto next = std::next(it);
        if (
          next != block.insts.end() && simplify_constant_operation(
                                         function,
                                         block,
                                         it,
                                         next,
                                         uses,
                                         sign_extended_i32_regs,
                                         hot_loop_blocks.count(&block) != 0
                                       )
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
