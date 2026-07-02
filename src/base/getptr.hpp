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

// getptrplan 可以说是为了 high ir getptr 设计打的补丁
// 我为了可读性，让 getptr 实际上承担了加载指针的功能
// 在 ir 阅读上不存在什么问题，但是底层一定要有一个 load pointer 的过程
// 不然语义就不正确，既然 getptr 承担了这么复杂的功能
// 我们就需要把他完成，plan 负责了拆解偏移和补填隐式 load 的功能。
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

  auto cursor = std::static_pointer_cast<Ptr>(base_ptr_type)->target;
  auto result_target =
    result_ptr_type && result_ptr_type->is_ptr()
      ? std::static_pointer_cast<Ptr>(result_ptr_type)->target
      : nullptr;

  auto emit_load = [&](size_t index_pos, const std::shared_ptr<Type> &from) {
    auto target = std::static_pointer_cast<Ptr>(from)->target;
    plan.steps.push_back(
      {GetPtrStep::Kind::ImplicitLoad, index_pos, 0, from, target}
    );
    plan.reads_memory = true;
    return from;
  };

  if (cursor && cursor->is_ptr()) {
    cursor = emit_load(0, cursor);
  }

  for (size_t i = 0; i < index_count; ++i) {
    auto from = cursor;
    int scale = from ? from->byte_size() : 4;

    if (cursor && cursor->is_array()) {
      cursor = std::static_pointer_cast<Array>(cursor)->base;
      scale = cursor ? cursor->byte_size() : 4;
      plan.steps.push_back({GetPtrStep::Kind::Index, i, scale, from, cursor});
      if (cursor && cursor->is_ptr()) {
        cursor = emit_load(i, cursor);
      }
      continue;

    } else if (cursor && cursor->is_ptr()) {
      auto target = std::static_pointer_cast<Ptr>(cursor)->target;
      scale = target ? target->byte_size() : 4;
      plan.steps.push_back({GetPtrStep::Kind::Index, i, scale, from, cursor});
      cursor = target;
      if (cursor && cursor->is_ptr()) {
        cursor = emit_load(i, cursor);
      }
      continue;
    }

    plan.steps.push_back({GetPtrStep::Kind::Index, i, scale, from, cursor});
  }

  return plan;
}

} // namespace exodus::ir
