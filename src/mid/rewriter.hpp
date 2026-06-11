#pragma once

#include "../base/rewriter.hpp"
#include "ir.hpp"

namespace exodus::mid_ir {

struct MidIRRewriter : ir::RewriterBase<Op> {
  auto finalize(LinearFunction &func) -> void;
};

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
}

} // namespace exodus::mid_ir
