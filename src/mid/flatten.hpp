#pragma once

#include "../high/ir.hpp"
#include "ir.hpp"
#include <cassert>
#include <vector>

namespace exodus::mid_ir {

class CFGEditor;

struct Flattener {
  Flattener(high_ir::Module *m) : old_module(m) {}
  auto flatten() -> std::unique_ptr<MidModule>;

private:
  high_ir::Module *old_module;
  MidModule *new_module = nullptr;
  LinearFunction *cur_func = nullptr;
  Block *cur_block = nullptr;
  CFGEditor *cur_cfg = nullptr;

  int b_cnt = 0;
  std::vector<std::pair<Block *, Block *>> loop_stk;

  auto visit(high_ir::Function *f) -> std::unique_ptr<LinearFunction>;
  auto visit(const high_ir::Region &region) -> void;
  auto visit(high_ir::Op *op) -> void;
  auto create_block(const std::string &name) -> Block *;
  auto build_cfg() -> void;
  auto convert_op(high_ir::Op *old_op) -> Op *;
};

} // namespace exodus::mid_ir
