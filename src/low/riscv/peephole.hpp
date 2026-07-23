#pragma once

#include "../ir.hpp"

namespace exodus::riscv {

// Simplify the machine instruction stream before register allocation.
// Rules are deliberately local and only fire when the constant producer has
// one use, so replacing a multiply/divide does not duplicate shared values.
auto run_peephole(low_ir::MachineFunction &function) -> void;

} // namespace exodus::riscv
