#pragma once

#include "ir.hpp"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace exodus::high_ir {

class IRRewriter {
  std::unordered_set<Op *> to_erase;
  std::unordered_map<Op *, Region *> to_replace;

public:
  auto replaceAllUsesWith(Value *old_val, Value *new_val) -> void;
  auto replaceOpWithRegion(Op *op, Region &r) -> void;
  auto replaceOp(Op *old_op, Value *new_val) -> void;
  auto eraseOp(Op *op) -> void;
  auto eraseRegion(Region &r) -> void;
  auto finalize(Region &r) -> void;
  auto finalize(Function &f) -> void;
  auto clear() -> void;
  auto empty() const -> bool;
  auto size() const -> size_t;
};

inline auto IRRewriter::replaceAllUsesWith(Value *old_val, Value *new_val)
  -> void {
  if (!old_val || !new_val || old_val == new_val)
    return;

  std::vector<OpBase *> users_copy(
    old_val->users.begin(), old_val->users.end()
  );

  for (auto *user_base : users_copy) {
    auto *user = static_cast<Op *>(user_base);
    for (auto &operand : user->operands) {
      if (operand == old_val) {
        operand = new_val;
        new_val->addUse(user);
      }
    }
  }
  old_val->users.clear();
}

inline auto IRRewriter::replaceOpWithRegion(Op *op, Region &r) -> void {
  if (!op)
    return;

  for (auto &p : op->operands) {
    if (p) {
      p->rmUse(op);
    }
  }

  to_replace[op] = &r;
}

inline auto IRRewriter::replaceOp(Op *old_op, Value *new_val) -> void {
  if (old_op->result) {
    replaceAllUsesWith(old_op->result, new_val);
  }
  eraseOp(old_op);
}

inline auto IRRewriter::eraseOp(Op *op) -> void {
  if (!op)
    return;
  for (auto *operand : op->operands) {
    if (operand) {
      operand->rmUse(op);
    }
  }
  to_erase.insert(op);
}

inline auto IRRewriter::eraseRegion(Region &r) -> void {
  for (auto &op : r) {
    eraseOp(op);
    if (op->code == OpCode::If) {
      auto &p = std::get<IfPayload>(op->payload);
      eraseRegion(*p.then_region);
      if (p.else_region)
        eraseRegion(*p.else_region);
    } else if (op->code == OpCode::While) {
      auto &p = std::get<WhilePayload>(op->payload);
      eraseRegion(*p.cond_region);
      eraseRegion(*p.loop_region);
    }
  }
}

inline auto IRRewriter::finalize(Region &r) -> void {
  auto it = r.begin();

  while (it != r.end()) {
    Op *op = *it;
    if (to_erase.count(op)) {
      it = r.erase(it);

    } else if (to_replace.count(op)) {
      Region *src = to_replace[op];
      finalize(*src);
      r.splice(it, *src);
      it = r.erase(it);

    } else {

      if (op->code == OpCode::If) {
        auto &p = std::get<IfPayload>(op->payload);
        finalize(*p.then_region);
        if (p.else_region)
          finalize(*p.else_region);

      } else if (op->code == OpCode::While) {
        auto &p = std::get<WhilePayload>(op->payload);
        finalize(*p.cond_region);
        finalize(*p.loop_region);
      }
      ++it;
    }
  }
}

inline auto IRRewriter::finalize(Function &f) -> void { finalize(f.body); }

inline auto IRRewriter::clear() -> void { to_erase.clear(); }

inline auto IRRewriter::empty() const -> bool { return to_erase.empty(); }

inline auto IRRewriter::size() const -> size_t { return to_erase.size(); }

} // namespace exodus::high_ir
