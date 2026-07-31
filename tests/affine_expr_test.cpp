#ifdef EXODUS_UNIT_TEST
#include "../src/mid/affine_expr.hpp"
#include <cassert>
#include <cstdint>
#include <limits>
#include <unordered_map>

using namespace exodus;
using namespace exodus::mid_ir;
using namespace exodus::ir;

auto main() -> int {
  IRContext context;
  auto *x = context.make_value<Argument>(I32::get(), 0);
  auto *y = context.make_value<Argument>(I32::get(), 1);

  AffineExpression lhs{3, {{x, 2}}, false};
  AffineExpression rhs{-4, {{x, -1}, {y, 5}}, false};
  auto combined = combine_affine_expressions(lhs, rhs, 1);
  assert(combined);
  assert(combined->constant == -1);
  assert(combined->coefficient(x) == 1);
  assert(combined->coefficient(y) == 5);

  auto scaled = scale_affine_expression(*combined, -2);
  assert(scaled);
  assert(scaled->constant == 2);
  assert(scaled->coefficient(x) == -2);
  assert(scaled->coefficient(y) == -10);

  auto overflow = scale_affine_expression(
    AffineExpression{std::numeric_limits<int64_t>::max(), {}, false}, 2
  );
  assert(!overflow);

  std::unordered_map<Value *, IntegerRange> ranges{
    {x, IntegerRange{1, 4, true}},
    {y, IntegerRange{-2, 3, false}},
  };
  auto range = affine_expression_range(*combined, ranges);
  assert(range);
  assert(range->minimum == -10);
  assert(range->maximum == 18);
  assert(!range->exact);

  auto form_range = affine_form_range(-3, 10, {1, 4, true});
  assert(form_range);
  assert(form_range->minimum == -2);
  assert(form_range->maximum == 7);
  assert(affine_range_fits_i32(*form_range));

  return 0;
}
#endif
