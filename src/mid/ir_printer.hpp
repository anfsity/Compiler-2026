#pragma once

#include "../base/ir_printer_base.hpp"
#include "flatten.hpp"
#include "ir.hpp"
#include <cassert>
#include <string>

namespace exodus::mid_ir {

struct LinearIRPrinter : public IRPrinterBase {
  auto dump(const MidModule &m) -> std::string;
  auto dump(const LinearFunction &f) -> std::string;
  auto dump(const Block &b) -> std::string;
  auto dump(const Op &op) -> std::string;
};

} // namespace exodus::mid_ir
