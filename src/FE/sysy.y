/*
 * @file sysy.y
 * @brief Parser for the SysY language.
 */

%{
#include "../../include/helper/log.hpp"
#include "../../include/high/ast.hpp"

#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

extern int yylex();

using exodus::Float;
using exodus::I32;
using exodus::Bool;
using exodus::Void;
using namespace exodus::ast;

template <typename T>
static T take(T *ptr) {
  T value = std::move(*ptr);
  delete ptr;
  return value;
}

template <typename T>
static auto list(T *item) -> std::vector<T> * {
  auto items = new std::vector<T>();
  items->push_back(take(item));
  return items;
}

template <typename T>
static auto append(std::vector<T> *items, T *item) -> std::vector<T> * {
  items->push_back(take(item));
  return items;
}

template <typename T>
static auto ptr_list(T *item) -> std::vector<std::unique_ptr<T>> * {
  auto items = new std::vector<std::unique_ptr<T>>();
  items->emplace_back(item);
  return items;
}

template <typename T>
static auto append_ptr(std::vector<std::unique_ptr<T>> *items, T *item)
  -> std::vector<std::unique_ptr<T>> * {
  items->emplace_back(item);
  return items;
}

static auto opt_init(InitVal *init) -> std::optional<InitVal> {
  return std::optional<InitVal>(take(init));
}

// Node at line, column
template <typename Node, typename Loc>
static auto at(Node *node, const Loc &loc) -> Node * {
  node->line = loc.first_line;
  node->col = loc.first_column;
  return node;
}

template <typename Node, typename Loc, typename... Args>
static auto make_ast(const Loc &loc, Args &&...args) -> std::unique_ptr<Node> {
  auto node = std::make_unique<Node>(std::forward<Args>(args)...);
  at(node.get(), loc);
  return node;
}

template <typename Loc>
static void set_expr_loc(Expr &expr, const Loc &loc) {
  std::visit(
    [&](auto &node) {
      if (!node) return;
      node->line = loc.first_line;
      node->col = loc.first_column;
    },
    expr
  );
}

static auto expr_type(const Expr &expr) -> std::shared_ptr<exodus::Type> {
  return std::visit(
    [](const auto &node) -> std::shared_ptr<exodus::Type> {
      if (!node) return nullptr;
      return node->eval_type;
    },
    expr
  );
}

template <typename Loc>
static auto number_expr(int value, const Loc &loc) -> Expr * {
  auto expr = new Expr(std::make_unique<NumberAST>(value));
  auto &number = std::get<std::unique_ptr<NumberAST>>(*expr);
  number->eval_type = I32::get();
  number->line = loc.first_line;
  number->col = loc.first_column;
  return expr;
}

template <typename Loc>
static auto number_expr(float value, const Loc &loc) -> Expr * {
  auto expr = new Expr(std::make_unique<NumberAST>(value));
  auto &number = std::get<std::unique_ptr<NumberAST>>(*expr);
  number->eval_type = Float::get();
  number->line = loc.first_line;
  number->col = loc.first_column;
  return expr;
}

static auto arithmetic_type(const Expr &left, const Expr &right)
  -> std::shared_ptr<exodus::Type> {
  auto lhs = expr_type(left);
  auto rhs = expr_type(right);
  if (!lhs || !rhs) {
    return nullptr;
  }
  if (lhs->is_f32() || rhs->is_f32()) {
    return Float::get();
  }
  if (lhs->is_i32() && rhs->is_i32()) {
    return I32::get();
  }
  return nullptr;
}

static auto is_bool_op(BinaryOp op) -> bool {
  switch (op) {
  case BinaryOp::Lt:
  case BinaryOp::Gt:
  case BinaryOp::Le:
  case BinaryOp::Ge:
  case BinaryOp::Eq:
  case BinaryOp::Ne:
  case BinaryOp::And:
  case BinaryOp::Or:
    return true;
  default:
    return false;
  }
}

template <typename Loc>
static auto unary_expr(UnaryOp op, Expr *operand, const Loc &loc) -> Expr * {
  auto value = take(operand);
  auto node = make_ast<UnaryExprAST>(loc, op, std::move(value));
  node->eval_type = op == UnaryOp::Not ? Bool::get() : expr_type(node->expr);
  return new Expr(std::move(node));
}

template <typename Loc>
static auto binary_expr(BinaryOp op, Expr *left, Expr *right, const Loc &loc)
  -> Expr * {
  auto lhs = take(left);
  auto rhs = take(right);
  auto type = is_bool_op(op) ? Bool::get() : arithmetic_type(lhs, rhs);
  auto node = make_ast<BinaryExprAST>(loc, op, std::move(lhs), std::move(rhs));
  node->eval_type = std::move(type);
  return new Expr(std::move(node));
}

template <typename Loc>
static auto var_decl(
  std::shared_ptr<exodus::Type> type,
  std::vector<std::unique_ptr<VarDefAST>> *defs,
  bool is_const,
  const Loc &loc
) -> Decl * {
  return new Decl(make_ast<VarDeclAST>(loc, std::move(type), take(defs), is_const));
}

template <typename Loc>
static auto call_expr(std::string *name, std::vector<Expr> args, const Loc &loc)
  -> Expr * {
  return new Expr(make_ast<CallExprAST>(loc, take(name), std::move(args)));
}

%}

%code requires {
#include "../../include/high/ast_base.hpp"

namespace exodus { namespace ast { struct BlockSAST; } }
namespace exodus { namespace ast { struct FuncDefAST; } }
namespace exodus { namespace ast { struct FuncParamAST; } }
namespace exodus { namespace ast { struct LvalAST; } }
namespace exodus { namespace ast { struct VarDefAST; } }

#include <memory>
#include <string>
#include <vector>
}

%parse-param { exodus::ast::CompUnitAST &ast }
%locations

%code provides {
void yyerror(exodus::ast::CompUnitAST &ast, const char *s);
}

%union {
  std::string *str;
  int i32;
  float f32;
  exodus::ast::Expr *expr;
  exodus::ast::InitVal *init;
  exodus::ast::Stmt *stmt;
  exodus::ast::Decl *decl;
  exodus::ast::BlockItem *block_item;
  exodus::ast::LvalAST *lval;
  exodus::ast::VarDefAST *var_def;
  exodus::ast::FuncParamAST *param;
  exodus::ast::FuncDefAST *func_def;
  exodus::ast::BlockSAST *block;
  std::vector<exodus::ast::Expr> *exprs;
  std::vector<exodus::ast::InitVal> *inits;
  std::vector<std::unique_ptr<exodus::ast::VarDefAST>> *var_defs;
  std::vector<std::unique_ptr<exodus::ast::FuncParamAST>> *params;
  std::vector<exodus::ast::BlockItem> *block_items;
}

%token <str> IDENT
%token <i32> INT_CONST
%token <f32> FLOAT_CONST
%token INT FLOAT VOID CONST RETURN IF ELSE WHILE BREAK CONTINUE
%token AND OR LE GE EQ NE LT GT

%type <decl> Decl ConstDecl VarDecl
%type <func_def> FuncDef
%type <var_defs> ConstDefList VarDefList
%type <var_def> ConstDef VarDef
%type <params> FuncFParams
%type <param> FuncFParam
%type <block> Block
%type <block_items> BlockItems
%type <block_item> BlockItem
%type <stmt> Stmt
%type <lval> LVal
%type <expr> Exp Cond ConstExp PrimaryExp UnaryExp MulExp AddExp RelExp EqExp LAndExp LOrExp
%type <exprs> ConstArrayDims ArrayIndices FuncParamDims FuncArgList
%type <init> InitVal
%type <inits> InitValList

%destructor { delete $$; } <str> <expr> <init> <stmt> <decl> <block_item>
%destructor { delete $$; } <lval> <var_def> <param> <func_def> <block>
%destructor { delete $$; } <exprs> <inits> <var_defs> <params> <block_items>

%precedence THEN
%precedence ELSE

%start CompUnit

%%

CompUnit
    : %empty { ast.items.clear(); }
    | CompUnit GlobalItem
    ;

GlobalItem
    : Decl { ast.items.emplace_back(take($1)); }
    | FuncDef { ast.items.emplace_back(std::unique_ptr<FuncDefAST>($1)); }
    ;

Decl
    : ConstDecl { $$ = $1; }
    | VarDecl { $$ = $1; }
    ;

ConstDecl
    : CONST INT ConstDefList ';' { $$ = var_decl(I32::get(), $3, true, @$); }
    | CONST FLOAT ConstDefList ';' { $$ = var_decl(Float::get(), $3, true, @$); }
    ;

ConstDefList
    : ConstDef { $$ = ptr_list($1); }
    | ConstDefList ',' ConstDef { $$ = append_ptr($1, $3); }
    ;

ConstDef
    : IDENT '=' InitVal { $$ = at(new VarDefAST(take($1), {}, opt_init($3)), @$); }
    | IDENT ConstArrayDims '=' InitVal {
        $$ = at(new VarDefAST(take($1), take($2), opt_init($4)), @$);
      }
    ;

VarDecl
    : INT VarDefList ';' { $$ = var_decl(I32::get(), $2, false, @$); }
    | FLOAT VarDefList ';' { $$ = var_decl(Float::get(), $2, false, @$); }
    ;

VarDefList
    : VarDef { $$ = ptr_list($1); }
    | VarDefList ',' VarDef { $$ = append_ptr($1, $3); }
    ;

VarDef
    : IDENT { $$ = at(new VarDefAST(take($1), {}, std::nullopt), @$); }
    | IDENT '=' InitVal { $$ = at(new VarDefAST(take($1), {}, opt_init($3)), @$); }
    | IDENT ConstArrayDims { $$ = at(new VarDefAST(take($1), take($2), std::nullopt), @$); }
    | IDENT ConstArrayDims '=' InitVal {
        $$ = at(new VarDefAST(take($1), take($2), opt_init($4)), @$);
      }
    ;

ConstArrayDims
    : '[' ConstExp ']' { $$ = list($2); }
    | ConstArrayDims '[' ConstExp ']' { $$ = append($1, $3); }
    ;

InitVal
    : Exp { $$ = new InitVal(take($1)); }
    | '{' '}' { $$ = new InitVal(make_ast<InitListAST>(@$, std::vector<InitVal>())); }
    | '{' InitValList '}' { $$ = new InitVal(make_ast<InitListAST>(@$, take($2))); }
    ;

InitValList
    : InitVal { $$ = list($1); }
    | InitValList ',' InitVal { $$ = append($1, $3); }
    ;

FuncDef
    : INT IDENT '(' ')' Block {
        $$ = make_ast<FuncDefAST>(
          @$, I32::get(), take($2), std::vector<std::unique_ptr<FuncParamAST>>(), std::unique_ptr<BlockSAST>($5)
        ).release();
      }
    | FLOAT IDENT '(' ')' Block {
        $$ = make_ast<FuncDefAST>(
          @$, Float::get(), take($2), std::vector<std::unique_ptr<FuncParamAST>>(), std::unique_ptr<BlockSAST>($5)
        ).release();
      }
    | VOID IDENT '(' ')' Block {
        $$ = make_ast<FuncDefAST>(
          @$, Void::get(), take($2), std::vector<std::unique_ptr<FuncParamAST>>(), std::unique_ptr<BlockSAST>($5)
        ).release();
      }
    | INT IDENT '(' FuncFParams ')' Block {
        $$ = make_ast<FuncDefAST>(@$, I32::get(), take($2), take($4), std::unique_ptr<BlockSAST>($6)).release();
      }
    | FLOAT IDENT '(' FuncFParams ')' Block {
        $$ = make_ast<FuncDefAST>(@$, Float::get(), take($2), take($4), std::unique_ptr<BlockSAST>($6)).release();
      }
    | VOID IDENT '(' FuncFParams ')' Block {
        $$ = make_ast<FuncDefAST>(@$, Void::get(), take($2), take($4), std::unique_ptr<BlockSAST>($6)).release();
      }
    ;

FuncFParams
    : FuncFParam { $$ = ptr_list($1); }
    | FuncFParams ',' FuncFParam { $$ = append_ptr($1, $3); }
    ;

FuncFParam
    : INT IDENT FuncParamDims { $$ = make_ast<FuncParamAST>(@$, I32::get(), take($2), take($3)).release(); }
    | FLOAT IDENT FuncParamDims { $$ = make_ast<FuncParamAST>(@$, Float::get(), take($2), take($3)).release(); }
    ;

FuncParamDims
    : %empty { $$ = new std::vector<Expr>(); }
    | '[' ']' {
        $$ = new std::vector<Expr>();
        $$->push_back(take(number_expr(-1, @$)));
      }
    | '[' ']' ConstArrayDims {
        auto dims = take($3);
        dims.insert(dims.begin(), take(number_expr(-1, @$)));
        $$ = new std::vector<Expr>(std::move(dims));
      }
    ;

Block
    : '{' '}' { $$ = make_ast<BlockSAST>(@$, std::vector<BlockItem>()).release(); }
    | '{' BlockItems '}' { $$ = make_ast<BlockSAST>(@$, take($2)).release(); }
    ;

BlockItems
    : BlockItem { $$ = list($1); }
    | BlockItems BlockItem { $$ = append($1, $2); }
    ;

BlockItem
    : Decl { $$ = new BlockItem(take($1)); }
    | Stmt { $$ = new BlockItem(take($1)); }
    ;

Stmt
    : LVal '=' Exp ';' {
        $$ = new Stmt(make_ast<AssignStmtAST>(@$, std::unique_ptr<LvalAST>($1), take($3)));
      }
    | Exp ';' { $$ = new Stmt(make_ast<ExprStmtAST>(@$, take($1))); }
    | ';' { $$ = new Stmt(make_ast<ExprStmtAST>(@$)); }
    | Block { $$ = new Stmt(std::unique_ptr<BlockSAST>($1)); }
    | IF '(' Cond ')' Stmt %prec THEN {
        $$ = new Stmt(make_ast<IfStmtAST>(@$, take($3), take($5), std::nullopt));
      }
    | IF '(' Cond ')' Stmt ELSE Stmt {
        $$ = new Stmt(make_ast<IfStmtAST>(
          @$,
          take($3), take($5), std::optional<Stmt>(take($7))
        ));
      }
    | WHILE '(' Cond ')' Stmt {
        $$ = new Stmt(make_ast<WhileStmtAST>(@$, take($3), take($5)));
      }
    | BREAK ';' { $$ = new Stmt(make_ast<BreakStmtAST>(@$)); }
    | CONTINUE ';' { $$ = new Stmt(make_ast<ContinueStmtAST>(@$)); }
    | RETURN ';' { $$ = new Stmt(make_ast<ReturnStmtAST>(@$, std::nullopt)); }
    | RETURN Exp ';' {
        $$ = new Stmt(make_ast<ReturnStmtAST>(@$, std::optional<Expr>(take($2))));
      }
    ;

Exp
    : LOrExp { $$ = $1; }
    ;

Cond
    : LOrExp { $$ = $1; }
    ;

LVal
    : IDENT { $$ = make_ast<LvalAST>(@$, take($1), std::vector<Expr>()).release(); }
    | IDENT ArrayIndices { $$ = make_ast<LvalAST>(@$, take($1), take($2)).release(); }
    ;

ArrayIndices
    : '[' Exp ']' { $$ = list($2); }
    | ArrayIndices '[' Exp ']' { $$ = append($1, $3); }
    ;

PrimaryExp
    : '(' Exp ')' { set_expr_loc(*$2, @$); $$ = $2; }
    | LVal { $$ = new Expr(std::unique_ptr<LvalAST>($1)); }
    | INT_CONST { $$ = number_expr($1, @$); }
    | FLOAT_CONST { $$ = number_expr($1, @$); }
    ;

UnaryExp
    : PrimaryExp { $$ = $1; }
    | IDENT '(' ')' { $$ = call_expr($1, {}, @$); }
    | IDENT '(' FuncArgList ')' { $$ = call_expr($1, take($3), @$); }
    | '+' UnaryExp { $$ = unary_expr(UnaryOp::Pos, $2, @$); }
    | '-' UnaryExp { $$ = unary_expr(UnaryOp::Neg, $2, @$); }
    | '!' UnaryExp { $$ = unary_expr(UnaryOp::Not, $2, @$); }
    ;

FuncArgList
    : Exp { $$ = list($1); }
    | FuncArgList ',' Exp { $$ = append($1, $3); }
    ;

MulExp
    : UnaryExp { $$ = $1; }
    | MulExp '*' UnaryExp { $$ = binary_expr(BinaryOp::Mul, $1, $3, @$); }
    | MulExp '/' UnaryExp { $$ = binary_expr(BinaryOp::Div, $1, $3, @$); }
    | MulExp '%' UnaryExp { $$ = binary_expr(BinaryOp::Mod, $1, $3, @$); }
    ;

AddExp
    : MulExp { $$ = $1; }
    | AddExp '+' MulExp { $$ = binary_expr(BinaryOp::Add, $1, $3, @$); }
    | AddExp '-' MulExp { $$ = binary_expr(BinaryOp::Sub, $1, $3, @$); }
    ;

RelExp
    : AddExp { $$ = $1; }
    | RelExp LT AddExp { $$ = binary_expr(BinaryOp::Lt, $1, $3, @$); }
    | RelExp GT AddExp { $$ = binary_expr(BinaryOp::Gt, $1, $3, @$); }
    | RelExp LE AddExp { $$ = binary_expr(BinaryOp::Le, $1, $3, @$); }
    | RelExp GE AddExp { $$ = binary_expr(BinaryOp::Ge, $1, $3, @$); }
    ;

EqExp
    : RelExp { $$ = $1; }
    | EqExp EQ RelExp { $$ = binary_expr(BinaryOp::Eq, $1, $3, @$); }
    | EqExp NE RelExp { $$ = binary_expr(BinaryOp::Ne, $1, $3, @$); }
    ;

LAndExp
    : EqExp { $$ = $1; }
    | LAndExp AND EqExp { $$ = binary_expr(BinaryOp::And, $1, $3, @$); }
    ;

LOrExp
    : LAndExp { $$ = $1; }
    | LOrExp OR LAndExp { $$ = binary_expr(BinaryOp::Or, $1, $3, @$); }
    ;

ConstExp
    : Exp { $$ = $1; }
    ;

%%

void yyerror(CompUnitAST &ast, const char *s) {
  (void)ast;
#ifdef __DEBUG
  exodus::Log::log_error("parse error at {}:{}: {}", 
    yylloc.first_line, 
    yylloc.first_column, 
    s
  );
#endif
  std::cerr << "parse error at " << yylloc.first_line << ":"
            << yylloc.first_column << ": " << s << std::endl;
}
