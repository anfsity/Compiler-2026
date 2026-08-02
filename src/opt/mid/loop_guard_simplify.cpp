#include "loop_guard_simplify.hpp"

#include <algorithm>
#include <limits>
#include <unordered_set>

namespace exodus::mid_ir::opt {
namespace {

auto is_compare(OpCode code) -> bool {
  return code == OpCode::Eq || code == OpCode::Ne || code == OpCode::Lt ||
         code == OpCode::Le || code == OpCode::Gt || code == OpCode::Ge;
}

auto same_form(
  const LoopGuardSimplify::LinearForm &lhs,
  const LoopGuardSimplify::LinearForm &rhs
) -> bool {
  return lhs == rhs;
}

auto form_key(const LoopGuardSimplify::LinearForm &form) -> std::string {
  std::string key = std::to_string(form.constant) + ":";
  for (const auto &[value, coefficient] : form.terms) {
    key += std::to_string(reinterpret_cast<uintptr_t>(value));
    key += ":" + std::to_string(coefficient) + ";";
  }
  return key;
}

auto normalize(LoopGuardSimplify::LinearForm form)
  -> LoopGuardSimplify::LinearForm {
  for (auto it = form.terms.begin(); it != form.terms.end();) {
    if (it->second == 0)
      it = form.terms.erase(it);
    else
      ++it;
  }
  return form;
}

} // namespace

auto LoopGuardSimplify::run(
  LinearFunction &func, exodus::opt::LinearFunctionAnalysisManager &am
) -> exodus::opt::PreservedAnalysis {
  if (func.blocks.empty())
    return exodus::opt::PreservedAnalysis::all();

  auto &loop_info = am.get_result<LoopAnalysis>(func);
  auto &affine = am.get_result<AffineLoopAnalysis>(func);
  build_op_block_map(func);

  bool changed = false;
  for (auto *loop : loop_info.get_loops_innermost_first()) {
    for (auto *block : loop->get_blocks()) {
      // The loop-header branch is a control predicate, not a fact about the
      // body.  Folding it would remove the loop's exit edge.
      if (block == loop->get_header() || block->insts.empty())
        continue;
      auto *branch = block->insts.back();
      if (
        branch->code != OpCode::Branch || branch->operands.size() != 1 ||
        !branch->operands[0]
      ) {
        continue;
      }

      std::unordered_set<Value *> active;
      auto truth =
        prove_value(branch->operands[0], *loop, loop_info, affine, active);
      if (!truth)
        continue;

      auto *constant = module->ctx->make_const(Bool::get(), *truth ? 1 : 0);
      reset_operands(branch, {constant});
      changed = true;
    }
  }

  op_blocks.clear();
  return changed ? exodus::opt::PreservedAnalysis::none()
                 : exodus::opt::PreservedAnalysis::all();
}

auto LoopGuardSimplify::build_op_block_map(LinearFunction &func) -> void {
  op_blocks.clear();
  for (auto &block : func.blocks) {
    for (auto *op : block->insts)
      op_blocks[op] = block.get();
  }
}

auto LoopGuardSimplify::form(
  Value *value, std::unordered_set<Value *> &active
) const -> std::optional<LinearForm> {
  if (!value)
    return std::nullopt;
  if (auto constant = AffineLoopInfo::integer_constant(value))
    return LinearForm{{}, *constant};
  if (value->kind != ValueKind::OpResult)
    return LinearForm{{{value, 1}}, 0};
  if (!active.insert(value).second)
    return std::nullopt;

  auto *op = static_cast<Op *>(static_cast<OpResult *>(value)->creator);
  std::optional<LinearForm> result;
  if (op && op->operands.size() == 2) {
    auto lhs = form(op->operands[0], active);
    auto rhs = form(op->operands[1], active);
    if (lhs && rhs) {
      switch (op->code) {
      case OpCode::Add:
        result = add(*lhs, *rhs);
        break;
      case OpCode::Sub:
        result = add(*lhs, negate(*rhs));
        break;
      case OpCode::Mul:
        if (lhs->terms.empty()) {
          result = *rhs;
          for (auto &[_, coefficient] : result->terms)
            coefficient *= lhs->constant;
          result->constant *= lhs->constant;
        } else if (rhs->terms.empty()) {
          result = *lhs;
          for (auto &[_, coefficient] : result->terms)
            coefficient *= rhs->constant;
          result->constant *= rhs->constant;
        }
        break;
      default:
        break;
      }
    }
  }
  active.erase(value);
  if (!result)
    result = LinearForm{{{value, 1}}, 0};
  return normalize(*result);
}

auto LoopGuardSimplify::add(const LinearForm &lhs, const LinearForm &rhs) const
  -> LinearForm {
  LinearForm result = lhs;
  result.constant += rhs.constant;
  for (const auto &[value, coefficient] : rhs.terms)
    result.terms[value] += coefficient;
  return normalize(std::move(result));
}

auto LoopGuardSimplify::negate(const LinearForm &value) const -> LinearForm {
  LinearForm result = value;
  result.constant = -result.constant;
  for (auto &[_, coefficient] : result.terms)
    coefficient = -coefficient;
  return normalize(std::move(result));
}

auto LoopGuardSimplify::relation_form(
  OpCode predicate, Value *lhs, Value *rhs, std::unordered_set<Value *> &active
) const -> std::optional<LinearForm> {
  auto left = form(lhs, active);
  auto right = form(rhs, active);
  if (!left || !right)
    return std::nullopt;

  LinearForm result;
  switch (predicate) {
  case OpCode::Ge:
    result = add(*left, negate(*right));
    break;
  case OpCode::Gt:
    result = add(*left, negate(*right));
    --result.constant;
    break;
  case OpCode::Le:
    result = add(*right, negate(*left));
    break;
  case OpCode::Lt:
    result = add(*right, negate(*left));
    --result.constant;
    break;
  default:
    return std::nullopt;
  }
  return normalize(std::move(result));
}

auto LoopGuardSimplify::collect_facts(
  const Loop &scope, const LoopInfo &loops, const AffineLoopInfo &affine
) const -> std::vector<LinearForm> {
  std::vector<LinearForm> facts;
  for (const Loop *current = &scope; current; current = current->get_parent()) {
    auto counted = affine.match_counted_loop(*current, true);
    if (!counted)
      continue;

    std::unordered_set<Value *> active;
    auto induction = form(counted->induction.phi->result, active);
    auto initial = form(counted->induction.initial, active);
    auto bound = form(counted->induction.bound, active);
    if (!induction || !initial || !bound)
      continue;

    // A positive counted induction is never below its initial value in the
    // loop body; a negative one is never above it.
    if (counted->induction.step > 0)
      facts.push_back(add(*induction, negate(*initial)));
    else if (counted->induction.step < 0)
      facts.push_back(add(*initial, negate(*induction)));

    switch (counted->induction.predicate) {
    case OpCode::Lt:
      facts.push_back(add(*bound, negate(*induction)));
      --facts.back().constant;
      break;
    case OpCode::Le:
      facts.push_back(add(*bound, negate(*induction)));
      break;
    case OpCode::Gt:
      facts.push_back(add(*induction, negate(*bound)));
      --facts.back().constant;
      break;
    case OpCode::Ge:
      facts.push_back(add(*induction, negate(*bound)));
      break;
    default:
      break;
    }
  }
  (void)loops;
  return facts;
}

auto LoopGuardSimplify::prove_relation(
  OpCode predicate,
  Value *lhs,
  Value *rhs,
  const Loop &scope,
  const LoopInfo &loops,
  const AffineLoopInfo &affine
) const -> std::optional<bool> {
  if (predicate == OpCode::Eq || predicate == OpCode::Ne)
    return std::nullopt;

  std::unordered_set<Value *> active;
  auto target = relation_form(predicate, lhs, rhs, active);
  if (!target)
    return std::nullopt;

  auto facts = collect_facts(scope, loops, affine);
  std::vector<LinearForm> closure;
  std::unordered_set<std::string> seen;
  for (auto fact : facts) {
    fact = normalize(std::move(fact));
    if (seen.insert(form_key(fact)).second)
      closure.push_back(std::move(fact));
  }

  // The useful nested-loop proofs are short sums of lower/upper-bound facts.
  // Limit closure growth so unrelated affine code cannot turn this pass into
  // an unbounded integer solver.
  for (size_t round = 0; round < 4 && closure.size() < 128; ++round) {
    const auto size = closure.size();
    for (size_t i = 0; i < size && closure.size() < 128; ++i) {
      for (size_t j = i; j < size && closure.size() < 128; ++j) {
        auto combined = add(closure[i], closure[j]);
        if (seen.insert(form_key(combined)).second)
          closure.push_back(std::move(combined));
      }
    }
  }
  if (std::any_of(closure.begin(), closure.end(), [&](const auto &fact) {
        return same_form(fact, *target);
      })) {
    return true;
  }
  return std::nullopt;
}

auto LoopGuardSimplify::prove_value(
  Value *value,
  const Loop &scope,
  const LoopInfo &loops,
  const AffineLoopInfo &affine,
  std::unordered_set<Value *> &active
) const -> std::optional<bool> {
  if (!value)
    return std::nullopt;
  if (auto constant = AffineLoopInfo::integer_constant(value))
    return *constant != 0;
  if (!active.insert(value).second)
    return std::nullopt;
  auto finish = [&active, value](std::optional<bool> result) {
    active.erase(value);
    return result;
  };

  if (value->kind != ValueKind::OpResult)
    return finish(std::nullopt);
  auto *op = static_cast<Op *>(static_cast<OpResult *>(value)->creator);
  if (!op)
    return finish(std::nullopt);
  if (is_compare(op->code) && op->operands.size() == 2) {
    auto result = prove_relation(
      op->code, op->operands[0], op->operands[1], scope, loops, affine
    );
    return finish(result);
  }
  if (op->code == OpCode::Phi) {
    const auto &incoming = std::get<PhiPayload>(op->payload).incoming;
    if (incoming.empty())
      return finish(std::nullopt);
    for (const auto &[_, incoming_value] : incoming) {
      auto result = prove_value(incoming_value, scope, loops, affine, active);
      if (!result || !*result)
        return finish(std::nullopt);
    }
    return finish(true);
  }
  return finish(std::nullopt);
}

auto LoopGuardSimplify::reset_operands(
  Op *op, std::vector<Value *> operands
) const -> void {
  for (auto *operand : op->operands) {
    if (operand)
      operand->rmUse(op);
  }
  op->operands = std::move(operands);
  for (auto *operand : op->operands) {
    if (operand)
      operand->addUse(op);
  }
}

} // namespace exodus::mid_ir::opt
