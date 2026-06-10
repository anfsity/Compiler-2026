#pragma once

#include "ir.hpp"
#include <unordered_set>
#include <vector>

namespace exodus::ir {

template <typename OpT>
struct RewriterBase {
  auto replaceAllUsesWith(Value *old_val, Value *new_val) -> void;

  auto eraseOp(OpT *op) -> void;

  auto clear() -> void { to_erase.clear(); }
  auto empty() const -> bool { return to_erase.empty(); }
  auto size() const -> size_t { return to_erase.size(); }

protected:
  std::unordered_set<OpT *> to_erase; // NOLINT
};

template <typename OpT>
inline auto
RewriterBase<OpT>::replaceAllUsesWith(Value *old_val, Value *new_val) -> void {
  if (!old_val || !new_val || old_val == new_val)
    return;

  std::vector<OpBase *> users_copy(
    old_val->users.begin(), old_val->users.end()
  );

  for (auto *user_base : users_copy) {
    auto *user = static_cast<OpT *>(user_base);
    for (auto &operand : user->operands) {
      if (operand == old_val) {
        operand = new_val;
        new_val->addUse(user);
      }
    }
  }
  old_val->users.clear();
}

template <typename OpT>
inline auto RewriterBase<OpT>::eraseOp(OpT *op) -> void {
  if (!op)
    return;
  for (auto *operand : op->operands) {
    if (operand) {
      operand->rmUse(op);
    }
  }
  to_erase.insert(op);
}

} // namespace exodus::ir
