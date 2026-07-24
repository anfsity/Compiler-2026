#include "memory.hpp"

#include "../base/getptr.hpp"
#include <limits>

namespace exodus::mid_ir {
namespace {

auto integer_constant(Value *value) -> std::optional<int64_t> {
  if (!value || value->kind != ValueKind::Constant)
    return std::nullopt;
  auto *constant = static_cast<Constant *>(value);
  if (!std::holds_alternative<int>(constant->val))
    return std::nullopt;
  return std::get<int>(constant->val);
}

auto checked_add(int64_t lhs, int64_t rhs) -> std::optional<int64_t> {
  if (
    (rhs > 0 && lhs > std::numeric_limits<int64_t>::max() - rhs) ||
    (rhs < 0 && lhs < std::numeric_limits<int64_t>::min() - rhs)
  ) {
    return std::nullopt;
  }
  return lhs + rhs;
}

auto checked_mul(int64_t lhs, int64_t rhs) -> std::optional<int64_t> {
  constexpr auto min = std::numeric_limits<int64_t>::min();
  constexpr auto max = std::numeric_limits<int64_t>::max();
  if (lhs == 0 || rhs == 0)
    return int64_t{0};
  if ((lhs == -1 && rhs == min) || (rhs == -1 && lhs == min))
    return std::nullopt;
  if (lhs > 0) {
    if ((rhs > 0 && lhs > max / rhs) || (rhs < 0 && rhs < min / lhs))
      return std::nullopt;
  } else if ((rhs > 0 && lhs < min / rhs) || (rhs < 0 && lhs < max / rhs)) {
    return std::nullopt;
  }
  return lhs * rhs;
}

auto storage_size(const std::shared_ptr<Type> &type) -> size_t {
  return type->is_ptr() ? pointer_storage_size
                        : static_cast<size_t>(type->byte_size());
}

auto is_scalar_global(Value *root) -> bool {
  if (!root || root->kind != ValueKind::GlobalVar || !root->type->is_ptr())
    return false;
  return !std::static_pointer_cast<Ptr>(root->type)->target->is_array();
}

} // namespace

auto BasicAliasAnalysis::get_location(Value *pointer, size_t size) const
  -> MemoryLocation {
  std::unordered_set<Value *> active;
  return get_location_impl(pointer, size, active);
}

auto BasicAliasAnalysis::get_location_impl(
  Value *pointer, size_t size, std::unordered_set<Value *> &active
) const -> MemoryLocation {
  MemoryLocation location{pointer, nullptr, std::nullopt, size};
  if (!pointer)
    return location;
  if (!active.insert(pointer).second)
    return location;
  auto finish = [&active, pointer](MemoryLocation result) {
    active.erase(pointer);
    return result;
  };

  if (
    pointer->kind == ValueKind::GlobalVar ||
    pointer->kind == ValueKind::Argument
  ) {
    location.root = pointer;
    location.offset = 0;
    return finish(location);
  }

  if (pointer->kind != ValueKind::OpResult)
    return finish(location);

  auto *creator = static_cast<Op *>(static_cast<OpResult *>(pointer)->creator);
  if (!creator)
    return finish(location);
  if (creator->code == OpCode::Alloca) {
    location.root = pointer;
    location.offset = 0;
    return finish(location);
  }
  if (creator->code == OpCode::Phi) {
    auto &incoming = std::get<PhiPayload>(creator->payload).incoming;
    bool have_root = false;
    bool has_recurrence = false;
    Value *root = nullptr;
    std::optional<int64_t> offset;
    for (const auto &[pred, value] : incoming) {
      (void)pred;
      if (!value)
        continue;
      bool recurrence = false;
      if (value->kind == ValueKind::OpResult) {
        auto *incoming_op =
          static_cast<Op *>(static_cast<OpResult *>(value)->creator);
        recurrence = incoming_op && incoming_op->code == OpCode::GetPtr &&
                     !incoming_op->operands.empty() &&
                     incoming_op->operands[0] == pointer &&
                     incoming_op->result &&
                     incoming_op->operands[0]->type->is_ptr() &&
                     incoming_op->result->type->is_ptr() &&
                     !ir::analyze_getptr(
                        incoming_op->operands[0]->type,
                        incoming_op->result->type,
                        incoming_op->operands.size() - 1
                     )
                        .reads_memory;
      }
      if (recurrence) {
        has_recurrence = true;
        continue;
      }
      auto incoming_location = get_location_impl(value, size, active);
      if (!incoming_location.root) {
        return finish(location);
      }
      if (!have_root) {
        have_root = true;
        root = incoming_location.root;
        offset = incoming_location.offset;
      } else if (root != incoming_location.root) {
        return finish(location);
      } else if (offset != incoming_location.offset) {
        offset = std::nullopt;
      }
    }
    if (!have_root)
      return finish(location);
    location.root = root;
    location.offset = has_recurrence ? std::nullopt : offset;
    return finish(location);
  }
  if (
    creator->code != OpCode::GetPtr || creator->operands.empty() ||
    !creator->result || !creator->operands[0]->type->is_ptr() ||
    !creator->result->type->is_ptr()
  ) {
    return finish(location);
  }

  auto plan = ir::analyze_getptr(
    creator->operands[0]->type,
    creator->result->type,
    creator->operands.size() - 1
  );
  if (plan.reads_memory)
    return finish(location);

  auto base = get_location_impl(creator->operands[0], size, active);
  location.root = base.root;
  location.offset = base.offset;
  if (!location.root || !location.offset)
    return finish(location);

  for (const auto &step : plan.steps) {
    if (step.kind != ir::GetPtrStep::Kind::Index)
      continue;
    size_t operand_index = step.index_pos + 1;
    if (operand_index >= creator->operands.size()) {
      location.offset = std::nullopt;
      break;
    }
    auto index = integer_constant(creator->operands[operand_index]);
    if (!index) {
      location.offset = std::nullopt;
      break;
    }
    auto scaled = checked_mul(*index, step.scale);
    if (!scaled) {
      location.offset = std::nullopt;
      break;
    }
    location.offset = checked_add(*location.offset, *scaled);
    if (!location.offset)
      break;
  }
  return finish(location);
}

auto BasicAliasAnalysis::get_location(const Op &op) const
  -> std::optional<MemoryLocation> {
  switch (op.code) {
  case OpCode::Load:
    if (op.operands.empty() || !op.result)
      return std::nullopt;
    return get_location(op.operands[0], storage_size(op.result->type));
  case OpCode::Store:
    if (op.operands.size() < 2)
      return std::nullopt;
    return get_location(op.operands[1], storage_size(op.operands[0]->type));
  case OpCode::Memset:
    if (op.operands.empty())
      return std::nullopt;
    return get_location(op.operands[0]);
  default:
    return std::nullopt;
  }
}

auto BasicAliasAnalysis::alias(
  const MemoryLocation &lhs, const MemoryLocation &rhs
) const -> AliasResult {
  if (lhs.pointer && lhs.pointer == rhs.pointer)
    return AliasResult::MustAlias;

  if (!lhs.root || !rhs.root)
    return AliasResult::MayAlias;

  if (lhs.root != rhs.root) {
    // SysY cannot take the address of a scalar. Pointer arguments originate
    // from array objects, so they cannot alias a scalar global even though
    // both lower to an i32* or f32* in Mid IR.
    if (
      (is_scalar_global(lhs.root) && rhs.root->kind == ValueKind::Argument) ||
      (is_scalar_global(rhs.root) && lhs.root->kind == ValueKind::Argument)
    ) {
      return AliasResult::NoAlias;
    }
    if (is_identified_object(lhs.root) && is_identified_object(rhs.root))
      return AliasResult::NoAlias;
    return AliasResult::MayAlias;
  }

  if (lhs.offset && rhs.offset) {
    if (*lhs.offset == *rhs.offset)
      return AliasResult::MustAlias;
    if (lhs.size != 0 && rhs.size != 0) {
      auto lhs_end = checked_add(*lhs.offset, static_cast<int64_t>(lhs.size));
      auto rhs_end = checked_add(*rhs.offset, static_cast<int64_t>(rhs.size));
      if (
        lhs_end && rhs_end &&
        (*lhs_end <= *rhs.offset || *rhs_end <= *lhs.offset)
      ) {
        return AliasResult::NoAlias;
      }
    }
  }

  return AliasResult::MayAlias;
}

auto BasicAliasAnalysis::is_dereferenceable(
  const MemoryLocation &location
) const -> bool {
  if (
    !location.root || !location.offset || location.size == 0 ||
    *location.offset < 0 || !is_identified_object(location.root) ||
    !location.root->type->is_ptr()
  ) {
    return false;
  }

  auto target = std::static_pointer_cast<Ptr>(location.root->type)->target;
  size_t object_size = storage_size(target);
  auto end = checked_add(*location.offset, static_cast<int64_t>(location.size));
  return end && static_cast<uint64_t>(*end) <= object_size;
}

auto BasicAliasAnalysis::is_identified_object(Value *root) const -> bool {
  if (!root)
    return false;
  if (root->kind == ValueKind::GlobalVar)
    return true;
  if (root->kind != ValueKind::OpResult)
    return false;
  auto *creator = static_cast<Op *>(static_cast<OpResult *>(root)->creator);
  return creator && creator->code == OpCode::Alloca;
}

} // namespace exodus::mid_ir
