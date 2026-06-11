#pragma once

#include "../base/ir_printer_base.hpp"
#include "ir.hpp"
#include <string>

namespace exodus::high_ir {

struct IRPrinter : public IRPrinterBase {
  auto dump(const Module &m) -> std::string;
  auto dump(const Function &f) -> std::string;
  auto dump(const Region &r) -> std::string;
  auto dump_global(const GlobalVar &v) -> std::string;
  auto dump_op_common(const Op &op) -> std::string;
  auto dump(const Op &op) -> std::string;
};

} // namespace exodus::high_ir
