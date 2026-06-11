// include/high/ast.hpp
#pragma once

#include "ast_base.hpp"
#include <optional>
#include <string>
#include <vector>

namespace exodus::ast {

// --- Expr ---

struct NumberAST : Sourcelocation, Typed {
  using var = std::variant<int, float>;
  var val;
  NumberAST(var v);
};

struct LvalAST : Sourcelocation, Typed {
  std::string name;
  std::vector<Expr> indices;
  LvalAST(std::string n, std::vector<Expr> idx);
};

struct BinaryExprAST : Sourcelocation, Typed {
  BinaryOp op;
  Expr left, right;
  BinaryExprAST(BinaryOp o, Expr l, Expr r);
};

struct UnaryExprAST : Sourcelocation, Typed {
  UnaryOp op;
  Expr expr;
  UnaryExprAST(UnaryOp o, Expr e);
};

struct CallExprAST : Sourcelocation, Typed {
  std::string name;
  std::vector<Expr> args;
  CallExprAST(std::string n, std::vector<Expr> a);
};

struct InitListAST : Sourcelocation {
  std::vector<InitVal> values;
  InitListAST(std::vector<InitVal> v);
};

// --- Stmt ---

struct ExprStmtAST : Sourcelocation {
  std::optional<Expr> expr;
  ExprStmtAST();
  ExprStmtAST(Expr e);
};

struct AssignStmtAST : Sourcelocation {
  std::unique_ptr<LvalAST> lval;
  Expr expr;
  AssignStmtAST(std::unique_ptr<LvalAST> l, Expr e);
};

struct BlockSAST : Sourcelocation {
  std::vector<BlockItem> items;
  BlockSAST(std::vector<BlockItem> i);
};

struct IfStmtAST : Sourcelocation {
  Expr cond;
  Stmt then_body;
  std::optional<Stmt> else_body;
  IfStmtAST(Expr c, Stmt t, std::optional<Stmt> e);
};

struct WhileStmtAST : Sourcelocation {
  Expr cond;
  Stmt body;
  WhileStmtAST(Expr c, Stmt b);
};

struct BreakStmtAST : Sourcelocation {
  BreakStmtAST();
};

struct ContinueStmtAST : Sourcelocation {
  ContinueStmtAST();
};

struct ReturnStmtAST : Sourcelocation {
  std::optional<Expr> expr;
  ReturnStmtAST(std::optional<Expr> e);
};

// --- Decl ---

struct VarDefAST : Sourcelocation {
  std::string name;
  std::vector<Expr> dims;
  std::optional<InitVal> init;
  VarDefAST(std::string n, std::vector<Expr> d, std::optional<InitVal> i);
};

struct VarDeclAST : Sourcelocation {
  std::shared_ptr<Type> type;
  std::vector<std::unique_ptr<VarDefAST>> defs;
  bool is_const;
  VarDeclAST(
    std::shared_ptr<Type> t, std::vector<std::unique_ptr<VarDefAST>> d, bool c
  );
};

struct FuncParamAST : Sourcelocation {
  std::shared_ptr<Type> type;
  std::string name;
  std::vector<Expr> dims;
  FuncParamAST(std::shared_ptr<Type> t, std::string n, std::vector<Expr> d);
};

struct FuncDefAST : Sourcelocation {
  std::shared_ptr<Type> ret_type;
  std::string name;
  std::vector<std::unique_ptr<FuncParamAST>> params;
  std::unique_ptr<BlockSAST> body;
  FuncDefAST(
    std::shared_ptr<Type> ret,
    std::string n,
    std::vector<std::unique_ptr<FuncParamAST>> p,
    std::unique_ptr<BlockSAST> bd
  );
};

// --- 这延迟初始化写着真掉 san ---

inline NumberAST::NumberAST(std::variant<int, float> v) : val(v) {}

inline LvalAST::LvalAST(std::string n, std::vector<Expr> idx)
    : name(std::move(n)), indices(std::move(idx)) {}

inline BinaryExprAST::BinaryExprAST(BinaryOp o, Expr l, Expr r)
    : op(o), left(std::move(l)), right(std::move(r)) {}

inline UnaryExprAST::UnaryExprAST(UnaryOp o, Expr e)
    : op(o), expr(std::move(e)) {}

inline CallExprAST::CallExprAST(std::string n, std::vector<Expr> a)
    : name(std::move(n)), args(std::move(a)) {}

inline InitListAST::InitListAST(std::vector<InitVal> v)
    : values(std::move(v)) {}

inline ExprStmtAST::ExprStmtAST() : expr(std::nullopt) {}
inline ExprStmtAST::ExprStmtAST(Expr e) : expr(std::move(e)) {}

inline AssignStmtAST::AssignStmtAST(std::unique_ptr<LvalAST> l, Expr e)
    : lval(std::move(l)), expr(std::move(e)) {}

inline BlockSAST::BlockSAST(std::vector<BlockItem> i) : items(std::move(i)) {}

inline IfStmtAST::IfStmtAST(Expr c, Stmt t, std::optional<Stmt> e)
    : cond(std::move(c)), then_body(std::move(t)), else_body(std::move(e)) {}

inline WhileStmtAST::WhileStmtAST(Expr c, Stmt b)
    : cond(std::move(c)), body(std::move(b)) {}

inline BreakStmtAST::BreakStmtAST() {}
inline ContinueStmtAST::ContinueStmtAST() {}

inline ReturnStmtAST::ReturnStmtAST(std::optional<Expr> e)
    : expr(std::move(e)) {}

inline VarDefAST::VarDefAST(
  std::string n, std::vector<Expr> d, std::optional<InitVal> i
)
    : name(std::move(n)), dims(std::move(d)), init(std::move(i)) {}

inline VarDeclAST::VarDeclAST(
  std::shared_ptr<Type> t, std::vector<std::unique_ptr<VarDefAST>> d, bool c
)
    : type(std::move(t)), defs(std::move(d)), is_const(c) {}

inline FuncParamAST::FuncParamAST(
  std::shared_ptr<Type> t, std::string n, std::vector<Expr> d
)
    : type(std::move(t)), name(std::move(n)), dims(std::move(d)) {}

inline FuncDefAST::FuncDefAST(
  std::shared_ptr<Type> ret,
  std::string n,
  std::vector<std::unique_ptr<FuncParamAST>> p,
  std::unique_ptr<BlockSAST> bd
)
    : ret_type(std::move(ret)), name(std::move(n)), params(std::move(p)),
      body(std::move(bd)) {}

} // namespace exodus::ast
