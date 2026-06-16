#pragma once

#include "../../base/ir_printer_base.hpp"
#include "../../mid/ir.hpp"
#include "../ir.hpp"
#include <memory>
#include <string>
#include <vector>

namespace exodus::riscv {

struct MachinePrinter : public IRPrinterBase {
  auto to_string(
    const mid_ir::MidModule &module,
    const std::vector<std::unique_ptr<low_ir::MachineFunction>> &functions
  ) -> std::string;
  static auto to_string(const low_ir::MachineFunction &mf) -> std::string;
};

} // namespace exodus::riscv
