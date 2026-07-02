/* A Bison parser, made by GNU Bison 3.8.2.  */

/* Bison interface for Yacc-like parsers in C

   Copyright (C) 1984, 1989-1990, 2000-2015, 2018-2021 Free Software Foundation,
   Inc.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.  */

/* As a special exception, you may create a larger work that contains
   part or all of the Bison parser skeleton and distribute that work
   under terms of your choice, so long as that work isn't itself a
   parser generator using the skeleton or a modified version thereof
   as a parser skeleton.  Alternatively, if you modify or redistribute
   the parser skeleton itself, you may (at your option) remove this
   special exception, which will cause the skeleton and the resulting
   Bison output files to be licensed under the GNU General Public
   License without this special exception.

   This special exception was added by the Free Software Foundation in
   version 2.2 of Bison.  */

/* DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
   especially those whose name start with YY_ or yy_.  They are
   private implementation details that can be changed or removed.  */

#ifndef YY_YY_HOME_ANFSITY_PROJECT_EXODUS_BUILD_SRC_SYSY_TAB_HPP_INCLUDED
#define YY_YY_HOME_ANFSITY_PROJECT_EXODUS_BUILD_SRC_SYSY_TAB_HPP_INCLUDED
/* Debug traces.  */
#ifndef YYDEBUG
#define YYDEBUG 0
#endif
#if YYDEBUG
extern int yydebug;
#endif
/* "%code requires" blocks.  */
#line 189 "/home/anfsity/Project/Exodus/src/FE/sysy.y"

#include "../../src/high/ast_base.hpp"

namespace exodus {
namespace ast {
struct BlockSAST;
}
} // namespace exodus
namespace exodus {
namespace ast {
struct FuncDefAST;
}
} // namespace exodus
namespace exodus {
namespace ast {
struct FuncParamAST;
}
} // namespace exodus
namespace exodus {
namespace ast {
struct LvalAST;
}
} // namespace exodus
namespace exodus {
namespace ast {
struct VarDefAST;
}
} // namespace exodus

#include <memory>
#include <string>
#include <vector>

#line 63 "/home/anfsity/Project/Exodus/build/src/sysy.tab.hpp"

/* Token kinds.  */
#ifndef YYTOKENTYPE
#define YYTOKENTYPE
enum yytokentype {
  YYEMPTY = -2,
  YYEOF = 0,         /* "end of file"  */
  YYerror = 256,     /* error  */
  YYUNDEF = 257,     /* "invalid token"  */
  IDENT = 258,       /* IDENT  */
  INT_CONST = 259,   /* INT_CONST  */
  FLOAT_CONST = 260, /* FLOAT_CONST  */
  INT = 261,         /* INT  */
  FLOAT = 262,       /* FLOAT  */
  VOID = 263,        /* VOID  */
  CONST = 264,       /* CONST  */
  RETURN = 265,      /* RETURN  */
  IF = 266,          /* IF  */
  ELSE = 267,        /* ELSE  */
  WHILE = 268,       /* WHILE  */
  BREAK = 269,       /* BREAK  */
  CONTINUE = 270,    /* CONTINUE  */
  AND = 271,         /* AND  */
  OR = 272,          /* OR  */
  LE = 273,          /* LE  */
  GE = 274,          /* GE  */
  EQ = 275,          /* EQ  */
  NE = 276,          /* NE  */
  LT = 277,          /* LT  */
  GT = 278,          /* GT  */
  THEN = 279         /* THEN  */
};
typedef enum yytokentype yytoken_kind_t;
#endif

/* Value type.  */
#if !defined YYSTYPE && !defined YYSTYPE_IS_DECLARED
union YYSTYPE {
#line 210 "/home/anfsity/Project/Exodus/src/FE/sysy.y"

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

#line 125 "/home/anfsity/Project/Exodus/build/src/sysy.tab.hpp"
};
typedef union YYSTYPE YYSTYPE;
#define YYSTYPE_IS_TRIVIAL 1
#define YYSTYPE_IS_DECLARED 1
#endif

/* Location type.  */
#if !defined YYLTYPE && !defined YYLTYPE_IS_DECLARED
typedef struct YYLTYPE YYLTYPE;
struct YYLTYPE {
  int first_line;
  int first_column;
  int last_line;
  int last_column;
};
#define YYLTYPE_IS_DECLARED 1
#define YYLTYPE_IS_TRIVIAL 1
#endif

extern YYSTYPE yylval;
extern YYLTYPE yylloc;

int yyparse(exodus::ast::CompUnitAST &ast);

/* "%code provides" blocks.  */
#line 206 "/home/anfsity/Project/Exodus/src/FE/sysy.y"

void yyerror(exodus::ast::CompUnitAST &ast, const char *s);

#line 158 "/home/anfsity/Project/Exodus/build/src/sysy.tab.hpp"

#endif /* !YY_YY_HOME_ANFSITY_PROJECT_EXODUS_BUILD_SRC_SYSY_TAB_HPP_INCLUDED   \
        */
