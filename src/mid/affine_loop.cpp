#include "affine_loop.hpp"

#include <algorithm>
#include <limits>

namespace exodus::mid_ir {

namespace {

auto swap_predicate(OpCode predicate) -> OpCode {
  switch (predicate) {
  case OpCode::Lt:
    return OpCode::Gt;
  case OpCode::Le:
    return OpCode::Ge;
  case OpCode::Gt:
    return OpCode::Lt;
  case OpCode::Ge:
    return OpCode::Le;
  default:
    return predicate;
  }
}

auto invert_predicate(OpCode predicate) -> OpCode {
  switch (predicate) {
  case OpCode::Eq:
    return OpCode::Ne;
  case OpCode::Ne:
    return OpCode::Eq;
  case OpCode::Lt:
    return OpCode::Ge;
  case OpCode::Le:
    return OpCode::Gt;
  case OpCode::Gt:
    return OpCode::Le;
  case OpCode::Ge:
    return OpCode::Lt;
  default:
    return predicate;
  }
}

auto is_compare(OpCode code) -> bool {
  return code == OpCode::Eq || code == OpCode::Ne || code == OpCode::Lt ||
         code == OpCode::Le || code == OpCode::Gt || code == OpCode::Ge;
}

} // namespace

auto AffineLoopInfo::compute(LinearFunction &func, LoopInfo &) -> void {
  op_blocks.clear();
  for (auto &block : func.blocks) {
    for (auto *op : block->insts)
      op_blocks[op] = block.get();
  }
}

auto AffineLoopInfo::integer_constant(Value *value) -> std::optional<int32_t> {
  if (!value || value->kind != ValueKind::Constant)
    return std::nullopt;
  const auto &constant = static_cast<Constant *>(value)->val;
  if (!std::holds_alternative<int>(constant))
    return std::nullopt;
  return static_cast<int32_t>(std::get<int>(constant));
}

auto AffineLoopInfo::definition_block(Value *value) const -> Block * {
  if (!value || value->kind != ValueKind::OpResult)
    return nullptr;
  auto *definition = static_cast<Op *>(static_cast<OpResult *>(value)->creator);
  auto it = op_blocks.find(definition);
  return it == op_blocks.end() ? nullptr : it->second;
}

auto AffineLoopInfo::loop_invariant(Value *value, const Loop &loop) const
  -> bool {
  if (!value)
    return false;
  if (value->kind != ValueKind::OpResult)
    return true;
  auto *block = definition_block(value);
  return block && !loop.contains(block);
}

auto AffineLoopInfo::get_inductions(const Loop &loop) const
  -> std::vector<InductionInfo> {
  std::vector<InductionInfo> result;
  if (!loop.get_preheader() || loop.get_back_edges().size() != 1)
    return result;

  auto *latch = loop.get_back_edges().front().first;
  for (auto *phi : loop.get_header()->insts) {
    if (phi->code != OpCode::Phi)
      break;
    if (!phi->result || !phi->result->type->is_i32())
      continue;

    Value *initial = nullptr;
    Value *backedge = nullptr;
    size_t preheader_incoming = 0;
    size_t latch_incoming = 0;
    bool unexpected_incoming = false;
    for (const auto &[pred, value] :
         std::get<PhiPayload>(phi->payload).incoming) {
      if (pred == loop.get_preheader()) {
        initial = value;
        ++preheader_incoming;
      } else if (pred == latch) {
        backedge = value;
        ++latch_incoming;
      } else {
        unexpected_incoming = true;
      }
    }
    if (
      unexpected_incoming || preheader_incoming != 1 || latch_incoming != 1 ||
      !initial || !backedge || backedge->kind != ValueKind::OpResult
    ) {
      continue;
    }

    auto *update =
      static_cast<Op *>(static_cast<OpResult *>(backedge)->creator);
    if (
      !update || definition_block(backedge) != latch || !update->result ||
      update->result != backedge || update->operands.size() != 2
    ) {
      continue;
    }

    std::optional<int64_t> step;
    if (update->code == OpCode::Add) {
      if (update->operands[0] == phi->result) {
        step = integer_constant(update->operands[1]);
      } else if (update->operands[1] == phi->result) {
        step = integer_constant(update->operands[0]);
      }
    } else if (
      update->code == OpCode::Sub && update->operands[0] == phi->result
    ) {
      if (auto amount = integer_constant(update->operands[1]))
        step = -static_cast<int64_t>(*amount);
    }
    if (!step || *step == 0)
      continue;

    result.push_back({phi, initial, nullptr, update, latch, *step, OpCode::Eq});
  }
  return result;
}

auto AffineLoopInfo::normalized_condition(
  Op *compare, Value *induction, bool true_is_continue
) const -> std::optional<std::pair<OpCode, Value *>> {
  if (!compare || !is_compare(compare->code) || compare->operands.size() != 2) {
    return std::nullopt;
  }

  auto predicate = compare->code;
  Value *bound = nullptr;
  if (compare->operands[0] == induction) {
    bound = compare->operands[1];
  } else if (compare->operands[1] == induction) {
    bound = compare->operands[0];
    predicate = swap_predicate(predicate);
  } else {
    return std::nullopt;
  }
  if (!true_is_continue)
    predicate = invert_predicate(predicate);
  return std::pair{predicate, bound};
}

auto AffineLoopInfo::match_counted_loop(const Loop &loop) const
  -> std::optional<CountedLoopInfo> {
  if (
    !loop.get_preheader() || loop.get_back_edges().size() != 1 ||
    loop.get_exiting_blocks().size() != 1 ||
    loop.get_exiting_blocks().front() != loop.get_header() ||
    loop.get_exit_blocks().size() != 1
  ) {
    return std::nullopt;
  }

  auto *header = loop.get_header();
  auto *latch = loop.get_back_edges().front().first;
  if (
    latch == header || !loop.contains(latch) || header->insts.empty() ||
    latch->insts.empty()
  ) {
    return std::nullopt;
  }

  auto *branch = header->insts.back();
  if (
    branch->code != OpCode::Branch || branch->operands.size() != 1 ||
    branch->successors.size() != 2
  ) {
    return std::nullopt;
  }
  auto true_is_continue = loop.contains(branch->successors[0]);
  if (true_is_continue == loop.contains(branch->successors[1]))
    return std::nullopt;
  auto *continuation = branch->successors[true_is_continue ? 0 : 1];
  auto *exit = branch->successors[true_is_continue ? 1 : 0];
  if (exit != loop.get_exit_blocks().front())
    return std::nullopt;

  auto *condition = branch->operands[0];
  if (!condition || condition->kind != ValueKind::OpResult)
    return std::nullopt;
  auto *compare =
    static_cast<Op *>(static_cast<OpResult *>(condition)->creator);
  if (definition_block(condition) != header)
    return std::nullopt;

  for (auto induction : get_inductions(loop)) {
    auto normalized =
      normalized_condition(compare, induction.phi->result, true_is_continue);
    if (!normalized)
      continue;
    auto [predicate, bound] = *normalized;
    if (!bound || !bound->type->is_i32() || !loop_invariant(bound, loop))
      continue;
    induction.bound = bound;
    induction.predicate = predicate;
    return CountedLoopInfo{
      loop.get_preheader(),
      header,
      latch,
      continuation,
      exit,
      compare,
      true_is_continue,
      compare->operands[0] == induction.phi->result,
      induction,
    };
  }
  return std::nullopt;
}

auto AffineLoopInfo::exact_trip_count(const CountedLoopInfo &loop) const
  -> std::optional<uint64_t> {
  auto initial = integer_constant(loop.induction.initial);
  auto bound = integer_constant(loop.induction.bound);
  if (!initial || !bound || loop.induction.step == 0)
    return std::nullopt;

  const int64_t start = *initial;
  const int64_t limit = *bound;
  const int64_t step = loop.induction.step;
  uint64_t trips = 0;

  switch (loop.induction.predicate) {
  case OpCode::Lt:
    if (step <= 0)
      return std::nullopt;
    if (start < limit)
      trips = static_cast<uint64_t>((limit - start + step - 1) / step);
    break;
  case OpCode::Le:
    if (step <= 0)
      return std::nullopt;
    if (start <= limit)
      trips = static_cast<uint64_t>((limit - start) / step + 1);
    break;
  case OpCode::Gt: {
    if (step >= 0)
      return std::nullopt;
    const int64_t distance = -step;
    if (start > limit)
      trips = static_cast<uint64_t>((start - limit + distance - 1) / distance);
    break;
  }
  case OpCode::Ge: {
    if (step >= 0)
      return std::nullopt;
    const int64_t distance = -step;
    if (start >= limit)
      trips = static_cast<uint64_t>((start - limit) / distance + 1);
    break;
  }
  default:
    return std::nullopt;
  }

  // The latch update executes after the final body iteration, even though its
  // result is used only by the failing header comparison.  Reject the loop if
  // that source-level i32 update would overflow.
  const auto final_value = start + static_cast<__int128>(trips) * step;
  if (
    final_value < std::numeric_limits<int32_t>::min() ||
    final_value > std::numeric_limits<int32_t>::max()
  ) {
    return std::nullopt;
  }
  return trips;
}

} // namespace exodus::mid_ir
