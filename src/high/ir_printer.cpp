#include "ir_printer.hpp"

namespace exodus::high_ir {

auto IRPrinter::dump(const Module &m) -> std::string {
  std::string res;
  for (auto &g : m.globals) {
    res += dump_global(*g);
  }
  for (auto &f : m.functions) {
    reset_context();
    res += dump(*f);
  }
  return res;
}

auto IRPrinter::dump(const Function &f) -> std::string {
  std::string args_s;
  for (auto &arg : f.args) {
    args_s += arg->dump() + (&arg == &f.args.back() ? "" : ", ");
  }

  if (f.is_decl) {
    return fmt::format(
      "decl @{}({}) : {}\n", f.name, args_s, f.type->to_string()
    );
  }

  return fmt::format(
    "func @{}({}) : {} {{\n{}}}\n",
    f.name,
    args_s,
    f.type->to_string(),
    dump(f.body)
  );
}

auto IRPrinter::dump(const Region &r) -> std::string {
  std::string res;
  idt++;
  for (auto &op : r) {
    res += dump(*op);
  }
  idt--;
  return res;
}

auto IRPrinter::dump_global(const GlobalVar &v) -> std::string {
  return IRPrinterBase::dump_global(v.addr, v.init, v.type);
}

auto opcode_to_str(OpCode oc) -> std::string {
  switch (oc) {
    // clang-format off
    case OpCode::Add: return "add";
    case OpCode::Sub: return "sub";
    case OpCode::Mul: return "mul";
    case OpCode::Div: return "div";
    case OpCode::Mod: return "mod";
    case OpCode::FAdd: return "fadd";
    case OpCode::FSub: return "fsub";
    case OpCode::FMul: return "fmul";
    case OpCode::FDiv: return "fdiv";
    case OpCode::I2F: return "i2f";
    case OpCode::F2I: return "f2i";
    case OpCode::ZExt: return "zext";
    case OpCode::Eq: return "eq";
    case OpCode::Ne: return "ne";
    case OpCode::Lt: return "lt";
    case OpCode::Gt: return "gt";
    case OpCode::Le: return "le";
    case OpCode::Ge: return "ge";
    case OpCode::And: return "and";
    case OpCode::Or: return "or";
    case OpCode::Xor: return "xor";
    case OpCode::Shl: return "shl";
    case OpCode::Shr: return "shr";
    case OpCode::Alloca: return "alloca";
    case OpCode::Load: return "load";
    case OpCode::Store: return "store";
    case OpCode::GetPtr: return "getptr";
    case OpCode::Call: return "call";
    case OpCode::Ret: return "ret";
    case OpCode::If: return "if";
    case OpCode::While: return "while";
    case OpCode::Break: return "break";
    case OpCode::Continue: return "continue";
    case OpCode::Jump: return "jump";
    case OpCode::Branch: return "branch";
    case OpCode::Condition: return "condition";
    case OpCode::Memset: return "memset";
    default:
      return "unknown";
    // clang-format on
  }
}

auto is_compare_opcode(OpCode oc) -> bool {
  return oc == OpCode::Eq || oc == OpCode::Ne || oc == OpCode::Lt ||
         oc == OpCode::Gt || oc == OpCode::Le || oc == OpCode::Ge;
}

auto is_cast_opcode(OpCode oc) -> bool {
  return oc == OpCode::I2F || oc == OpCode::F2I || oc == OpCode::ZExt;
}

auto IRPrinter::dump_op_common(const Op &op) -> std::string {
  std::string line;
  if (op.result && !op.result->type->is_void()) {
    line += get_value_name(op.result) + " = ";
  }

  if (op.code == OpCode::Call) {
    auto &cp = std::get<CallPayload>(op.payload);
    line += fmt::format(
      "call @{}({}) : ({})",
      cp.func_name,
      join_operands(op.operands),
      join_operand_types(op.operands)
    );
  } else {
    line += opcode_to_str(op.code);
    if (!op.operands.empty()) {
      if (op.code == OpCode::Condition) {
        line += "(" + join_operands(op.operands) + ")";
      } else {
        line += " " + join_operands(op.operands);
      }
    }

    if (op.code == OpCode::Alloca) {
      auto ptr_type = std::static_pointer_cast<Ptr>(op.result->type);
      line += " : " + ptr_type->target->to_string();
    } else if (op.code == OpCode::Load || op.code == OpCode::Store) {
      line += " : " + join_operand_types(op.operands);
    } else if (
      op.code == OpCode::GetPtr || is_compare_opcode(op.code) ||
      is_cast_opcode(op.code)
    ) {
      if (!op.operands.empty())
        line += " : " + op.operands[0]->type->to_string();
    }
  }

  if (op.result) {
    line += " -> " + op.result->type->to_string();
  }
  return line;
}

auto IRPrinter::dump(const Op &op) -> std::string {
  std::string prefix = ident();
  std::string line = dump_op_common(op);

  return std::visit(
    overload{
      [&](const IfPayload &ifp) -> std::string {
        std::string res = fmt::format(
          "{}{} {{\n{}{}}}", prefix, line, dump(*ifp.then_region), ident()
        );
        if (ifp.else_region.has_value()) {
          res +=
            fmt::format(" else {{\n{}{}}}", dump(*ifp.else_region), ident());
        }
        return res + "\n";
      },

      [&](const WhilePayload &whp) -> std::string {
        std::string res = fmt::format(
          "{}{} {{\n{}{}}} {{\n{}{}}}",
          prefix,
          line,
          dump(*whp.cond_region),
          ident(),
          dump(*whp.loop_region),
          ident()
        );
        return res + "\n";
      },

      [&](const auto &) -> std::string { return prefix + line + "\n"; }
    },
    op.payload
  );
}

} // namespace exodus::high_ir