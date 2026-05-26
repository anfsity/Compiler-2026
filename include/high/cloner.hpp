#pragma once

#include "../helper/overload.hpp"
#include "ir.hpp"
#include <unordered_map>

namespace exodus::high_ir {

struct IRCloner {
  std::unordered_map<Value *, Value *> value_map;
  IRContext *ctx;

  IRCloner(IRContext *_ctx) : ctx(_ctx) {}

  Value *mapValue(Value *v) {
    if (value_map.count(v))
      return value_map[v];
    return v;
  }

  Op *cloneOp(Op *op) {
    Op *new_op = ctx->make_op(op->code);
    for (auto *operand : op->operands) {
      Value *mapped = mapValue(operand);
      new_op->operands.push_back(mapped);
      if (mapped)
        mapped->addUse(new_op);
    }
    if (op->result) {
      new_op->result = ctx->make_value<OpResult>(op->result->type, new_op);
      value_map[op->result] = new_op->result;
    }

    std::visit(
      overload{
        [&](const EmptyPayload &p) { new_op->payload = p; },
        [&](const CallPayload &p) { new_op->payload = p; },
        [&](const IfPayload &p) {
          auto then_r = std::make_unique<Region>(cloneRegion(*p.then_region));
          std::optional<Region> else_r;
          if (p.else_region) {
            else_r = cloneRegion(*p.else_region);
          }
          new_op->payload = IfPayload{std::move(then_r), std::move(else_r)};
        },
        [&](const WhilePayload &p) {
          auto cond_r = std::make_unique<Region>(cloneRegion(*p.cond_region));
          auto loop_r = std::make_unique<Region>(cloneRegion(*p.loop_region));
          new_op->payload = WhilePayload{std::move(cond_r), std::move(loop_r)};
        }
      },
      op->payload
    );

    return new_op;
  }

  Region cloneRegion(const Region &r) {
    Region new_r;
    for (auto *op : r) {
      new_r.push_back(cloneOp(op));
    }
    return new_r;
  }
};

} // namespace exodus::high_ir
