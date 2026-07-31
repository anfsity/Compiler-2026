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

constexpr int kI12Min = -2048;
constexpr int kI12Max = 2047;
constexpr int kI32SignBit = 31;
constexpr int kI32Width = 32;
constexpr int kXlenWidth = 64;

struct ConstantOperationMatch {
  int constant_reg = 0;
  int value = 0;
  int dst = 0;
  int lhs = 0;
  int rhs = 0;
  int source = 0;
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

auto match_constant_operation(
  const MachineInst &li,
  const MachineInst &op,
  const std::unordered_map<int, int> &uses
) -> std::optional<ConstantOperationMatch> {
  if (li.opcode != LI || li.operands.size() != 2)
    return std::nullopt;
  auto constant_reg = reg_at(li, 0);
  auto value = imm_at(li, 1);
  if (!constant_reg || !value || !is_virtual(*constant_reg))
    return std::nullopt;
  auto use_count = uses.find(*constant_reg);
  if (use_count == uses.end() || use_count->second != 1)
    return std::nullopt;
  if (op.operands.size() != 3)
    return std::nullopt;
  auto dst = reg_at(op, 0);
  auto lhs = reg_at(op, 1);
  auto rhs = reg_at(op, 2);
  if (!dst || !lhs || !rhs)
    return std::nullopt;
  if (*lhs != *constant_reg && *rhs != *constant_reg)
    return std::nullopt;
  auto source = *lhs == *constant_reg ? *rhs : *lhs;
  return ConstantOperationMatch{
    *constant_reg, *value, *dst, *lhs, *rhs, source
  };
}

auto lower_add_sub_constant(
  const MachineInst &op, const ConstantOperationMatch &match
) -> std::vector<MachineInst> {
  std::vector<MachineInst> replacement;
  if (op.opcode == ADD || op.opcode == ADDW) {
    if (match.value == 0) {
      replacement.push_back(make_copy(match.dst, match.source));
    } else if (match.value >= kI12Min && match.value <= kI12Max) {
      replacement.push_back(make_unary_imm(
        op.opcode == ADDW ? ADDIW : ADDI, match.dst, match.source, match.value
      ));
    }
  } else if (match.rhs == match.constant_reg && match.value == 0) {
    replacement.push_back(make_copy(match.dst, match.lhs));
  } else if (match.lhs == match.constant_reg && match.value == 0) {
    replacement.push_back(make_binary(op.opcode, match.dst, ZERO, match.rhs));
  }
  return replacement;
}

auto lower_bitwise_constant(
  const MachineInst &op, const ConstantOperationMatch &match
) -> std::vector<MachineInst> {
  std::vector<MachineInst> replacement;
  if (op.opcode == AND && match.value == 0) {
    replacement.push_back(make_copy(match.dst, ZERO));
  } else if (match.value == 0) {
    replacement.push_back(make_copy(match.dst, match.source));
  } else if (match.value >= kI12Min && match.value <= kI12Max) {
    auto immediate_opcode = op.opcode == AND  ? ANDI
                            : op.opcode == OR ? ORI
                                              : XORI;
    replacement.push_back(
      make_unary_imm(immediate_opcode, match.dst, match.source, match.value)
    );
  }
  return replacement;
}

auto lower_shift_constant(
  const MachineInst &op, const ConstantOperationMatch &match
) -> std::vector<MachineInst> {
  std::vector<MachineInst> replacement;
  if (
    match.value < 0 || match.value >= kXlenWidth ||
    match.rhs != match.constant_reg
  ) {
    return replacement;
  }
  auto immediate_opcode = op.opcode == SLL   ? SLLI
                          : op.opcode == SRL ? SRLI
                                             : SRAI;
  replacement.push_back(
    make_unary_imm(immediate_opcode, match.dst, match.lhs, match.value)
  );
  return replacement;
}

auto lower_mul_constant(
  const MachineInst &op, const ConstantOperationMatch &match
) -> std::vector<MachineInst> {
  std::vector<MachineInst> replacement;
  if (match.value == 0) {
    replacement.push_back(make_li(match.dst, 0));
  } else if (match.value == 1) {
    replacement.push_back(make_copy(match.dst, match.source));
  } else if (match.value == -1) {
    replacement.push_back(
      make_binary(op.opcode == MULW ? SUBW : SUB, match.dst, ZERO, match.source)
    );
  } else if (match.value == 2) {
    replacement.push_back(make_binary(
      op.opcode == MULW ? ADDW : ADD, match.dst, match.source, match.source
    ));
  } else if (op.opcode == MUL && is_power_of_two(match.value)) {
    replacement.push_back(make_unary_imm(
      SLLI, match.dst, match.source, log2_power_of_two(match.value)
    ));
  } else if (match.dst != match.source && match.value > 2) {
    auto lower = static_cast<int64_t>(match.value) - 1;
    auto upper = static_cast<int64_t>(match.value) + 1;
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
        make_unary_imm(SLLI, match.dst, match.source, log2_i64(lower))
      );
      replacement.push_back(make_binary(
        op.opcode == MULW ? ADDW : ADD, match.dst, match.dst, match.source
      ));
    } else if (is_power_of_two_i64(upper)) {
      replacement.push_back(
        make_unary_imm(SLLI, match.dst, match.source, log2_i64(upper))
      );
      replacement.push_back(make_binary(
        op.opcode == MULW ? SUBW : SUB, match.dst, match.dst, match.source
      ));
    }
  }
  return replacement;
}

auto lower_div_constant(
  MachineFunction &function,
  const ConstantOperationMatch &match,
  const std::unordered_set<int> &sign_extended_i32_regs,
  bool in_hot_loop
) -> std::vector<MachineInst> {
  std::vector<MachineInst> replacement;
  if (match.rhs != match.constant_reg)
    return replacement;
  if (match.value == 1) {
    replacement.push_back(make_copy(match.dst, match.source));
  } else if (match.value == -1) {
    replacement.push_back(make_binary(SUBW, match.dst, ZERO, match.source));
  } else if (
    is_power_of_two(match.value) && match.value <= (1 << 30) &&
    match.dst != match.source
  ) {
    // Signed division truncates toward zero.  Add the sign-dependent bias
    // before the arithmetic shift to avoid rounding negative values down.
    auto shift = log2_power_of_two(match.value);
    if (sign_extended_i32_regs.count(match.source) == 0) {
      replacement.push_back(make_unary_imm(ADDIW, match.dst, match.source, 0));
      replacement.push_back(
        make_unary_imm(SRAI, match.dst, match.dst, kI32SignBit)
      );
    } else {
      replacement.push_back(
        make_unary_imm(SRAI, match.dst, match.source, kI32SignBit)
      );
    }
    if (match.value - 1 <= kI12Max) {
      replacement.push_back(
        make_unary_imm(ANDI, match.dst, match.dst, match.value - 1)
      );
    } else {
      auto mask = function.new_vreg();
      replacement.push_back(make_li(mask, match.value - 1));
      replacement.push_back(make_binary(AND, match.dst, match.dst, mask));
    }
    replacement.push_back(
      make_binary(ADDW, match.dst, match.source, match.dst)
    );
    replacement.push_back(make_unary_imm(SRAI, match.dst, match.dst, shift));
  } else if (in_hot_loop && match.dst != match.source) {
    auto magic = signed_division_magic(match.value);
    if (!magic)
      return replacement;
    auto multiplier = function.new_vreg();
    auto correction = function.new_vreg();
    auto normalized = match.source;
    replacement.push_back(make_li(multiplier, magic->multiplier));
    if (sign_extended_i32_regs.count(match.source) == 0) {
      replacement.push_back(make_unary_imm(ADDIW, correction, match.source, 0));
      normalized = correction;
    }
    replacement.push_back(make_binary(MUL, match.dst, normalized, multiplier));
    replacement.push_back(
      make_unary_imm(SRAI, match.dst, match.dst, kI32Width)
    );
    if (match.value > 0 && magic->multiplier < 0) {
      replacement.push_back(
        make_binary(ADDW, match.dst, match.dst, normalized)
      );
    } else if (match.value < 0 && magic->multiplier > 0) {
      replacement.push_back(
        make_binary(SUBW, match.dst, match.dst, normalized)
      );
    }
    if (magic->shift > 0)
      replacement.push_back(
        make_unary_imm(SRAI, match.dst, match.dst, magic->shift)
      );
    replacement.push_back(
      make_unary_imm(SRLI, correction, match.dst, kXlenWidth - 1)
    );
    replacement.push_back(make_binary(ADDW, match.dst, match.dst, correction));
  }
  return replacement;
}

auto lower_rem_constant(
  MachineFunction &function,
  const ConstantOperationMatch &match,
  const std::unordered_set<int> &sign_extended_i32_regs,
  bool in_loop
) -> std::vector<MachineInst> {
  std::vector<MachineInst> replacement;
  if (match.rhs != match.constant_reg)
    return replacement;
  if (match.value == 1 || match.value == -1) {
    replacement.push_back(make_li(match.dst, 0));
    return replacement;
  }

  auto scratch = match.constant_reg;
  if (scratch == match.dst || scratch == match.source)
    scratch = function.new_vreg();
  auto result = match.dst;
  if (result == match.source || result == scratch)
    result = function.new_vreg();
  auto finish_result = [&] {
    if (result != match.dst)
      replacement.push_back(make_copy(match.dst, result));
  };

  auto absolute_divisor = match.value < 0 ? -static_cast<int64_t>(match.value)
                                          : static_cast<int64_t>(match.value);
  if (
    absolute_divisor <= std::numeric_limits<int>::max() &&
    is_power_of_two(static_cast<int>(absolute_divisor))
  ) {
    // For d = 2^k, signed remainder can be computed over the full i32
    // domain as ((x + bias) & (d - 1)) - bias, where bias is d - 1 for
    // negative x and zero otherwise.
    auto divisor = static_cast<int>(absolute_divisor);
    auto shift = log2_power_of_two(divisor);
    if (sign_extended_i32_regs.count(match.source) == 0) {
      replacement.push_back(make_unary_imm(ADDIW, scratch, match.source, 0));
      replacement.push_back(
        make_unary_imm(SRAI, scratch, scratch, kI32SignBit)
      );
    } else {
      replacement.push_back(
        make_unary_imm(SRAI, scratch, match.source, kI32SignBit)
      );
    }
    replacement.push_back(
      make_unary_imm(SRLI, scratch, scratch, kXlenWidth - shift)
    );
    replacement.push_back(make_binary(ADDW, result, match.source, scratch));
    if (divisor - 1 <= kI12Max) {
      replacement.push_back(make_unary_imm(ANDI, result, result, divisor - 1));
    } else {
      replacement.push_back(
        make_unary_imm(SLLI, result, result, kXlenWidth - shift)
      );
      replacement.push_back(
        make_unary_imm(SRLI, result, result, kXlenWidth - shift)
      );
    }
    replacement.push_back(make_binary(SUBW, result, result, scratch));
    finish_result();
    return replacement;
  }

  if (!in_loop)
    return replacement;
  auto magic = signed_division_magic(match.value);
  if (!magic)
    return replacement;

  auto normalized = match.source;
  if (sign_extended_i32_regs.count(match.source) == 0) {
    normalized = function.new_vreg();
    replacement.push_back(make_unary_imm(ADDIW, normalized, match.source, 0));
  }
  replacement.push_back(make_li(scratch, magic->multiplier));
  replacement.push_back(make_binary(MUL, result, normalized, scratch));
  replacement.push_back(make_unary_imm(SRAI, result, result, kI32Width));
  if (match.value > 0 && magic->multiplier < 0) {
    replacement.push_back(make_binary(ADDW, result, result, normalized));
  } else if (match.value < 0 && magic->multiplier > 0) {
    replacement.push_back(make_binary(SUBW, result, result, normalized));
  }
  if (magic->shift > 0)
    replacement.push_back(make_unary_imm(SRAI, result, result, magic->shift));
  replacement.push_back(make_unary_imm(SRLI, scratch, result, kXlenWidth - 1));
  replacement.push_back(make_binary(ADDW, result, result, scratch));
  replacement.push_back(make_li(scratch, match.value));
  replacement.push_back(make_binary(MULW, result, result, scratch));
  replacement.push_back(make_binary(SUBW, result, normalized, result));
  finish_result();
  return replacement;
}

// Match operations with a one-use LI immediately before the operation.
// Materializing an arbitrary constant is more expensive than an immediate
// form, and this pattern is the canonical output of isel.  Rule families are
// delegated to small lowerers so target-specific arithmetic does not share one
// growing switch body.
auto simplify_constant_operation(
  MachineFunction &function,
  MachineBasicBlock &block,
  std::list<MachineInst>::iterator li,
  std::list<MachineInst>::iterator op,
  const std::unordered_map<int, int> &uses,
  const std::unordered_set<int> &sign_extended_i32_regs,
  bool in_hot_loop,
  bool in_loop
) -> bool {
  auto match = match_constant_operation(*li, *op, uses);
  if (!match)
    return false;

  std::vector<MachineInst> replacement;
  switch (op->opcode) {
  case ADD:
  case ADDW:
  case SUB:
  case SUBW:
    replacement = lower_add_sub_constant(*op, *match);
    break;
  case OR:
  case XOR:
  case AND:
    replacement = lower_bitwise_constant(*op, *match);
    break;
  case SLL:
  case SRL:
  case SRA:
    replacement = lower_shift_constant(*op, *match);
    break;
  case MUL:
  case MULW:
    replacement = lower_mul_constant(*op, *match);
    break;
  case DIVW:
    replacement =
      lower_div_constant(function, *match, sign_extended_i32_regs, in_hot_loop);
    break;
  case REMW:
    replacement =
      lower_rem_constant(function, *match, sign_extended_i32_regs, in_loop);
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
  std::unordered_set<MachineBasicBlock *> loop_blocks;
  const auto &loops = loop_info.get_loops();
  for (const auto &loop : loops) {
    loop_blocks.insert(loop.blocks.begin(), loop.blocks.end());
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
                                         hot_loop_blocks.count(&block) != 0,
                                         loop_blocks.count(&block) != 0
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
