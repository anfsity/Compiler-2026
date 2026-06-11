#pragma once

#include "../helper/overload.hpp"
#include "ir.hpp"
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

} // namespace exodus
