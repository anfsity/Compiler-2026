#include "ir_builder.hpp"

namespace exodus::high_ir {

auto IRBuilder::build(const ast::CompUnitAST &ast) -> std::unique_ptr<Module> {
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
auto IRBuilder::visit(const ast::GlobalItem &ast_item) -> void {
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
            cv = ctx->make_const(type, g_var->init);
          }

          g_var->addr = ctx->make_value<GlobalAddr>(type->ptr_to(), def->name);
          symtab.push(
            def->name,
            {type, g_var->addr, cv, nullptr, d->is_const, d->is_tensor}
          );
          module->globals.push_back(std::move(g_var));
        }
      },
      [&](const std::unique_ptr<ast::FuncDefAST> &f) { visit(*f); }
    },
    ast_item
  );
}

// int a, b = 1, arr[2][3], x = c[1][2], y[2] = {1, 2};
auto IRBuilder::visit(const ast::VarDeclAST &ast_decl) -> void {
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
            if (
              ast_decl.is_tensor && target_type->is_array() &&
              tensor_assign(alloca_res, target_type, init_val)
            ) {
              return;
            }
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

    symtab.push(
      def->name,
      {type, alloca_res, cv, nullptr, ast_decl.is_const, ast_decl.is_tensor}
    );
  }
}

// int fun(int a, int b)
// func @fun(%a, %b) : (i32, i32) -> i32
auto IRBuilder::visit(const ast::FuncDefAST &ast_func) -> void {
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
  symtab.push(func->name, {func->type, nullptr, nullptr, func, false, false});
  symtab.enter_scope();

  for (size_t i = 0; i < ast_func.params.size(); ++i) {
    const auto &p = ast_func.params[i];
    auto param_type = params_type[i];
    auto sym_type = params_sym_type[i];

    auto *arg_val = ctx->make_value<Argument>(param_type, i);
    func->args.emplace_back(arg_val);

    Value *alloca_res = emit_val(OpCode::Alloca, param_type->ptr_to());
    emit(OpCode::Store, nullptr, arg_val, alloca_res);
    symtab.push(p->name, {sym_type, alloca_res, nullptr, nullptr, false, false});
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
auto IRBuilder::visit(const ast::Stmt &ast_stmt) -> void {
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
        auto lhs_sym = symtab.lookup(assign->lval->name);
        if (
          lhs_sym && lhs_sym->is_tensor && assign->lval->indices.empty() &&
          target_type->is_array() && tensor_assign(ptr, target_type, val)
        ) {
          return;
        }
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

auto IRBuilder::visit(const ast::Expr &ast_expr) -> Value * {
  return std::visit(
    overload{
      [&](const std::unique_ptr<ast::NumberAST> &n) -> Value * {
        auto type =
          std::holds_alternative<int>(n->val) ? I32::get() : Float::get();
        return ctx->make_const(type, n->val);
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
          if (sym_opt && sym_opt->is_tensor && lval->indices.empty()) {
            return ptr;
          }
          auto base = std::static_pointer_cast<Array>(tar_type)->base;
          auto zero = ctx->make_zero(I32::get());
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
          if (is_tensor_ptr(lhs)) {
            Log::log_error("tensor does not support logical operations");
            return ctx->make_zero(I32::get());
          }
          auto lhs_bool = coerce(lhs, Bool::get());
          emit(OpCode::Store, nullptr, lhs_bool, res_ptr);

          auto *bool_zero = ctx->make_zero(Bool::get());
          Value *if_cond =
            (bin->op == ast::BinaryOp::And
               ? lhs_bool
               : emit_val(OpCode::Eq, Bool::get(), lhs_bool, bool_zero));

          auto old_region = cur_region;
          auto then_region = std::make_unique<Region>();
          cur_region = then_region.get();

          Value *rhs = visit(bin->right);
          if (is_tensor_ptr(rhs)) {
            Log::log_error("tensor does not support logical operations");
            return ctx->make_zero(I32::get());
          }
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

        if (is_tensor_ptr(lhs) || is_tensor_ptr(rhs)) {
          if (cmp_map.count(bin->op)) {
            Log::log_error("tensor does not support comparison operations");
            return ctx->make_zero(Bool::get());
          }
          if (bin->op == ast::BinaryOp::MatMul) {
            return tensor_matmul(lhs, rhs);
          }
          if (op_map.count(bin->op)) {
            return tensor_elementwise(bin->op, lhs, rhs);
          }
          Log::log_error("unsupported tensor binary operation");
          return ctx->make_zero(I32::get());
        }

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

          return ctx->make_const(eval_type, res_val);
        }

        lhs = coerce(lhs, eval_type);
        rhs = coerce(rhs, eval_type);

        if (symtab.is_global()) {
          Log::log_error("Initializer element is not a compile-time constant");
          return ctx->make_zero(result_type);
        }

        bool is_f = eval_type->is_f32();

        if (op_map.count(bin->op)) {
          auto &[f, s] = op_map.at(bin->op);
          return emit_val(is_f ? s : f, eval_type, lhs, rhs);
        }

        if (cmp_map.count(bin->op)) {
          return emit_val(cmp_map.at(bin->op), Bool::get(), lhs, rhs);
        }

        if (bin->op == ast::BinaryOp::MatMul) {
          Log::log_error("operator '@' requires tensor operands");
          return ctx->make_zero(I32::get());
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

          return ctx->make_const(res_type, res_val);
        }

        if (cur_region == nullptr) {
          Log::log_error("Initializer element is not a compile-time constant");
          return ctx->make_zero(val->type);
        }

        switch (una->op) {
        case ast::UnaryOp::Neg: {
          val = coerce(val, val->type->is_bool() ? I32::get() : val->type);
          Value *zero = ctx->make_zero(val->type);
          OpCode code = val->type->is_f32() ? OpCode::FSub : OpCode::Sub;
          return emit_val(code, val->type, zero, val);
        }
        case ast::UnaryOp::Not: {
          Value *zero = ctx->make_zero(val->type);
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
          Value *zero = ctx->make_zero(I32::get());
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

        auto func_name = call->name;
        if (call->name == "starttime" || call->name == "stoptime") {
          func_name =
            call->name == "starttime" ? "_sysy_starttime" : "_sysy_stoptime";
          args.emplace_back(ctx->make_const(I32::get(), call->line));
        }

        Op *op = emit(OpCode::Call, func_type->ret_type, std::move(args));
        op->payload = CallPayload{func_name};
        return op->result;
      }
    },
    ast_expr
  );
}

auto IRBuilder::eval_gbinit(
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

auto IRBuilder::visit(const ast::LvalAST &ast_lval) -> Value * {
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

auto IRBuilder::flatten_list(
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
      indices.emplace_back(ctx->make_const(I32::get(), rem / stride));
      rem %= stride;
    }

    Value *ptr =
      emit_val(OpCode::GetPtr, scalar_type->ptr_to(), base_ptr, indices);
    emit(OpCode::Store, nullptr, val, ptr);
  };

  auto store_zero = [&](int flat_idx) {
    Value *zero = ctx->make_zero(scalar_type);
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
      indices.emplace_back(ctx->make_const(I32::get(), rem / stride));
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
      Value *count_val = ctx->make_const(I32::get(), count);
      Value *zero_val = ctx->make_zero(scalar_type);
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

auto IRBuilder::flatten_gb_list(
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

auto IRBuilder::is_tensor_ptr(const Value *v) const -> bool {
  if (!v || !v->type || !v->type->is_ptr()) {
    return false;
  }
  auto target = std::static_pointer_cast<Ptr>(v->type)->target;
  return target && target->is_array();
}

auto IRBuilder::tensor_dims_from_ptr(const Value *v) const -> std::vector<int> {
  std::vector<int> dims;
  if (!is_tensor_ptr(v)) {
    return dims;
  }
  auto t = std::static_pointer_cast<Ptr>(v->type)->target;
  while (t->is_array()) {
    auto arr_t = std::static_pointer_cast<Array>(t);
    dims.push_back(arr_t->len);
    t = arr_t->base;
  }
  return dims;
}

auto IRBuilder::tensor_scalar_from_ptr(const Value *v) const
  -> std::shared_ptr<Type> {
  if (!is_tensor_ptr(v)) {
    return nullptr;
  }
  auto t = std::static_pointer_cast<Ptr>(v->type)->target;
  while (t->is_array()) {
    t = std::static_pointer_cast<Array>(t)->base;
  }
  return t;
}

auto IRBuilder::tensor_numel_from_dims(const std::vector<int> &dims) const
  -> int {
  int total = 1;
  for (int d : dims) {
    total *= d;
  }
  return total;
}

auto IRBuilder::tensor_element_ptr(
  Value *base_ptr, const std::vector<int> &dims, int flat_idx
) -> Value * {
  if (dims.empty()) {
    return base_ptr;
  }

  std::vector<int> strides(dims.size(), 1);
  for (int i = static_cast<int>(dims.size()) - 2; i >= 0; --i) {
    strides[i] = strides[i + 1] * dims[i + 1];
  }

  std::vector<Value *> indices;
  int rem = flat_idx;
  for (size_t i = 0; i < dims.size(); ++i) {
    int idx = rem / strides[i];
    rem %= strides[i];
    indices.push_back(ctx->make_const(I32::get(), idx));
  }

  auto scalar_t = tensor_scalar_from_ptr(base_ptr);
  return emit_val(OpCode::GetPtr, scalar_t->ptr_to(), base_ptr, indices);
}

auto IRBuilder::tensor_elem_value(
  Value *v, const std::vector<int> &dims, int flat_idx
) -> Value * {
  if (!is_tensor_ptr(v)) {
    return v;
  }
  auto scalar_t = tensor_scalar_from_ptr(v);
  auto ptr = tensor_element_ptr(v, dims, flat_idx);
  return emit_val(OpCode::Load, scalar_t, ptr);
}

auto IRBuilder::tensor_assign(
  Value *dst_ptr, const std::shared_ptr<Type> &dst_type, Value *src
) -> bool {
  if (!dst_type->is_array()) {
    return false;
  }

  auto dst_dims = tensor_dims_from_ptr(dst_ptr);
  auto dst_scalar = tensor_scalar_from_ptr(dst_ptr);
  int numel = tensor_numel_from_dims(dst_dims);

  if (is_tensor_ptr(src)) {
    auto src_dims = tensor_dims_from_ptr(src);
    if (src_dims != dst_dims) {
      Log::log_error("tensor shape mismatch in assignment");
      return true;
    }
    for (int i = 0; i < numel; ++i) {
      Value *val = tensor_elem_value(src, src_dims, i);
      val = coerce(val, dst_scalar);
      Value *ptr = tensor_element_ptr(dst_ptr, dst_dims, i);
      emit(OpCode::Store, nullptr, val, ptr);
    }
    return true;
  }

  if (src && !src->type->is_i32() && !src->type->is_f32() && !src->type->is_bool()) {
    Log::log_error("cannot assign non-scalar value to tensor");
    return true;
  }

  Value *fill = coerce(src, dst_scalar);
  for (int i = 0; i < numel; ++i) {
    Value *ptr = tensor_element_ptr(dst_ptr, dst_dims, i);
    emit(OpCode::Store, nullptr, fill, ptr);
  }
  return true;
}

auto IRBuilder::tensor_elementwise(ast::BinaryOp op, Value *lhs, Value *rhs)
  -> Value * {
  bool lhs_tensor = is_tensor_ptr(lhs);
  bool rhs_tensor = is_tensor_ptr(rhs);
  auto lhs_dims = lhs_tensor ? tensor_dims_from_ptr(lhs) : std::vector<int>{};
  auto rhs_dims = rhs_tensor ? tensor_dims_from_ptr(rhs) : std::vector<int>{};
  auto out_dims = lhs_tensor ? lhs_dims : rhs_dims;

  if (lhs_tensor && rhs_tensor && lhs_dims != rhs_dims) {
    Log::log_error("tensor shape mismatch in elementwise operation");
  }

  auto lhs_scalar = lhs_tensor ? tensor_scalar_from_ptr(lhs) : lhs->type;
  auto rhs_scalar = rhs_tensor ? tensor_scalar_from_ptr(rhs) : rhs->type;
  auto out_scalar =
    (lhs_scalar->is_f32() || rhs_scalar->is_f32()) ? Float::get() : I32::get();

  if (op == ast::BinaryOp::Mod && !out_scalar->is_i32()) {
    Log::log_error("tensor modulo only supports integer operands");
  }

  auto out_type = out_scalar;
  for (auto it = out_dims.rbegin(); it != out_dims.rend(); ++it) {
    out_type = out_type->array_of(*it);
  }

  Value *out_ptr = emit_val(OpCode::Alloca, out_type->ptr_to());
  int numel = tensor_numel_from_dims(out_dims);

  for (int i = 0; i < numel; ++i) {
    Value *lv = tensor_elem_value(lhs, lhs_dims, i);
    Value *rv = tensor_elem_value(rhs, rhs_dims, i);
    lv = coerce(lv, out_scalar);
    rv = coerce(rv, out_scalar);

    Value *res = nullptr;
    if (op == ast::BinaryOp::Add) {
      res = emit_val(out_scalar->is_f32() ? OpCode::FAdd : OpCode::Add, out_scalar, lv, rv);
    } else if (op == ast::BinaryOp::Sub) {
      res = emit_val(out_scalar->is_f32() ? OpCode::FSub : OpCode::Sub, out_scalar, lv, rv);
    } else if (op == ast::BinaryOp::Mul) {
      res = emit_val(out_scalar->is_f32() ? OpCode::FMul : OpCode::Mul, out_scalar, lv, rv);
    } else if (op == ast::BinaryOp::Div) {
      res = emit_val(out_scalar->is_f32() ? OpCode::FDiv : OpCode::Div, out_scalar, lv, rv);
    } else if (op == ast::BinaryOp::Mod) {
      res = emit_val(OpCode::Mod, I32::get(), lv, rv);
    }

    Value *dst = tensor_element_ptr(out_ptr, out_dims, i);
    emit(OpCode::Store, nullptr, res, dst);
  }

  return out_ptr;
}

auto IRBuilder::tensor_matmul(Value *lhs, Value *rhs) -> Value * {
  if (!is_tensor_ptr(lhs) || !is_tensor_ptr(rhs)) {
    Log::log_error("operator '@' requires tensor operands");
    return ctx->make_zero(I32::get());
  }

  auto ld = tensor_dims_from_ptr(lhs);
  auto rd = tensor_dims_from_ptr(rhs);
  if (ld.size() != 2u || rd.size() != 2u || ld[1] != rd[0]) {
    Log::log_error("matrix multiplication shape mismatch");
    return ctx->make_zero(I32::get());
  }

  auto lhs_scalar = tensor_scalar_from_ptr(lhs);
  auto rhs_scalar = tensor_scalar_from_ptr(rhs);
  auto out_scalar =
    (lhs_scalar->is_f32() || rhs_scalar->is_f32()) ? Float::get() : I32::get();

  int m = ld[0], k = ld[1], n = rd[1];
  auto out_type = out_scalar->array_of(n)->array_of(m);
  Value *out_ptr = emit_val(OpCode::Alloca, out_type->ptr_to());

  for (int i = 0; i < m; ++i) {
    for (int j = 0; j < n; ++j) {
      Value *acc = ctx->make_zero(out_scalar);
      for (int t = 0; t < k; ++t) {
        int l_flat = i * k + t;
        int r_flat = t * n + j;
        Value *lv = tensor_elem_value(lhs, ld, l_flat);
        Value *rv = tensor_elem_value(rhs, rd, r_flat);
        lv = coerce(lv, out_scalar);
        rv = coerce(rv, out_scalar);
        Value *mul =
          emit_val(out_scalar->is_f32() ? OpCode::FMul : OpCode::Mul, out_scalar, lv, rv);
        acc = emit_val(out_scalar->is_f32() ? OpCode::FAdd : OpCode::Add, out_scalar, acc, mul);
      }
      int o_flat = i * n + j;
      Value *dst = tensor_element_ptr(out_ptr, {m, n}, o_flat);
      emit(OpCode::Store, nullptr, acc, dst);
    }
  }

  return out_ptr;
}

auto IRBuilder::eval_arith(ast::BinaryOp op, Constant::Data l, Constant::Data r)
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
    case ast::BinaryOp::MatMul:
      return 0;
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
    case ast::BinaryOp::MatMul:
      return 0.0f;
    default:
      return 0.0f;
    }
  }
  return 0;
}

auto IRBuilder::eval_unary(ast::UnaryOp op, Constant::Data v)
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
auto push_operand(Op *op, T &&val) -> void {
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
auto IRBuilder::emit(OpCode c, const std::shared_ptr<Type> &rt, Args &&...args)
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
auto IRBuilder::emit_val(
  OpCode c, const std::shared_ptr<Type> &rt, Args &&...args
) -> Value * {
  return emit(c, rt, std::forward<Args>(args)...)->result;
}

} // namespace exodus::high_ir
