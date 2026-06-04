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
  void replaceAllUsesWith(Value *old_val, Value *new_val) {
    if (!old_val || !new_val || old_val == new_val)
      return;

    std::vector<Op *> users_copy(old_val->users.begin(), old_val->users.end());

    for (auto *user : users_copy) {
      for (auto &operand : user->operands) {
        if (operand == old_val) {
          operand = new_val;
          new_val->addUse(user);
        }
      }
    }
    old_val->users.clear();
  }

  void replaceOpWithRegion(Op *op, Region &r) {
    if (!op)
      return;

    for (auto &p : op->operands) {
      if (p) {
        p->rmUse(op);
      }
    }

    to_replace[op] = &r;
  }

  void replaceOp(Op *old_op, Value *new_val) {
    if (old_op->result) {
      replaceAllUsesWith(old_op->result, new_val);
    }
    eraseOp(old_op);
  }

  void eraseOp(Op *op) {
    if (!op)
      return;
    for (auto *operand : op->operands) {
      if (operand) {
        operand->rmUse(op);
      }
    }
    to_erase.insert(op);
  }

  void eraseRegion(Region &r) {
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

  void finalize(Region &r) {
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

  void finalize(Function &f) { finalize(f.body); }

  void clear() { to_erase.clear(); }

  bool empty() const { return to_erase.empty(); }
  size_t size() const { return to_erase.size(); }
};

} // namespace exodus::high_ir
