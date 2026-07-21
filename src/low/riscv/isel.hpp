#pragma once

#include "../../mid/ir.hpp"
#include "../ir.hpp"
#include "instr.hpp"
#include <memory>
#include <unordered_map>
#include <vector>

namespace exodus::riscv {

using InstSeq = std::vector<low_ir::MachineInst>;

struct LoweringContext {
  low_ir::MachineFunction *function = nullptr;
  low_ir::MachineBasicBlock *block = nullptr;

  std::unordered_map<ir::Value *, int> value_regs;
  std::unordered_map<ir::Value *, int> invariant_global_load_regs;
  std::unordered_map<ir::Value *, int> block_global_addr_regs;
  std::unordered_map<const mid_ir::Block *, low_ir::MachineBasicBlock *>
    block_map;
};

auto lower_function(const mid_ir::LinearFunction &f)
  -> std::unique_ptr<low_ir::MachineFunction>;

auto select_op(LoweringContext &ctx, const mid_ir::Op &op) -> InstSeq;

} // namespace exodus::riscv
