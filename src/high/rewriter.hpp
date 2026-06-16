#pragma once

#include "../base/rewriter.hpp"
#include "ir.hpp"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace exodus::high_ir {

struct IRRewriter : ir::RewriterBase<Op> {
  auto replace_op_with_region(Op *op, Region &r) -> void;
  auto replace_op(Op *old_op, Value *new_val) -> void;
  auto erase_region(Region &r) -> void;

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
