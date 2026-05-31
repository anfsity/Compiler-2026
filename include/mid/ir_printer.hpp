#pragma once

#include "../helper/ir_printer_base.hpp"
#include "flatten.hpp"
#include "ir.hpp"
#include <string>

namespace exodus::mid_ir {

struct LinearIRPrinter : public IRPrinterBase {
  auto dump(const MidModule &m) -> std::string {
    std::string res;
    for (auto *g : m.globals) {
      res += dump_global(*g);
    }
    for (auto &f : m.functions) {
      reset_context();
      res += dump(*f);
    }
    return res;
  }

  auto dump(const LinearFunction &f) -> std::string {
    std::string args_s;
    for (auto &arg : f.args) {
      args_s += arg->dump() + (&arg == &f.args.back() ? "" : ", ");
    }

    if (f.is_decl) {
      return fmt::format(
        "decl @{}({}) : {}\n", f.name, args_s, f.type->to_string()
      );
    }

    std::string body;
    idt++;
    for (auto &b : f.blocks) {
      body += dump(*b);
    }
    idt--;

    return fmt::format(
      "func @{}({}) : {} {{\n{}}}\n", f.name, args_s, f.type->to_string(), body
    );
  }

  auto dump(const Block &b) -> std::string {
    std::string res = fmt::format("{}^{}:\n", ident(), b.name);
    idt++;
    for (auto *op : b.insts) {
      res += ident() + dump(*op) + "\n";
    }
    idt--;
    return res;
  }

private:
  auto dump(const high_ir::Op &op) -> std::string {
    std::string line = dump_op_common(op);

    // Add Mid-IR specific control flow info
    if (op.code == high_ir::OpCode::Jump) {
      line += " ^" + op.successors[0]->name;
    } else if (op.code == high_ir::OpCode::Branch) {
      line += fmt::format(
        ", ^{}, ^{}", op.successors[0]->name, op.successors[1]->name
      );
    }
    return line;
  }
};

} // namespace exodus::mid_ir
