#pragma once

#include "ir.hpp"
#include <cstdint>
#include <optional>
#include <unordered_map>

namespace exodus::mid_ir {

struct IntegerRange {
  // Inclusive enclosure; minimum > maximum denotes an empty set.
  int64_t minimum = 0;
  int64_t maximum = -1;
  bool exact = false;

  auto empty() const -> bool { return minimum > maximum; }
};

// A checked affine expression over SSA values.  `no_wrap` is a semantic fact
// carried by ScalarEvolution; the arithmetic helpers intentionally reset it
// because callers must recompute it from their own range/effect proof.
struct AffineExpression {
  int64_t constant = 0;
  std::unordered_map<Value *, int64_t> coefficients;
  bool no_wrap = false;

  auto coefficient(Value *symbol) const -> int64_t {
    auto it = coefficients.find(symbol);
    return it == coefficients.end() ? 0 : it->second;
  }
  auto is_constant() const -> bool { return coefficients.empty(); }
};

auto checked_affine_i64(__int128 value) -> std::optional<int64_t>;
auto combine_affine_expressions(
  const AffineExpression &lhs, const AffineExpression &rhs, int64_t rhs_sign
) -> std::optional<AffineExpression>;
auto scale_affine_expression(const AffineExpression &expression, int64_t factor)
  -> std::optional<AffineExpression>;

auto affine_expression_range(
  const AffineExpression &expression,
  const std::unordered_map<Value *, IntegerRange> &symbol_ranges
) -> std::optional<IntegerRange>;
auto affine_form_range(
  int64_t coefficient, int64_t offset, const IntegerRange &range
) -> std::optional<IntegerRange>;
auto affine_range_fits_i32(const IntegerRange &range) -> bool;

} // namespace exodus::mid_ir
