#include "local_array_loop_specialize.hpp"

#include "../../high/cloner.hpp"
#include "../../high/effects.hpp"
#include "../../high/rewriter.hpp"
#include "local_array_summary.hpp"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <unordered_set>
#include <vector>

namespace exodus::high_ir::opt {
namespace {

constexpr size_t max_segments = 4;
constexpr size_t max_loop_ops = 160;
constexpr size_t max_growth_ops = 480;
constexpr size_t max_function_ops = 850;

auto defining_op(Value *value) -> Op * {
  if (!value || value->kind != ValueKind::OpResult)
    return nullptr;
  return static_cast<Op *>(static_cast<OpResult *>(value)->creator);
}

auto int_constant(Value *value) -> std::optional<int64_t> {
  if (!value || value->kind != ValueKind::Constant)
    return std::nullopt;
  const auto &data = static_cast<Constant *>(value)->val;
  if (!std::holds_alternative<int>(data))
    return std::nullopt;
  return std::get<int>(data);
}

auto checked_add(int64_t lhs, int64_t rhs) -> std::optional<int64_t> {
  int64_t result = 0;
  return __builtin_add_overflow(lhs, rhs, &result)
           ? std::nullopt
           : std::optional<int64_t>(result);
}

auto count_ops(const Region &region) -> size_t {
  size_t count = 0;
  for (auto *op : region) {
    ++count;
    if (op->code == OpCode::If) {
      const auto &payload = std::get<IfPayload>(op->payload);
      count += count_ops(*payload.then_region);
      if (payload.else_region)
        count += count_ops(*payload.else_region);
    } else if (op->code == OpCode::While) {
      const auto &payload = std::get<WhilePayload>(op->payload);
      count += count_ops(*payload.cond_region);
      count += count_ops(*payload.loop_region);
    }
  }
  return count;
}

auto region_contains(const Region &region, const Op *target) -> bool {
  for (auto *op : region) {
    if (op == target)
      return true;
    if (op->code == OpCode::If) {
      const auto &payload = std::get<IfPayload>(op->payload);
      if (region_contains(*payload.then_region, target))
        return true;
      if (payload.else_region && region_contains(*payload.else_region, target))
        return true;
    } else if (op->code == OpCode::While) {
      const auto &payload = std::get<WhilePayload>(op->payload);
      if (
        region_contains(*payload.cond_region, target) ||
        region_contains(*payload.loop_region, target)
      ) {
        return true;
      }
    }
  }
  return false;
}

auto has_unsupported_exit(const Region &region) -> bool {
  for (auto *op : region) {
    if (
      op->code == OpCode::Break || op->code == OpCode::Continue ||
      op->code == OpCode::Ret
    ) {
      return true;
    }
    if (op->code == OpCode::If) {
      const auto &payload = std::get<IfPayload>(op->payload);
      if (has_unsupported_exit(*payload.then_region))
        return true;
      if (payload.else_region && has_unsupported_exit(*payload.else_region))
        return true;
    } else if (op->code == OpCode::While) {
      const auto &payload = std::get<WhilePayload>(op->payload);
      if (
        has_unsupported_exit(*payload.cond_region) ||
        has_unsupported_exit(*payload.loop_region)
      ) {
        return true;
      }
    }
  }
  return false;
}

auto scan_counter_uses(
  const Region &region,
  Value *counter_address,
  Op *allowed_store,
  bool reject_other_stores
) -> bool {
  for (auto *op : region) {
    for (size_t index = 0; index < op->operands.size(); ++index) {
      if (op->operands[index] != counter_address)
        continue;
      const bool direct_load =
        op->code == OpCode::Load && index == 0 && op->operands.size() == 1;
      const bool direct_store =
        op->code == OpCode::Store && index == 1 && op->operands.size() == 2;
      if (!direct_load && !direct_store)
        return false;
      if (direct_store && reject_other_stores && op != allowed_store)
        return false;
    }

    if (op->code == OpCode::If) {
      const auto &payload = std::get<IfPayload>(op->payload);
      if (!scan_counter_uses(
            *payload.then_region,
            counter_address,
            allowed_store,
            reject_other_stores
          )) {
        return false;
      }
      if (
        payload.else_region && !scan_counter_uses(
                                 *payload.else_region,
                                 counter_address,
                                 allowed_store,
                                 reject_other_stores
                               )
      ) {
        return false;
      }
    } else if (op->code == OpCode::While) {
      const auto &payload = std::get<WhilePayload>(op->payload);
      if (
        !scan_counter_uses(
          *payload.cond_region,
          counter_address,
          allowed_store,
          reject_other_stores
        ) ||
        !scan_counter_uses(
          *payload.loop_region,
          counter_address,
          allowed_store,
          reject_other_stores
        )
      ) {
        return false;
      }
    }
  }
  return true;
}

struct CountedLoop {
  Op *loop = nullptr;
  Value *counter_address = nullptr;
  Op *compare = nullptr;
  size_t bound_operand = 0;
  Op *tail_store = nullptr;
  int64_t initial = 0;
  int64_t bound = 0;
  size_t op_count = 0;
};

auto match_counted_loop(
  Function &function, Region &parent, Region::iterator position
) -> std::optional<CountedLoop> {
  auto *loop = *position;
  if (loop->code != OpCode::While || position == parent.begin())
    return std::nullopt;
  auto &payload = std::get<WhilePayload>(loop->payload);
  if (
    payload.cond_region->size() != 3 || payload.loop_region->size() < 3 ||
    has_unsupported_exit(*payload.loop_region)
  ) {
    return std::nullopt;
  }

  auto condition_it = payload.cond_region->begin();
  auto *condition_load = *condition_it++;
  auto *compare = *condition_it++;
  auto *condition = *condition_it;
  if (
    condition_load->code != OpCode::Load ||
    condition_load->operands.size() != 1 || !condition_load->result ||
    compare->operands.size() != 2 || !compare->result ||
    condition->code != OpCode::Condition || condition->operands.size() != 1 ||
    condition->operands[0] != compare->result
  ) {
    return std::nullopt;
  }

  size_t bound_operand = 0;
  Value *bound_value = nullptr;
  if (
    compare->code == OpCode::Lt &&
    compare->operands[0] == condition_load->result
  ) {
    bound_operand = 1;
    bound_value = compare->operands[1];
  } else if (
    compare->code == OpCode::Gt &&
    compare->operands[1] == condition_load->result
  ) {
    bound_operand = 0;
    bound_value = compare->operands[0];
  } else {
    return std::nullopt;
  }
  auto bound = int_constant(bound_value);
  if (!bound)
    return std::nullopt;

  auto *counter_address = condition_load->operands[0];
  auto *counter_alloca = defining_op(counter_address);
  if (
    !counter_alloca || counter_alloca->code != OpCode::Alloca ||
    !counter_address->type->is_ptr() ||
    !std::static_pointer_cast<Ptr>(counter_address->type)->target->is_i32()
  ) {
    return std::nullopt;
  }

  auto *initial_store = *std::prev(position);
  if (
    initial_store->code != OpCode::Store ||
    initial_store->operands.size() != 2 ||
    initial_store->operands[1] != counter_address
  ) {
    return std::nullopt;
  }
  auto initial = int_constant(initial_store->operands[0]);
  if (!initial || *initial >= *bound)
    return std::nullopt;

  auto tail_store_position = std::prev(payload.loop_region->end());
  auto tail_add_position = std::prev(tail_store_position);
  auto tail_load_position = std::prev(tail_add_position);
  auto *tail_load = *tail_load_position;
  auto *tail_add = *tail_add_position;
  auto *tail_store = *tail_store_position;
  if (
    tail_load->code != OpCode::Load || tail_load->operands.size() != 1 ||
    tail_load->operands[0] != counter_address || !tail_load->result ||
    tail_add->code != OpCode::Add || tail_add->operands.size() != 2 ||
    !tail_add->result || tail_store->code != OpCode::Store ||
    tail_store->operands.size() != 2 ||
    tail_store->operands[0] != tail_add->result ||
    tail_store->operands[1] != counter_address
  ) {
    return std::nullopt;
  }
  auto lhs = int_constant(tail_add->operands[0]);
  auto rhs = int_constant(tail_add->operands[1]);
  auto *counter_value = lhs ? tail_add->operands[1] : tail_add->operands[0];
  auto step = lhs ? lhs : rhs;
  if (!step || *step != 1 || counter_value != tail_load->result)
    return std::nullopt;

  if (
    !scan_counter_uses(function.body, counter_address, nullptr, false) ||
    !scan_counter_uses(*payload.loop_region, counter_address, tail_store, true)
  ) {
    return std::nullopt;
  }
  auto op_count = size_t{1} + count_ops(*payload.cond_region) +
                  count_ops(*payload.loop_region);
  if (op_count > max_loop_ops)
    return std::nullopt;
  return CountedLoop{
    loop,
    counter_address,
    compare,
    bound_operand,
    tail_store,
    *initial,
    *bound,
    op_count
  };
}

auto value_depends_on(
  Value *value, Value *target, std::unordered_set<Value *> &active
) -> bool {
  if (value == target)
    return true;
  if (
    !value || value->kind != ValueKind::OpResult || !active.insert(value).second
  ) {
    return false;
  }
  auto *op = defining_op(value);
  bool depends =
    op &&
    std::any_of(op->operands.begin(), op->operands.end(), [&](Value *operand) {
      return value_depends_on(operand, target, active);
    });
  active.erase(value);
  return depends;
}

auto region_depends_on(const Region &region, Value *target) -> bool {
  for (auto *op : region) {
    for (auto *operand : op->operands) {
      std::unordered_set<Value *> active;
      if (value_depends_on(operand, target, active))
        return true;
    }
    if (op->code == OpCode::If) {
      const auto &payload = std::get<IfPayload>(op->payload);
      if (region_depends_on(*payload.then_region, target))
        return true;
      if (
        payload.else_region && region_depends_on(*payload.else_region, target)
      )
        return true;
    } else if (op->code == OpCode::While) {
      const auto &payload = std::get<WhilePayload>(op->payload);
      if (
        region_depends_on(*payload.cond_region, target) ||
        region_depends_on(*payload.loop_region, target)
      ) {
        return true;
      }
    }
  }
  return false;
}

auto controls_nested_loop(const Region &region, Value *value) -> bool {
  for (auto *op : region) {
    if (op->code == OpCode::If) {
      const auto &payload = std::get<IfPayload>(op->payload);
      if (controls_nested_loop(*payload.then_region, value))
        return true;
      if (
        payload.else_region && controls_nested_loop(*payload.else_region, value)
      ) {
        return true;
      }
    } else if (op->code == OpCode::While) {
      const auto &payload = std::get<WhilePayload>(op->payload);
      if (region_depends_on(*payload.cond_region, value))
        return true;
      if (controls_nested_loop(*payload.loop_region, value))
        return true;
    }
  }
  return false;
}

struct SummarizedAccess {
  Op *load = nullptr;
  const LocalArraySummary *summary = nullptr;
  int64_t offset = 0;
};

auto collect_accesses(
  const Region &region,
  const CountedLoop &counted,
  const LocalArraySummaryInfo &summaries,
  std::vector<SummarizedAccess> &result
) -> void {
  for (auto *op : region) {
    if (op->code == OpCode::Load) {
      auto access = summaries.analyze_load(*op);
      if (
        access && access->terms.size() == 1 && access->terms[0].coefficient == 1
      ) {
        auto *index_load = defining_op(access->terms[0].value);
        if (
          index_load && index_load->code == OpCode::Load &&
          index_load->operands.size() == 1 &&
          index_load->operands[0] == counted.counter_address &&
          region_contains(region, index_load)
        ) {
          result.push_back({op, access->summary, access->constant_offset});
        }
      }
    }
    if (op->code == OpCode::If) {
      const auto &payload = std::get<IfPayload>(op->payload);
      collect_accesses(*payload.then_region, counted, summaries, result);
      if (payload.else_region)
        collect_accesses(*payload.else_region, counted, summaries, result);
    } else if (op->code == OpCode::While) {
      const auto &payload = std::get<WhilePayload>(op->payload);
      collect_accesses(*payload.cond_region, counted, summaries, result);
      collect_accesses(*payload.loop_region, counted, summaries, result);
    }
  }
}

struct Segment {
  int64_t begin = 0;
  int64_t end = 0;
  int32_t value = 0;
};

auto segments_for_access(
  const SummarizedAccess &access, int64_t begin, int64_t end
) -> std::optional<std::vector<Segment>> {
  auto first_index = checked_add(begin, access.offset);
  auto end_index = checked_add(end, access.offset);
  if (
    !first_index || !end_index || *first_index < 0 ||
    *end_index <= *first_index ||
    static_cast<uint64_t>(*end_index) > access.summary->values.size()
  ) {
    return std::nullopt;
  }

  std::vector<Segment> result;
  int64_t cursor = begin;
  while (cursor < end) {
    auto index = checked_add(cursor, access.offset);
    if (!index)
      return std::nullopt;
    auto value = access.summary->values[static_cast<size_t>(*index)];
    int64_t next = cursor + 1;
    while (next < end) {
      auto next_index = checked_add(next, access.offset);
      if (
        !next_index ||
        access.summary->values[static_cast<size_t>(*next_index)] != value
      ) {
        break;
      }
      ++next;
    }
    result.push_back({cursor, next, value});
    cursor = next;
  }
  return result;
}

auto constant_on_segment(
  const SummarizedAccess &access, int64_t begin, int64_t end
) -> std::optional<int32_t> {
  auto segments = segments_for_access(access, begin, end);
  if (!segments || segments->size() != 1)
    return std::nullopt;
  return segments->front().value;
}

auto normalized_compare(Op *op, Value *counter_address)
  -> std::optional<std::pair<OpCode, int64_t>> {
  if (!op || op->operands.size() != 2)
    return std::nullopt;
  auto is_counter_load = [&](Value *value) {
    auto *load = defining_op(value);
    return load && load->code == OpCode::Load && load->operands.size() == 1 &&
           load->operands[0] == counter_address;
  };
  auto lhs_constant = int_constant(op->operands[0]);
  auto rhs_constant = int_constant(op->operands[1]);
  if (is_counter_load(op->operands[0]) && rhs_constant)
    return std::pair{op->code, *rhs_constant};
  if (!lhs_constant || !is_counter_load(op->operands[1]))
    return std::nullopt;
  OpCode predicate = op->code;
  switch (predicate) {
  case OpCode::Lt:
    predicate = OpCode::Gt;
    break;
  case OpCode::Le:
    predicate = OpCode::Ge;
    break;
  case OpCode::Gt:
    predicate = OpCode::Lt;
    break;
  case OpCode::Ge:
    predicate = OpCode::Le;
    break;
  default:
    break;
  }
  return std::pair{predicate, *lhs_constant};
}

auto constant_compare_result(
  OpCode predicate, int64_t constant, int64_t minimum, int64_t maximum
) -> std::optional<bool> {
  switch (predicate) {
  case OpCode::Eq:
    if (minimum == maximum && minimum == constant)
      return true;
    if (constant < minimum || constant > maximum)
      return false;
    break;
  case OpCode::Ne: {
    auto equal =
      constant_compare_result(OpCode::Eq, constant, minimum, maximum);
    if (equal)
      return !*equal;
    break;
  }
  case OpCode::Lt:
    if (maximum < constant)
      return true;
    if (minimum >= constant)
      return false;
    break;
  case OpCode::Le:
    if (maximum <= constant)
      return true;
    if (minimum > constant)
      return false;
    break;
  case OpCode::Gt:
    if (minimum > constant)
      return true;
    if (maximum <= constant)
      return false;
    break;
  case OpCode::Ge:
    if (minimum >= constant)
      return true;
    if (maximum < constant)
      return false;
    break;
  default:
    break;
  }
  return std::nullopt;
}

auto fold_counter_comparisons(
  Region &region,
  Value *counter_address,
  int64_t minimum,
  int64_t maximum,
  Module &module,
  IRRewriter &rewriter
) -> void {
  for (auto *op : region) {
    auto compare = normalized_compare(op, counter_address);
    if (compare) {
      auto value = constant_compare_result(
        compare->first, compare->second, minimum, maximum
      );
      if (value && op->result && op->result->type->is_bool()) {
        rewriter.replace_op(
          op, module.ctx.make_const(Bool::get(), *value ? 1 : 0)
        );
      }
    }
    if (op->code == OpCode::If) {
      auto &payload = std::get<IfPayload>(op->payload);
      fold_counter_comparisons(
        *payload.then_region,
        counter_address,
        minimum,
        maximum,
        module,
        rewriter
      );
      if (payload.else_region) {
        fold_counter_comparisons(
          *payload.else_region,
          counter_address,
          minimum,
          maximum,
          module,
          rewriter
        );
      }
    } else if (op->code == OpCode::While) {
      auto &payload = std::get<WhilePayload>(op->payload);
      fold_counter_comparisons(
        *payload.cond_region,
        counter_address,
        minimum,
        maximum,
        module,
        rewriter
      );
      fold_counter_comparisons(
        *payload.loop_region,
        counter_address,
        minimum,
        maximum,
        module,
        rewriter
      );
    }
  }
}

auto unlink_region(Region &region) -> void {
  for (auto *op : region) {
    if (op->code == OpCode::If) {
      auto &payload = std::get<IfPayload>(op->payload);
      unlink_region(*payload.then_region);
      if (payload.else_region)
        unlink_region(*payload.else_region);
    } else if (op->code == OpCode::While) {
      auto &payload = std::get<WhilePayload>(op->payload);
      unlink_region(*payload.cond_region);
      unlink_region(*payload.loop_region);
    }
    for (auto *operand : op->operands) {
      if (operand)
        operand->rmUse(op);
    }
  }
}

auto specialize_clone(
  Op *clone,
  IRCloner &cloner,
  const CountedLoop &counted,
  const std::vector<SummarizedAccess> &accesses,
  const Segment &segment,
  Module &module
) -> void {
  auto *mapped_compare_value = cloner.value_map.at(counted.compare->result);
  auto *mapped_compare = defining_op(mapped_compare_value);
  auto *new_bound =
    module.ctx.make_const(I32::get(), static_cast<int>(segment.end));
  mapped_compare->operands[counted.bound_operand]->rmUse(mapped_compare);
  mapped_compare->operands[counted.bound_operand] = new_bound;
  new_bound->addUse(mapped_compare);

  IRRewriter rewriter;
  for (const auto &access : accesses) {
    auto value = constant_on_segment(access, segment.begin, segment.end);
    if (!value)
      continue;
    auto mapped = cloner.value_map.find(access.load->result);
    if (mapped == cloner.value_map.end())
      continue;
    auto *mapped_load = defining_op(mapped->second);
    if (!mapped_load || mapped_load->code != OpCode::Load)
      continue;
    rewriter.replace_op(
      mapped_load, module.ctx.make_const(I32::get(), static_cast<int>(*value))
    );
  }

  auto &payload = std::get<WhilePayload>(clone->payload);
  fold_counter_comparisons(
    *payload.loop_region,
    counted.counter_address,
    segment.begin,
    segment.end - 1,
    module,
    rewriter
  );
  rewriter.finalize(*payload.cond_region);
  rewriter.finalize(*payload.loop_region);
}

class Specializer {
public:
  Specializer(
    Module &module, Function &function, const LocalArraySummaryInfo &summaries
  )
      : module(module), function(function), summaries(summaries),
        function_ops(count_ops(function.body)) {}

  auto run() -> bool { return specialize_one(function.body); }

private:
  Module &module;
  Function &function;
  const LocalArraySummaryInfo &summaries;
  size_t function_ops = 0;

  auto specialize_one(Region &region) -> bool {
    for (auto position = region.begin(); position != region.end(); ++position) {
      auto *op = *position;
      if (op->code == OpCode::If) {
        auto &payload = std::get<IfPayload>(op->payload);
        if (specialize_one(*payload.then_region))
          return true;
        if (payload.else_region && specialize_one(*payload.else_region))
          return true;
        continue;
      }
      if (op->code != OpCode::While)
        continue;

      auto &payload = std::get<WhilePayload>(op->payload);
      if (
        specialize_one(*payload.cond_region) ||
        specialize_one(*payload.loop_region)
      ) {
        return true;
      }
      auto counted = match_counted_loop(function, region, position);
      if (!counted)
        continue;

      std::vector<SummarizedAccess> accesses;
      collect_accesses(*payload.loop_region, *counted, summaries, accesses);
      if (accesses.empty())
        continue;

      std::optional<std::vector<Segment>> partition;
      for (const auto &access : accesses) {
        auto candidate =
          segments_for_access(access, counted->initial, counted->bound);
        if (
          !candidate || candidate->size() < 2 ||
          candidate->size() > max_segments
        ) {
          continue;
        }
        if (!controls_nested_loop(*payload.loop_region, access.load->result)) {
          continue;
        }
        if (!partition || candidate->size() < partition->size())
          partition = std::move(candidate);
      }
      if (!partition)
        continue;

      const auto growth = counted->op_count * (partition->size() - 1);
      if (growth > max_growth_ops || function_ops + growth > max_function_ops) {
        continue;
      }

      Region replacement;
      for (const auto &segment : *partition) {
        IRCloner cloner(&module.ctx);
        auto *clone = cloner.cloneOp(counted->loop);
        specialize_clone(clone, cloner, *counted, accesses, segment, module);
        replacement.push_back(clone);
      }

      unlink_region(*payload.cond_region);
      unlink_region(*payload.loop_region);
      for (auto *operand : counted->loop->operands) {
        if (operand)
          operand->rmUse(counted->loop);
      }
      region.splice(position, replacement);
      region.erase(position);
      return true;
    }
    return false;
  }
};

} // namespace

auto LocalArrayLoopSpecialize::run(
  Function &function, exodus::opt::FunctionAnalysisManager &analysis_manager
) -> exodus::opt::PreservedAnalysis {
  auto &summaries =
    analysis_manager.get_result<LocalArraySummaryAnalysis>(function);
  Specializer specializer(*module, function, summaries);
  return specializer.run() ? exodus::opt::PreservedAnalysis::none()
                           : exodus::opt::PreservedAnalysis::all();
}

} // namespace exodus::high_ir::opt
