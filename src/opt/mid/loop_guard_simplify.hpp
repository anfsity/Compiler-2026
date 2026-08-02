#pragma once

#include "../../mid/affine_loop.hpp"
#include "../../mid/loop.hpp"
#include "../../mid/rewriter.hpp"
#include "../AnalysisManager.hpp"
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace exodus::mid_ir::opt {

// Fold loop-local comparisons that follow from counted-loop lower/upper
// bounds.  The pass deliberately leaves loop-header tests untouched: those
// tests control whether another iteration exists and are not body facts.
class LoopGuardSimplify {
public:
  explicit LoopGuardSimplify(MidModule *m) : module(m) {}

  struct LinearForm {
    std::map<Value *, int64_t> terms;
    int64_t constant = 0;

    auto operator==(const LinearForm &other) const -> bool {
      return constant == other.constant && terms == other.terms;
    }
  };

  auto run(LinearFunction &func, exodus::opt::LinearFunctionAnalysisManager &am)
    -> exodus::opt::PreservedAnalysis;

private:
  MidModule *module;
  std::unordered_map<Op *, Block *> op_blocks;

  auto build_op_block_map(LinearFunction &func) -> void;
  auto form(Value *value, std::unordered_set<Value *> &active) const
    -> std::optional<LinearForm>;
  auto add(const LinearForm &lhs, const LinearForm &rhs) const -> LinearForm;
  auto negate(const LinearForm &value) const -> LinearForm;
  auto relation_form(
    OpCode predicate,
    Value *lhs,
    Value *rhs,
    std::unordered_set<Value *> &active
  ) const -> std::optional<LinearForm>;
  auto collect_facts(
    const Loop &scope, const LoopInfo &loops, const AffineLoopInfo &affine
  ) const -> std::vector<LinearForm>;
  auto prove_relation(
    OpCode predicate,
    Value *lhs,
    Value *rhs,
    const Loop &scope,
    const LoopInfo &loops,
    const AffineLoopInfo &affine
  ) const -> std::optional<bool>;
  auto prove_value(
    Value *value,
    const Loop &scope,
    const LoopInfo &loops,
    const AffineLoopInfo &affine,
    std::unordered_set<Value *> &active
  ) const -> std::optional<bool>;
  auto reset_operands(Op *op, std::vector<Value *> operands) const -> void;
};

} // namespace exodus::mid_ir::opt
