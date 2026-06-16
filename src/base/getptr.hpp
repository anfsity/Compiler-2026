#pragma once

#include "ir.hpp"
#include <memory>
#include <vector>

namespace exodus::ir {

struct GetPtrStep {
  enum class Kind : uint8_t {
    Index,
    ImplicitLoad,
  };

  Kind kind;
  size_t index_pos = 0;
  int scale = 0;
  std::shared_ptr<Type> from_type;
  std::shared_ptr<Type> to_type;
};

struct GetPtrPlan {
  std::vector<GetPtrStep> steps;
  bool reads_memory = false;
};

inline auto analyze_getptr(
  const std::shared_ptr<Type> &base_ptr_type,
  const std::shared_ptr<Type> &result_ptr_type,
  size_t index_count
) -> GetPtrPlan {
  GetPtrPlan plan;
  if (!base_ptr_type || !base_ptr_type->is_ptr()) {
    return plan;
  }

  auto cursor = std::static_pointer_cast<Ptr>(base_ptr_type)->target;
  auto result_target =
    result_ptr_type && result_ptr_type->is_ptr()
      ? std::static_pointer_cast<Ptr>(result_ptr_type)->target
      : nullptr;

  for (size_t i = 0; i < index_count; ++i) {
    auto from = cursor;
    int scale = from ? from->byte_size() : 4;

    if (cursor && cursor->is_array()) {
      cursor = std::static_pointer_cast<Array>(cursor)->base;
      scale = cursor->byte_size();

    } else if (cursor && cursor->is_ptr()) {
      scale = cursor->byte_size();
      auto target = std::static_pointer_cast<Ptr>(cursor)->target;
      if (result_target && target != result_target && i + 1 < index_count) {
        plan.steps.push_back({GetPtrStep::Kind::Index, i, scale, from, cursor});
        plan.steps.push_back(
          {GetPtrStep::Kind::ImplicitLoad, i, 0, cursor, target}
        );
        plan.reads_memory = true;
        cursor = target;
        continue;
      }
      cursor = target;
    }

    plan.steps.push_back({GetPtrStep::Kind::Index, i, scale, from, cursor});
  }

  return plan;
}

} // namespace exodus::ir
