#include "affine_expr.hpp"

#include <algorithm>
#include <limits>

namespace exodus::mid_ir {

auto checked_affine_i64(__int128 value) -> std::optional<int64_t> {
  if (
    value < std::numeric_limits<int64_t>::min() ||
    value > std::numeric_limits<int64_t>::max()
  ) {
    return std::nullopt;
  }
  return static_cast<int64_t>(value);
}

auto combine_affine_expressions(
  const AffineExpression &lhs, const AffineExpression &rhs, int64_t rhs_sign
) -> std::optional<AffineExpression> {
  auto constant = checked_affine_i64(
    static_cast<__int128>(lhs.constant) +
    static_cast<__int128>(rhs_sign) * rhs.constant
  );
  if (!constant)
    return std::nullopt;

  AffineExpression result;
  result.constant = *constant;
  result.coefficients = lhs.coefficients;
  for (const auto &[symbol, coefficient] : rhs.coefficients) {
    auto current = result.coefficient(symbol);
    auto next = checked_affine_i64(
      static_cast<__int128>(current) +
      static_cast<__int128>(rhs_sign) * coefficient
    );
    if (!next)
      return std::nullopt;
    if (*next == 0)
      result.coefficients.erase(symbol);
    else
      result.coefficients[symbol] = *next;
  }
  return result;
}

auto scale_affine_expression(const AffineExpression &expression, int64_t factor)
  -> std::optional<AffineExpression> {
  auto constant =
    checked_affine_i64(static_cast<__int128>(expression.constant) * factor);
  if (!constant)
    return std::nullopt;

  AffineExpression result;
  result.constant = *constant;
  for (const auto &[symbol, coefficient] : expression.coefficients) {
    auto scaled =
      checked_affine_i64(static_cast<__int128>(coefficient) * factor);
    if (!scaled)
      return std::nullopt;
    if (*scaled != 0)
      result.coefficients[symbol] = *scaled;
  }
  return result;
}

auto affine_expression_range(
  const AffineExpression &expression,
  const std::unordered_map<Value *, IntegerRange> &symbol_ranges
) -> std::optional<IntegerRange> {
  __int128 minimum = expression.constant;
  __int128 maximum = expression.constant;
  bool exact = true;

  for (const auto &[symbol, coefficient] : expression.coefficients) {
    auto it = symbol_ranges.find(symbol);
    if (it == symbol_ranges.end())
      return std::nullopt;
    if (it->second.empty())
      return IntegerRange{0, -1, it->second.exact};

    exact &= it->second.exact;
    auto first = static_cast<__int128>(coefficient) * it->second.minimum;
    auto last = static_cast<__int128>(coefficient) * it->second.maximum;
    minimum += std::min(first, last);
    maximum += std::max(first, last);
  }

  auto checked_minimum = checked_affine_i64(minimum);
  auto checked_maximum = checked_affine_i64(maximum);
  if (!checked_minimum || !checked_maximum)
    return std::nullopt;
  return IntegerRange{*checked_minimum, *checked_maximum, exact};
}

auto affine_form_range(
  int64_t coefficient, int64_t offset, const IntegerRange &range
) -> std::optional<IntegerRange> {
  if (range.empty())
    return IntegerRange{0, -1, range.exact};

  auto first = checked_affine_i64(
    static_cast<__int128>(coefficient) * range.minimum + offset
  );
  auto last = checked_affine_i64(
    static_cast<__int128>(coefficient) * range.maximum + offset
  );
  if (!first || !last)
    return std::nullopt;
  return IntegerRange{
    std::min(*first, *last), std::max(*first, *last), range.exact
  };
}

auto affine_range_fits_i32(const IntegerRange &range) -> bool {
  return range.empty() ||
         (range.minimum >= std::numeric_limits<int32_t>::min() &&
          range.maximum <= std::numeric_limits<int32_t>::max());
}

} // namespace exodus::mid_ir
