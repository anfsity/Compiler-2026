#pragma once

#include "../base/rewriter.hpp"
#include "ir.hpp"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace exodus::high_ir {

struct IRRewriter : ir::RewriterBase<Op> {
  auto replaceOpWithRegion(Op *op, Region &r) -> void;
  auto replaceOp(Op *old_op, Value *new_val) -> void;
  auto eraseRegion(Region &r) -> void;

  auto finalize(Region &r) -> void;
  auto finalize(Function &f) -> void { finalize(f.body); }

  auto clear() -> void {
    RewriterBase::clear();
    to_replace.clear();
  }

private:
  std::unordered_map<Op *, Region *> to_replace;
};

} // namespace exodus::high_ir
