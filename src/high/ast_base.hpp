// include/high/ast_base.hpp
#pragma once

#include "../type.hpp"
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace exodus::ast {

// --- Enums ---

enum class BinaryOp : uint8_t {
  Add, // +
  Sub, // -
  Mul, // *
  Div, // /
  Mod, // %
  MatMul, // @
  Lt,  // <
  Gt,  // >
  Le,  // <=
  Ge,  // >=
  Eq,  // ==
  Ne,  // !=
  And, // &&
  Or   // ||
};

enum class UnaryOp : uint8_t {
  Pos, // +
  Neg, // -
  Not  // !
};

// --- Mixin class ---

struct Sourcelocation {
  int line = 0, col = 0;
  virtual ~Sourcelocation() = default;

  Sourcelocation() = default;
  Sourcelocation(const Sourcelocation &) = default;
  Sourcelocation &operator=(const Sourcelocation &) = default;
  Sourcelocation(Sourcelocation &&) = default;
  Sourcelocation &operator=(Sourcelocation &&) = default;
};

struct Typed {
  std::shared_ptr<Type> eval_type;
  virtual ~Typed() = default;

  Typed() = default;
  Typed(const Typed &) = default;
  Typed &operator=(const Typed &) = default;
  Typed(Typed &&) = default;
  Typed &operator=(Typed &&) = default;
};

// --- Forward Declarations ---

struct NumberAST;
struct LvalAST;
struct BinaryExprAST;
struct UnaryExprAST;
struct CallExprAST;
struct InitListAST;

struct IfStmtAST;
struct WhileStmtAST;
struct BreakStmtAST;
struct ContinueStmtAST;
struct ReturnStmtAST;
struct AssignStmtAST;
struct BlockSAST;
// 作为一种特殊的 stmt， 我们会抛弃其返回值，保留其副作用
struct ExprStmtAST;

struct VarDeclAST;
struct FuncDefAST;

// --- Variant Systems ---

// Expr 表明表达式 「有值且可求值」, 具有返回值
using Expr = std::variant<
  std::unique_ptr<NumberAST>,
  std::unique_ptr<LvalAST>,
  std::unique_ptr<BinaryExprAST>,
  std::unique_ptr<UnaryExprAST>,
  std::unique_ptr<CallExprAST>>;

// Stmt 「不具备返回值，仅代表动作和控制流，通常具有副作用」
using Stmt = std::variant<
  std::unique_ptr<IfStmtAST>,
  std::unique_ptr<WhileStmtAST>,
  std::unique_ptr<BreakStmtAST>,
  std::unique_ptr<ContinueStmtAST>,
  std::unique_ptr<ReturnStmtAST>,
  std::unique_ptr<AssignStmtAST>,
  std::unique_ptr<BlockSAST>,
  std::unique_ptr<ExprStmtAST>>;

using Decl = std::unique_ptr<VarDeclAST>;

using InitVal = std::variant<Expr, std::unique_ptr<InitListAST>>;
using BlockItem = std::variant<Decl, Stmt>;
using GlobalItem = std::variant<Decl, std::unique_ptr<FuncDefAST>>;
struct CompUnitAST {
  std::vector<GlobalItem> items;
  std::string source_file; //< for meta data, such like file name, target arch.
};

} // namespace exodus::ast
