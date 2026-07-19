#include "effects.hpp"

#include "../base/getptr.hpp"

namespace exodus::high_ir {
namespace {

auto pure_effects(bool speculatable = true) -> OpEffects {
  OpEffects effects;
  effects.speculatable = speculatable;
  effects.cseable = true;
  return effects;
}

} // namespace

auto get_addr_root(Value *value) -> Value * {
  while (value && value->kind == ValueKind::OpResult) {
    auto *creator = static_cast<Op *>(static_cast<OpResult *>(value)->creator);
    if (
      !creator || creator->code != OpCode::GetPtr || creator->operands.empty()
    ) {
      break;
    }
    value = creator->operands[0];
  }
  return value;
}

auto get_region_effects(const Region &region) -> OpEffects {
  OpEffects effects;
  for (const auto *op : region) {
    effects.merge(get_op_effects(*op));
    if (op->code == OpCode::If) {
      const auto &payload = std::get<IfPayload>(op->payload);
      effects.merge(get_region_effects(*payload.then_region));
      if (payload.else_region)
        effects.merge(get_region_effects(*payload.else_region));
    } else if (op->code == OpCode::While) {
      const auto &payload = std::get<WhilePayload>(op->payload);
      effects.merge(get_region_effects(*payload.cond_region));
      effects.merge(get_region_effects(*payload.loop_region));
    }
  }
  return effects;
}

auto get_op_effects(const Op &op) -> OpEffects {
  switch (op.code) {
  case OpCode::Add:
  case OpCode::Sub:
  case OpCode::Mul:
  case OpCode::FAdd:
  case OpCode::FSub:
  case OpCode::FMul:
  case OpCode::I2F:
  case OpCode::F2I:
  case OpCode::ZExt:
  case OpCode::Eq:
  case OpCode::Ne:
  case OpCode::Lt:
  case OpCode::Gt:
  case OpCode::Le:
  case OpCode::Ge:
  case OpCode::And:
  case OpCode::Or:
  case OpCode::Xor:
  case OpCode::Shl:
  case OpCode::Shr:
    return pure_effects();
  case OpCode::Div:
  case OpCode::Mod:
  case OpCode::FDiv:
    return pure_effects(false);
  case OpCode::Alloca: {
    OpEffects effects;
    effects.has_unique_identity = true;
    return effects;
  }
  case OpCode::Load: {
    OpEffects effects;
    if (!op.operands.empty())
      effects.reads.insert(op.operands[0]);
    return effects;
  }
  case OpCode::Store: {
    OpEffects effects;
    if (op.operands.size() >= 2)
      effects.writes.insert(op.operands[1]);
    return effects;
  }
  case OpCode::GetPtr: {
    auto effects = pure_effects();
    if (
      op.result && !op.operands.empty() && op.operands[0]->type->is_ptr() &&
      op.result->type->is_ptr()
    ) {
      auto plan = ir::analyze_getptr(
        op.operands[0]->type, op.result->type, op.operands.size() - 1
      );
      if (plan.reads_memory) {
        effects.reads.insert(op.operands[0]);
        effects.speculatable = false;
        effects.cseable = false;
      }
    }
    return effects;
  }
  case OpCode::Memset: {
    OpEffects effects;
    if (!op.operands.empty())
      effects.writes.insert(op.operands[0]);
    return effects;
  }
  case OpCode::Call: {
    OpEffects effects;
    effects.has_unknown_effect = true;
    for (auto *operand : op.operands) {
      if (operand && operand->type->is_ptr()) {
        effects.reads.insert(operand);
        effects.writes.insert(operand);
      }
    }
    return effects;
  }
  case OpCode::Ret:
  case OpCode::Break:
  case OpCode::Continue:
  case OpCode::Jump:
  case OpCode::Branch: {
    OpEffects effects;
    effects.has_control_effect = true;
    return effects;
  }
  case OpCode::If:
  case OpCode::While:
  case OpCode::Condition:
    return {};
  }

  OpEffects effects;
  effects.has_unknown_effect = true;
  return effects;
}

} // namespace exodus::high_ir
