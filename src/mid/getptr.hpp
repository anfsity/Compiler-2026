#pragma once

#include "../base/getptr.hpp"
#include "ir.hpp"

namespace exodus::mid_ir {

inline auto getptr_layout_type(const Op &op) -> std::shared_ptr<Type> {
  if (const auto *payload = std::get_if<GetPtrPayload>(&op.payload))
    return payload->layout_type;
  return nullptr;
}

inline auto analyze_getptr(const Op &op) -> ir::GetPtrPlan {
  if (
    op.code != OpCode::GetPtr || op.operands.empty() || !op.result ||
    !op.operands[0] || !op.operands[0]->type || !op.result->type
  ) {
    ir::GetPtrPlan invalid;
    invalid.valid = false;
    invalid.reads_memory = true;
    return invalid;
  }

  auto layout_type = getptr_layout_type(op);
  if (!layout_type) {
    return ir::analyze_getptr(
      op.operands[0]->type, op.result->type, op.operands.size() - 1
    );
  }
  return ir::analyze_getptr_with_layout(
    op.operands[0]->type, layout_type, op.result->type, op.operands.size() - 1
  );
}

inline auto default_getptr_payload(Value *base) -> GetPtrPayload {
  if (!base || !base->type || !base->type->is_ptr())
    return {};
  return {
    std::static_pointer_cast<Ptr>(base->type)->target,
  };
}

} // namespace exodus::mid_ir
