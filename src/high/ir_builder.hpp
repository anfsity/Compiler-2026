#pragma once

#include "../helper/log.hpp"
#include "../helper/overload.hpp"
#include "ast.hpp"
#include "ir.hpp"
#include "sym_tab.hpp"
#include <cassert>
#include <memory>
#include <variant>

namespace exodus::high_ir {

struct IRBuilder {
  IRBuilder(IRContext *_ctx) : ctx(_ctx) {}
  auto build(const ast::CompUnitAST &ast) -> std::unique_ptr<Module>;

private:
  SymTab symtab;
  IRContext *ctx = nullptr;
  Module *module = nullptr;
  Function *func = nullptr;
  Region *cur_region = nullptr;

  template <typename... Args>
  auto emit(OpCode c, const std::shared_ptr<Type> &rt, Args &&...args) -> Op *;

  template <typename... Args>
  auto emit_val(OpCode c, const std::shared_ptr<Type> &rt, Args &&...args)
    -> Value *;

  auto eval_gbinit(const ast::InitVal &init, const std::shared_ptr<Type> &type)
    -> InitVal;

  auto flatten_list(
    const ast::InitListAST &list,
    const std::shared_ptr<Type> &type,
    Value *base_ptr,
    int &idx
  ) -> void;

  auto flatten_gb_list(
    const ast::InitListAST &list,
    const std::shared_ptr<Type> &type,
    std::vector<InitVal> &res,
    int &idx
  ) -> void;

  auto eval_arith(ast::BinaryOp op, Constant::Data l, Constant::Data r)
    -> Constant::Data;
  auto eval_unary(ast::UnaryOp op, Constant::Data v) -> Constant::Data;

  // clang-format off
  auto visit(const ast::GlobalItem &ast_item) -> void;
  auto visit(const ast::FuncDefAST &ast_func) -> void;
  auto visit(const ast::VarDeclAST &ast_decl) -> void; //< only for local variable
  auto visit(const ast::Stmt &ast_stmt) -> void;
  auto visit(const ast::Expr &ast_expr) -> Value *;
  auto visit(const ast::LvalAST &ast_lval) -> Value *;
  // clang-format on

  auto coerce(Value *v, const std::shared_ptr<Type> &target) -> Value * {
    if (!v || v->type == target)
      return v;
    if (target->is_f32() && v->type->is_i32())
      return emit_val(OpCode::I2F, Float::get(), v);
    if (target->is_i32() && v->type->is_f32())
      return emit_val(OpCode::F2I, I32::get(), v);
    if (target->is_bool()) {
      Value *zero = ctx->make_zero(v->type);
      return emit_val(OpCode::Ne, Bool::get(), v, zero);
    }
    if (target->is_i32() && v->type->is_bool())
      return emit_val(OpCode::ZExt, I32::get(), v);
    if (target->is_f32() && v->type->is_bool()) {
      Value *i = emit_val(OpCode::ZExt, I32::get(), v);
      return emit_val(OpCode::I2F, Float::get(), i);
    }
    return v;
  }
};

} // namespace exodus::high_ir
