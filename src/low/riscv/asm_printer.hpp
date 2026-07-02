#pragma once

#include "../../mid/ir.hpp"
#include "../ir.hpp"
#include <memory>
#include <string>
#include <vector>

namespace exodus::riscv {

struct AsmPrinter {
  auto to_string(
    const mid_ir::MidModule &module,
    const std::vector<std::unique_ptr<low_ir::MachineFunction>> &functions
  ) -> std::string;
  static auto to_string(const low_ir::MachineFunction &function) -> std::string;
};

} // namespace exodus::riscv
