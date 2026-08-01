#pragma once

#include "lsr_context.hpp"
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace exodus::mid_ir::opt {

struct AffineExpressionInfo {
  int coefficient = 0;
  unsigned cost = 0;
  bool migrated_shape = false;
};

// Performs the loop-local expression queries used by LSR.  The single summary
// query keeps coefficient, cost, and migrated-shape matching on the same
// recursive traversal, so those decisions cannot silently accept different
// expression languages as the pass evolves.
class AffineExpressionMatcher {
public:
  explicit AffineExpressionMatcher(const LSRContext &context)
      : context(context) {}

  auto analyze(Value *value, Value *induction, const Loop &loop) const
    -> std::optional<AffineExpressionInfo>;
  auto can_clone_initial_expression(
    Value *value,
    Value *induction,
    Value *initial,
    const Loop &loop,
    Block *preheader
  ) const -> bool;
  auto dynamic_guard_proves_no_wrap(
    Value *value, const CountedLoopInfo &counted, const Loop &loop
  ) const -> bool;
  auto constant_affine_difference(
    Value *value, Value *anchor, const Loop &loop
  ) const -> std::optional<int64_t>;

private:
  const LSRContext &context;

  auto analyze_impl(
    Value *value,
    Value *induction,
    const Loop &loop,
    std::unordered_set<Value *> &active
  ) const -> std::optional<AffineExpressionInfo>;
  auto is_clonable_expression(const Op &op, const Loop &loop) const -> bool;
};

class ImmutablePointerSlotAnalysis {
public:
  explicit ImmutablePointerSlotAnalysis(const LSRContext &context)
      : context(context) {}

  auto is_immutable_local_slot(Value *base, Block *preheader) const -> bool;

private:
  const LSRContext &context;
};

} // namespace exodus::mid_ir::opt
