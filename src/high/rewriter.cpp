#include "rewriter.hpp"

namespace exodus::high_ir {

auto IRRewriter::replace_op_with_region(Op *op, Region &r) -> void {
  if (!op)
    return;

  for (auto &p : op->operands) {
    if (p) {
      p->rmUse(op);
    }
  }

  to_replace[op] = &r;
}

auto IRRewriter::replace_op(Op *old_op, Value *new_val) -> void {
  if (old_op->result) {
    replace_all_uses_with(old_op->result, new_val);
  }
  eraseOp(old_op);
}

auto IRRewriter::erase_region(Region &r) -> void {
  for (auto &op : r) {
    eraseOp(op);
    if (op->code == OpCode::If) {
      auto &p = std::get<IfPayload>(op->payload);
      erase_region(*p.then_region);
      if (p.else_region)
        erase_region(*p.else_region);
    } else if (op->code == OpCode::While) {
      auto &p = std::get<WhilePayload>(op->payload);
      erase_region(*p.cond_region);
      erase_region(*p.loop_region);
    }
  }
}

auto IRRewriter::finalize(Region &r) -> void {
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

} // namespace exodus::high_ir