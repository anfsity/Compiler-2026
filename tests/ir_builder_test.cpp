#include "../include/high/ast.hpp"
#include "../include/high/ast_base.hpp"
#include "../include/high/ir.hpp"
#include "../include/high/ir_builder.hpp"
#include "../include/high/ir_printer.hpp"
#include "../include/type.hpp"

#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace exodus;
using namespace exodus::high_ir;

namespace {

// --- AST Helpers ---

template <typename T, typename... Args>
auto make_node(Args &&...args) -> std::unique_ptr<T> {
  return std::make_unique<T>(std::forward<Args>(args)...);
}

auto num(int v) -> ast::Expr {
  auto n = make_node<ast::NumberAST>(v);
  n->eval_type = I32::get();
  return n;
}

auto num(float v) -> ast::Expr {
  auto n = make_node<ast::NumberAST>(v);
  n->eval_type = Float::get();
  return n;
}

auto lval(std::string name, std::vector<ast::Expr> indices = {}) -> ast::Expr {
  return make_node<ast::LvalAST>(std::move(name), std::move(indices));
}

auto binary(
  ast::BinaryOp op, ast::Expr lhs, ast::Expr rhs, std::shared_ptr<Type> type
) -> ast::Expr {
  auto b = make_node<ast::BinaryExprAST>(op, std::move(lhs), std::move(rhs));
  b->eval_type = std::move(type);
  return b;
}

auto unary(ast::UnaryOp op, ast::Expr expr) -> ast::Expr {
  return make_node<ast::UnaryExprAST>(op, std::move(expr));
}

auto init_list(std::vector<ast::InitVal> vals) -> ast::InitVal {
  return make_node<ast::InitListAST>(std::move(vals));
}

auto var_def(
  std::string name,
  std::vector<ast::Expr> dims = {},
  std::optional<ast::InitVal> init = std::nullopt
) -> std::unique_ptr<ast::VarDefAST> {
  return make_node<ast::VarDefAST>(
    std::move(name), std::move(dims), std::move(init)
  );
}

auto var_decl(
  std::shared_ptr<Type> type,
  std::vector<std::unique_ptr<ast::VarDefAST>> defs,
  bool is_const = false
) -> ast::Decl {
  return make_node<ast::VarDeclAST>(std::move(type), std::move(defs), is_const);
}

// --- Test Cases ---

auto test_array_and_folding() -> std::string {
  ast::CompUnitAST unit;

  // 1. Global const: const int N = 1 + 2; const float PI = 3.14f;
  {
    std::vector<std::unique_ptr<ast::VarDefAST>> defs;
    defs.emplace_back(
      var_def("N", {}, binary(ast::BinaryOp::Add, num(1), num(2), I32::get()))
    );
    unit.items.emplace_back(var_decl(I32::get(), std::move(defs), true));

    std::vector<std::unique_ptr<ast::VarDefAST>> fdefs;
    fdefs.emplace_back(var_def("PI", {}, num(3.14f)));
    unit.items.emplace_back(var_decl(Float::get(), std::move(fdefs), true));
  }

  // 2. Global array: int arr[2][2] = {{1}, {N + 1}};
  {
    std::vector<std::unique_ptr<ast::VarDefAST>> defs;

    std::vector<ast::InitVal> inner1;
    inner1.emplace_back(num(1));

    std::vector<ast::InitVal> inner2;
    inner2.emplace_back(
      binary(ast::BinaryOp::Add, lval("N"), num(1), I32::get())
    );

    std::vector<ast::InitVal> outer;
    outer.emplace_back(init_list(std::move(inner1)));
    outer.emplace_back(init_list(std::move(inner2)));

    std::vector<ast::Expr> dims;
    dims.emplace_back(num(2));
    dims.emplace_back(num(2));

    defs.emplace_back(
      var_def("arr", std::move(dims), init_list(std::move(outer)))
    );
    unit.items.emplace_back(var_decl(I32::get(), std::move(defs), false));
  }

  // 3. Function with local array and folding
  {
    std::vector<ast::BlockItem> items;

    // const int M = 5;
    {
      std::vector<std::unique_ptr<ast::VarDefAST>> defs;
      defs.emplace_back(var_def("M", {}, num(5)));
      items.emplace_back(var_decl(I32::get(), std::move(defs), true));
    }

    // int b[2][2] = {{1}, {M}};
    {
      std::vector<std::unique_ptr<ast::VarDefAST>> defs;

      std::vector<ast::InitVal> inner1;
      inner1.emplace_back(num(1));

      std::vector<ast::InitVal> inner2;
      inner2.emplace_back(lval("M"));

      std::vector<ast::InitVal> outer;
      outer.emplace_back(init_list(std::move(inner1)));
      outer.emplace_back(init_list(std::move(inner2)));

      std::vector<ast::Expr> dims;
      dims.emplace_back(num(2));
      dims.emplace_back(num(2));

      defs.emplace_back(
        var_def("b", std::move(dims), init_list(std::move(outer)))
      );
      items.emplace_back(var_decl(I32::get(), std::move(defs), false));
    }

    // return b[0][0] + M + (-(-M));
    {
      std::vector<ast::Expr> indices;
      indices.emplace_back(num(0));
      indices.emplace_back(num(0));
      auto b_0_0 = lval("b", std::move(indices));

      // Use unary folding: -(-M) -> 5
      auto nested_neg =
        unary(ast::UnaryOp::Neg, unary(ast::UnaryOp::Neg, lval("M")));

      auto sum1 =
        binary(ast::BinaryOp::Add, std::move(b_0_0), lval("M"), I32::get());
      auto ret_expr = binary(
        ast::BinaryOp::Add, std::move(sum1), std::move(nested_neg), I32::get()
      );
      items.emplace_back(make_node<ast::ReturnStmtAST>(std::move(ret_expr)));
    }

    auto body = make_node<ast::BlockSAST>(std::move(items));
    auto main_func = make_node<ast::FuncDefAST>(
      I32::get(),
      "main",
      std::vector<std::unique_ptr<ast::FuncParamAST>>{},
      std::move(body)
    );
    unit.items.emplace_back(std::move(main_func));
  }

  IRContext ctx;
  IRBuilder builder(&ctx);
  auto module = builder.build(unit);

  IRPrinter printer;
  return printer.dump(*module);
}

} // namespace

#ifdef EXODUS_UNIT_TEST
int main() {
  const std::string actual = test_array_and_folding();

  const std::string expected =
    "@N = global 3 : i32\n"
    "@PI = global 3.140000f : f32\n"
    "@arr = global {1, zeroinit, 4, zeroinit} : i32[2][2]\n"
    "decl @_sysy_starttime() : (i32) -> void\n"
    "decl @_sysy_stoptime() : (i32) -> void\n"
    "decl @getarray() : (i32*) -> i32\n"
    "decl @getch() : () -> i32\n"
    "decl @getfarray() : (f32*) -> i32\n"
    "decl @getfloat() : () -> f32\n"
    "decl @getint() : () -> i32\n"
    "decl @putarray() : (i32, i32*) -> void\n"
    "decl @putch() : (i32) -> void\n"
    "decl @putfarray() : (i32, f32*) -> void\n"
    "decl @putfloat() : (f32) -> void\n"
    "decl @putint() : (i32) -> void\n"
    "decl @starttime() : () -> void\n"
    "decl @stoptime() : () -> void\n"
    "func @main() : () -> i32 {\n"
    "  %0 = alloca : i32 -> i32*\n"
    "  store 5, %0 : i32, i32*\n"
    "  %1 = alloca : i32[2][2] -> i32[2][2]*\n"
    "  %2 = getptr %1, 0, 0 : i32[2][2]* -> i32*\n"
    "  store 1, %2 : i32, i32*\n"
    "  %3 = getptr %1, 0, 1 : i32[2][2]* -> i32*\n"
    "  store 0, %3 : i32, i32*\n"
    "  %4 = getptr %1, 1, 0 : i32[2][2]* -> i32*\n"
    "  store 5, %4 : i32, i32*\n"
    "  %5 = getptr %1, 1, 1 : i32[2][2]* -> i32*\n"
    "  store 0, %5 : i32, i32*\n"
    "  %6 = getptr %1, 0, 0 : i32[2][2]* -> i32*\n"
    "  %7 = load %6 : i32* -> i32\n"
    "  %8 = add %7, 5 -> i32\n"
    "  %9 = add %8, 5 -> i32\n"
    "  ret %9\n"
    "}\n";

  if (actual != expected) {
    std::cerr << "IRBuilder test failed.\n\nExpected:\n"
              << expected << "\nActual:\n"
              << actual;
    return 1;
  }

  std::cout << "IRBuilder test passed!\n";
  return 0;
}
#endif
