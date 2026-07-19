#pragma once

#include "../base/rewriter.hpp"
#include "ir.hpp"
#include <unordered_set>

namespace exodus::mid_ir {

struct MidIRRewriter : ir::RewriterBase<Op> {
  auto set_scope(LinearFunction &func) -> void;
  auto replace_all_uses_with(Value *old_val, Value *new_val) -> void;
  auto eraseOp(Op *op) -> void;
  auto finalize(LinearFunction &func) -> void;

private:
  std::unordered_set<Op *> scope;
};

inline auto MidIRRewriter::set_scope(LinearFunction &func) -> void {
  scope.clear();
  for (auto &block : func.blocks) {
    for (auto *op : block->insts) {
      scope.insert(op);
    }
  }
}

inline auto MidIRRewriter::replace_all_uses_with(Value *old_val, Value *new_val)
  -> void {
  if (!old_val || !new_val || old_val == new_val)
    return;

  std::vector<OpBase *> users_copy(
    old_val->users.begin(), old_val->users.end()
  );

  for (auto *user_base : users_copy) {
    auto *user = dynamic_cast<Op *>(user_base);
    if (!user || (!scope.empty() && !scope.count(user))) {
      continue;
    }

    bool replaced = false;
    for (auto &operand : user->operands) {
      if (operand == old_val) {
        operand = new_val;
        new_val->addUse(user);
        replaced = true;
      }
    }
    if (user->code == OpCode::Phi) {
      auto &payload = std::get<PhiPayload>(user->payload);
      for (auto &[block, value] : payload.incoming) {
        (void)block;
        if (value == old_val) {
          value = new_val;
          new_val->addUse(user);
          replaced = true;
        }
      }
    }
    if (replaced) {
      old_val->rmUse(user);
    }
  }
}

inline auto MidIRRewriter::eraseOp(Op *op) -> void {
  if (!op)
    return;
  if (op->code == OpCode::Phi) {
    auto &payload = std::get<PhiPayload>(op->payload);
    for (auto &[block, value] : payload.incoming) {
      (void)block;
      if (value)
        value->rmUse(op);
    }
  }
  RewriterBase<Op>::eraseOp(op);
}

inline auto MidIRRewriter::finalize(LinearFunction &func) -> void {
  for (auto &block : func.blocks) {
    auto it = block->insts.begin();
    while (it != block->insts.end()) {
      if (to_erase.count(*it)) {
        it = block->insts.erase(it);
      } else {
        ++it;
      }
    }
  }
  to_erase.clear();
  scope.clear();
}

} // namespace exodus::mid_ir
