#pragma once

#include "../../mid/affine_loop.hpp"
#include <unordered_map>

namespace exodus::mid_ir::opt {

// All state below is owned by one LoopStrengthReduce::run invocation.  Keeping
// it outside the pass object makes the lifetime explicit and lets the helper
// components share the same definition-site map without retaining IR nodes
// between functions.
struct LSRContext {
  MidModule *module = nullptr;
  DomTree *dom = nullptr;
  AffineLoopInfo *affine_loops = nullptr;
  std::unordered_map<Op *, Block *> op_blocks;
};

auto build_lsr_op_block_map(LSRContext &context, LinearFunction &func) -> void;

} // namespace exodus::mid_ir::opt
