#pragma once

#include "ir.hpp"
#include <unordered_set>

namespace exodus::high_ir {

struct OpEffects {
  // Keep the exact address operands here. Consumers that understand aliasing
  // may compare get_addr_root(), but canonicalizing eagerly loses whether a
  // Store writes directly to an Alloca or indirectly through a GetPtr.
  std::unordered_set<Value *> reads;
  std::unordered_set<Value *> writes;
  bool has_unknown_effect = false;
  bool has_control_effect = false;
  bool speculatable = false;
  bool cseable = false;
  bool has_unique_identity = false;

  auto reads_memory() const -> bool {
    return has_unknown_effect || !reads.empty();
  }

  auto writes_memory() const -> bool {
    return has_unknown_effect || !writes.empty();
  }

  auto has_observable_effect() const -> bool {
    return has_unknown_effect || has_control_effect || !writes.empty();
  }

  auto merge(const OpEffects &other) -> void {
    reads.insert(other.reads.begin(), other.reads.end());
    writes.insert(other.writes.begin(), other.writes.end());
    has_unknown_effect |= other.has_unknown_effect;
    has_control_effect |= other.has_control_effect;
    speculatable = false;
    cseable = false;
    has_unique_identity |= other.has_unique_identity;
  }
};

auto get_addr_root(Value *value) -> Value *;
// Effects performed directly by op, excluding nested structured regions.
auto get_op_effects(const Op &op) -> OpEffects;
// Recursive summary of every operation nested in region.
auto get_region_effects(const Region &region) -> OpEffects;

} // namespace exodus::high_ir
