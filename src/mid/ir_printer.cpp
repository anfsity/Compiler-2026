#include "ir_printer.hpp"

namespace exodus::mid_ir {

auto LinearIRPrinter::dump(const MidModule &m) -> std::string {
  std::string res;
  for (auto *g : m.globals) {
    res += IRPrinterBase::dump_global(g->addr, g->init, g->type);
  }
  for (auto &f : m.functions) {
    reset_context();
    res += dump(*f);
  }
  return res;
}

auto LinearIRPrinter::dump(const LinearFunction &f) -> std::string {
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

auto LinearIRPrinter::dump(const Block &b) -> std::string {
  std::string res = fmt::format("{}^{}:\n", ident(), b.name);
  idt++;
  for (auto *op : b.insts) {
    res += ident() + dump(*op) + "\n";
  }
  idt--;
  return res;
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
    case OpCode::Jump: return "jump";
    case OpCode::Branch: return "branch";
    case OpCode::Phi: return "phi";
    case OpCode::Memset: return "memset";
    default: return "unknown";
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

auto LinearIRPrinter::dump(const Op &op) -> std::string {
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

  } else if (op.code == OpCode::Phi) {
    auto &pp = std::get<PhiPayload>(op.payload);
    line += "phi ";
    for (size_t i = 0; i < pp.incoming.size(); ++i) {
      auto &[block, value] = pp.incoming[i];
      line += fmt::format("[ {}, ^{} ]", get_value_name(value), block->name);
      if (i + 1 < pp.incoming.size()) {
        line += ", ";
      }
    }

  } else {
    line += opcode_to_str(op.code);
    if (!op.operands.empty()) {
      line += " " + join_operands(op.operands);
    }

    // Add types for specific ops
    if (op.code == OpCode::Alloca) {
      auto ptr_type = std::static_pointer_cast<Ptr>(op.result->type);
      line += " : " + ptr_type->target->to_string();
    } else if (op.code == OpCode::Load || op.code == OpCode::Store) {
      line += " : " + join_operand_types(op.operands);
    } else if (
      op.code == OpCode::GetPtr || is_compare_opcode(op.code) ||
      is_cast_opcode(op.code)
    ) {
      if (!op.operands.empty()) {
        line += " : " + op.operands[0]->type->to_string();
        if (op.code == OpCode::GetPtr) {
          if (const auto *payload = std::get_if<GetPtrPayload>(&op.payload)) {
            if (payload->layout_type)
              line += " [layout " + payload->layout_type->to_string() + "]";
          }
        }
      }
    }
  }

  if (op.result) {
    line += " -> " + op.result->type->to_string();
  }

  // Add Mid-IR specific control flow info
  if (op.code == OpCode::Jump) {
    assert(!op.successors.empty());
    line += " ^" + op.successors[0]->name;

  } else if (op.code == OpCode::Branch) {
    assert(!op.successors.empty());
    line +=
      fmt::format(", ^{}, ^{}", op.successors[0]->name, op.successors[1]->name);
  }
  return line;
}

} // namespace exodus::mid_ir
