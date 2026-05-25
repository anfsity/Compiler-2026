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
      Value *zero =
        v->type->is_f32()
          ? static_cast<Value *>(ctx->make_value<Constant>(Float::get(), 0.0f))
          : static_cast<Value *>(ctx->make_value<Constant>(I32::get(), 0));
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

inline auto IRBuilder::build(const ast::CompUnitAST &ast)
  -> std::unique_ptr<Module> {
  auto _module = std::make_unique<Module>();
  module = _module.get();
  ctx = &_module->ctx;

  for (auto const &[name, sym] : symtab.scopes[0]) {
    if (sym.type->is_func() && !sym.func) {
      auto f_decl = std::make_unique<Function>();
      f_decl->name = name;
      f_decl->type = sym.type;
      f_decl->is_decl = true;
      module->functions.push_back(std::move(f_decl));
    }
  }

  for (const auto &item : ast.items) {
    visit(item);
  }
  return _module;
}

// TODO: 错误处理。。。我不想写这个东西。。。要不不写算了（
inline auto IRBuilder::visit(const ast::GlobalItem &ast_item) -> void {
  std::visit(
    overload{
      [&](const ast::Decl &d) {
        assert(symtab.is_global());

        for (auto &def : d->defs) {
          auto type = d->type;
          for (auto it = def->dims.rbegin(); it != def->dims.rend(); ++it) {
            auto *cv = static_cast<Constant *>(visit(*it));
            type = type->array_of(std::get<int>(cv->val));
          }

          auto g_var = std::make_unique<GlobalVar>();
          g_var->name = def->name;
          g_var->type = type;
          g_var->is_const = d->is_const;

          if (def->init) {
            g_var->init = eval_gbinit(*def->init, type);
          } else {
            g_var->init = {ZeroInit{}};
          }

          Constant *cv = nullptr;
          if (d->is_const && !type->is_array()) {
            if (std::holds_alternative<int>(g_var->init.data)) {
              cv = ctx->make_value<Constant>(
                type, std::get<int>(g_var->init.data)
              );
            } else if (std::holds_alternative<float>(g_var->init.data)) {
              cv = ctx->make_value<Constant>(
                type, std::get<float>(g_var->init.data)
              );
            }
          }

          g_var->addr = ctx->make_value<GlobalAddr>(type->ptr_to(), def->name);
          symtab.push(def->name, {type, g_var->addr, cv, nullptr, d->is_const});
          module->globals.push_back(std::move(g_var));
        }
      },
      [&](const std::unique_ptr<ast::FuncDefAST> &f) { visit(*f); }
    },
    ast_item
  );
}

// int a, b = 1, arr[2][3], x = c[1][2], y[2] = {1, 2};
inline auto IRBuilder::visit(const ast::VarDeclAST &ast_decl) -> void {
  for (auto &def : ast_decl.defs) {
    auto type = ast_decl.type;
    for (auto it = def->dims.rbegin(); it != def->dims.rend(); ++it) {
      auto *cv = static_cast<Constant *>(visit(*it));
      type = type->array_of(std::get<int>(cv->val));
    }

    Value *alloca_res = emit_val(OpCode::Alloca, type->ptr_to());

    Constant *cv = nullptr;
    if (def->init) {
      std::visit(
        overload{
          [&](const ast::Expr &expr) {
            // store 1, %ptr : i32, i32*
            auto init_val = visit(expr);
            auto target_type =
              std::static_pointer_cast<Ptr>(alloca_res->type)->target;
            init_val = coerce(init_val, target_type);
            emit(OpCode::Store, nullptr, init_val, alloca_res);
            if (ast_decl.is_const && init_val->kind == ValueKind::Constant) {
              cv = static_cast<Constant *>(init_val);
            }
          },
          // {1, 2} {1, 2, {3, 4}} 噩梦。。
          [&](const std::unique_ptr<ast::InitListAST> &list) {
            int idx = 0;
            flatten_list(*list, type, alloca_res, idx);
          }
        },
        *def->init
      );
    }

    symtab.push(def->name, {type, alloca_res, cv, nullptr, ast_decl.is_const});
  }
}

// int fun(int a, int b)
// func @fun(%a, %b) : (i32, i32) -> i32
inline auto IRBuilder::visit(const ast::FuncDefAST &ast_func) -> void {
  // ret_type, name, params, body
  auto f_ptr = std::make_unique<Function>();
  func = f_ptr.get();
  func->name = ast_func.name;

  std::vector<std::shared_ptr<Type>> params_type;
  std::vector<std::shared_ptr<Type>> params_sym_type;
  params_type.reserve(ast_func.params.size());
  params_sym_type.reserve(ast_func.params.size());

  for (auto &p : ast_func.params) {
    auto param_type = p->type;
    auto sym_type = p->type;
    bool is_array = !p->dims.empty();
    if (is_array) {
      auto arr_type = p->type;
      for (size_t i = p->dims.size(); i-- > 1;) {
        auto *cv = static_cast<Constant *>(visit(p->dims[i]));
        arr_type = arr_type->array_of(std::get<int>(cv->val));
      }
      sym_type = arr_type;
      param_type = arr_type->ptr_to();
    }
    params_type.emplace_back(param_type);
    params_sym_type.emplace_back(sym_type);
  }

  func->type = Func::get(ast_func.ret_type, params_type);

  cur_region = &func->body;
  // sysy 不存在 const 函数
  symtab.push(func->name, {func->type, nullptr, nullptr, func, false});
  symtab.enter_scope();

  for (size_t i = 0; i < ast_func.params.size(); ++i) {
    const auto &p = ast_func.params[i];
    auto param_type = params_type[i];
    auto sym_type = params_sym_type[i];

    auto *arg_val = ctx->make_value<Argument>(param_type, i);
    func->args.emplace_back(arg_val);

    Value *alloca_res = emit_val(OpCode::Alloca, param_type->ptr_to());
    emit(OpCode::Store, nullptr, arg_val, alloca_res);
    symtab.push(p->name, {sym_type, alloca_res, nullptr, nullptr, false});
  }

  // FIXME: 用 visit(stmt) 优化？
  for (auto &item : ast_func.body->items) {
    std::visit(
      overload{
        [&](const ast::Decl &d) { visit(*d); },
        [&](const ast::Stmt &s) { visit(s); }
      },
      item
    );
  }

  symtab.exit_scope();
  module->functions.push_back(std::move(f_ptr));
  func = nullptr;
  cur_region = nullptr;
}

// 大工程！这何尝不是一种枚举(
inline auto IRBuilder::visit(const ast::Stmt &ast_stmt) -> void {
  std::visit(
    overload{
      [&](const std::unique_ptr<ast::ReturnStmtAST> &ret) {
        Value *val = ret->expr ? visit(*ret->expr) : nullptr;
        if (val && func) {
          auto func_type = std::static_pointer_cast<Func>(func->type);
          val = coerce(val, func_type->ret_type);
        }
        emit(OpCode::Ret, nullptr, val);
      },

      [&](const std::unique_ptr<ast::AssignStmtAST> &assign) {
        Value *val = visit(assign->expr);
        Value *ptr = visit(*assign->lval);
        auto target_type = std::static_pointer_cast<Ptr>(ptr->type)->target;
        val = coerce(val, target_type);
        emit(OpCode::Store, nullptr, val, ptr);
      },

      [&](const std::unique_ptr<ast::ExprStmtAST> &expr_stmt) {
        if (expr_stmt->expr)
          visit(*expr_stmt->expr);
      },

      [&](const std::unique_ptr<ast::BlockSAST> &block) {
        symtab.enter_scope();

        for (auto &item : block->items) {
          std::visit(
            overload{
              [&](const ast::Decl &d) { visit(*d); },
              [&](const ast::Stmt &s) { visit(s); }
            },
            item
          );
        }

        symtab.exit_scope();
      },

      [&](const std::unique_ptr<ast::IfStmtAST> &if_ast) {
        Value *cond = visit(if_ast->cond);
        cond = coerce(cond, Bool::get());

        auto then_region = std::make_unique<Region>();
        Region *old_region = cur_region;
        cur_region = then_region.get();

        visit(if_ast->then_body);

        std::optional<Region> else_region;
        if (if_ast->else_body) {
          else_region.emplace();
          cur_region = &*else_region;
          visit(*if_ast->else_body);
        }

        cur_region = old_region;
        auto op = emit(OpCode::If, nullptr, cond);
        op->payload = IfPayload{std::move(then_region), std::move(else_region)};
      },

      [&](const std::unique_ptr<ast::WhileStmtAST> &wh_ast) {
        auto cond_region = std::make_unique<Region>();
        auto loop_region = std::make_unique<Region>();
        Region *old_region = cur_region;

        cur_region = cond_region.get();
        Value *cond = visit(wh_ast->cond);
        cond = coerce(cond, Bool::get());

        emit(OpCode::Condition, nullptr, cond);

        cur_region = loop_region.get();
        visit(wh_ast->body);

        cur_region = old_region;
        auto op = emit(OpCode::While, nullptr);
        op->payload =
          WhilePayload{std::move(cond_region), std::move(loop_region)};
      },

      [&](const std::unique_ptr<ast::BreakStmtAST> &) {
        emit(OpCode::Break, nullptr);
      },

      [&](const std::unique_ptr<ast::ContinueStmtAST> &) {
        emit(OpCode::Continue, nullptr);
      }

    },
    ast_stmt
  );
}

inline auto IRBuilder::visit(const ast::Expr &ast_expr) -> Value * {
  return std::visit(
    overload{
      [&](const std::unique_ptr<ast::NumberAST> &n) -> Value * {
        auto type =
          std::holds_alternative<int>(n->val) ? I32::get() : Float::get();
        return ctx->make_value<Constant>(type, n->val);
      },

      [&](const std::unique_ptr<ast::LvalAST> &lval) -> Value * {
        auto sym_opt = symtab.lookup(lval->name);
        if (
          sym_opt && sym_opt->is_const && !sym_opt->type->is_array() &&
          lval->indices.empty() && sym_opt->const_val
        ) {
          return sym_opt->const_val;
        }

        Value *ptr = visit(*lval);
        auto tar_type = std::static_pointer_cast<Ptr>(ptr->type)->target;

        if (tar_type->is_array()) {
          auto base = std::static_pointer_cast<Array>(tar_type)->base;
          auto zero = ctx->make_value<Constant>(I32::get(), 0);
          return emit_val(
            OpCode::GetPtr, base->ptr_to(), ptr, std::vector<Value *>{zero}
          );
        }
        return emit_val(OpCode::Load, tar_type, ptr);
      },

      [&](const std::unique_ptr<ast::BinaryExprAST> &bin) -> Value * {
        if (bin->op == ast::BinaryOp::And || bin->op == ast::BinaryOp::Or) {
          // A && B 或 A || B
          Value *res_ptr = emit_val(OpCode::Alloca, Bool::get()->ptr_to());
          Value *lhs = visit(bin->left);
          auto lhs_bool = coerce(lhs, Bool::get());
          emit(OpCode::Store, nullptr, lhs_bool, res_ptr);

          auto *bool_zero = ctx->make_value<Constant>(Bool::get(), 0);
          Value *if_cond =
            (bin->op == ast::BinaryOp::And
               ? lhs_bool
               : emit_val(OpCode::Eq, Bool::get(), lhs_bool, bool_zero));

          auto old_region = cur_region;
          auto then_region = std::make_unique<Region>();
          cur_region = then_region.get();

          Value *rhs = visit(bin->right);
          Value *rhs_bool = coerce(rhs, Bool::get());
          emit(OpCode::Store, nullptr, rhs_bool, res_ptr);

          cur_region = old_region;
          Op *if_op = emit(OpCode::If, nullptr, if_cond);
          if_op->payload = IfPayload{std::move(then_region), std::nullopt};

          Value *res = emit_val(OpCode::Load, Bool::get(), res_ptr);
          return emit_val(OpCode::ZExt, I32::get(), res);
        }

        Value *lhs = visit(bin->left);
        Value *rhs = visit(bin->right);

        static const std::map<ast::BinaryOp, std::pair<OpCode, OpCode>> op_map =
          {
            {ast::BinaryOp::Add, {OpCode::Add, OpCode::FAdd}},
            {ast::BinaryOp::Sub, {OpCode::Sub, OpCode::FSub}},
            {ast::BinaryOp::Mul, {OpCode::Mul, OpCode::FMul}},
            {ast::BinaryOp::Div, {OpCode::Div, OpCode::FDiv}},
            {ast::BinaryOp::Mod, {OpCode::Mod, OpCode::Mod}},
          };

        static const std::map<ast::BinaryOp, OpCode> cmp_map = {
          {ast::BinaryOp::Eq, OpCode::Eq},
          {ast::BinaryOp::Ne, OpCode::Ne},
          {ast::BinaryOp::Lt, OpCode::Lt},
          {ast::BinaryOp::Gt, OpCode::Gt},
          {ast::BinaryOp::Le, OpCode::Le},
          {ast::BinaryOp::Ge, OpCode::Ge},
        };

        std::shared_ptr<Type> eval_type;
        if (lhs->type->is_f32() || rhs->type->is_f32()) {
          eval_type = Float::get();
        } else {
          eval_type = I32::get();
        }

        assert(eval_type && "ERROR eval type");
        auto result_type = cmp_map.count(bin->op) ? Bool::get() : eval_type;

        if (
          lhs->kind == ValueKind::Constant && rhs->kind == ValueKind::Constant
        ) {
          auto *lc = static_cast<Constant *>(lhs);
          auto *rc = static_cast<Constant *>(rhs);
          auto res_val = eval_arith(bin->op, lc->val, rc->val);

          // The result type of evaluation might be float even if operands were
          // int (if promoted) or int if it was a comparison.
          auto eval_type =
            std::holds_alternative<float>(res_val) ? Float::get() : I32::get();
          if (cmp_map.count(bin->op))
            eval_type = Bool::get();

          return ctx->make_value<Constant>(eval_type, res_val);
        }

        lhs = coerce(lhs, eval_type);
        rhs = coerce(rhs, eval_type);

        if (symtab.is_global()) {
          Log::log_error("Initializer element is not a compile-time constant");
          return ctx->make_value<Constant>(result_type, 0);
        }

        bool is_f = eval_type->is_f32();

        if (op_map.count(bin->op)) {
          auto &[f, s] = op_map.at(bin->op);
          return emit_val(is_f ? s : f, eval_type, lhs, rhs);
        }

        if (cmp_map.count(bin->op)) {
          return emit_val(cmp_map.at(bin->op), Bool::get(), lhs, rhs);
        }

        return nullptr;
      },

      [&](const std::unique_ptr<ast::UnaryExprAST> &una) -> Value * {
        Value *val = visit(una->expr);

        if (val->kind == ValueKind::Constant) {
          auto *cv = static_cast<Constant *>(val);
          auto res_val = eval_unary(una->op, cv->val);
          auto res_type = val->type;

          // Not 仅出现在条件表达式内
          if (una->op == ast::UnaryOp::Not) {
            res_type = Bool::get();
          } else if (
            (una->op == ast::UnaryOp::Neg || una->op == ast::UnaryOp::Pos) &&
            val->type->is_bool()
          ) {
            res_type = I32::get();
          }

          return ctx->make_value<Constant>(res_type, res_val);
        }

        if (cur_region == nullptr) {
          Log::log_error("Initializer element is not a compile-time constant");
          return ctx->make_value<Constant>(val->type, 0);
        }

        switch (una->op) {
        case ast::UnaryOp::Neg: {
          val = coerce(val, val->type->is_bool() ? I32::get() : val->type);
          bool is_f = val->type->is_f32();
          Value *zero =
            is_f
              ? static_cast<Value *>(
                  ctx->make_value<Constant>(Float::get(), 0.0f)
                )
              : static_cast<Value *>(ctx->make_value<Constant>(I32::get(), 0));
          OpCode code = is_f ? OpCode::FSub : OpCode::Sub;
          return emit_val(code, val->type, zero, val);
        }
        case ast::UnaryOp::Not: {
          Value *zero = ctx->make_value<Constant>(val->type, 0);
          return emit_val(OpCode::Eq, Bool::get(), val, zero);
        }
        case ast::UnaryOp::Pos:
          if (val->type->is_bool()) {
            return emit_val(OpCode::ZExt, I32::get(), val);
          }
          return val;
        }
        return nullptr;
      },

      [&](const std::unique_ptr<ast::CallExprAST> &call) -> Value * {
        auto sym = symtab.lookup(call->name);
        auto func_type = std::static_pointer_cast<Func>(sym->type);

        std::vector<Value *> args;
        auto decay_array_arg =
          [&](Value *arg_val, const std::shared_ptr<Type> &target) -> Value * {
          if (
            !arg_val || !target || !arg_val->type->is_ptr() || !target->is_ptr()
          )
            return arg_val;
          auto src_ptr = std::static_pointer_cast<Ptr>(arg_val->type);
          auto dst_ptr = std::static_pointer_cast<Ptr>(target);
          auto src_arr = std::dynamic_pointer_cast<Array>(src_ptr->target);
          if (!src_arr || src_arr->base != dst_ptr->target) {
            return arg_val;
          }
          Value *zero = ctx->make_value<Constant>(I32::get(), 0);
          return emit_val(OpCode::GetPtr, target, arg_val, zero);
        };
        for (size_t i = 0; i < call->args.size(); ++i) {
          Value *arg_val = visit(call->args[i]);
          if (i < func_type->params.size()) {
            arg_val = decay_array_arg(arg_val, func_type->params[i]);
            arg_val = coerce(arg_val, func_type->params[i]);
          }
          args.emplace_back(arg_val);
        }

        Op *op = emit(OpCode::Call, func_type->ret_type, std::move(args));
        op->payload = CallPayload{call->name};
        return op->result;
      }
    },
    ast_expr
  );
}

inline auto IRBuilder::eval_gbinit(
  const ast::InitVal &init, const std::shared_ptr<Type> &type
) -> InitVal {
  return std::visit(
    overload{
      [&](const ast::Expr &expr) -> InitVal {
        // 这里进行常量求值，所以 ok desu~
        auto *v = static_cast<Constant *>(visit(expr));

        if (std::holds_alternative<int>(v->val)) {
          return {std::get<int>(v->val)};
        } else {
          return {std::get<float>(v->val)};
        }
      },

      // int a[2][2] = {1, 2, {1, 2}};
      [&](const std::unique_ptr<ast::InitListAST> &list) -> InitVal {
        if (list->values.empty()) {
          return {ZeroInit{}};
        }
        int tot_size =
          type->is_array() ? std::static_pointer_cast<Array>(type)->size() : 1;
        std::vector<InitVal> flattened(tot_size, InitVal{ZeroInit{}});
        int idx = 0;

        flatten_gb_list(*list, type, flattened, idx);

        if (!type->is_array() && flattened.size() == 1) {
          return flattened[0];
        }

        return {InitList{std::move(flattened)}};
      }
    },
    init
  );
}

inline auto IRBuilder::visit(const ast::LvalAST &ast_lval) -> Value * {
  auto sym = symtab.lookup(ast_lval.name);
  auto base_ptr = sym->val;
  // int [2][3]*

  if (ast_lval.indices.empty()) {
    return base_ptr;
  }

  std::vector<Value *> indices;
  // int a[2][3]
  auto cur_type = sym->type;

  for (auto &idx : ast_lval.indices) {
    indices.emplace_back(visit(idx));
    if (auto arr_t = std::dynamic_pointer_cast<Array>(cur_type)) {
      cur_type = arr_t->base;
    } else if (auto ptr_t = std::dynamic_pointer_cast<Ptr>(cur_type)) {
      cur_type = ptr_t->target;
    } else {
      // 怎么会失败呢？肯定是发生了错误。
    }
  }

  return emit_val(OpCode::GetPtr, cur_type->ptr_to(), base_ptr, indices);
}

inline auto IRBuilder::flatten_list(
  const ast::InitListAST &list,
  const std::shared_ptr<Type> &type,
  Value *base_ptr,
  int &idx
) -> void {
  if (idx < 0) {
    return;
  }
  auto get_size = [](const std::shared_ptr<Type> &t) -> int {
    return t->is_array() ? std::static_pointer_cast<Array>(t)->size() : 1;
  };

  auto get_scalar_type = [](std::shared_ptr<Type> t) {
    while (t->is_array()) {
      t = std::static_pointer_cast<Array>(t)->base;
    }
    return t;
  };

  auto root_type = std::static_pointer_cast<Ptr>(base_ptr->type)->target;
  auto scalar_type = get_scalar_type(root_type);

  std::vector<int> dims;
  {
    auto t = root_type;
    while (t->is_array()) {
      auto arr_t = std::static_pointer_cast<Array>(t);
      dims.push_back(arr_t->len);
      t = arr_t->base;
    }
  }

  std::vector<int> strides(dims.size(), 1);
  for (int i = static_cast<int>(dims.size()) - 2; i >= 0; --i) {
    strides[i] = strides[i + 1] * dims[i + 1];
  }

  // 摊平维度，举个例子，如果我们需要在 int[2][3]
  // 中保存第四个元素，那么我们需要 得到偏移 (1, 1) 对应着 第 1 个 int[3]
  // 中的第一个元素，也就是 int[1][1]
  auto store_flat = [&](int flat_idx, Value *val) {
    if (dims.empty()) {
      emit(OpCode::Store, nullptr, val, base_ptr);
      return;
    }

    std::vector<Value *> indices;
    int rem = flat_idx;
    for (int stride : strides) {
      indices.emplace_back(ctx->make_value<Constant>(I32::get(), rem / stride));
      rem %= stride;
    }

    Value *ptr =
      emit_val(OpCode::GetPtr, scalar_type->ptr_to(), base_ptr, indices);
    emit(OpCode::Store, nullptr, val, ptr);
  };

  auto store_zero = [&](int flat_idx) {
    Value *zero =
      scalar_type->is_f32()
        ? static_cast<Value *>(ctx->make_value<Constant>(scalar_type, 0.0f))
        : static_cast<Value *>(ctx->make_value<Constant>(scalar_type, 0));
    store_flat(flat_idx, zero);
  };

  // int [2][2]
  auto arr_t = std::dynamic_pointer_cast<Array>(type);
  // int[2]
  auto sub_type = arr_t ? arr_t->base : type;
  int start_idx = idx;
  int end_idx = start_idx + get_size(type);

  for (auto &item : list.values) {

    if (idx >= end_idx) {
      Log::log_error(
        "[{}:{}] {}",
        list.line,
        list.col,
        type->is_array() ? "too many initializers for array"
                         : "excess elements in scalar initializer"
      );
      idx = -1;
      return;
    }

    std::visit(
      overload{
        [&](const ast::Expr &expr) {
          // 我们的目的构造一个扁平的数组。在我的构思中，high ir
          // 保留一个扁平的初始化 list 就足够了。在全局初始化中，value
          // 必须是常量 但是在局部数组初始化，value
          // 可以是左值，我们得把左值的实际值提取出来。 这里的处理就比较麻烦。
          Value *val = visit(expr);
          val = coerce(val, scalar_type);
          store_flat(idx, val);
          idx++;
        },

        [&](const std::unique_ptr<ast::InitListAST> &sub_list) {
          auto target_type = sub_type;
          while (target_type->is_array() && idx % get_size(target_type) != 0) {
            target_type = std::static_pointer_cast<Array>(target_type)->base;
          }

          flatten_list(*sub_list, target_type, base_ptr, idx);
          if (idx < 0) {
            return;
          }
        }
      },
      item
    );
  }

  auto get_flat_ptr = [&](int flat_idx) -> Value * {
    if (dims.empty())
      return base_ptr;
    std::vector<Value *> indices;
    int rem = flat_idx;
    for (int stride : strides) {
      indices.emplace_back(ctx->make_value<Constant>(I32::get(), rem / stride));
      rem %= stride;
    }
    return emit_val(OpCode::GetPtr, scalar_type->ptr_to(), base_ptr, indices);
  };

  // 如果 idx 不能和子数组对齐，则说明之前的填充还有空余，我们应该补 0
  // 来消除这些空余。举个例子 int a[2][2] = {{1}, {2}} -> {{1, 0}, {2,
  // 0}}; 但是对于 int a[2][2] = {1, {1, 2}};
  // 这就是一个错误。也就是说，在进入这个 递归前，如果 idx
  // 没有对齐，就代表着出现了错误。
  // 为了处理这种复杂的情况，我们需要好好的设计返回值。
  if (idx >= 0 && idx < end_idx) {
    int count = end_idx - idx;
    if (count > 16) {
      Value *start_ptr = get_flat_ptr(idx);
      Value *count_val = ctx->make_value<Constant>(I32::get(), count);
      Value *zero_val =
        scalar_type->is_f32()
          ? static_cast<Value *>(ctx->make_value<Constant>(Float::get(), 0.0f))
          : static_cast<Value *>(ctx->make_value<Constant>(I32::get(), 0));
      emit(OpCode::Memset, nullptr, start_ptr, count_val, zero_val);
      idx = end_idx;
    } else {
      while (idx < end_idx) {
        store_zero(idx);
        idx++;
      }
    }
  }
}

inline auto IRBuilder::flatten_gb_list(
  const ast::InitListAST &list,
  const std::shared_ptr<Type> &type,
  std::vector<InitVal> &res,
  int &idx
) -> void {
  if (idx < 0) {
    return;
  }

  auto get_size = [](const std::shared_ptr<Type> &t) -> int {
    return t->is_array() ? std::static_pointer_cast<Array>(t)->size() : 1;
  };

  auto arr_t = std::dynamic_pointer_cast<Array>(type);
  auto sub_type = arr_t ? arr_t->base : type;
  int end_idx = idx + get_size(type);

  for (auto &item : list.values) {
    if (idx >= end_idx) {
      Log::log_error(
        "[{}:{}] {}",
        list.line,
        list.col,
        type->is_array() ? "too many initializers for array"
                         : "excess elements in scalar initializer"
      );
      idx = -1;
      return;
    }

    std::visit(
      overload{
        [&](const ast::Expr &expr) {
          auto *v = static_cast<Constant *>(visit(expr));
          if (std::holds_alternative<int>(v->val)) {
            res[idx++] = {std::get<int>(v->val)};
          } else {
            res[idx++] = {std::get<float>(v->val)};
          }
        },

        [&](const std::unique_ptr<ast::InitListAST> &sub_list) {
          auto target_type = sub_type;
          while (target_type->is_array() && idx % get_size(target_type) != 0) {
            target_type = std::static_pointer_cast<Array>(target_type)->base;
          }

          int sub_stride = get_size(target_type);
          int sub_idx = idx;
          flatten_gb_list(*sub_list, target_type, res, idx);
          if (idx >= 0) {
            idx = sub_idx + sub_stride;
          }
        }
      },
      item
    );
  }
}

inline auto
IRBuilder::eval_arith(ast::BinaryOp op, Constant::Data l, Constant::Data r)
  -> Constant::Data {
  if (std::holds_alternative<int>(l) && std::holds_alternative<int>(r)) {
    int v1 = std::get<int>(l), v2 = std::get<int>(r);
    switch (op) {
    case ast::BinaryOp::Add:
      return v1 + v2;
    case ast::BinaryOp::Sub:
      return v1 - v2;
    case ast::BinaryOp::Mul:
      return v1 * v2;
    case ast::BinaryOp::Div:
      return v2 != 0 ? v1 / v2 : 0;
    case ast::BinaryOp::Mod:
      return v2 != 0 ? v1 % v2 : 0;
    case ast::BinaryOp::Lt:
      return v1 < v2 ? 1 : 0;
    case ast::BinaryOp::Gt:
      return v1 > v2 ? 1 : 0;
    case ast::BinaryOp::Le:
      return v1 <= v2 ? 1 : 0;
    case ast::BinaryOp::Ge:
      return v1 >= v2 ? 1 : 0;
    case ast::BinaryOp::Eq:
      return v1 == v2 ? 1 : 0;
    case ast::BinaryOp::Ne:
      return v1 != v2 ? 1 : 0;
    case ast::BinaryOp::And:
      return (v1 != 0 && v2 != 0) ? 1 : 0;
    case ast::BinaryOp::Or:
      return (v1 != 0 || v2 != 0) ? 1 : 0;
    }
  } else {
    float v1 = std::holds_alternative<float>(l)
                 ? std::get<float>(l)
                 : static_cast<float>(std::get<int>(l));
    float v2 = std::holds_alternative<float>(r)
                 ? std::get<float>(r)
                 : static_cast<float>(std::get<int>(r));
    switch (op) {
    case ast::BinaryOp::Add:
      return v1 + v2;
    case ast::BinaryOp::Sub:
      return v1 - v2;
    case ast::BinaryOp::Mul:
      return v1 * v2;
    case ast::BinaryOp::Div:
      return v1 / v2;
    case ast::BinaryOp::Lt:
      return v1 < v2 ? 1 : 0;
    case ast::BinaryOp::Gt:
      return v1 > v2 ? 1 : 0;
    case ast::BinaryOp::Le:
      return v1 <= v2 ? 1 : 0;
    case ast::BinaryOp::Ge:
      return v1 >= v2 ? 1 : 0;
    case ast::BinaryOp::Eq:
      return v1 == v2 ? 1 : 0;
    case ast::BinaryOp::Ne:
      return v1 != v2 ? 1 : 0;
    case ast::BinaryOp::And:
      return (v1 != 0.0f && v2 != 0.0f) ? 1 : 0;
    case ast::BinaryOp::Or:
      return (v1 != 0.0f || v2 != 0.0f) ? 1 : 0;
    default:
      return 0.0f;
    }
  }
  return 0;
}

inline auto IRBuilder::eval_unary(ast::UnaryOp op, Constant::Data v)
  -> Constant::Data {
  if (std::holds_alternative<int>(v)) {
    int val = std::get<int>(v);
    switch (op) {
    case ast::UnaryOp::Pos:
      return val;
    case ast::UnaryOp::Neg:
      return -val;
    case ast::UnaryOp::Not:
      return val == 0 ? 1 : 0;
    }
  } else {
    float val = std::get<float>(v);
    switch (op) {
    case ast::UnaryOp::Pos:
      return val;
    case ast::UnaryOp::Neg:
      return -val;
    case ast::UnaryOp::Not:
      return val == 0.0f ? 1 : 0;
    }
  }
  return 0;
}

template <typename T>
inline auto push_operand(Op *op, T &&val) -> void {
  if constexpr (std::is_same_v<std::decay_t<T>, std::vector<Value *>>) {
    for (auto *v : val) {
      if (v) {
        op->operands.push_back(v);
        v->addUse(op);
      }
    }
  } else {
    if (val != nullptr) {
      op->operands.emplace_back(std::forward<T>(val));
      val->addUse(op);
    }
  }
}

template <typename... Args>
inline auto
IRBuilder::emit(OpCode c, const std::shared_ptr<Type> &rt, Args &&...args)
  -> Op * {
  Op *op = ctx->make_op(c);

  // what a pity! we cannot use template lambda due to c++ standard
  // limitations c++20... help help!
  (push_operand(op, std::forward<Args>(args)), ...);
  if (rt) {
    op->result = ctx->make_value<OpResult>(rt, op);
  }
  if (cur_region) {
    cur_region->push_back(op);
  }
  return op;
}

template <typename... Args>
inline auto
IRBuilder::emit_val(OpCode c, const std::shared_ptr<Type> &rt, Args &&...args)
  -> Value * {
  return emit(c, rt, std::forward<Args>(args)...)->result;
}

} // namespace exodus::high_ir
