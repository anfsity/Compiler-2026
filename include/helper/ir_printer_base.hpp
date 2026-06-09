#pragma once

#include "../base/ir.hpp"
#include "../helper/overload.hpp"
#include <string>
#include <unordered_map>
#include <vector>

namespace exodus {

struct IRPrinterBase {
  virtual ~IRPrinterBase() = default;

  IRPrinterBase() = default;
  IRPrinterBase(const IRPrinterBase &) = delete;
  IRPrinterBase &operator=(const IRPrinterBase &) = delete;
  IRPrinterBase(IRPrinterBase &&) = delete;
  IRPrinterBase &operator=(IRPrinterBase &&) = delete;

  auto get_value_name(const ir::Value *v) -> std::string;
  auto dump_global(
    const ir::GlobalAddr *addr,
    const ir::InitVal &init,
    const std::shared_ptr<Type> &type
  ) -> std::string;
  auto dump_init(const ir::InitVal &i) -> std::string;

  auto join_operands(const std::vector<ir::Value *> &ops) -> std::string;
  auto join_operand_types(const std::vector<ir::Value *> &ops) -> std::string;
  auto reset_context() -> void;

protected:
  auto ident() -> std::string;

  int id_cnt = 0;                                     // NOLINT
  int idt = 0;                                        // NOLINT
  std::unordered_map<const ir::Value *, int> val_ids; // NOLINT
};

inline auto IRPrinterBase::get_value_name(const ir::Value *v) -> std::string {
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

inline auto IRPrinterBase::dump_global(
  const ir::GlobalAddr *addr,
  const ir::InitVal &init,
  const std::shared_ptr<Type> &type
) -> std::string {
  return fmt::format(
    "{} = global {} : {}\n", addr->dump(), dump_init(init), type->to_string()
  );
}

inline auto IRPrinterBase::dump_init(const ir::InitVal &i) -> std::string {
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

inline auto IRPrinterBase::join_operands(const std::vector<ir::Value *> &ops)
  -> std::string {
  std::string res;
  for (size_t i = 0; i < ops.size(); ++i) {
    res += get_value_name(ops[i]);
    if (i + 1 < ops.size())
      res += ", ";
  }
  return res;
}

inline auto
IRPrinterBase::join_operand_types(const std::vector<ir::Value *> &ops)
  -> std::string {
  std::string res;
  for (size_t i = 0; i < ops.size(); ++i) {
    res += ops[i]->type->to_string();
    if (i + 1 < ops.size())
      res += ", ";
  }
  return res;
}

inline auto IRPrinterBase::reset_context() -> void {
  id_cnt = 0;
  val_ids.clear();
}

inline auto IRPrinterBase::ident() -> std::string {
  return std::string(static_cast<size_t>(idt) * 2, ' ');
}

} // namespace exodus
