#pragma once

#include "ir.hpp"
#include <algorithm>
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
  bool valid = true;
};

inline auto same_getptr_step(const GetPtrStep &lhs, const GetPtrStep &rhs)
  -> bool {
  return lhs.kind == rhs.kind && lhs.index_pos == rhs.index_pos &&
         lhs.scale == rhs.scale && lhs.from_type == rhs.from_type &&
         lhs.to_type == rhs.to_type;
}

inline auto
same_getptr_byte_offset_plan(const GetPtrPlan &lhs, const GetPtrPlan &rhs)
  -> bool {
  auto lhs_it = lhs.steps.begin();
  auto rhs_it = rhs.steps.begin();
  while (true) {
    lhs_it = std::find_if(lhs_it, lhs.steps.end(), [](const GetPtrStep &step) {
      return step.kind == GetPtrStep::Kind::Index;
    });
    rhs_it = std::find_if(rhs_it, rhs.steps.end(), [](const GetPtrStep &step) {
      return step.kind == GetPtrStep::Kind::Index;
    });
    if (lhs_it == lhs.steps.end() || rhs_it == rhs.steps.end())
      return lhs_it == lhs.steps.end() && rhs_it == rhs.steps.end();
    if (!same_getptr_step(*lhs_it, *rhs_it))
      return false;
    ++lhs_it;
    ++rhs_it;
  }
}

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

// Mid IR may carry the type cursor used before the first index independently
// from the physical base pointer.  This is the same distinction LLVM GEP makes
// between its opaque pointer operand and source element type.
inline auto analyze_getptr_with_layout(
  const std::shared_ptr<Type> &base_ptr_type,
  const std::shared_ptr<Type> &layout_cursor_type,
  const std::shared_ptr<Type> &result_ptr_type,
  size_t index_count
) -> GetPtrPlan {
  if (
    !base_ptr_type || !base_ptr_type->is_ptr() || !layout_cursor_type ||
    !result_ptr_type || !result_ptr_type->is_ptr()
  ) {
    GetPtrPlan invalid;
    invalid.valid = false;
    invalid.reads_memory = true;
    return invalid;
  }

  const auto physical_target =
    std::static_pointer_cast<Ptr>(base_ptr_type)->target;
  const bool direct = base_ptr_type == layout_cursor_type;
  const bool indirect = physical_target == layout_cursor_type;
  if (!direct && !indirect) {
    GetPtrPlan invalid;
    invalid.valid = false;
    invalid.reads_memory = true;
    return invalid;
  }

  GetPtrPlan plan;
  auto cursor = layout_cursor_type;

  auto emit_load = [&](size_t index_pos, const std::shared_ptr<Type> &from) {
    auto target = std::static_pointer_cast<Ptr>(from)->target;
    plan.steps.push_back(
      {GetPtrStep::Kind::ImplicitLoad, index_pos, 0, from, target}
    );
    plan.reads_memory = true;
  };

  if (indirect && cursor->is_ptr())
    emit_load(0, cursor);

  for (size_t i = 0; i < index_count; ++i) {
    auto from = cursor;
    int scale = from ? from->byte_size() : 4;

    if (cursor && cursor->is_array()) {
      cursor = std::static_pointer_cast<Array>(cursor)->base;
      scale = cursor ? cursor->byte_size() : 4;
      plan.steps.push_back({GetPtrStep::Kind::Index, i, scale, from, cursor});
      if (cursor && cursor->is_ptr())
        emit_load(i, cursor);
      continue;
    }

    if (cursor && cursor->is_ptr()) {
      auto target = std::static_pointer_cast<Ptr>(cursor)->target;
      scale = target ? target->byte_size() : 4;
      plan.steps.push_back({GetPtrStep::Kind::Index, i, scale, from, cursor});
      cursor = target;
      if (cursor && cursor->is_ptr())
        emit_load(i, cursor);
      continue;
    }

    plan.steps.push_back({GetPtrStep::Kind::Index, i, scale, from, cursor});
  }

  return plan;
}

} // namespace exodus::ir
