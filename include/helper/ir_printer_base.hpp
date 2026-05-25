#pragma once

#include "../helper/overload.hpp"
#include "../high/ir.hpp"
#include <string>
#include <unordered_map>
#include <vector>

namespace exodus {

inline auto is_compare_opcode(high_ir::OpCode oc) -> bool {
  switch (oc) {
  case high_ir::OpCode::Eq:
  case high_ir::OpCode::Ne:
  case high_ir::OpCode::Lt:
  case high_ir::OpCode::Gt:
  case high_ir::OpCode::Le:
  case high_ir::OpCode::Ge:
    return true;
  default:
    return false;
  }
}

inline auto is_cast_opcode(high_ir::OpCode oc) -> bool {
  switch (oc) {
  case high_ir::OpCode::I2F:
  case high_ir::OpCode::F2I:
  case high_ir::OpCode::ZExt:
    return true;
  default:
    return false;
  }
}

struct IRPrinterBase {
  virtual ~IRPrinterBase() = default;

  IRPrinterBase() = default;
  IRPrinterBase(const IRPrinterBase &) = delete;
  IRPrinterBase &operator=(const IRPrinterBase &) = delete;
  IRPrinterBase(IRPrinterBase &&) = delete;
  IRPrinterBase &operator=(IRPrinterBase &&) = delete;

  auto get_value_name(const high_ir::Value *v) -> std::string {
    if (v->kind == high_ir::ValueKind::Constant) {
      return v->dump();
    }
    if (v->kind == high_ir::ValueKind::GlobalVar) {
      return v->dump(); // @name
    }
    if (v->kind == high_ir::ValueKind::Argument) {
      return v->dump(); // %arg_name
    }

    // For OpResult, we manage IDs locally in the printer
    if (val_ids.find(v) == val_ids.end()) {
      val_ids[v] = id_cnt++;
    }
    return "%" + std::to_string(val_ids[v]);
  }

  auto dump_global(const high_ir::GlobalVar &v) -> std::string {
    return fmt::format(
      "{} = global {} : {}\n",
      v.addr->dump(),
      dump_init(v.init),
      v.type->to_string()
    );
  }

  auto dump_init(const high_ir::InitVal &i) -> std::string {
    return std::visit(
      overload{
        [](int val) -> std::string { return std::to_string(val); },
        [](float f) -> std::string { return std::to_string(f) + "f"; },
        [](high_ir::ZeroInit) -> std::string {
          return std::string("zeroinit");
        },
        [&](const high_ir::InitList &list) -> std::string {
          std::string s = "{";
          for (auto &l : list.values) {
            s += dump_init(l) + (&l == &list.values.back() ? "" : ", ");
          }
          return s + "}";
        }
      },
      i.data
    );
  }

  auto opcode_to_str(high_ir::OpCode oc) -> std::string {
    switch (oc) {
      // clang-format off
    case high_ir::OpCode::Add: return "add";
    case high_ir::OpCode::Sub: return "sub";
    case high_ir::OpCode::Mul: return "mul";
    case high_ir::OpCode::Div: return "div";
    case high_ir::OpCode::Mod: return "mod";
    case high_ir::OpCode::FAdd: return "fadd";
    case high_ir::OpCode::FSub: return "fsub";
    case high_ir::OpCode::FMul: return "fmul";
    case high_ir::OpCode::FDiv: return "fdiv";
    case high_ir::OpCode::I2F: return "i2f";
    case high_ir::OpCode::F2I: return "f2i";
    case high_ir::OpCode::ZExt: return "zext";
    case high_ir::OpCode::Eq: return "eq";
    case high_ir::OpCode::Ne: return "ne";
    case high_ir::OpCode::Lt: return "lt";
    case high_ir::OpCode::Gt: return "gt";
    case high_ir::OpCode::Le: return "le";
    case high_ir::OpCode::Ge: return "ge";
    case high_ir::OpCode::And: return "and";
    case high_ir::OpCode::Or: return "or";
    case high_ir::OpCode::Xor: return "xor";
    case high_ir::OpCode::Shl: return "shl";
    case high_ir::OpCode::Shr: return "shr";
    case high_ir::OpCode::Alloca: return "alloca";
    case high_ir::OpCode::Load: return "load";
    case high_ir::OpCode::Store: return "store";
    case high_ir::OpCode::GetPtr: return "getptr";
    case high_ir::OpCode::Call: return "call";
    case high_ir::OpCode::Ret: return "ret";
    case high_ir::OpCode::If: return "if";
    case high_ir::OpCode::While: return "while";
    case high_ir::OpCode::Break: return "break";
    case high_ir::OpCode::Continue: return "continue";
    case high_ir::OpCode::Jump: return "jump";
    case high_ir::OpCode::Branch: return "branch";
    case high_ir::OpCode::Condition: return "condition";
    case high_ir::OpCode::Memset: return "memset";
    default: return "unknown";
      // clang-format on
    }
  }

  auto dump_op_common(const high_ir::Op &op) -> std::string {
    std::string line;
    if (op.result && !op.result->type->is_void()) {
      line += get_value_name(op.result) + " = ";
    }

    if (op.code == high_ir::OpCode::Call) {
      auto &cp = std::get<high_ir::CallPayload>(op.payload);
      line += fmt::format(
        "call @{}({}) : ({})",
        cp.func_name,
        join_operands(op.operands),
        join_operand_types(op.operands)
      );

    } else {
      line += opcode_to_str(op.code);
      if (!op.operands.empty()) {
        if (op.code == high_ir::OpCode::Condition) {
          line += "(" + join_operands(op.operands) + ")";
        } else {
          line += " " + join_operands(op.operands);
        }
      }

      // Add types for specific ops
      if (op.code == high_ir::OpCode::Alloca) {
        auto ptr_type = std::static_pointer_cast<Ptr>(op.result->type);
        line += " : " + ptr_type->target->to_string();

      } else if (
        op.code == high_ir::OpCode::Load || op.code == high_ir::OpCode::Store
      ) {
        line += " : " + join_operand_types(op.operands);

      } else if (
        op.code == high_ir::OpCode::GetPtr || is_compare_opcode(op.code) ||
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

  auto join_operands(const std::vector<high_ir::Value *> &ops) -> std::string {
    std::string res;
    for (size_t i = 0; i < ops.size(); ++i) {
      res += get_value_name(ops[i]);
      if (i + 1 < ops.size())
        res += ", ";
    }
    return res;
  }

  auto join_operand_types(const std::vector<high_ir::Value *> &ops)
    -> std::string {
    std::string res;
    for (size_t i = 0; i < ops.size(); ++i) {
      res += ops[i]->type->to_string();
      if (i + 1 < ops.size())
        res += ", ";
    }
    return res;
  }

  auto reset_context() -> void {
    id_cnt = 0;
    val_ids.clear();
  }

protected:
  auto ident() -> std::string {
    return std::string(static_cast<size_t>(idt) * 2, ' ');
  }

  int id_cnt = 0;                                          // NOLINT
  int idt = 0;                                             // NOLINT
  std::unordered_map<const high_ir::Value *, int> val_ids; // NOLINT
};

} // namespace exodus
