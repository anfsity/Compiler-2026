#include "polyhedral.hpp"

#include "../../../3rd-party/fmt/format.h"
#include "../../base/getptr.hpp"
#include <algorithm>
#include <limits>
#include <numeric>
#include <unordered_map>

namespace exodus::mid_ir {
namespace {

struct LinearConstraint {
  // Sum(coefficients[i] * variable[i]) <= constant.
  std::vector<int64_t> coefficients;
  int64_t constant = 0;
};

enum class Feasibility { Empty, MayBeFeasible, Overflow };

auto checked_i64(__int128 value) -> std::optional<int64_t> {
  if (
    value < std::numeric_limits<int64_t>::min() ||
    value > std::numeric_limits<int64_t>::max()
  ) {
    return std::nullopt;
  }
  return static_cast<int64_t>(value);
}

auto normalize(LinearConstraint &constraint) -> void {
  uint64_t divisor = 0;
  auto accumulate_gcd = [&divisor](int64_t value) {
    const auto absolute =
      value < 0 ? static_cast<uint64_t>(-static_cast<__int128>(value))
                : static_cast<uint64_t>(value);
    divisor = std::gcd(divisor, absolute);
  };
  for (auto coefficient : constraint.coefficients)
    accumulate_gcd(coefficient);
  accumulate_gcd(constraint.constant);
  if (
    divisor <= 1 ||
    divisor > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
  )
    return;
  for (auto &coefficient : constraint.coefficients)
    coefficient /= static_cast<int64_t>(divisor);
  constraint.constant /= static_cast<int64_t>(divisor);
}

auto combine_constraints(
  const LinearConstraint &positive,
  const LinearConstraint &negative,
  size_t variable
) -> std::optional<LinearConstraint> {
  const auto positive_factor =
    -static_cast<__int128>(negative.coefficients[variable]);
  const auto negative_factor =
    static_cast<__int128>(positive.coefficients[variable]);
  LinearConstraint result;
  result.coefficients.resize(positive.coefficients.size());
  for (size_t index = 0; index < result.coefficients.size(); ++index) {
    auto coefficient = checked_i64(
      positive_factor * positive.coefficients[index] +
      negative_factor * negative.coefficients[index]
    );
    if (!coefficient)
      return std::nullopt;
    result.coefficients[index] = *coefficient;
  }
  auto constant = checked_i64(
    positive_factor * positive.constant + negative_factor * negative.constant
  );
  if (!constant)
    return std::nullopt;
  result.constant = *constant;
  normalize(result);
  return result;
}

auto prove_feasibility(
  std::vector<LinearConstraint> constraints, size_t variable_count
) -> Feasibility {
  for (size_t variable = variable_count; variable-- > 0;) {
    std::vector<const LinearConstraint *> positive;
    std::vector<const LinearConstraint *> negative;
    std::vector<LinearConstraint> next;
    for (const auto &constraint : constraints) {
      const auto coefficient = constraint.coefficients[variable];
      if (coefficient > 0)
        positive.push_back(&constraint);
      else if (coefficient < 0)
        negative.push_back(&constraint);
      else
        next.push_back(constraint);
    }
    for (auto *upper : positive) {
      for (auto *lower : negative) {
        auto combined = combine_constraints(*upper, *lower, variable);
        if (!combined)
          return Feasibility::Overflow;
        next.push_back(std::move(*combined));
        if (next.size() > 4096)
          return Feasibility::Overflow;
      }
    }
    constraints = std::move(next);
  }

  for (const auto &constraint : constraints) {
    if (constraint.constant < 0)
      return Feasibility::Empty;
  }
  return Feasibility::MayBeFeasible;
}

auto is_jump_to(Block *block, Block *successor) -> bool {
  if (!block || block->insts.empty())
    return false;
  auto *op = block->insts.back();
  return op->code == OpCode::Jump && op->successors.size() == 1 &&
         op->successors.front() == successor;
}

auto has_movable_preheader_ops(Block *block) -> bool {
  if (!block || block->insts.empty())
    return false;
  auto terminator = std::prev(block->insts.end());
  for (auto it = block->insts.begin(); it != terminator; ++it) {
    auto *op = *it;
    if (
      op->code != OpCode::GetPtr || !op->result || op->operands.empty() ||
      !op->operands[0]->type->is_ptr() || !op->result->type->is_ptr()
    ) {
      return false;
    }
    auto plan = ir::analyze_getptr(
      op->operands[0]->type, op->result->type, op->operands.size() - 1
    );
    if (plan.reads_memory)
      return false;
  }
  return true;
}

auto control_only_header(const CountedLoopInfo &counted) -> bool {
  size_t phis = 0;
  for (auto *op : counted.header->insts) {
    if (op->code == OpCode::Phi) {
      ++phis;
      continue;
    }
    if (op != counted.compare && op != counted.header->insts.back())
      return false;
  }
  return phis == 1 && counted.header->insts.back()->code == OpCode::Branch;
}

auto latch_contains_only_update_and_jump(const CountedLoopInfo &counted)
  -> bool {
  if (!is_jump_to(counted.latch, counted.header))
    return false;
  for (auto *op : counted.latch->insts) {
    if (op != counted.induction.update && op != counted.latch->insts.back())
      return false;
  }
  return true;
}

auto add_parameter(std::vector<Value *> &parameters, Value *value) -> void {
  if (
    value &&
    std::find(parameters.begin(), parameters.end(), value) == parameters.end()
  ) {
    parameters.push_back(value);
  }
}

auto collect_parameters(
  const SCEVAffineExpr &expr,
  const std::vector<Value *> &iterators,
  std::vector<Value *> &parameters
) -> void {
  for (const auto &[symbol, coefficient] : expr.coefficients) {
    if (
      coefficient != 0 &&
      std::find(iterators.begin(), iterators.end(), symbol) == iterators.end()
    ) {
      add_parameter(parameters, symbol);
    }
  }
}

auto append_term(
  fmt::memory_buffer &out,
  int64_t coefficient,
  const std::string &name,
  bool &first
) -> void {
  if (coefficient == 0)
    return;
  if (!first)
    fmt::format_to(
      std::back_inserter(out), "{}", coefficient < 0 ? " - " : " + "
    );
  else if (coefficient < 0)
    fmt::format_to(std::back_inserter(out), "-");
  auto magnitude = coefficient < 0 ? -static_cast<__int128>(coefficient)
                                   : static_cast<__int128>(coefficient);
  if (magnitude != 1)
    fmt::format_to(
      std::back_inserter(out), "{} * ", static_cast<uint64_t>(magnitude)
    );
  fmt::format_to(std::back_inserter(out), "{}", name);
  first = false;
}

auto affine_string(
  const SCEVAffineExpr &expr,
  const std::vector<Value *> &iterators,
  const std::vector<Value *> &parameters
) -> std::string {
  fmt::memory_buffer out;
  bool first = true;
  for (size_t index = 0; index < iterators.size(); ++index)
    append_term(
      out, expr.coefficient(iterators[index]), fmt::format("i{}", index), first
    );
  for (size_t index = 0; index < parameters.size(); ++index)
    append_term(
      out, expr.coefficient(parameters[index]), fmt::format("p{}", index), first
    );
  if (expr.constant != 0 || first) {
    if (!first)
      fmt::format_to(
        std::back_inserter(out), "{}", expr.constant < 0 ? " - " : " + "
      );
    else if (expr.constant < 0)
      fmt::format_to(std::back_inserter(out), "-");
    auto magnitude = expr.constant < 0 ? -static_cast<__int128>(expr.constant)
                                       : static_cast<__int128>(expr.constant);
    fmt::format_to(
      std::back_inserter(out), "{}", static_cast<uint64_t>(magnitude)
    );
  }
  return fmt::to_string(out);
}

auto parameter_prefix(const std::vector<Value *> &parameters) -> std::string {
  if (parameters.empty())
    return {};
  fmt::memory_buffer out;
  fmt::format_to(std::back_inserter(out), "[");
  for (size_t index = 0; index < parameters.size(); ++index) {
    if (index)
      fmt::format_to(std::back_inserter(out), ", ");
    fmt::format_to(std::back_inserter(out), "p{}", index);
  }
  fmt::format_to(std::back_inserter(out), "] -> ");
  return fmt::to_string(out);
}

auto tuple(const char *prefix, size_t count) -> std::string {
  fmt::memory_buffer out;
  fmt::format_to(std::back_inserter(out), "S[");
  for (size_t index = 0; index < count; ++index) {
    if (index)
      fmt::format_to(std::back_inserter(out), ", ");
    fmt::format_to(std::back_inserter(out), "{}{}", prefix, index);
  }
  fmt::format_to(std::back_inserter(out), "]");
  return fmt::to_string(out);
}

auto checked_stride_add(int64_t lhs, int64_t coefficient, int scale)
  -> std::optional<int64_t> {
  auto value =
    static_cast<__int128>(lhs) + static_cast<__int128>(coefficient) * scale;
  if (
    value < std::numeric_limits<int64_t>::min() ||
    value > std::numeric_limits<int64_t>::max()
  ) {
    return std::nullopt;
  }
  return static_cast<int64_t>(value);
}

auto magnitude(int64_t value) -> uint64_t {
  return value < 0 ? static_cast<uint64_t>(-static_cast<__int128>(value))
                   : static_cast<uint64_t>(value);
}

auto add_scaled_expression(
  SCEVAffineExpr &result, const SCEVAffineExpr &source, int64_t scale
) -> bool {
  auto constant = checked_i64(
    static_cast<__int128>(result.constant) +
    static_cast<__int128>(source.constant) * scale
  );
  if (!constant)
    return false;
  result.constant = *constant;
  for (const auto &[symbol, coefficient] : source.coefficients) {
    auto next = checked_i64(
      static_cast<__int128>(result.coefficient(symbol)) +
      static_cast<__int128>(coefficient) * scale
    );
    if (!next)
      return false;
    if (*next == 0)
      result.coefficients.erase(symbol);
    else
      result.coefficients[symbol] = *next;
  }
  return true;
}

auto add_expression_coefficients(
  LinearConstraint &constraint,
  const SCEVAffineExpr &expr,
  int64_t sign,
  size_t iteration_base,
  const std::vector<Value *> &iterators,
  const std::vector<Value *> &parameters
) -> bool {
  for (size_t index = 0; index < parameters.size(); ++index) {
    auto next = checked_i64(
      static_cast<__int128>(constraint.coefficients[index]) +
      static_cast<__int128>(sign) * expr.coefficient(parameters[index])
    );
    if (!next)
      return false;
    constraint.coefficients[index] = *next;
  }
  for (size_t index = 0; index < iterators.size(); ++index) {
    auto next = checked_i64(
      static_cast<__int128>(constraint.coefficients[iteration_base + index]) +
      static_cast<__int128>(sign) * expr.coefficient(iterators[index])
    );
    if (!next)
      return false;
    constraint.coefficients[iteration_base + index] = *next;
  }
  return true;
}

auto add_less_equal(
  std::vector<LinearConstraint> &constraints,
  const SCEVAffineExpr &lhs,
  size_t lhs_iteration_base,
  const SCEVAffineExpr &rhs,
  size_t rhs_iteration_base,
  int64_t rhs_adjustment,
  const std::vector<Value *> &iterators,
  const std::vector<Value *> &parameters
) -> bool {
  LinearConstraint constraint;
  constraint.coefficients.resize(parameters.size() + 4);
  if (
    !add_expression_coefficients(
      constraint, lhs, 1, lhs_iteration_base, iterators, parameters
    ) ||
    !add_expression_coefficients(
      constraint, rhs, -1, rhs_iteration_base, iterators, parameters
    )
  ) {
    return false;
  }
  auto constant = checked_i64(
    static_cast<__int128>(rhs.constant) + rhs_adjustment - lhs.constant
  );
  if (!constant)
    return false;
  constraint.constant = *constant;
  normalize(constraint);
  constraints.push_back(std::move(constraint));
  return true;
}

auto add_parameter_ranges(
  std::vector<LinearConstraint> &constraints, size_t parameter_count
) -> void {
  const auto variable_count = parameter_count + 4;
  for (size_t parameter = 0; parameter < parameter_count; ++parameter) {
    LinearConstraint upper;
    upper.coefficients.resize(variable_count);
    upper.coefficients[parameter] = 1;
    upper.constant = std::numeric_limits<int32_t>::max();
    constraints.push_back(std::move(upper));

    LinearConstraint lower;
    lower.coefficients.resize(variable_count);
    lower.coefficients[parameter] = -1;
    lower.constant = -static_cast<int64_t>(std::numeric_limits<int32_t>::min());
    constraints.push_back(std::move(lower));
  }
}

} // namespace

auto PolyhedralInfo::compute(
  LinearFunction &func,
  LoopInfo &loop_info,
  AffineLoopInfo &affine,
  ScalarEvolution &scev,
  MidModule *mid_module
) -> void {
  scops.clear();
  op_blocks.clear();
  function = &func;
  module = mid_module;
  for (auto &block : func.blocks) {
    for (auto *op : block->insts)
      op_blocks[op] = block.get();
  }

  for (auto *inner : loop_info.get_loops_innermost_first()) {
    auto *outer = inner->get_parent();
    if (!outer)
      continue;
    auto scop = build_scop(*outer, *inner, affine, scev);
    if (scop)
      scops.push_back(std::move(*scop));
  }
}

auto PolyhedralInfo::build_scop(
  const Loop &outer,
  const Loop &inner,
  AffineLoopInfo &affine,
  ScalarEvolution &scev
) const -> std::optional<PolyhedralScop> {
  if (
    outer.get_subloops().size() != 1 ||
    outer.get_subloops().front() != &inner || !inner.get_subloops().empty()
  ) {
    return std::nullopt;
  }

  auto outer_counted = affine.match_counted_loop(outer);
  auto inner_counted = affine.match_counted_loop(inner);
  if (!outer_counted || !inner_counted)
    return std::nullopt;
  if (
    outer_counted->induction.step != 1 || inner_counted->induction.step != 1 ||
    outer_counted->induction.predicate != OpCode::Lt ||
    inner_counted->induction.predicate != OpCode::Lt ||
    !affine.is_no_wrap(
      outer_counted->induction.phi->result, *outer_counted, outer
    ) ||
    !affine.is_no_wrap(
      inner_counted->induction.phi->result, *inner_counted, inner
    )
  ) {
    return std::nullopt;
  }

  if (
    outer_counted->continuation != inner_counted->preheader ||
    inner_counted->exit != outer_counted->latch ||
    !is_jump_to(inner_counted->preheader, inner_counted->header) ||
    !has_movable_preheader_ops(inner_counted->preheader) ||
    !control_only_header(*outer_counted) ||
    !control_only_header(*inner_counted) ||
    !latch_contains_only_update_and_jump(*outer_counted)
  ) {
    return std::nullopt;
  }

  auto preheader_terminator = std::prev(inner_counted->preheader->insts.end());
  for (auto it = inner_counted->preheader->insts.begin();
       it != preheader_terminator;
       ++it) {
    auto *op = *it;
    if (!op->result)
      return std::nullopt;
    for (auto *user_base : op->result->users) {
      auto *user = static_cast<Op *>(user_base);
      auto block = op_blocks.find(user);
      // Module-owned operations outlive their removal from a function.  Only
      // users still present in this function constrain the CFG rewrite.
      if (block != op_blocks.end() && !inner.contains(block->second))
        return std::nullopt;
    }
  }

  std::unordered_set<Block *> expected_outer_only{
    outer_counted->header,
    inner_counted->preheader,
    outer_counted->latch,
  };
  for (auto *block : outer.get_blocks()) {
    if (!inner.contains(block) && !expected_outer_only.count(block))
      return std::nullopt;
  }

  std::vector<CountedLoopInfo> iteration_space{*outer_counted, *inner_counted};
  auto outer_initial =
    scev.get_affine(outer_counted->induction.initial, iteration_space);
  auto outer_bound =
    scev.get_affine(outer_counted->induction.bound, iteration_space);
  auto inner_initial =
    scev.get_affine(inner_counted->induction.initial, iteration_space);
  auto inner_bound =
    scev.get_affine(inner_counted->induction.bound, iteration_space);
  if (!outer_initial || !outer_bound || !inner_initial || !inner_bound)
    return std::nullopt;

  const std::vector<Value *> iterators{
    outer_counted->induction.phi->result,
    inner_counted->induction.phi->result,
  };
  auto depends_on_iterator = [&iterators](const SCEVAffineExpr &expr) {
    return std::any_of(iterators.begin(), iterators.end(), [&](Value *value) {
      return expr.coefficient(value) != 0;
    });
  };
  if (
    depends_on_iterator(*outer_initial) || depends_on_iterator(*outer_bound) ||
    depends_on_iterator(*inner_initial) || depends_on_iterator(*inner_bound)
  ) {
    return std::nullopt;
  }

  PolyhedralScop scop;
  scop.outer = &outer;
  scop.inner = &inner;
  scop.outer_counted = *outer_counted;
  scop.inner_counted = *inner_counted;
  scop.lower_bounds = {*outer_initial, *inner_initial};
  scop.upper_bounds = {*outer_bound, *inner_bound};
  for (const auto *expr :
       {&*outer_initial, &*outer_bound, &*inner_initial, &*inner_bound})
    collect_parameters(*expr, iterators, scop.parameters);

  if (!collect_accesses(scop, scev) || !build_polyhedral_model(scop))
    return std::nullopt;
  scop.interchange_legal = has_legal_interchange(scop);
  return scop;
}

auto PolyhedralInfo::collect_accesses(
  PolyhedralScop &scop, ScalarEvolution &scev
) const -> bool {
  const std::vector<CountedLoopInfo> iteration_space{
    scop.outer_counted, scop.inner_counted
  };
  const std::vector<Value *> iterators{
    scop.outer_counted.induction.phi->result,
    scop.inner_counted.induction.phi->result,
  };

  std::unordered_map<Value *, MemoryLocation> roots;
  for (auto *block : scop.inner->get_blocks()) {
    for (auto *op : block->insts) {
      if (
        op->code == OpCode::Call || op->code == OpCode::Memset ||
        op->code == OpCode::Ret || op->code == OpCode::Alloca
      ) {
        return false;
      }
      if (op->code != OpCode::Load && op->code != OpCode::Store)
        continue;

      Value *pointer = nullptr;
      if (op->code == OpCode::Load && op->operands.size() == 1)
        pointer = op->operands[0];
      else if (op->code == OpCode::Store && op->operands.size() == 2)
        pointer = op->operands[1];
      auto *accessed_value = op->code == OpCode::Load
                               ? static_cast<Value *>(op->result)
                               : op->operands.front();
      if (
        !accessed_value ||
        (!accessed_value->type->is_i32() && !accessed_value->type->is_f32())
      ) {
        return false;
      }
      if (!pointer || pointer->kind != ValueKind::OpResult)
        return false;
      auto *getptr =
        static_cast<Op *>(static_cast<OpResult *>(pointer)->creator);
      if (
        !getptr || getptr->code != OpCode::GetPtr || !getptr->result ||
        getptr->operands.size() < 2 || !getptr->operands[0]->type->is_ptr() ||
        !getptr->result->type->is_ptr()
      ) {
        return false;
      }

      auto plan = ir::analyze_getptr(
        getptr->operands[0]->type,
        getptr->result->type,
        getptr->operands.size() - 1
      );
      if (
        std::count_if(
          plan.steps.begin(), plan.steps.end(), [](const auto &step) {
            return step.kind == ir::GetPtrStep::Kind::Index;
          }
        ) != static_cast<std::ptrdiff_t>(getptr->operands.size() - 1)
      ) {
        return false;
      }

      PolyhedralAccess access;
      access.operation = op;
      access.kind = op->code == OpCode::Store ? PolyhedralAccessKind::Write
                                              : PolyhedralAccessKind::Read;
      auto location = alias.get_location(pointer);
      if (plan.reads_memory) {
        const auto implicit_loads = std::count_if(
          plan.steps.begin(), plan.steps.end(), [](const auto &step) {
            return step.kind == ir::GetPtrStep::Kind::ImplicitLoad;
          }
        );
        auto *loaded_pointer =
          implicit_loads == 1
            ? resolve_invariant_pointer_slot(getptr->operands[0])
            : nullptr;
        if (!loaded_pointer)
          return false;
        location = alias.get_location(loaded_pointer);
      }
      if (!location.root)
        return false;
      access.root = location.root;
      roots.emplace(location.root, alias.get_location(location.root));

      for (size_t index = 1; index < getptr->operands.size(); ++index) {
        auto expr = scev.get_affine(getptr->operands[index], iteration_space);
        if (!expr || !expr->no_wrap)
          return false;
        collect_parameters(*expr, iterators, scop.parameters);
        access.subscripts.push_back(std::move(*expr));
      }

      int64_t outer_stride = 0;
      int64_t inner_stride = 0;
      access.byte_offset.no_wrap = true;
      for (const auto &step : plan.steps) {
        if (step.kind == ir::GetPtrStep::Kind::ImplicitLoad)
          continue;
        if (step.index_pos >= access.subscripts.size())
          return false;
        auto next_outer = checked_stride_add(
          outer_stride,
          access.subscripts[step.index_pos].coefficient(iterators[0]),
          step.scale
        );
        auto next_inner = checked_stride_add(
          inner_stride,
          access.subscripts[step.index_pos].coefficient(iterators[1]),
          step.scale
        );
        if (!next_outer || !next_inner)
          return false;
        if (!add_scaled_expression(
              access.byte_offset, access.subscripts[step.index_pos], step.scale
            )) {
          return false;
        }
        outer_stride = *next_outer;
        inner_stride = *next_inner;
      }
      access.outer_stride = outer_stride;
      access.inner_stride = inner_stride;
      scop.accesses.push_back(std::move(access));
    }
  }
  if (scop.accesses.empty())
    return false;

  for (auto lhs = roots.begin(); lhs != roots.end(); ++lhs) {
    for (auto rhs = std::next(lhs); rhs != roots.end(); ++rhs) {
      if (
        alias.may_alias(lhs->second, rhs->second) &&
        !roots_are_proven_distinct(lhs->first, rhs->first)
      ) {
        return false;
      }
    }
  }

  for (const auto &access : scop.accesses) {
    auto original = magnitude(access.inner_stride);
    auto interchanged = magnitude(access.outer_stride);
    if (
      scop.original_stride_cost >
        std::numeric_limits<uint64_t>::max() - original ||
      scop.interchanged_stride_cost >
        std::numeric_limits<uint64_t>::max() - interchanged
    ) {
      return false;
    }
    scop.original_stride_cost += original;
    scop.interchanged_stride_cost += interchanged;
  }
  return true;
}

auto PolyhedralInfo::resolve_invariant_pointer_slot(Value *slot) const
  -> Value * {
  if (!slot || slot->kind != ValueKind::OpResult)
    return nullptr;
  auto *alloca = static_cast<Op *>(static_cast<OpResult *>(slot)->creator);
  if (!alloca || alloca->code != OpCode::Alloca)
    return nullptr;

  Value *stored_pointer = nullptr;
  for (auto *user_base : slot->users) {
    auto *user = static_cast<Op *>(user_base);
    if (!op_blocks.count(user))
      continue;
    if (
      user->code == OpCode::Store && user->operands.size() == 2 &&
      user->operands[1] == slot
    ) {
      if (stored_pointer && stored_pointer != user->operands[0])
        return nullptr;
      stored_pointer = user->operands[0];
      continue;
    }
    if (
      user->code != OpCode::GetPtr || user->operands.empty() ||
      user->operands[0] != slot
    ) {
      return nullptr;
    }
  }
  return stored_pointer;
}

auto PolyhedralInfo::roots_are_proven_distinct(Value *lhs, Value *rhs) const
  -> bool {
  if (
    !lhs || !rhs || lhs->kind != ValueKind::Argument ||
    rhs->kind != ValueKind::Argument || !function || !module
  ) {
    return false;
  }

  const auto lhs_index = static_cast<Argument *>(lhs)->idx;
  const auto rhs_index = static_cast<Argument *>(rhs)->idx;
  bool found_call = false;
  for (const auto &caller : module->functions) {
    for (const auto &block : caller->blocks) {
      for (auto *op : block->insts) {
        if (
          op->code != OpCode::Call ||
          std::get<CallPayload>(op->payload).func_name != function->name
        ) {
          continue;
        }
        found_call = true;
        if (
          lhs_index < 0 || rhs_index < 0 ||
          static_cast<size_t>(std::max(lhs_index, rhs_index)) >=
            op->operands.size()
        ) {
          return false;
        }
        auto lhs_location = alias.get_location(op->operands[lhs_index]);
        auto rhs_location = alias.get_location(op->operands[rhs_index]);
        if (alias.may_alias(lhs_location, rhs_location))
          return false;
      }
    }
  }
  return found_call;
}

auto PolyhedralInfo::build_polyhedral_model(PolyhedralScop &scop) const
  -> bool {
  const std::vector<Value *> iterators{
    scop.outer_counted.induction.phi->result,
    scop.inner_counted.induction.phi->result,
  };
  fmt::memory_buffer constraints;
  for (size_t depth = 0; depth < 2; ++depth) {
    if (depth)
      fmt::format_to(std::back_inserter(constraints), " and ");
    fmt::format_to(
      std::back_inserter(constraints),
      "{} <= i{} and i{} < {}",
      affine_string(scop.lower_bounds[depth], iterators, scop.parameters),
      depth,
      depth,
      affine_string(scop.upper_bounds[depth], iterators, scop.parameters)
    );
  }
  const auto constraint_string = fmt::to_string(constraints);
  scop.domain = fmt::format(
    "{}{{ {} : {} }}",
    parameter_prefix(scop.parameters),
    tuple("i", 2),
    constraint_string
  );

  std::vector<Value *> roots;
  for (auto &access : scop.accesses) {
    auto root = std::find(roots.begin(), roots.end(), access.root);
    if (root == roots.end()) {
      roots.push_back(access.root);
      root = std::prev(roots.end());
    }
    auto root_id = std::distance(roots.begin(), root);
    fmt::memory_buffer relation;
    fmt::format_to(
      std::back_inserter(relation),
      "{}{{ {} -> A{}[o0] : {} and o0 = {}",
      parameter_prefix(scop.parameters),
      tuple("i", 2),
      root_id,
      constraint_string,
      affine_string(access.byte_offset, iterators, scop.parameters)
    );
    fmt::format_to(std::back_inserter(relation), " }}");
    access.relation = fmt::to_string(relation);
  }
  return true;
}

auto PolyhedralInfo::has_legal_interchange(const PolyhedralScop &scop) const
  -> bool {
  const std::vector<Value *> iterators{
    scop.outer_counted.induction.phi->result,
    scop.inner_counted.induction.phi->result,
  };
  SCEVAffineExpr outer_iterator{0, {{iterators[0], 1}}, true};
  SCEVAffineExpr inner_iterator{0, {{iterators[1], 1}}, true};
  const size_t source_base = scop.parameters.size();
  const size_t sink_base = source_base + 2;

  for (const auto &source : scop.accesses) {
    for (const auto &sink : scop.accesses) {
      if (
        source.root != sink.root ||
        (source.kind != PolyhedralAccessKind::Write &&
         sink.kind != PolyhedralAccessKind::Write)
      ) {
        continue;
      }
      std::vector<LinearConstraint> constraints;
      add_parameter_ranges(constraints, scop.parameters.size());
      for (size_t depth = 0; depth < 2; ++depth) {
        const auto &iterator = depth == 0 ? outer_iterator : inner_iterator;
        for (auto base : {source_base, sink_base}) {
          if (
            !add_less_equal(
              constraints,
              scop.lower_bounds[depth],
              base,
              iterator,
              base,
              0,
              iterators,
              scop.parameters
            ) ||
            !add_less_equal(
              constraints,
              iterator,
              base,
              scop.upper_bounds[depth],
              base,
              -1,
              iterators,
              scop.parameters
            )
          ) {
            return false;
          }
        }
      }

      // A two-dimensional adjacent swap is illegal exactly when an original
      // dependence moves forward in the old outer dimension and backward in
      // the old inner dimension.
      if (
        !add_less_equal(
          constraints,
          outer_iterator,
          source_base,
          outer_iterator,
          sink_base,
          -1,
          iterators,
          scop.parameters
        ) ||
        !add_less_equal(
          constraints,
          inner_iterator,
          sink_base,
          inner_iterator,
          source_base,
          -1,
          iterators,
          scop.parameters
        ) ||
        !add_less_equal(
          constraints,
          source.byte_offset,
          source_base,
          sink.byte_offset,
          sink_base,
          0,
          iterators,
          scop.parameters
        ) ||
        !add_less_equal(
          constraints,
          sink.byte_offset,
          sink_base,
          source.byte_offset,
          source_base,
          0,
          iterators,
          scop.parameters
        )
      ) {
        return false;
      }
      if (
        prove_feasibility(std::move(constraints), scop.parameters.size() + 4) !=
        Feasibility::Empty
      ) {
        return false;
      }
    }
  }
  return true;
}

} // namespace exodus::mid_ir
