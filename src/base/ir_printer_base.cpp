#include "ir_printer_base.hpp"

namespace exodus {

auto IRPrinterBase::get_value_name(const ir::Value *v) -> std::string {
  if (v->kind == ir::ValueKind::Constant) {
    return v->dump();
  }
  if (v->kind == ir::ValueKind::GlobalVar) {
    return v->dump(); // @name
  }
  if (v->kind == ir::ValueKind::Argument) {
    return v->dump(); // %arg_name
  }

  // For OpResult, we manage IDs locally in the printer
  if (val_ids.find(v) == val_ids.end()) {
    val_ids[v] = id_cnt++;
  }
  return "%" + std::to_string(val_ids[v]);
}

auto IRPrinterBase::dump_global(
  const ir::GlobalAddr *addr,
  const ir::InitVal &init,
  const std::shared_ptr<Type> &type
) -> std::string {
  return fmt::format(
    "{} = global {} : {}\n", addr->dump(), dump_init(init), type->to_string()
  );
}

auto IRPrinterBase::dump_init(const ir::InitVal &i) -> std::string {
  return std::visit(
    overload{
      [](int val) -> std::string { return std::to_string(val); },
      [](float f) -> std::string { return std::to_string(f) + "f"; },
      [](ir::ZeroInit) -> std::string { return std::string("zeroinit"); },
      [&](const ir::InitList &list) -> std::string {
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

auto IRPrinterBase::join_operands(const std::vector<ir::Value *> &ops)
  -> std::string {
  std::string res;
  for (size_t i = 0; i < ops.size(); ++i) {
    res += get_value_name(ops[i]);
    if (i + 1 < ops.size())
      res += ", ";
  }
  return res;
}

auto IRPrinterBase::join_operand_types(const std::vector<ir::Value *> &ops)
  -> std::string {
  std::string res;
  for (size_t i = 0; i < ops.size(); ++i) {
    res += ops[i]->type->to_string();
    if (i + 1 < ops.size())
      res += ", ";
  }
  return res;
}

auto IRPrinterBase::reset_context() -> void {
  id_cnt = 0;
  val_ids.clear();
}

auto IRPrinterBase::ident() -> std::string {
  return std::string(static_cast<size_t>(idt) * 2, ' ');
}

} // namespace exodus