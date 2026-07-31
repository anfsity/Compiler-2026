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

auto checked_i64(__int128 value) -> std::optional<int64_t> {
  if (
    value < std::numeric_limits<int64_t>::min() ||
    value > std::numeric_limits<int64_t>::max()
  ) {
    return std::nullopt;
  }
  return static_cast<int64_t>(value);
}

auto range_of_form(const AffineForm &form, const IntegerRange &range)
  -> std::optional<IntegerRange> {
  if (range.empty())
    return IntegerRange{0, -1, range.exact};

  auto first = checked_i64(
    static_cast<__int128>(form.coefficient) * range.minimum + form.offset
  );
  auto last = checked_i64(
    static_cast<__int128>(form.coefficient) * range.maximum + form.offset
  );
  if (!first || !last)
    return std::nullopt;
  return IntegerRange{
    std::min(*first, *last), std::max(*first, *last), range.exact
  };
}

auto fits_i32(const IntegerRange &range) -> bool {
  return range.empty() ||
         (range.minimum >= std::numeric_limits<int32_t>::min() &&
          range.maximum <= std::numeric_limits<int32_t>::max());
}

} // namespace

auto AffineLoopInfo::compute(
  LinearFunction &func, LoopInfo &, DomTree &dom_tree
) -> void {
  op_blocks.clear();
  dom = &dom_tree;
  for (auto &block : func.blocks) {
    for (auto *op : block->insts)
      op_blocks[op] = block.get();
  }
}

auto AffineLoopInfo::affine_form(
  Value *value, const CountedLoopInfo &counted, const Loop &loop
) const -> std::optional<AffineForm> {
  if (!value)
    return std::nullopt;
  if (value == counted.induction.phi->result)
    return AffineForm{1, 0};
  if (auto constant = integer_constant(value))
    return AffineForm{0, *constant};
  if (value->kind != ValueKind::OpResult)
    return std::nullopt;

  auto *creator = static_cast<Op *>(static_cast<OpResult *>(value)->creator);
  auto block = op_blocks.find(creator);
  if (
    !creator || block == op_blocks.end() || !loop.contains(block->second) ||
    creator->operands.size() != 2 ||
    (creator->code != OpCode::Add && creator->code != OpCode::Sub &&
     creator->code != OpCode::Mul)
  ) {
    return std::nullopt;
  }

  auto lhs = affine_form(creator->operands[0], counted, loop);
  auto rhs = affine_form(creator->operands[1], counted, loop);
  if (!lhs || !rhs)
    return std::nullopt;

  std::optional<int64_t> coefficient;
  std::optional<int64_t> offset;
  if (creator->code == OpCode::Mul) {
    if (lhs->coefficient != 0 && rhs->coefficient != 0)
      return std::nullopt;
    coefficient = checked_i64(
      static_cast<__int128>(lhs->coefficient) * rhs->offset +
      static_cast<__int128>(rhs->coefficient) * lhs->offset
    );
    offset = checked_i64(static_cast<__int128>(lhs->offset) * rhs->offset);
  } else {
    coefficient = checked_i64(
      creator->code == OpCode::Add
        ? static_cast<__int128>(lhs->coefficient) + rhs->coefficient
        : static_cast<__int128>(lhs->coefficient) - rhs->coefficient
    );
    offset = checked_i64(
      creator->code == OpCode::Add
        ? static_cast<__int128>(lhs->offset) + rhs->offset
        : static_cast<__int128>(lhs->offset) - rhs->offset
    );
  }
  if (!coefficient || !offset)
    return std::nullopt;
  return AffineForm{*coefficient, *offset};
}

auto AffineLoopInfo::induction_range(const CountedLoopInfo &loop) const
  -> std::optional<IntegerRange> {
  auto initial = integer_constant(loop.induction.initial);
  if (!initial || loop.induction.step == 0)
    return std::nullopt;

  auto bound = integer_constant(loop.induction.bound);
  const auto step = loop.induction.step;
  int64_t minimum = *initial;
  int64_t maximum = *initial;

  if (step > 0) {
    if (
      loop.induction.predicate != OpCode::Lt &&
      loop.induction.predicate != OpCode::Le
    ) {
      return std::nullopt;
    }
    maximum = bound ? static_cast<int64_t>(*bound)
                    : std::numeric_limits<int32_t>::max();
    if (loop.induction.predicate == OpCode::Lt)
      --maximum;
  } else {
    if (
      loop.induction.predicate != OpCode::Gt &&
      loop.induction.predicate != OpCode::Ge
    ) {
      return std::nullopt;
    }
    minimum = bound ? static_cast<int64_t>(*bound)
                    : std::numeric_limits<int32_t>::min();
    if (loop.induction.predicate == OpCode::Gt)
      ++minimum;
  }

  const bool empty = minimum > maximum;
  const bool exact = bound && (empty || step == 1 || step == -1);
  return IntegerRange{minimum, maximum, exact};
}

auto AffineLoopInfo::induction_update_is_no_wrap(
  const CountedLoopInfo &counted, const IntegerRange &range
) const -> bool {
  if (range.empty())
    return true;

  const auto step = counted.induction.step;
  if (
    (step == 1 && counted.induction.predicate == OpCode::Lt) ||
    (step == -1 && counted.induction.predicate == OpCode::Gt)
  ) {
    return true;
  }
  if (!integer_constant(counted.induction.bound))
    return false;

  const auto endpoint = step > 0 ? range.maximum : range.minimum;
  const auto updated = static_cast<__int128>(endpoint) + step;
  return updated >= std::numeric_limits<int32_t>::min() &&
         updated <= std::numeric_limits<int32_t>::max();
}

auto AffineLoopInfo::expression_is_no_wrap(
  Value *value,
  const CountedLoopInfo &counted,
  const Loop &loop,
  const IntegerRange &range
) const -> bool {
  if (!value)
    return false;
  if (value == counted.induction.phi->result || integer_constant(value))
    return true;
  if (value->kind != ValueKind::OpResult)
    return false;

  auto *creator = static_cast<Op *>(static_cast<OpResult *>(value)->creator);
  auto block = op_blocks.find(creator);
  if (
    !creator || block == op_blocks.end() || !loop.contains(block->second) ||
    creator->operands.size() != 2 ||
    (creator->code != OpCode::Add && creator->code != OpCode::Sub &&
     creator->code != OpCode::Mul)
  ) {
    return false;
  }
  if (
    !expression_is_no_wrap(creator->operands[0], counted, loop, range) ||
    !expression_is_no_wrap(creator->operands[1], counted, loop, range)
  ) {
    return false;
  }

  auto form = affine_form(value, counted, loop);
  auto value_range = form ? range_of_form(*form, range) : std::nullopt;
  return value_range && fits_i32(*value_range);
}

auto AffineLoopInfo::is_no_wrap(
  Value *value, const CountedLoopInfo &counted, const Loop &loop
) const -> bool {
  auto range = induction_range(counted);
  auto initial = integer_constant(counted.induction.initial);
  if (!range || !initial || !induction_update_is_no_wrap(counted, *range))
    return false;

  // Some consumers materialize the initial affine expression in a preheader.
  // Keep the shared query safe for them even when the loop body never runs.
  auto evaluation_range =
    range->empty() ? IntegerRange{*initial, *initial, true} : *range;
  return affine_form(value, counted, loop) &&
         expression_is_no_wrap(value, counted, loop, evaluation_range);
}

auto AffineLoopInfo::is_non_negative(
  Value *value, const CountedLoopInfo &counted, const Loop &loop
) const -> bool {
  if (!is_no_wrap(value, counted, loop))
    return false;
  auto range = induction_range(counted);
  auto initial = integer_constant(counted.induction.initial);
  auto form = affine_form(value, counted, loop);
  if (!range || !initial || !form)
    return false;
  auto evaluation_range =
    range->empty() ? IntegerRange{*initial, *initial, true} : *range;
  auto value_range = range_of_form(*form, evaluation_range);
  return value_range && !value_range->empty() && value_range->minimum >= 0;
}

auto AffineLoopInfo::is_available_at_preheader(
  Value *value, Block *preheader
) const -> bool {
  if (!value || !preheader)
    return false;
  if (value->kind != ValueKind::OpResult)
    return true;

  auto *creator = static_cast<Op *>(static_cast<OpResult *>(value)->creator);
  auto block = op_blocks.find(creator);
  if (!creator || block == op_blocks.end())
    return false;
  auto *definition = block->second;
  if (definition != preheader)
    return dom && dom->dominate(definition, preheader);
  if (preheader->insts.empty())
    return false;

  auto insertion_point = std::prev(preheader->insts.end());
  return std::find(preheader->insts.begin(), insertion_point, creator) !=
         insertion_point;
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

auto AffineLoopInfo::get_inductions(
  const Loop &loop, bool allow_dominating_update
) const -> std::vector<InductionInfo> {
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
    auto *update_block = definition_block(backedge);
    if (
      !update || !update_block ||
      (update_block != latch &&
       (!allow_dominating_update || !loop.contains(update_block) || !dom ||
        !dom->dominate(update_block, latch))) ||
      !update->result || update->result != backedge ||
      update->operands.size() != 2
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

auto AffineLoopInfo::match_counted_loop(
  const Loop &loop, bool allow_dominating_update
) const -> std::optional<CountedLoopInfo> {
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

  for (auto induction : get_inductions(loop, allow_dominating_update)) {
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
