#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>

namespace exodus::analysis {

struct IntegerRange {
  int64_t min = 0;
  int64_t max = 0;

  auto fits_i32() const -> bool {
    return min >= std::numeric_limits<int32_t>::min() &&
           max <= std::numeric_limits<int32_t>::max();
  }

  static auto add(const IntegerRange &lhs, const IntegerRange &rhs)
    -> std::optional<IntegerRange> {
    auto checked = [](int64_t a, int64_t b) -> std::optional<int64_t> {
      if (
        (b > 0 && a > std::numeric_limits<int64_t>::max() - b) ||
        (b < 0 && a < std::numeric_limits<int64_t>::min() - b)
      ) {
        return std::nullopt;
      }
      return a + b;
    };
    auto lower = checked(lhs.min, rhs.min);
    auto upper = checked(lhs.max, rhs.max);
    if (!lower || !upper)
      return std::nullopt;
    return IntegerRange{*lower, *upper};
  }

  static auto multiply(const IntegerRange &lhs, const IntegerRange &rhs)
    -> std::optional<IntegerRange> {
    auto checked = [](int64_t a, int64_t b) -> std::optional<int64_t> {
      int64_t product = 0;
      if (__builtin_mul_overflow(a, b, &product))
        return std::nullopt;
      return product;
    };

    std::optional<int64_t> products[4] = {
      checked(lhs.min, rhs.min),
      checked(lhs.min, rhs.max),
      checked(lhs.max, rhs.min),
      checked(lhs.max, rhs.max),
    };
    for (const auto &product : products) {
      if (!product)
        return std::nullopt;
    }
    int64_t lower = *products[0];
    int64_t upper = *products[0];
    for (size_t i = 1; i < 4; ++i) {
      lower = std::min(lower, *products[i]);
      upper = std::max(upper, *products[i]);
    }
    return IntegerRange{lower, upper};
  }
};

} // namespace exodus::analysis
