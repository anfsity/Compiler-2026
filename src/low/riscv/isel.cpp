#include "isel.hpp"

#include "../../base/getptr.hpp"
#include <cassert>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace exodus::riscv {
namespace {

using exodus::ir::Constant;
using exodus::ir::GlobalAddr;
using exodus::ir::OpResult;
using exodus::ir::Value;
using exodus::ir::ValueKind;
using exodus::low_ir::MachineBasicBlock;
using exodus::low_ir::MachineFunction;
using exodus::low_ir::MachineInst;

auto inst(int opcode) -> MachineInst { return MachineInst(opcode); }

auto is_float_value(const Value *v) -> bool { return v->type->is_f32(); }

auto append(MachineBasicBlock &block, InstSeq insts) -> void {
  for (auto &mi : insts) {
    block.insts.push_back(std::move(mi));
  }
}

struct Seq {
  InstSeq insts;

  auto emit(int opcode) -> MachineInst & {
    insts.emplace_back(opcode);
    return insts.back();
  }

  auto take() -> InstSeq { return std::move(insts); }
};

auto storage_size(const std::shared_ptr<Type> &type) -> int {
  return type->is_ptr() ? 8 : type->byte_size();
}

auto new_vreg(LoweringContext &ctx, Value *value) -> int {
  auto reg = ctx.function->new_vreg(storage_size(value->type));
  ctx.value_regs[value] = reg;
  return reg;
}

auto def_reg(LoweringContext &ctx, OpResult *result) -> int {
  assert(result && "instruction result is required");
  if (auto it = ctx.value_regs.find(result); it != ctx.value_regs.end()) {
    return it->second;
  }
  return new_vreg(ctx, result);
}

auto float_bits(float value) -> int {
  static_assert(sizeof(float) == sizeof(int));
  int bits = 0;
  std::memcpy(&bits, &value, sizeof(float));
  return bits;
}

auto storage_align(const std::shared_ptr<Type> &type) -> int {
  if (type->is_ptr())
    return 8;
  if (type->is_array())
    return storage_align(static_cast<Array *>(type.get())->base);
  return 4;
}

auto materialize_int_constant(LoweringContext &ctx, int value, Seq &seq)
  -> int {
  auto reg = ctx.function->new_vreg();
  seq.emit(LI).add_reg(reg, true, false).add_imm(value);
  return reg;
}

auto materialize_float_constant(LoweringContext &ctx, float value, Seq &seq)
  -> int {
  auto bits = materialize_int_constant(ctx, float_bits(value), seq);
  auto reg = ctx.function->new_vreg();
  seq.emit(FMV_W_X).add_reg(reg, true, false).add_reg(bits);
  return reg;
}

auto materialize_global_addr(LoweringContext &ctx, GlobalAddr *global, Seq &seq)
  -> int {
  auto reg = ctx.function->new_vreg(8);
  seq.emit(LA)
    .add_reg(reg, true, false)
    .add_operand(low_ir::MachineOperand::symbol(global->name));
  return reg;
}

auto use_reg(LoweringContext &ctx, Value *value, Seq &seq) -> int {
  if (value->kind == ValueKind::Constant) {
    auto *constant = static_cast<Constant *>(value);
    if (std::holds_alternative<int>(constant->val)) {
      return materialize_int_constant(ctx, std::get<int>(constant->val), seq);
    }
    return materialize_float_constant(ctx, std::get<float>(constant->val), seq);
  }

  if (value->kind == ValueKind::GlobalVar) {
    return materialize_global_addr(ctx, static_cast<GlobalAddr *>(value), seq);
  }

  if (auto it = ctx.value_regs.find(value); it != ctx.value_regs.end()) {
    return it->second;
  }

  return new_vreg(ctx, value);
}

auto select_binary_opcode(mid_ir::OpCode code) -> int {
  switch (code) {
  case mid_ir::OpCode::Add:
    return ADDW;
  case mid_ir::OpCode::Sub:
    return SUBW;
  case mid_ir::OpCode::Mul:
    return MULW;
  case mid_ir::OpCode::Div:
    return DIVW;
  case mid_ir::OpCode::Mod:
    return REMW;
  case mid_ir::OpCode::And:
    return AND;
  case mid_ir::OpCode::Or:
    return OR;
  case mid_ir::OpCode::Xor:
    return XOR;
  case mid_ir::OpCode::Shl:
    return SLL;
  case mid_ir::OpCode::Shr:
    return SRA;
  case mid_ir::OpCode::FAdd:
    return FADD_S;
  case mid_ir::OpCode::FSub:
    return FSUB_S;
  case mid_ir::OpCode::FMul:
    return FMUL_S;
  case mid_ir::OpCode::FDiv:
    return FDIV_S;
  default:
    break;
  }
  throw std::logic_error("unsupported binary opcode");
}

auto select_binary(LoweringContext &ctx, const mid_ir::Op &op) -> InstSeq {
  Seq seq;
  auto dst = def_reg(ctx, op.result);
  auto lhs = use_reg(ctx, op.operands[0], seq);
  auto rhs = use_reg(ctx, op.operands[1], seq);
  seq.emit(select_binary_opcode(op.code))
    .add_reg(dst, true, false)
    .add_reg(lhs)
    .add_reg(rhs);
  return seq.take();
}

auto select_compare(LoweringContext &ctx, const mid_ir::Op &op) -> InstSeq {
  Seq seq;
  auto dst = def_reg(ctx, op.result);
  auto lhs = use_reg(ctx, op.operands[0], seq);
  auto rhs = use_reg(ctx, op.operands[1], seq);
  auto tmp = ctx.function->new_vreg();

  if (is_float_value(op.operands[0])) {
    switch (op.code) {
    case mid_ir::OpCode::Eq:
      seq.emit(FEQ_S).add_reg(dst, true, false).add_reg(lhs).add_reg(rhs);
      return seq.take();
    case mid_ir::OpCode::Lt:
      seq.emit(FLT_S).add_reg(dst, true, false).add_reg(lhs).add_reg(rhs);
      return seq.take();
    case mid_ir::OpCode::Le:
      seq.emit(FLE_S).add_reg(dst, true, false).add_reg(lhs).add_reg(rhs);
      return seq.take();
    case mid_ir::OpCode::Gt:
      seq.emit(FLT_S).add_reg(dst, true, false).add_reg(rhs).add_reg(lhs);
      return seq.take();
    case mid_ir::OpCode::Ge:
      seq.emit(FLE_S).add_reg(dst, true, false).add_reg(rhs).add_reg(lhs);
      return seq.take();
    case mid_ir::OpCode::Ne:
      seq.emit(FEQ_S).add_reg(tmp, true, false).add_reg(lhs).add_reg(rhs);
      seq.emit(XORI).add_reg(dst, true, false).add_reg(tmp).add_imm(1);
      return seq.take();
    default:
      break;
    }
  }

  switch (op.code) {
  case mid_ir::OpCode::Eq:
    seq.emit(XOR).add_reg(tmp, true, false).add_reg(lhs).add_reg(rhs);
    seq.emit(SLTIU).add_reg(dst, true, false).add_reg(tmp).add_imm(1);
    break;
  case mid_ir::OpCode::Ne:
    seq.emit(XOR).add_reg(tmp, true, false).add_reg(lhs).add_reg(rhs);
    seq.emit(SLTU).add_reg(dst, true, false).add_reg(ZERO).add_reg(tmp);
    break;
  case mid_ir::OpCode::Lt:
    seq.emit(SLT).add_reg(dst, true, false).add_reg(lhs).add_reg(rhs);
    break;
  case mid_ir::OpCode::Gt:
    seq.emit(SLT).add_reg(dst, true, false).add_reg(rhs).add_reg(lhs);
    break;
  case mid_ir::OpCode::Le:
    seq.emit(SLT).add_reg(tmp, true, false).add_reg(rhs).add_reg(lhs);
    seq.emit(XORI).add_reg(dst, true, false).add_reg(tmp).add_imm(1);
    break;
  case mid_ir::OpCode::Ge:
    seq.emit(SLT).add_reg(tmp, true, false).add_reg(lhs).add_reg(rhs);
    seq.emit(XORI).add_reg(dst, true, false).add_reg(tmp).add_imm(1);
    break;
  default:
    throw std::logic_error("unsupported compare opcode");
  }
  return seq.take();
}

auto select_cast(LoweringContext &ctx, const mid_ir::Op &op) -> InstSeq {
  Seq seq;
  auto dst = def_reg(ctx, op.result);
  auto src = use_reg(ctx, op.operands[0], seq);

  switch (op.code) {
  case mid_ir::OpCode::I2F:
    seq.emit(FCVT_S_W).add_reg(dst, true, false).add_reg(src);
    break;
  case mid_ir::OpCode::F2I:
    seq.emit(FCVT_W_S).add_reg(dst, true, false).add_reg(src);
    break;
  case mid_ir::OpCode::ZExt:
    seq.emit(ANDI).add_reg(dst, true, false).add_reg(src).add_imm(1);
    break;
  default:
    throw std::logic_error("unsupported cast opcode");
  }

  return seq.take();
}

auto select_alloca(LoweringContext &ctx, const mid_ir::Op &op) -> InstSeq {
  Seq seq;
  auto dst = def_reg(ctx, op.result);
  auto ptr_type = std::static_pointer_cast<Ptr>(op.result->type);
  auto slot = ctx.function->add_stack_slot(
    storage_size(ptr_type->target), storage_align(ptr_type->target)
  );
  seq.emit(ADDI).add_reg(dst, true, false).add_reg(SP).add_fi(slot);
  return seq.take();
}

auto select_load(LoweringContext &ctx, const mid_ir::Op &op) -> InstSeq {
  Seq seq;
  auto dst = def_reg(ctx, op.result);
  auto addr = use_reg(ctx, op.operands[0], seq);
  auto opcode = op.result->type->is_f32()   ? FLW
                : op.result->type->is_ptr() ? LD
                                            : LW;
  seq.emit(opcode).add_reg(dst, true, false).add_reg(addr).add_imm(0);
  return seq.take();
}

auto select_store(LoweringContext &ctx, const mid_ir::Op &op) -> InstSeq {
  Seq seq;
  auto value = use_reg(ctx, op.operands[0], seq);
  auto addr = use_reg(ctx, op.operands[1], seq);
  auto opcode = op.operands[0]->type->is_f32()   ? FSW
                : op.operands[0]->type->is_ptr() ? SD
                                                 : SW;
  seq.emit(opcode).add_reg(value).add_reg(addr).add_imm(0);
  return seq.take();
}

auto pointee_size(const std::shared_ptr<Type> &ptr_type) -> int {
  assert(ptr_type->is_ptr());
  return std::static_pointer_cast<Ptr>(ptr_type)->target->byte_size();
}

auto arg_reg_for_type(const std::shared_ptr<Type> &type, int index) -> int {
  return type->is_f32() ? static_cast<int>(FA0) + index
                        : static_cast<int>(A0) + index;
}

auto select_getptr(LoweringContext &ctx, const mid_ir::Op &op) -> InstSeq {
  Seq seq;
  auto dst = def_reg(ctx, op.result);
  auto base = use_reg(ctx, op.operands[0], seq);
  auto cur = base;

  auto plan = ir::analyze_getptr(
    op.operands[0]->type, op.result->type, op.operands.size() - 1
  );

  bool defined_dst = false;
  for (const auto &step : plan.steps) {
    if (step.kind == ir::GetPtrStep::Kind::ImplicitLoad) {
      auto loaded = ctx.function->new_vreg(8);
      seq.emit(LD).add_reg(loaded, true, false).add_reg(cur).add_imm(0);
      cur = loaded;
      continue;
    }

    auto idx = use_reg(ctx, op.operands[step.index_pos + 1], seq);
    auto scaled = ctx.function->new_vreg();
    if (step.scale == 1) {
      scaled = idx;
    } else if (
      step.scale > 0 && (step.scale & (step.scale - 1)) == 0 &&
      step.scale <= (1 << 10)
    ) {
      int shift = 0;
      for (auto size = step.scale; size > 1; size >>= 1) {
        ++shift;
      }
      seq.emit(SLLI).add_reg(scaled, true, false).add_reg(idx).add_imm(shift);
    } else {
      auto size_reg = materialize_int_constant(ctx, step.scale, seq);
      seq.emit(MUL).add_reg(scaled, true, false).add_reg(idx).add_reg(size_reg);
    }
    auto next = (&step == &plan.steps.back()) ? dst : ctx.function->new_vreg(8);
    seq.emit(ADD).add_reg(next, true, false).add_reg(cur).add_reg(scaled);
    cur = next;
    defined_dst = next == dst;
  }

  if (!defined_dst) {
    seq.emit(COPY).add_reg(dst, true, false).add_reg(cur);
  }

  return seq.take();
}

auto select_call(LoweringContext &ctx, const mid_ir::Op &op) -> InstSeq {
  Seq seq;
  int int_arg = 0;
  int float_arg = 0;

  for (size_t arg_index = 0; arg_index < op.operands.size(); ++arg_index) {
    auto *operand = op.operands[arg_index];
    auto src = use_reg(ctx, operand, seq);
    if (operand->type->is_f32()) {
      if (float_arg < 8) {
        seq.emit(COPY)
          .add_reg(arg_reg_for_type(operand->type, float_arg), true, false)
          .add_reg(src);
      } else {
        auto slot = ctx.function->add_outgoing_arg_slot(
          static_cast<int>(arg_index), 8, 8
        );
        seq.emit(FSW).add_reg(src).add_fi(slot).add_imm(0);
      }
      ++float_arg;
    } else {
      if (int_arg < 8) {
        seq.emit(COPY)
          .add_reg(arg_reg_for_type(operand->type, int_arg), true, false)
          .add_reg(src);
      } else {
        auto slot = ctx.function->add_outgoing_arg_slot(
          static_cast<int>(arg_index), 8, 8
        );
        seq.emit(operand->type->is_ptr() ? SD : SW)
          .add_reg(src)
          .add_fi(slot)
          .add_imm(0);
      }
      ++int_arg;
    }
  }

  const auto &payload = std::get<mid_ir::CallPayload>(op.payload);
  seq.emit(CALL).add_operand(low_ir::MachineOperand::symbol(payload.func_name));

  if (op.result && !op.result->type->is_void()) {
    auto dst = def_reg(ctx, op.result);
    auto ret_reg =
      op.result->type->is_f32() ? static_cast<int>(FA0) : static_cast<int>(A0);
    seq.emit(COPY).add_reg(dst, true, false).add_reg(ret_reg);
  }

  return seq.take();
}

auto select_ret(LoweringContext &ctx, const mid_ir::Op &op) -> InstSeq {
  Seq seq;
  if (!op.operands.empty()) {
    auto value = use_reg(ctx, op.operands[0], seq);
    auto ret_reg = op.operands[0]->type->is_f32() ? static_cast<int>(FA0)
                                                  : static_cast<int>(A0);
    seq.emit(COPY).add_reg(ret_reg, true, false).add_reg(value);
  }
  seq.emit(RET);
  return seq.take();
}

auto select_jump(LoweringContext &ctx, const mid_ir::Op &op) -> InstSeq {
  assert(op.successors.size() == 1);
  Seq seq;
  seq.emit(JAL)
    .add_reg(ZERO, true, false)
    .add_mbb(ctx.block_map.at(op.successors[0]));
  return seq.take();
}

auto select_branch(LoweringContext &ctx, const mid_ir::Op &op) -> InstSeq {
  assert(op.operands.size() == 1);
  assert(op.successors.size() == 2);
  Seq seq;
  auto condition = op.operands[0];
  auto branch_opcode = BNE;
  Value *branch_value = condition;

  if (condition->kind == ValueKind::OpResult) {
    auto *condition_op =
      static_cast<mid_ir::Op *>(static_cast<OpResult *>(condition)->creator);
    if (
      condition_op &&
      (condition_op->code == mid_ir::OpCode::Eq ||
       condition_op->code == mid_ir::OpCode::Ne) &&
      condition_op->operands.size() == 2
    ) {
      auto is_zero = [](Value *value) {
        if (value->kind != ValueKind::Constant)
          return false;
        auto *constant = static_cast<Constant *>(value);
        return std::holds_alternative<int>(constant->val) &&
               std::get<int>(constant->val) == 0;
      };

      if (is_zero(condition_op->operands[1])) {
        branch_value = condition_op->operands[0];
        branch_opcode = condition_op->code == mid_ir::OpCode::Eq ? BEQ : BNE;
      } else if (is_zero(condition_op->operands[0])) {
        branch_value = condition_op->operands[1];
        branch_opcode = condition_op->code == mid_ir::OpCode::Eq ? BEQ : BNE;
      }
    }
  }

  auto cond = use_reg(ctx, branch_value, seq);
  seq.emit(branch_opcode)
    .add_reg(cond)
    .add_reg(ZERO)
    .add_mbb(ctx.block_map.at(op.successors[0]));
  seq.emit(JAL)
    .add_reg(ZERO, true, false)
    .add_mbb(ctx.block_map.at(op.successors[1]));
  return seq.take();
}

auto select_phi(LoweringContext &ctx, const mid_ir::Op &op) -> InstSeq {
  Seq seq;
  auto dst = def_reg(ctx, op.result);
  auto &payload = std::get<mid_ir::PhiPayload>(op.payload);
  std::vector<std::pair<int, MachineBasicBlock *>> incoming;
  incoming.reserve(payload.incoming.size());
  for (auto &[block, value] : payload.incoming) {
    incoming.push_back({use_reg(ctx, value, seq), ctx.block_map.at(block)});
  }

  auto phi = inst(PHI);
  phi.add_reg(dst, true, false);
  for (auto &[value_reg, block] : incoming) {
    phi.add_reg(value_reg);
    phi.add_mbb(block);
  }
  seq.insts.push_back(std::move(phi));
  return seq.take();
}

auto select_memset(LoweringContext &ctx, const mid_ir::Op &op) -> InstSeq {
  Seq seq;
  auto ptr = use_reg(ctx, op.operands[0], seq);
  auto elem_count = use_reg(ctx, op.operands[1], seq);
  auto elem_size = pointee_size(op.operands[0]->type);
  auto byte_count = elem_count;

  if (elem_size > 1) {
    byte_count = ctx.function->new_vreg();
    if ((elem_size & (elem_size - 1)) == 0 && elem_size <= (1 << 10)) {
      auto shift = 0;
      for (auto size = elem_size; size > 1; size >>= 1) {
        ++shift;
      }
      seq.emit(SLLI)
        .add_reg(byte_count, true, false)
        .add_reg(elem_count)
        .add_imm(shift);
    } else {
      auto size_reg = materialize_int_constant(ctx, elem_size, seq);
      seq.emit(MUL)
        .add_reg(byte_count, true, false)
        .add_reg(elem_count)
        .add_reg(size_reg);
    }
  }

  seq.emit(COPY).add_reg(A0, true, false).add_reg(ptr);
  seq.emit(COPY).add_reg(A1, true, false).add_reg(ZERO);
  seq.emit(COPY).add_reg(A2, true, false).add_reg(byte_count);
  seq.emit(CALL).add_operand(low_ir::MachineOperand::symbol("memset"));
  return seq.take();
}

auto bind_arguments(LoweringContext &ctx, const mid_ir::LinearFunction &f)
  -> InstSeq {
  Seq seq;
  int int_arg = 0;
  int float_arg = 0;
  for (size_t arg_index = 0; arg_index < f.args.size(); ++arg_index) {
    auto *arg = f.args[arg_index];
    auto dst = new_vreg(ctx, arg);
    if (arg->type->is_f32()) {
      if (float_arg < 8) {
        seq.emit(COPY)
          .add_reg(dst, true, false)
          .add_reg(arg_reg_for_type(arg->type, float_arg));
      } else {
        auto slot = ctx.function->add_incoming_arg_slot(
          static_cast<int>(arg_index), 8, 8
        );
        seq.emit(FLW).add_reg(dst, true, false).add_fi(slot).add_imm(0);
      }
      ++float_arg;
    } else {
      if (int_arg < 8) {
        seq.emit(COPY)
          .add_reg(dst, true, false)
          .add_reg(arg_reg_for_type(arg->type, int_arg));
      } else {
        auto slot = ctx.function->add_incoming_arg_slot(
          static_cast<int>(arg_index), 8, 8
        );
        seq.emit(arg->type->is_ptr() ? LD : LW)
          .add_reg(dst, true, false)
          .add_fi(slot)
          .add_imm(0);
      }
      ++int_arg;
    }
  }
  return seq.take();
}

} // namespace

auto select_op(LoweringContext &ctx, const mid_ir::Op &op) -> InstSeq {
  switch (op.code) {
  case mid_ir::OpCode::Add:
  case mid_ir::OpCode::Sub:
  case mid_ir::OpCode::Mul:
  case mid_ir::OpCode::Div:
  case mid_ir::OpCode::Mod:
  case mid_ir::OpCode::FAdd:
  case mid_ir::OpCode::FSub:
  case mid_ir::OpCode::FMul:
  case mid_ir::OpCode::FDiv:
  case mid_ir::OpCode::And:
  case mid_ir::OpCode::Or:
  case mid_ir::OpCode::Xor:
  case mid_ir::OpCode::Shl:
  case mid_ir::OpCode::Shr:
    return select_binary(ctx, op);
  case mid_ir::OpCode::Eq:
  case mid_ir::OpCode::Ne:
  case mid_ir::OpCode::Lt:
  case mid_ir::OpCode::Gt:
  case mid_ir::OpCode::Le:
  case mid_ir::OpCode::Ge:
    return select_compare(ctx, op);
  case mid_ir::OpCode::I2F:
  case mid_ir::OpCode::F2I:
  case mid_ir::OpCode::ZExt:
    return select_cast(ctx, op);
  case mid_ir::OpCode::Alloca:
    return select_alloca(ctx, op);
  case mid_ir::OpCode::Load:
    return select_load(ctx, op);
  case mid_ir::OpCode::Store:
    return select_store(ctx, op);
  case mid_ir::OpCode::GetPtr:
    return select_getptr(ctx, op);
  case mid_ir::OpCode::Call:
    return select_call(ctx, op);
  case mid_ir::OpCode::Ret:
    return select_ret(ctx, op);
  case mid_ir::OpCode::Jump:
    return select_jump(ctx, op);
  case mid_ir::OpCode::Branch:
    return select_branch(ctx, op);
  case mid_ir::OpCode::Phi:
    return select_phi(ctx, op);
  case mid_ir::OpCode::Memset:
    return select_memset(ctx, op);
  }
  throw std::logic_error("unsupported mid IR opcode");
}

auto is_dead_def_candidate(int opcode) -> bool {
  switch (opcode) {
  case COPY:
  case ADD:
  case ADDI:
  case SUB:
  case SLL:
  case SLLI:
  case SRL:
  case SRLI:
  case SRA:
  case SRAI:
  case AND:
  case ANDI:
  case OR:
  case ORI:
  case XOR:
  case XORI:
  case SLT:
  case SLTI:
  case SLTU:
  case SLTIU:
  case MUL:
  case DIV:
  case REM:
  case ADDW:
  case SUBW:
  case MULW:
  case DIVW:
  case REMW:
  case FADD_S:
  case FSUB_S:
  case FMUL_S:
  case FDIV_S:
  case FSGNJ_S:
  case FSGNJN_S:
  case FSGNJX_S:
  case FEQ_S:
  case FLT_S:
  case FLE_S:
  case FCVT_W_S:
  case FCVT_WU_S:
  case FCVT_S_W:
  case FCVT_S_WU:
  case FMV_X_W:
  case FMV_W_X:
  case LI:
  case LA:
    return true;
  default:
    return false;
  }
}

auto eliminate_dead_defs(MachineFunction &function) -> void {
  bool changed = true;
  while (changed) {
    changed = false;
    std::unordered_map<int, int> use_count;
    for (const auto &block : function.blocks) {
      for (const auto &mi : block->insts) {
        for (const auto &operand : mi.operands) {
          if (!operand.is_reg())
            continue;
          const auto &reg =
            std::get<low_ir::MachineOperand::RegData>(operand.data);
          if (reg.is_use && reg.id >= 128)
            ++use_count[reg.id];
        }
      }
    }

    for (auto &block : function.blocks) {
      for (auto it = block->insts.begin(); it != block->insts.end();) {
        bool has_virtual_def = false;
        bool removable = is_dead_def_candidate(it->opcode);
        for (const auto &operand : it->operands) {
          if (!operand.is_def())
            continue;
          auto reg = operand.get_reg();
          if (reg < 128 || use_count[reg] != 0) {
            removable = false;
            break;
          }
          has_virtual_def = true;
        }
        if (removable && has_virtual_def) {
          it = block->insts.erase(it);
          changed = true;
          continue;
        }
        ++it;
      }
    }
  }
}

auto lower_function(const mid_ir::LinearFunction &f)
  -> std::unique_ptr<MachineFunction> {
  auto mf = std::make_unique<MachineFunction>();
  mf->name = f.name;

  LoweringContext ctx;
  ctx.function = mf.get();

  for (auto &block : f.blocks) {
    auto mbb = std::make_unique<MachineBasicBlock>(block->id, block->name);
    ctx.block_map[block.get()] = mbb.get();
    mf->blocks.push_back(std::move(mbb));
  }

  for (auto &block : f.blocks) {
    auto *mbb = ctx.block_map.at(block.get());
    for (auto *pred : block->preds) {
      mbb->preds.push_back(ctx.block_map.at(pred));
    }
    for (auto *succ : block->succs) {
      mbb->succs.push_back(ctx.block_map.at(succ));
    }
  }

  bool is_entry = true;
  for (auto &block : f.blocks) {
    ctx.block = ctx.block_map.at(block.get());
    if (is_entry) {
      append(*ctx.block, bind_arguments(ctx, f));
      is_entry = false;
    }
    for (auto *op : block->insts) {
      append(*ctx.block, select_op(ctx, *op));
    }
  }

  eliminate_dead_defs(*mf);

  return mf;
}

} // namespace exodus::riscv
