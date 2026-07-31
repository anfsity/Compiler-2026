#include "loop_strength_reduce_context.hpp"

namespace exodus::mid_ir::opt {

auto build_lsr_op_block_map(LSRContext &context, LinearFunction &func) -> void {
  context.op_blocks.clear();
  for (auto &block : func.blocks) {
    for (auto *op : block->insts)
      context.op_blocks[op] = block.get();
  }
}

} // namespace exodus::mid_ir::opt
