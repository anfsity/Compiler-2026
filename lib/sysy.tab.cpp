/* A Bison parser, made by GNU Bison 3.8.2.  */

/* Bison implementation for Yacc-like parsers in C

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

/* C LALR(1) parser skeleton written by Richard Stallman, by
   simplifying the original so-called "semantic" parser.  */

/* DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
   especially those whose name start with YY_ or yy_.  They are
   private implementation details that can be changed or removed.  */

/* All symbols defined below should begin with yy or YY, to avoid
   infringing on user name space.  This should be done even for local
   variables, as they might otherwise be expanded by user macros.
   There are some unavoidable exceptions within include files to
   define necessary library symbols; they are noted "INFRINGES ON
   USER NAME SPACE" below.  */

/* Identify Bison output, and Bison version.  */
#define YYBISON 30802

/* Bison version string.  */
#define YYBISON_VERSION "3.8.2"

/* Skeleton name.  */
#define YYSKELETON_NAME "yacc.c"

/* Pure parsers.  */
#define YYPURE 0

/* Push parsers.  */
#define YYPUSH 0

/* Pull parsers.  */
#define YYPULL 1

/* First part of user prologue.  */
#line 6 "/home/anfsity/Project/Exodus/src/FE/sysy.y"

#include "../../src/helper/log.hpp"
#include "../../src/high/ast.hpp"

#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

extern int yylex();

using exodus::Bool;
using exodus::Float;
using exodus::I32;
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
      if (!node)
        return;
      node->line = loc.first_line;
      node->col = loc.first_column;
    },
    expr
  );
}

static auto expr_type(const Expr &expr) -> std::shared_ptr<exodus::Type> {
  return std::visit(
    [](const auto &node) -> std::shared_ptr<exodus::Type> {
      if (!node)
        return nullptr;
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
  return new Decl(
    make_ast<VarDeclAST>(loc, std::move(type), take(defs), is_const)
  );
}

template <typename Loc>
static auto call_expr(std::string *name, std::vector<Expr> args, const Loc &loc)
  -> Expr * {
  return new Expr(make_ast<CallExprAST>(loc, take(name), std::move(args)));
}

#line 254 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"

#ifndef YY_CAST
#ifdef __cplusplus
#define YY_CAST(Type, Val) static_cast<Type>(Val)
#define YY_REINTERPRET_CAST(Type, Val) reinterpret_cast<Type>(Val)
#else
#define YY_CAST(Type, Val) ((Type)(Val))
#define YY_REINTERPRET_CAST(Type, Val) ((Type)(Val))
#endif
#endif
#ifndef YY_NULLPTR
#if defined __cplusplus
#if 201103L <= __cplusplus
#define YY_NULLPTR nullptr
#else
#define YY_NULLPTR 0
#endif
#else
#define YY_NULLPTR ((void *)0)
#endif
#endif

#include "sysy.tab.hpp"
/* Symbol kind.  */
enum yysymbol_kind_t {
  YYSYMBOL_YYEMPTY = -2,
  YYSYMBOL_YYEOF = 0,           /* "end of file"  */
  YYSYMBOL_YYerror = 1,         /* error  */
  YYSYMBOL_YYUNDEF = 2,         /* "invalid token"  */
  YYSYMBOL_IDENT = 3,           /* IDENT  */
  YYSYMBOL_INT_CONST = 4,       /* INT_CONST  */
  YYSYMBOL_FLOAT_CONST = 5,     /* FLOAT_CONST  */
  YYSYMBOL_INT = 6,             /* INT  */
  YYSYMBOL_FLOAT = 7,           /* FLOAT  */
  YYSYMBOL_VOID = 8,            /* VOID  */
  YYSYMBOL_CONST = 9,           /* CONST  */
  YYSYMBOL_RETURN = 10,         /* RETURN  */
  YYSYMBOL_IF = 11,             /* IF  */
  YYSYMBOL_ELSE = 12,           /* ELSE  */
  YYSYMBOL_WHILE = 13,          /* WHILE  */
  YYSYMBOL_BREAK = 14,          /* BREAK  */
  YYSYMBOL_CONTINUE = 15,       /* CONTINUE  */
  YYSYMBOL_AND = 16,            /* AND  */
  YYSYMBOL_OR = 17,             /* OR  */
  YYSYMBOL_LE = 18,             /* LE  */
  YYSYMBOL_GE = 19,             /* GE  */
  YYSYMBOL_EQ = 20,             /* EQ  */
  YYSYMBOL_NE = 21,             /* NE  */
  YYSYMBOL_LT = 22,             /* LT  */
  YYSYMBOL_GT = 23,             /* GT  */
  YYSYMBOL_THEN = 24,           /* THEN  */
  YYSYMBOL_25_ = 25,            /* ';'  */
  YYSYMBOL_26_ = 26,            /* ','  */
  YYSYMBOL_27_ = 27,            /* '='  */
  YYSYMBOL_28_ = 28,            /* '['  */
  YYSYMBOL_29_ = 29,            /* ']'  */
  YYSYMBOL_30_ = 30,            /* '{'  */
  YYSYMBOL_31_ = 31,            /* '}'  */
  YYSYMBOL_32_ = 32,            /* '('  */
  YYSYMBOL_33_ = 33,            /* ')'  */
  YYSYMBOL_34_ = 34,            /* '+'  */
  YYSYMBOL_35_ = 35,            /* '-'  */
  YYSYMBOL_36_ = 36,            /* '!'  */
  YYSYMBOL_37_ = 37,            /* '*'  */
  YYSYMBOL_38_ = 38,            /* '/'  */
  YYSYMBOL_39_ = 39,            /* '%'  */
  YYSYMBOL_YYACCEPT = 40,       /* $accept  */
  YYSYMBOL_CompUnit = 41,       /* CompUnit  */
  YYSYMBOL_GlobalItem = 42,     /* GlobalItem  */
  YYSYMBOL_Decl = 43,           /* Decl  */
  YYSYMBOL_ConstDecl = 44,      /* ConstDecl  */
  YYSYMBOL_ConstDefList = 45,   /* ConstDefList  */
  YYSYMBOL_ConstDef = 46,       /* ConstDef  */
  YYSYMBOL_VarDecl = 47,        /* VarDecl  */
  YYSYMBOL_VarDefList = 48,     /* VarDefList  */
  YYSYMBOL_VarDef = 49,         /* VarDef  */
  YYSYMBOL_ConstArrayDims = 50, /* ConstArrayDims  */
  YYSYMBOL_InitVal = 51,        /* InitVal  */
  YYSYMBOL_InitValList = 52,    /* InitValList  */
  YYSYMBOL_FuncDef = 53,        /* FuncDef  */
  YYSYMBOL_FuncFParams = 54,    /* FuncFParams  */
  YYSYMBOL_FuncFParam = 55,     /* FuncFParam  */
  YYSYMBOL_FuncParamDims = 56,  /* FuncParamDims  */
  YYSYMBOL_Block = 57,          /* Block  */
  YYSYMBOL_BlockItems = 58,     /* BlockItems  */
  YYSYMBOL_BlockItem = 59,      /* BlockItem  */
  YYSYMBOL_Stmt = 60,           /* Stmt  */
  YYSYMBOL_Exp = 61,            /* Exp  */
  YYSYMBOL_Cond = 62,           /* Cond  */
  YYSYMBOL_LVal = 63,           /* LVal  */
  YYSYMBOL_ArrayIndices = 64,   /* ArrayIndices  */
  YYSYMBOL_PrimaryExp = 65,     /* PrimaryExp  */
  YYSYMBOL_UnaryExp = 66,       /* UnaryExp  */
  YYSYMBOL_FuncArgList = 67,    /* FuncArgList  */
  YYSYMBOL_MulExp = 68,         /* MulExp  */
  YYSYMBOL_AddExp = 69,         /* AddExp  */
  YYSYMBOL_RelExp = 70,         /* RelExp  */
  YYSYMBOL_EqExp = 71,          /* EqExp  */
  YYSYMBOL_LAndExp = 72,        /* LAndExp  */
  YYSYMBOL_LOrExp = 73,         /* LOrExp  */
  YYSYMBOL_ConstExp = 74        /* ConstExp  */
};
typedef enum yysymbol_kind_t yysymbol_kind_t;

#ifdef short
#undef short
#endif

/* On compilers that do not define __PTRDIFF_MAX__ etc., make sure
   <limits.h> and (if available) <stdint.h> are included
   so that the code can choose integer types of a good width.  */

#ifndef __PTRDIFF_MAX__
#include <limits.h> /* INFRINGES ON USER NAME SPACE */
#if defined __STDC_VERSION__ && 199901 <= __STDC_VERSION__
#include <stdint.h> /* INFRINGES ON USER NAME SPACE */
#define YY_STDINT_H
#endif
#endif

/* Narrow types that promote to a signed type and that can represent a
   signed or unsigned integer of at least N bits.  In tables they can
   save space and decrease cache pressure.  Promoting to a signed type
   helps avoid bugs in integer arithmetic.  */

#ifdef __INT_LEAST8_MAX__
typedef __INT_LEAST8_TYPE__ yytype_int8;
#elif defined YY_STDINT_H
typedef int_least8_t yytype_int8;
#else
typedef signed char yytype_int8;
#endif

#ifdef __INT_LEAST16_MAX__
typedef __INT_LEAST16_TYPE__ yytype_int16;
#elif defined YY_STDINT_H
typedef int_least16_t yytype_int16;
#else
typedef short yytype_int16;
#endif

/* Work around bug in HP-UX 11.23, which defines these macros
   incorrectly for preprocessor constants.  This workaround can likely
   be removed in 2023, as HPE has promised support for HP-UX 11.23
   (aka HP-UX 11i v2) only through the end of 2022; see Table 2 of
   <https://h20195.www2.hpe.com/V2/getpdf.aspx/4AA4-7673ENW.pdf>.  */
#ifdef __hpux
#undef UINT_LEAST8_MAX
#undef UINT_LEAST16_MAX
#define UINT_LEAST8_MAX 255
#define UINT_LEAST16_MAX 65535
#endif

#if defined __UINT_LEAST8_MAX__ && __UINT_LEAST8_MAX__ <= __INT_MAX__
typedef __UINT_LEAST8_TYPE__ yytype_uint8;
#elif (                                                                        \
  !defined __UINT_LEAST8_MAX__ && defined YY_STDINT_H &&                       \
  UINT_LEAST8_MAX <= INT_MAX                                                   \
)
typedef uint_least8_t yytype_uint8;
#elif !defined __UINT_LEAST8_MAX__ && UCHAR_MAX <= INT_MAX
typedef unsigned char yytype_uint8;
#else
typedef short yytype_uint8;
#endif

#if defined __UINT_LEAST16_MAX__ && __UINT_LEAST16_MAX__ <= __INT_MAX__
typedef __UINT_LEAST16_TYPE__ yytype_uint16;
#elif (                                                                        \
  !defined __UINT_LEAST16_MAX__ && defined YY_STDINT_H &&                      \
  UINT_LEAST16_MAX <= INT_MAX                                                  \
)
typedef uint_least16_t yytype_uint16;
#elif !defined __UINT_LEAST16_MAX__ && USHRT_MAX <= INT_MAX
typedef unsigned short yytype_uint16;
#else
typedef int yytype_uint16;
#endif

#ifndef YYPTRDIFF_T
#if defined __PTRDIFF_TYPE__ && defined __PTRDIFF_MAX__
#define YYPTRDIFF_T __PTRDIFF_TYPE__
#define YYPTRDIFF_MAXIMUM __PTRDIFF_MAX__
#elif defined PTRDIFF_MAX
#ifndef ptrdiff_t
#include <stddef.h> /* INFRINGES ON USER NAME SPACE */
#endif
#define YYPTRDIFF_T ptrdiff_t
#define YYPTRDIFF_MAXIMUM PTRDIFF_MAX
#else
#define YYPTRDIFF_T long
#define YYPTRDIFF_MAXIMUM LONG_MAX
#endif
#endif

#ifndef YYSIZE_T
#ifdef __SIZE_TYPE__
#define YYSIZE_T __SIZE_TYPE__
#elif defined size_t
#define YYSIZE_T size_t
#elif defined __STDC_VERSION__ && 199901 <= __STDC_VERSION__
#include <stddef.h> /* INFRINGES ON USER NAME SPACE */
#define YYSIZE_T size_t
#else
#define YYSIZE_T unsigned
#endif
#endif

#define YYSIZE_MAXIMUM                                                         \
  YY_CAST(                                                                     \
    YYPTRDIFF_T,                                                               \
    (YYPTRDIFF_MAXIMUM < YY_CAST(YYSIZE_T, -1) ? YYPTRDIFF_MAXIMUM             \
                                               : YY_CAST(YYSIZE_T, -1))        \
  )

#define YYSIZEOF(X) YY_CAST(YYPTRDIFF_T, sizeof(X))

/* Stored state numbers (used for stacks). */
typedef yytype_uint8 yy_state_t;

/* State numbers in computations.  */
typedef int yy_state_fast_t;

#ifndef YY_
#if defined YYENABLE_NLS && YYENABLE_NLS
#if ENABLE_NLS
#include <libintl.h> /* INFRINGES ON USER NAME SPACE */
#define YY_(Msgid) dgettext("bison-runtime", Msgid)
#endif
#endif
#ifndef YY_
#define YY_(Msgid) Msgid
#endif
#endif

#ifndef YY_ATTRIBUTE_PURE
#if defined __GNUC__ && 2 < __GNUC__ + (96 <= __GNUC_MINOR__)
#define YY_ATTRIBUTE_PURE __attribute__((__pure__))
#else
#define YY_ATTRIBUTE_PURE
#endif
#endif

#ifndef YY_ATTRIBUTE_UNUSED
#if defined __GNUC__ && 2 < __GNUC__ + (7 <= __GNUC_MINOR__)
#define YY_ATTRIBUTE_UNUSED __attribute__((__unused__))
#else
#define YY_ATTRIBUTE_UNUSED
#endif
#endif

/* Suppress unused-variable warnings by "using" E.  */
#if !defined lint || defined __GNUC__
#define YY_USE(E) ((void)(E))
#else
#define YY_USE(E) /* empty */
#endif

/* Suppress an incorrect diagnostic about yylval being uninitialized.  */
#if defined __GNUC__ && !defined __ICC && 406 <= __GNUC__ * 100 + __GNUC_MINOR__
#if __GNUC__ * 100 + __GNUC_MINOR__ < 407
#define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN                                    \
  _Pragma("GCC diagnostic push")                                               \
    _Pragma("GCC diagnostic ignored \"-Wuninitialized\"")
#else
#define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN                                    \
  _Pragma("GCC diagnostic push")                                               \
    _Pragma("GCC diagnostic ignored \"-Wuninitialized\"")                      \
      _Pragma("GCC diagnostic ignored \"-Wmaybe-uninitialized\"")
#endif
#define YY_IGNORE_MAYBE_UNINITIALIZED_END _Pragma("GCC diagnostic pop")
#else
#define YY_INITIAL_VALUE(Value) Value
#endif
#ifndef YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
#define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
#define YY_IGNORE_MAYBE_UNINITIALIZED_END
#endif
#ifndef YY_INITIAL_VALUE
#define YY_INITIAL_VALUE(Value) /* Nothing. */
#endif

#if defined __cplusplus && defined __GNUC__ && !defined __ICC && 6 <= __GNUC__
#define YY_IGNORE_USELESS_CAST_BEGIN                                           \
  _Pragma("GCC diagnostic push")                                               \
    _Pragma("GCC diagnostic ignored \"-Wuseless-cast\"")
#define YY_IGNORE_USELESS_CAST_END _Pragma("GCC diagnostic pop")
#endif
#ifndef YY_IGNORE_USELESS_CAST_BEGIN
#define YY_IGNORE_USELESS_CAST_BEGIN
#define YY_IGNORE_USELESS_CAST_END
#endif

#define YY_ASSERT(E) ((void)(0 && (E)))

#if !defined yyoverflow

/* The parser invokes alloca or malloc; define the necessary symbols.  */

#ifdef YYSTACK_USE_ALLOCA
#if YYSTACK_USE_ALLOCA
#ifdef __GNUC__
#define YYSTACK_ALLOC __builtin_alloca
#elif defined __BUILTIN_VA_ARG_INCR
#include <alloca.h> /* INFRINGES ON USER NAME SPACE */
#elif defined _AIX
#define YYSTACK_ALLOC __alloca
#elif defined _MSC_VER
#include <malloc.h> /* INFRINGES ON USER NAME SPACE */
#define alloca _alloca
#else
#define YYSTACK_ALLOC alloca
#if !defined _ALLOCA_H && !defined EXIT_SUCCESS
#include <stdlib.h> /* INFRINGES ON USER NAME SPACE */
/* Use EXIT_SUCCESS as a witness for stdlib.h.  */
#ifndef EXIT_SUCCESS
#define EXIT_SUCCESS 0
#endif
#endif
#endif
#endif
#endif

#ifdef YYSTACK_ALLOC
/* Pacify GCC's 'empty if-body' warning.  */
#define YYSTACK_FREE(Ptr)                                                      \
  do { /* empty */                                                             \
    ;                                                                          \
  } while (0)
#ifndef YYSTACK_ALLOC_MAXIMUM
/* The OS might guarantee only one guard page at the bottom of the stack,
   and a page size can be as small as 4096 bytes.  So we cannot safely
   invoke alloca (N) if N exceeds 4096.  Use a slightly smaller number
   to allow for a few compiler-allocated temporary stack slots.  */
#define YYSTACK_ALLOC_MAXIMUM 4032 /* reasonable circa 2006 */
#endif
#else
#define YYSTACK_ALLOC YYMALLOC
#define YYSTACK_FREE YYFREE
#ifndef YYSTACK_ALLOC_MAXIMUM
#define YYSTACK_ALLOC_MAXIMUM YYSIZE_MAXIMUM
#endif
#if (                                                                          \
  defined __cplusplus && !defined EXIT_SUCCESS &&                              \
  !((defined YYMALLOC || defined malloc) && (defined YYFREE || defined free))  \
)
#include <stdlib.h> /* INFRINGES ON USER NAME SPACE */
#ifndef EXIT_SUCCESS
#define EXIT_SUCCESS 0
#endif
#endif
#ifndef YYMALLOC
#define YYMALLOC malloc
#if !defined malloc && !defined EXIT_SUCCESS
void *malloc(YYSIZE_T); /* INFRINGES ON USER NAME SPACE */
#endif
#endif
#ifndef YYFREE
#define YYFREE free
#if !defined free && !defined EXIT_SUCCESS
void free(void *); /* INFRINGES ON USER NAME SPACE */
#endif
#endif
#endif
#endif /* !defined yyoverflow */

#if (                                                                          \
  !defined yyoverflow && (!defined __cplusplus ||                              \
                          (defined YYLTYPE_IS_TRIVIAL && YYLTYPE_IS_TRIVIAL && \
                           defined YYSTYPE_IS_TRIVIAL && YYSTYPE_IS_TRIVIAL))  \
)

/* A type that is properly aligned for any stack member.  */
union yyalloc {
  yy_state_t yyss_alloc;
  YYSTYPE yyvs_alloc;
  YYLTYPE yyls_alloc;
};

/* The size of the maximum gap between one aligned stack and the next.  */
#define YYSTACK_GAP_MAXIMUM (YYSIZEOF(union yyalloc) - 1)

/* The size of an array large to enough to hold all stacks, each with
   N elements.  */
#define YYSTACK_BYTES(N)                                                       \
  ((N) * (YYSIZEOF(yy_state_t) + YYSIZEOF(YYSTYPE) + YYSIZEOF(YYLTYPE)) +      \
   2 * YYSTACK_GAP_MAXIMUM)

#define YYCOPY_NEEDED 1

/* Relocate STACK from its old location to the new one.  The
   local variables YYSIZE and YYSTACKSIZE give the old and new number of
   elements in the stack, and YYPTR gives the new location of the
   stack.  Advance YYPTR to a properly aligned location for the next
   stack.  */
#define YYSTACK_RELOCATE(Stack_alloc, Stack)                                   \
  do {                                                                         \
    YYPTRDIFF_T yynewbytes;                                                    \
    YYCOPY(&yyptr->Stack_alloc, Stack, yysize);                                \
    Stack = &yyptr->Stack_alloc;                                               \
    yynewbytes = yystacksize * YYSIZEOF(*Stack) + YYSTACK_GAP_MAXIMUM;         \
    yyptr += yynewbytes / YYSIZEOF(*yyptr);                                    \
  } while (0)

#endif

#if defined YYCOPY_NEEDED && YYCOPY_NEEDED
/* Copy COUNT objects from SRC to DST.  The source and destination do
   not overlap.  */
#ifndef YYCOPY
#if defined __GNUC__ && 1 < __GNUC__
#define YYCOPY(Dst, Src, Count)                                                \
  __builtin_memcpy(Dst, Src, YY_CAST(YYSIZE_T, (Count)) * sizeof(*(Src)))
#else
#define YYCOPY(Dst, Src, Count)                                                \
  do {                                                                         \
    YYPTRDIFF_T yyi;                                                           \
    for (yyi = 0; yyi < (Count); yyi++)                                        \
      (Dst)[yyi] = (Src)[yyi];                                                 \
  } while (0)
#endif
#endif
#endif /* !YYCOPY_NEEDED */

/* YYFINAL -- State number of the termination state.  */
#define YYFINAL 2
/* YYLAST -- Last index in YYTABLE.  */
#define YYLAST 250

/* YYNTOKENS -- Number of terminals.  */
#define YYNTOKENS 40
/* YYNNTS -- Number of nonterminals.  */
#define YYNNTS 35
/* YYNRULES -- Number of rules.  */
#define YYNRULES 96
/* YYNSTATES -- Number of states.  */
#define YYNSTATES 188

/* YYMAXUTOK -- Last valid token kind.  */
#define YYMAXUTOK 279

/* YYTRANSLATE(TOKEN-NUM) -- Symbol number corresponding to TOKEN-NUM
   as returned by yylex, with out-of-bounds checking.  */
#define YYTRANSLATE(YYX)                                                       \
  (0 <= (YYX) && (YYX) <= YYMAXUTOK                                            \
     ? YY_CAST(yysymbol_kind_t, yytranslate[YYX])                              \
     : YYSYMBOL_YYUNDEF)

/* YYTRANSLATE[TOKEN-NUM] -- Symbol number corresponding to TOKEN-NUM
   as returned by yylex.  */
static const yytype_int8 yytranslate[] = {
  0,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2, 2,  2,  2, 2,  2,
  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2, 36, 2,  2, 2,  39,
  2,  2,  32, 33, 37, 34, 26, 35, 2,  38, 2,  2,  2,  2, 2,  2,  2, 2,  2,
  2,  2,  25, 2,  27, 2,  2,  2,  2,  2,  2,  2,  2,  2, 2,  2,  2, 2,  2,
  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2, 2,  28, 2, 29, 2,
  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2, 2,  2,  2, 2,  2,
  2,  2,  2,  2,  2,  2,  2,  2,  2,  30, 2,  31, 2,  2, 2,  2,  2, 2,  2,
  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2, 2,  2,  2, 2,  2,
  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2, 2,  2,  2, 2,  2,
  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2, 2,  2,  2, 2,  2,
  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2, 2,  2,  2, 2,  2,
  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2, 2,  2,  2, 2,  2,
  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2, 2,  2,  2, 2,  2,
  2,  2,  2,  2,  2,  2,  2,  2,  2,  1,  2,  3,  4,  5, 6,  7,  8, 9,  10,
  11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24
};

#if YYDEBUG
/* YYRLINE[YYN] -- Source line where rule number YYN was defined.  */
static const yytype_int16 yyrline[] = {
  0,   265, 265, 266, 270, 271, 275, 276, 280, 281, 285, 286, 290, 291,
  297, 298, 302, 303, 307, 308, 309, 310, 316, 317, 321, 322, 323, 327,
  328, 332, 337, 342, 347, 350, 353, 359, 360, 364, 365, 369, 370, 374,
  382, 383, 387, 388, 392, 393, 397, 400, 401, 402, 403, 406, 412, 415,
  416, 417, 418, 424, 428, 432, 433, 437, 438, 442, 443, 444, 445, 449,
  450, 451, 452, 453, 454, 458, 459, 463, 464, 465, 466, 470, 471, 472,
  476, 477, 478, 479, 480, 484, 485, 486, 490, 491, 495, 496, 500
};
#endif

/** Accessing symbol of state STATE.  */
#define YY_ACCESSING_SYMBOL(State) YY_CAST(yysymbol_kind_t, yystos[State])

#if YYDEBUG || 0
/* The user-facing name of the symbol whose (internal) number is
   YYSYMBOL.  No bounds checking.  */
static const char *yysymbol_name(yysymbol_kind_t yysymbol) YY_ATTRIBUTE_UNUSED;

/* YYTNAME[SYMBOL-NUM] -- String name of the symbol SYMBOL-NUM.
   First, the terminals, then, starting at YYNTOKENS, nonterminals.  */
static const char *const yytname[] = {
  "\"end of file\"",
  "error",
  "\"invalid token\"",
  "IDENT",
  "INT_CONST",
  "FLOAT_CONST",
  "INT",
  "FLOAT",
  "VOID",
  "CONST",
  "RETURN",
  "IF",
  "ELSE",
  "WHILE",
  "BREAK",
  "CONTINUE",
  "AND",
  "OR",
  "LE",
  "GE",
  "EQ",
  "NE",
  "LT",
  "GT",
  "THEN",
  "';'",
  "','",
  "'='",
  "'['",
  "']'",
  "'{'",
  "'}'",
  "'('",
  "')'",
  "'+'",
  "'-'",
  "'!'",
  "'*'",
  "'/'",
  "'%'",
  "$accept",
  "CompUnit",
  "GlobalItem",
  "Decl",
  "ConstDecl",
  "ConstDefList",
  "ConstDef",
  "VarDecl",
  "VarDefList",
  "VarDef",
  "ConstArrayDims",
  "InitVal",
  "InitValList",
  "FuncDef",
  "FuncFParams",
  "FuncFParam",
  "FuncParamDims",
  "Block",
  "BlockItems",
  "BlockItem",
  "Stmt",
  "Exp",
  "Cond",
  "LVal",
  "ArrayIndices",
  "PrimaryExp",
  "UnaryExp",
  "FuncArgList",
  "MulExp",
  "AddExp",
  "RelExp",
  "EqExp",
  "LAndExp",
  "LOrExp",
  "ConstExp",
  YY_NULLPTR
};

static const char *yysymbol_name(yysymbol_kind_t yysymbol) {
  return yytname[yysymbol];
}
#endif

#define YYPACT_NINF (-97)

#define yypact_value_is_default(Yyn) ((Yyn) == YYPACT_NINF)

#define YYTABLE_NINF (-1)

#define yytable_value_is_error(Yyn) 0

/* YYPACT[STATE-NUM] -- Index in YYTABLE of the portion describing
   STATE-NUM.  */
static const yytype_int16 yypact[] = {
  -97, 17,  -97, 12,  73,  101, 82,  -97, -97, -97, -97, -97, -22, 83,  -97,
  104, 93,  38,  118, 118, 24,  191, 2,   102, -97, 144, 15,  -97, 59,  111,
  115, -97, 130, 29,  -97, -97, 37,  191, 191, 191, 191, -97, -97, -97, -97,
  -97, 8,   132, 56,  159, 117, 142, -97, 134, 169, 182, 138, 10,  -97, 24,
  191, 154, -97, 138, 11,  138, 58,  24,  165, -97, 118, -97, 191, 78,  176,
  -97, -97, 89,  164, -97, -97, -97, 191, 191, 191, 191, 191, 191, 191, 191,
  191, 191, 191, 191, 191, -97, 178, 178, 92,  -97, 193, 138, -97, 181, -97,
  138, -97, 138, -97, 24,  -97, 187, -97, -97, 67,  191, 24,  -97, -97, -97,
  -97, -97, 8,   8,   132, 132, 132, 132, 56,  56,  159, 117, 188, -97, -97,
  144, 144, 186, 192, 196, 194, 204, -97, -97, -97, -97, 139, -97, -97, 205,
  206, -97, -97, -97, -97, -97, -97, -97, 191, -97, 202, -97, 207, -97, 209,
  191, 191, -97, -97, -97, -97, -97, 191, -97, -97, 208, -97, 199, 142, 210,
  212, 173, 173, -97, 226, -97, 173, -97
};

/* YYDEFACT[STATE-NUM] -- Default reduction number in state STATE-NUM.
   Performed when YYTABLE does not specify something else to do.  Zero
   means the default is an error.  */
static const yytype_int8 yydefact[] = {
  2,  0,  1,  0,  0,  0,  0,  3,  4,  6,  7,  5,  18, 0,  16, 18, 0,  0,  0,
  0,  0,  0,  0,  20, 14, 0,  0,  15, 0,  0,  0,  10, 0,  61, 67, 68, 0,  0,
  0,  0,  0,  19, 24, 66, 69, 77, 81, 84, 89, 92, 94, 59, 96, 0,  0,  0,  0,
  0,  35, 0,  0,  18, 17, 0,  0,  0,  0,  0,  0,  8,  0,  9,  0,  0,  62, 25,
  27, 0,  0,  72, 73, 74, 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  22, 39, 39, 0,  29, 0,  0,  21, 0,  30, 0,  31, 0,  12, 0,  11, 0,  70, 75,
  0,  0,  0,  26, 65, 78, 79, 80, 82, 83, 87, 88, 85, 86, 90, 91, 93, 95, 0,
  37, 38, 0,  0,  0,  0,  0,  0,  0,  50, 42, 46, 51, 0,  44, 47, 0,  66, 36,
  32, 23, 33, 34, 13, 63, 0,  71, 0,  28, 40, 57, 0,  0,  0,  55, 56, 43, 45,
  49, 0,  76, 64, 41, 58, 0,  60, 0,  0,  0,  0,  48, 52, 54, 0,  53
};

/* YYPGOTO[NTERM-NUM].  */
static const yytype_int16 yypgoto[] = {-97, -97, -97, 238, -97, 221, 171,
                                       -97, -1,  217, -28, -29, -97, -97,
                                       -14, 145, 147, -52, -97, 100, -24,
                                       -21, 81,  -96, -97, -97, -20, -97,
                                       116, -56, 121, 155, 156, 49,  189};

/* YYDEFGOTO[NTERM-NUM].  */
static const yytype_uint8 yydefgoto[] = {0,  1,   7,   144, 9,   30,  31,
                                         10, 13,  14,  23,  41,  77,  11,
                                         57, 58,  133, 145, 146, 147, 148,
                                         42, 177, 43,  74,  44,  45,  114,
                                         46, 47,  48,  49,  50,  51,  53};

/* YYTABLE[YYPACT[STATE-NUM]] -- What to do in state STATE-NUM.  If
   positive, shift that token.  If negative, reduce the rule whose
   number is the opposite.  If YYTABLE_NINF, syntax error.  */
static const yytype_uint8 yytable[] = {
  52,  68,  150, 16,  99,  20,  21,  76,  54,  55,  22,  104, 64,  106, 66,
  12,  78,  2,   79,  80,  81,  54,  55,  3,   4,   5,   6,   33,  34,  35,
  102, 124, 125, 126, 127, 56,  100, 100, 108, 52,  33,  34,  35,  101, 105,
  82,  83,  84,  63,  152, 150, 111, 113, 154, 36,  155, 37,  72,  38,  39,
  40,  73,  119, 120, 121, 54,  55,  36,  75,  37,  28,  38,  39,  40,  87,
  88,  15,  149, 89,  90,  156, 33,  34,  35,  100, 150, 150, 161, 18,  19,
  150, 107, 65,  158, 160, 33,  34,  35,  135, 136, 159, 6,   137, 138, 17,
  139, 140, 141, 24,  25,  37,  112, 38,  39,  40,  116, 164, 142, 27,  25,
  117, 29,  98,  143, 37,  149, 38,  39,  40,  59,  60,  20,  21,  93,  175,
  16,  26,  173, 67,  21,  69,  70,  33,  34,  35,  135, 136, 61,  6,   137,
  138, 180, 139, 140, 141, 71,  70,  184, 185, 94,  149, 149, 187, 95,  142,
  149, 85,  86,  98,  98,  169, 37,  96,  38,  39,  40,  33,  34,  35,  91,
  92,  20,  21,  137, 138, 97,  139, 140, 141, 33,  34,  35,  109, 60,  33,
  34,  35,  118, 142, 54,  55,  122, 123, 98,  115, 37,  132, 38,  39,  40,
  153, 163, 128, 129, 178, 178, 157, 162, 37,  167, 38,  39,  40,  37,  165,
  38,  39,  40,  166, 168, 171, 174, 181, 172, 176, 21,  60,  183, 186, 8,
  32,  110, 62,  182, 134, 151, 170, 179, 130, 103, 131
};

static const yytype_uint8 yycheck[] = {
  21,  29,  98,  4,  56,  27,  28,  36,  6,  7,   32,  63, 26, 65,  28,  3,
  37,  0,   38,  39, 40,  6,   7,   6,   7,  8,   9,   3,  4,  5,   59,  87,
  88,  89,  90,  33, 26,  26,  67,  60,  3,  4,   5,   33, 33, 37,  38,  39,
  33,  101, 146, 72, 73,  105, 30,  107, 32, 28,  34,  35, 36, 32,  82,  83,
  84,  6,   7,   30, 31,  32,  32,  34,  35, 36,  18,  19, 3,  98,  22,  23,
  109, 3,   4,   5,  26,  181, 182, 116, 6,  7,   186, 33, 33, 26,  115, 3,
  4,   5,   6,   7,  33,  9,   10,  11,  3,  13,  14,  15, 25, 26,  32,  33,
  34,  35,  36,  26, 137, 25,  25,  26,  31, 3,   30,  31, 32, 146, 34,  35,
  36,  27,  28,  27, 28,  16,  162, 136, 32, 158, 27,  28, 25, 26,  3,   4,
  5,   6,   7,   3,  9,   10,  11,  172, 13, 14,  15,  25, 26, 181, 182, 17,
  181, 182, 186, 29, 25,  186, 34,  35,  30, 30,  31,  32, 3,  34,  35,  36,
  3,   4,   5,   20, 21,  27,  28,  10,  11, 3,   13,  14, 15, 3,   4,   5,
  27,  28,  3,   4,  5,   33,  25,  6,   7,  85,  86,  30, 28, 32,  28,  34,
  35,  36,  29,  25, 91,  92,  165, 166, 29, 29,  32,  25, 34, 35,  36,  32,
  32,  34,  35,  36, 32,  25,  25,  29,  33, 27,  25,  28, 28, 25,  12,  1,
  19,  70,  25,  33, 97,  100, 146, 166, 93, 60,  94
};

/* YYSTOS[STATE-NUM] -- The symbol kind of the accessing symbol of
   state STATE-NUM.  */
static const yytype_int8 yystos[] = {
  0,  41, 0,  6,  7,  8,  9,  42, 43, 44, 47, 53, 3,  48, 49, 3,  48, 3,  6,
  7,  27, 28, 32, 50, 25, 26, 32, 25, 32, 3,  45, 46, 45, 3,  4,  5,  30, 32,
  34, 35, 36, 51, 61, 63, 65, 66, 68, 69, 70, 71, 72, 73, 61, 74, 6,  7,  33,
  54, 55, 27, 28, 3,  49, 33, 54, 33, 54, 27, 50, 25, 26, 25, 28, 32, 64, 31,
  51, 52, 61, 66, 66, 66, 37, 38, 39, 34, 35, 18, 19, 22, 23, 20, 21, 16, 17,
  29, 3,  3,  30, 57, 26, 33, 51, 74, 57, 33, 57, 33, 51, 27, 46, 61, 33, 61,
  67, 28, 26, 31, 33, 66, 66, 66, 68, 68, 69, 69, 69, 69, 70, 70, 71, 72, 28,
  56, 56, 6,  7,  10, 11, 13, 14, 15, 25, 31, 43, 57, 58, 59, 60, 61, 63, 55,
  57, 29, 57, 57, 51, 29, 26, 33, 61, 51, 29, 25, 61, 32, 32, 25, 25, 31, 59,
  25, 27, 61, 29, 50, 25, 62, 73, 62, 61, 33, 33, 25, 60, 60, 12, 60
};

/* YYR1[RULE-NUM] -- Symbol kind of the left-hand side of rule RULE-NUM.  */
static const yytype_int8 yyr1[] = {
  0,  40, 41, 41, 42, 42, 43, 43, 44, 44, 45, 45, 46, 46, 47, 47, 48,
  48, 49, 49, 49, 49, 50, 50, 51, 51, 51, 52, 52, 53, 53, 53, 53, 53,
  53, 54, 54, 55, 55, 56, 56, 56, 57, 57, 58, 58, 59, 59, 60, 60, 60,
  60, 60, 60, 60, 60, 60, 60, 60, 61, 62, 63, 63, 64, 64, 65, 65, 65,
  65, 66, 66, 66, 66, 66, 66, 67, 67, 68, 68, 68, 68, 69, 69, 69, 70,
  70, 70, 70, 70, 71, 71, 71, 72, 72, 73, 73, 74
};

/* YYR2[RULE-NUM] -- Number of symbols on the right-hand side of rule RULE-NUM.
 */
static const yytype_int8 yyr2[] = {
  0, 2, 0, 2, 1, 1, 1, 1, 4, 4, 1, 3, 3, 4, 3, 3, 1, 3, 1, 3, 2, 4, 3, 4, 1,
  2, 3, 1, 3, 5, 5, 5, 6, 6, 6, 1, 3, 3, 3, 0, 2, 3, 2, 3, 1, 2, 1, 1, 4, 2,
  1, 1, 5, 7, 5, 2, 2, 2, 3, 1, 1, 1, 2, 3, 4, 3, 1, 1, 1, 1, 3, 4, 2, 2, 2,
  1, 3, 1, 3, 3, 3, 1, 3, 3, 1, 3, 3, 3, 3, 1, 3, 3, 1, 3, 1, 3, 1
};

enum { YYENOMEM = -2 };

#define yyerrok (yyerrstatus = 0)
#define yyclearin (yychar = YYEMPTY)

#define YYACCEPT goto yyacceptlab
#define YYABORT goto yyabortlab
#define YYERROR goto yyerrorlab
#define YYNOMEM goto yyexhaustedlab

#define YYRECOVERING() (!!yyerrstatus)

#define YYBACKUP(Token, Value)                                                 \
  do                                                                           \
    if (yychar == YYEMPTY) {                                                   \
      yychar = (Token);                                                        \
      yylval = (Value);                                                        \
      YYPOPSTACK(yylen);                                                       \
      yystate = *yyssp;                                                        \
      goto yybackup;                                                           \
    } else {                                                                   \
      yyerror(ast, YY_("syntax error: cannot back up"));                       \
      YYERROR;                                                                 \
    }                                                                          \
  while (0)

/* Backward compatibility with an undocumented macro.
   Use YYerror or YYUNDEF. */
#define YYERRCODE YYUNDEF

/* YYLLOC_DEFAULT -- Set CURRENT to span from RHS[1] to RHS[N].
   If N is 0, then set CURRENT to the empty location which ends
   the previous symbol: RHS[0] (always defined).  */

#ifndef YYLLOC_DEFAULT
#define YYLLOC_DEFAULT(Current, Rhs, N)                                        \
  do                                                                           \
    if (N) {                                                                   \
      (Current).first_line = YYRHSLOC(Rhs, 1).first_line;                      \
      (Current).first_column = YYRHSLOC(Rhs, 1).first_column;                  \
      (Current).last_line = YYRHSLOC(Rhs, N).last_line;                        \
      (Current).last_column = YYRHSLOC(Rhs, N).last_column;                    \
    } else {                                                                   \
      (Current).first_line = (Current).last_line = YYRHSLOC(Rhs, 0).last_line; \
      (Current).first_column = (Current).last_column =                         \
        YYRHSLOC(Rhs, 0).last_column;                                          \
    }                                                                          \
  while (0)
#endif

#define YYRHSLOC(Rhs, K) ((Rhs)[K])

/* Enable debugging if requested.  */
#if YYDEBUG

#ifndef YYFPRINTF
#include <stdio.h> /* INFRINGES ON USER NAME SPACE */
#define YYFPRINTF fprintf
#endif

#define YYDPRINTF(Args)                                                        \
  do {                                                                         \
    if (yydebug)                                                               \
      YYFPRINTF Args;                                                          \
  } while (0)

/* YYLOCATION_PRINT -- Print the location on the stream.
   This macro was not mandated originally: define only if we know
   we won't break user code: when these are the locations we know.  */

#ifndef YYLOCATION_PRINT

#if defined YY_LOCATION_PRINT

/* Temporary convenience wrapper in case some people defined the
   undocumented and private YY_LOCATION_PRINT macros.  */
#define YYLOCATION_PRINT(File, Loc) YY_LOCATION_PRINT(File, *(Loc))

#elif defined YYLTYPE_IS_TRIVIAL && YYLTYPE_IS_TRIVIAL

/* Print *YYLOCP on YYO.  Private, do not rely on its existence. */

YY_ATTRIBUTE_UNUSED
static int yy_location_print_(FILE *yyo, YYLTYPE const *const yylocp) {
  int res = 0;
  int end_col = 0 != yylocp->last_column ? yylocp->last_column - 1 : 0;
  if (0 <= yylocp->first_line) {
    res += YYFPRINTF(yyo, "%d", yylocp->first_line);
    if (0 <= yylocp->first_column)
      res += YYFPRINTF(yyo, ".%d", yylocp->first_column);
  }
  if (0 <= yylocp->last_line) {
    if (yylocp->first_line < yylocp->last_line) {
      res += YYFPRINTF(yyo, "-%d", yylocp->last_line);
      if (0 <= end_col)
        res += YYFPRINTF(yyo, ".%d", end_col);
    } else if (0 <= end_col && yylocp->first_column < end_col)
      res += YYFPRINTF(yyo, "-%d", end_col);
  }
  return res;
}

#define YYLOCATION_PRINT yy_location_print_

/* Temporary convenience wrapper in case some people defined the
   undocumented and private YY_LOCATION_PRINT macros.  */
#define YY_LOCATION_PRINT(File, Loc) YYLOCATION_PRINT(File, &(Loc))

#else

#define YYLOCATION_PRINT(File, Loc) ((void)0)
/* Temporary convenience wrapper in case some people defined the
   undocumented and private YY_LOCATION_PRINT macros.  */
#define YY_LOCATION_PRINT YYLOCATION_PRINT

#endif
#endif /* !defined YYLOCATION_PRINT */

#define YY_SYMBOL_PRINT(Title, Kind, Value, Location)                          \
  do {                                                                         \
    if (yydebug) {                                                             \
      YYFPRINTF(stderr, "%s ", Title);                                         \
      yy_symbol_print(stderr, Kind, Value, Location, ast);                     \
      YYFPRINTF(stderr, "\n");                                                 \
    }                                                                          \
  } while (0)

/*-----------------------------------.
| Print this symbol's value on YYO.  |
`-----------------------------------*/

static void yy_symbol_value_print(
  FILE *yyo,
  yysymbol_kind_t yykind,
  YYSTYPE const *const yyvaluep,
  YYLTYPE const *const yylocationp,
  exodus::ast::CompUnitAST &ast
) {
  FILE *yyoutput = yyo;
  YY_USE(yyoutput);
  YY_USE(yylocationp);
  YY_USE(ast);
  if (!yyvaluep)
    return;
  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  YY_USE(yykind);
  YY_IGNORE_MAYBE_UNINITIALIZED_END
}

/*---------------------------.
| Print this symbol on YYO.  |
`---------------------------*/

static void yy_symbol_print(
  FILE *yyo,
  yysymbol_kind_t yykind,
  YYSTYPE const *const yyvaluep,
  YYLTYPE const *const yylocationp,
  exodus::ast::CompUnitAST &ast
) {
  YYFPRINTF(
    yyo,
    "%s %s (",
    yykind < YYNTOKENS ? "token" : "nterm",
    yysymbol_name(yykind)
  );

  YYLOCATION_PRINT(yyo, yylocationp);
  YYFPRINTF(yyo, ": ");
  yy_symbol_value_print(yyo, yykind, yyvaluep, yylocationp, ast);
  YYFPRINTF(yyo, ")");
}

/*------------------------------------------------------------------.
| yy_stack_print -- Print the state stack from its BOTTOM up to its |
| TOP (included).                                                   |
`------------------------------------------------------------------*/

static void yy_stack_print(yy_state_t *yybottom, yy_state_t *yytop) {
  YYFPRINTF(stderr, "Stack now");
  for (; yybottom <= yytop; yybottom++) {
    int yybot = *yybottom;
    YYFPRINTF(stderr, " %d", yybot);
  }
  YYFPRINTF(stderr, "\n");
}

#define YY_STACK_PRINT(Bottom, Top)                                            \
  do {                                                                         \
    if (yydebug)                                                               \
      yy_stack_print((Bottom), (Top));                                         \
  } while (0)

/*------------------------------------------------.
| Report that the YYRULE is going to be reduced.  |
`------------------------------------------------*/

static void yy_reduce_print(
  yy_state_t *yyssp,
  YYSTYPE *yyvsp,
  YYLTYPE *yylsp,
  int yyrule,
  exodus::ast::CompUnitAST &ast
) {
  int yylno = yyrline[yyrule];
  int yynrhs = yyr2[yyrule];
  int yyi;
  YYFPRINTF(
    stderr, "Reducing stack by rule %d (line %d):\n", yyrule - 1, yylno
  );
  /* The symbols being reduced.  */
  for (yyi = 0; yyi < yynrhs; yyi++) {
    YYFPRINTF(stderr, "   $%d = ", yyi + 1);
    yy_symbol_print(
      stderr,
      YY_ACCESSING_SYMBOL(+yyssp[yyi + 1 - yynrhs]),
      &yyvsp[(yyi + 1) - (yynrhs)],
      &(yylsp[(yyi + 1) - (yynrhs)]),
      ast
    );
    YYFPRINTF(stderr, "\n");
  }
}

#define YY_REDUCE_PRINT(Rule)                                                  \
  do {                                                                         \
    if (yydebug)                                                               \
      yy_reduce_print(yyssp, yyvsp, yylsp, Rule, ast);                         \
  } while (0)

/* Nonzero means print parse trace.  It is left uninitialized so that
   multiple parsers can coexist.  */
int yydebug;
#else /* !YYDEBUG */
#define YYDPRINTF(Args) ((void)0)
#define YY_SYMBOL_PRINT(Title, Kind, Value, Location)
#define YY_STACK_PRINT(Bottom, Top)
#define YY_REDUCE_PRINT(Rule)
#endif /* !YYDEBUG */

/* YYINITDEPTH -- initial size of the parser's stacks.  */
#ifndef YYINITDEPTH
#define YYINITDEPTH 200
#endif

/* YYMAXDEPTH -- maximum size the stacks can grow to (effective only
   if the built-in stack extension method is used).

   Do not make this value too large; the results are undefined if
   YYSTACK_ALLOC_MAXIMUM < YYSTACK_BYTES (YYMAXDEPTH)
   evaluated with infinite-precision integer arithmetic.  */

#ifndef YYMAXDEPTH
#define YYMAXDEPTH 10000
#endif

/*-----------------------------------------------.
| Release the memory associated to this symbol.  |
`-----------------------------------------------*/

static void yydestruct(
  const char *yymsg,
  yysymbol_kind_t yykind,
  YYSTYPE *yyvaluep,
  YYLTYPE *yylocationp,
  exodus::ast::CompUnitAST &ast
) {
  YY_USE(yyvaluep);
  YY_USE(yylocationp);
  YY_USE(ast);
  if (!yymsg)
    yymsg = "Deleting";
  YY_SYMBOL_PRINT(yymsg, yykind, yyvaluep, yylocationp);

  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  switch (yykind) {
  case YYSYMBOL_IDENT: /* IDENT  */
#line 253 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    delete ((*yyvaluep).str);
  }
#line 1282 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case YYSYMBOL_Decl: /* Decl  */
#line 253 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    delete ((*yyvaluep).decl);
  }
#line 1288 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case YYSYMBOL_ConstDecl: /* ConstDecl  */
#line 253 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    delete ((*yyvaluep).decl);
  }
#line 1294 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case YYSYMBOL_ConstDefList: /* ConstDefList  */
#line 255 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    delete ((*yyvaluep).var_defs);
  }
#line 1300 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case YYSYMBOL_ConstDef: /* ConstDef  */
#line 254 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    delete ((*yyvaluep).var_def);
  }
#line 1306 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case YYSYMBOL_VarDecl: /* VarDecl  */
#line 253 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    delete ((*yyvaluep).decl);
  }
#line 1312 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case YYSYMBOL_VarDefList: /* VarDefList  */
#line 255 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    delete ((*yyvaluep).var_defs);
  }
#line 1318 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case YYSYMBOL_VarDef: /* VarDef  */
#line 254 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    delete ((*yyvaluep).var_def);
  }
#line 1324 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case YYSYMBOL_ConstArrayDims: /* ConstArrayDims  */
#line 255 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    delete ((*yyvaluep).exprs);
  }
#line 1330 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case YYSYMBOL_InitVal: /* InitVal  */
#line 253 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    delete ((*yyvaluep).init);
  }
#line 1336 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case YYSYMBOL_InitValList: /* InitValList  */
#line 255 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    delete ((*yyvaluep).inits);
  }
#line 1342 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case YYSYMBOL_FuncDef: /* FuncDef  */
#line 254 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    delete ((*yyvaluep).func_def);
  }
#line 1348 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case YYSYMBOL_FuncFParams: /* FuncFParams  */
#line 255 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    delete ((*yyvaluep).params);
  }
#line 1354 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case YYSYMBOL_FuncFParam: /* FuncFParam  */
#line 254 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    delete ((*yyvaluep).param);
  }
#line 1360 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case YYSYMBOL_FuncParamDims: /* FuncParamDims  */
#line 255 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    delete ((*yyvaluep).exprs);
  }
#line 1366 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case YYSYMBOL_Block: /* Block  */
#line 254 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    delete ((*yyvaluep).block);
  }
#line 1372 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case YYSYMBOL_BlockItems: /* BlockItems  */
#line 255 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    delete ((*yyvaluep).block_items);
  }
#line 1378 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case YYSYMBOL_BlockItem: /* BlockItem  */
#line 253 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    delete ((*yyvaluep).block_item);
  }
#line 1384 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case YYSYMBOL_Stmt: /* Stmt  */
#line 253 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    delete ((*yyvaluep).stmt);
  }
#line 1390 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case YYSYMBOL_Exp: /* Exp  */
#line 253 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    delete ((*yyvaluep).expr);
  }
#line 1396 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case YYSYMBOL_Cond: /* Cond  */
#line 253 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    delete ((*yyvaluep).expr);
  }
#line 1402 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case YYSYMBOL_LVal: /* LVal  */
#line 254 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    delete ((*yyvaluep).lval);
  }
#line 1408 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case YYSYMBOL_ArrayIndices: /* ArrayIndices  */
#line 255 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    delete ((*yyvaluep).exprs);
  }
#line 1414 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case YYSYMBOL_PrimaryExp: /* PrimaryExp  */
#line 253 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    delete ((*yyvaluep).expr);
  }
#line 1420 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case YYSYMBOL_UnaryExp: /* UnaryExp  */
#line 253 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    delete ((*yyvaluep).expr);
  }
#line 1426 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case YYSYMBOL_FuncArgList: /* FuncArgList  */
#line 255 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    delete ((*yyvaluep).exprs);
  }
#line 1432 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case YYSYMBOL_MulExp: /* MulExp  */
#line 253 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    delete ((*yyvaluep).expr);
  }
#line 1438 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case YYSYMBOL_AddExp: /* AddExp  */
#line 253 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    delete ((*yyvaluep).expr);
  }
#line 1444 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case YYSYMBOL_RelExp: /* RelExp  */
#line 253 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    delete ((*yyvaluep).expr);
  }
#line 1450 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case YYSYMBOL_EqExp: /* EqExp  */
#line 253 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    delete ((*yyvaluep).expr);
  }
#line 1456 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case YYSYMBOL_LAndExp: /* LAndExp  */
#line 253 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    delete ((*yyvaluep).expr);
  }
#line 1462 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case YYSYMBOL_LOrExp: /* LOrExp  */
#line 253 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    delete ((*yyvaluep).expr);
  }
#line 1468 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case YYSYMBOL_ConstExp: /* ConstExp  */
#line 253 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    delete ((*yyvaluep).expr);
  }
#line 1474 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  default:
    break;
  }
  YY_IGNORE_MAYBE_UNINITIALIZED_END
}

/* Lookahead token kind.  */
int yychar;

/* The semantic value of the lookahead symbol.  */
YYSTYPE yylval;
/* Location data for the lookahead symbol.  */
YYLTYPE yylloc
#if defined YYLTYPE_IS_TRIVIAL && YYLTYPE_IS_TRIVIAL
  = {1, 1, 1, 1}
#endif
;
/* Number of syntax errors so far.  */
int yynerrs;

/*----------.
| yyparse.  |
`----------*/

int yyparse(exodus::ast::CompUnitAST &ast) {
  yy_state_fast_t yystate = 0;
  /* Number of tokens to shift before error messages enabled.  */
  int yyerrstatus = 0;

  /* Refer to the stacks through separate pointers, to allow yyoverflow
     to reallocate them elsewhere.  */

  /* Their size.  */
  YYPTRDIFF_T yystacksize = YYINITDEPTH;

  /* The state stack: array, bottom, top.  */
  yy_state_t yyssa[YYINITDEPTH];
  yy_state_t *yyss = yyssa;
  yy_state_t *yyssp = yyss;

  /* The semantic value stack: array, bottom, top.  */
  YYSTYPE yyvsa[YYINITDEPTH];
  YYSTYPE *yyvs = yyvsa;
  YYSTYPE *yyvsp = yyvs;

  /* The location stack: array, bottom, top.  */
  YYLTYPE yylsa[YYINITDEPTH];
  YYLTYPE *yyls = yylsa;
  YYLTYPE *yylsp = yyls;

  int yyn;
  /* The return value of yyparse.  */
  int yyresult;
  /* Lookahead symbol kind.  */
  yysymbol_kind_t yytoken = YYSYMBOL_YYEMPTY;
  /* The variables used to return semantic value and location from the
     action routines.  */
  YYSTYPE yyval;
  YYLTYPE yyloc;

  /* The locations where the error started and ended.  */
  YYLTYPE yyerror_range[3];

#define YYPOPSTACK(N) (yyvsp -= (N), yyssp -= (N), yylsp -= (N))

  /* The number of symbols on the RHS of the reduced rule.
     Keep to zero when no symbol should be popped.  */
  int yylen = 0;

  YYDPRINTF((stderr, "Starting parse\n"));

  yychar = YYEMPTY; /* Cause a token to be read.  */

  yylsp[0] = yylloc;
  goto yysetstate;

/*------------------------------------------------------------.
| yynewstate -- push a new state, which is found in yystate.  |
`------------------------------------------------------------*/
yynewstate:
  /* In all cases, when you get here, the value and location stacks
     have just been pushed.  So pushing a state here evens the stacks.  */
  yyssp++;

/*--------------------------------------------------------------------.
| yysetstate -- set current state (the top of the stack) to yystate.  |
`--------------------------------------------------------------------*/
yysetstate:
  YYDPRINTF((stderr, "Entering state %d\n", yystate));
  YY_ASSERT(0 <= yystate && yystate < YYNSTATES);
  YY_IGNORE_USELESS_CAST_BEGIN
  *yyssp = YY_CAST(yy_state_t, yystate);
  YY_IGNORE_USELESS_CAST_END
  YY_STACK_PRINT(yyss, yyssp);

  if (yyss + yystacksize - 1 <= yyssp)
#if !defined yyoverflow && !defined YYSTACK_RELOCATE
    YYNOMEM;
#else
  {
    /* Get the current used size of the three stacks, in elements.  */
    YYPTRDIFF_T yysize = yyssp - yyss + 1;

#if defined yyoverflow
    {
      /* Give user a chance to reallocate the stack.  Use copies of
         these so that the &'s don't force the real ones into
         memory.  */
      yy_state_t *yyss1 = yyss;
      YYSTYPE *yyvs1 = yyvs;
      YYLTYPE *yyls1 = yyls;

      /* Each stack pointer address is followed by the size of the
         data in use in that stack, in bytes.  This used to be a
         conditional around just the two extra args, but that might
         be undefined if yyoverflow is a macro.  */
      yyoverflow(
        YY_("memory exhausted"),
        &yyss1,
        yysize * YYSIZEOF(*yyssp),
        &yyvs1,
        yysize * YYSIZEOF(*yyvsp),
        &yyls1,
        yysize * YYSIZEOF(*yylsp),
        &yystacksize
      );
      yyss = yyss1;
      yyvs = yyvs1;
      yyls = yyls1;
    }
#else /* defined YYSTACK_RELOCATE */
    /* Extend the stack our own way.  */
    if (YYMAXDEPTH <= yystacksize)
      YYNOMEM;
    yystacksize *= 2;
    if (YYMAXDEPTH < yystacksize)
      yystacksize = YYMAXDEPTH;

    {
      yy_state_t *yyss1 = yyss;
      union yyalloc *yyptr = YY_CAST(
        union yyalloc *,
        YYSTACK_ALLOC(YY_CAST(YYSIZE_T, YYSTACK_BYTES(yystacksize)))
      );
      if (!yyptr)
        YYNOMEM;
      YYSTACK_RELOCATE(yyss_alloc, yyss);
      YYSTACK_RELOCATE(yyvs_alloc, yyvs);
      YYSTACK_RELOCATE(yyls_alloc, yyls);
#undef YYSTACK_RELOCATE
      if (yyss1 != yyssa)
        YYSTACK_FREE(yyss1);
    }
#endif

    yyssp = yyss + yysize - 1;
    yyvsp = yyvs + yysize - 1;
    yylsp = yyls + yysize - 1;

    YY_IGNORE_USELESS_CAST_BEGIN
    YYDPRINTF(
      (stderr, "Stack size increased to %ld\n", YY_CAST(long, yystacksize))
    );
    YY_IGNORE_USELESS_CAST_END

    if (yyss + yystacksize - 1 <= yyssp)
      YYABORT;
  }
#endif /* !defined yyoverflow && !defined YYSTACK_RELOCATE */

  if (yystate == YYFINAL)
    YYACCEPT;

  goto yybackup;

/*-----------.
| yybackup.  |
`-----------*/
yybackup:
  /* Do appropriate processing given the current state.  Read a
     lookahead token if we need one and don't already have one.  */

  /* First try to decide what to do without reference to lookahead token.  */
  yyn = yypact[yystate];
  if (yypact_value_is_default(yyn))
    goto yydefault;

  /* Not known => get a lookahead token if don't already have one.  */

  /* YYCHAR is either empty, or end-of-input, or a valid lookahead.  */
  if (yychar == YYEMPTY) {
    YYDPRINTF((stderr, "Reading a token\n"));
    yychar = yylex();
  }

  if (yychar <= YYEOF) {
    yychar = YYEOF;
    yytoken = YYSYMBOL_YYEOF;
    YYDPRINTF((stderr, "Now at end of input.\n"));
  } else if (yychar == YYerror) {
    /* The scanner already issued an error message, process directly
       to error recovery.  But do not keep the error token as
       lookahead, it is too special and may lead us to an endless
       loop in error recovery. */
    yychar = YYUNDEF;
    yytoken = YYSYMBOL_YYerror;
    yyerror_range[1] = yylloc;
    goto yyerrlab1;
  } else {
    yytoken = YYTRANSLATE(yychar);
    YY_SYMBOL_PRINT("Next token is", yytoken, &yylval, &yylloc);
  }

  /* If the proper action on seeing token YYTOKEN is to reduce or to
     detect an error, take that action.  */
  yyn += yytoken;
  if (yyn < 0 || YYLAST < yyn || yycheck[yyn] != yytoken)
    goto yydefault;
  yyn = yytable[yyn];
  if (yyn <= 0) {
    if (yytable_value_is_error(yyn))
      goto yyerrlab;
    yyn = -yyn;
    goto yyreduce;
  }

  /* Count tokens shifted since error; after three, turn off error
     status.  */
  if (yyerrstatus)
    yyerrstatus--;

  /* Shift the lookahead token.  */
  YY_SYMBOL_PRINT("Shifting", yytoken, &yylval, &yylloc);
  yystate = yyn;
  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  *++yyvsp = yylval;
  YY_IGNORE_MAYBE_UNINITIALIZED_END
  *++yylsp = yylloc;

  /* Discard the shifted token.  */
  yychar = YYEMPTY;
  goto yynewstate;

/*-----------------------------------------------------------.
| yydefault -- do the default action for the current state.  |
`-----------------------------------------------------------*/
yydefault:
  yyn = yydefact[yystate];
  if (yyn == 0)
    goto yyerrlab;
  goto yyreduce;

/*-----------------------------.
| yyreduce -- do a reduction.  |
`-----------------------------*/
yyreduce:
  /* yyn is the number of a rule to reduce with.  */
  yylen = yyr2[yyn];

  /* If YYLEN is nonzero, implement the default value of the action:
     '$$ = $1'.

     Otherwise, the following line sets YYVAL to garbage.
     This behavior is undocumented and Bison
     users should not rely upon it.  Assigning to YYVAL
     unconditionally makes the parser a bit smaller, and it avoids a
     GCC warning that YYVAL may be used uninitialized.  */
  yyval = yyvsp[1 - yylen];

  /* Default location. */
  YYLLOC_DEFAULT(yyloc, (yylsp - yylen), yylen);
  yyerror_range[1] = yyloc;
  YY_REDUCE_PRINT(yyn);
  switch (yyn) {
  case 2: /* CompUnit: %empty  */
#line 265 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    ast.items.clear();
  }
#line 1769 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 4: /* GlobalItem: Decl  */
#line 270 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    ast.items.emplace_back(take((yyvsp[0].decl)));
  }
#line 1775 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 5: /* GlobalItem: FuncDef  */
#line 271 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    ast.items.emplace_back(std::unique_ptr<FuncDefAST>((yyvsp[0].func_def)));
  }
#line 1781 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 6: /* Decl: ConstDecl  */
#line 275 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.decl) = (yyvsp[0].decl);
  }
#line 1787 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 7: /* Decl: VarDecl  */
#line 276 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.decl) = (yyvsp[0].decl);
  }
#line 1793 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 8: /* ConstDecl: CONST INT ConstDefList ';'  */
#line 280 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.decl) = var_decl(I32::get(), (yyvsp[-1].var_defs), true, (yyloc));
  }
#line 1799 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 9: /* ConstDecl: CONST FLOAT ConstDefList ';'  */
#line 281 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.decl) = var_decl(Float::get(), (yyvsp[-1].var_defs), true, (yyloc));
  }
#line 1805 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 10: /* ConstDefList: ConstDef  */
#line 285 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.var_defs) = ptr_list((yyvsp[0].var_def));
  }
#line 1811 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 11: /* ConstDefList: ConstDefList ',' ConstDef  */
#line 286 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.var_defs) = append_ptr((yyvsp[-2].var_defs), (yyvsp[0].var_def));
  }
#line 1817 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 12: /* ConstDef: IDENT '=' InitVal  */
#line 290 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.var_def) = at(
      new VarDefAST(take((yyvsp[-2].str)), {}, opt_init((yyvsp[0].init))),
      (yyloc)
    );
  }
#line 1823 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 13: /* ConstDef: IDENT ConstArrayDims '=' InitVal  */
#line 291 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.var_def) = at(
      new VarDefAST(
        take((yyvsp[-3].str)),
        take((yyvsp[-2].exprs)),
        opt_init((yyvsp[0].init))
      ),
      (yyloc)
    );
  }
#line 1831 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 14: /* VarDecl: INT VarDefList ';'  */
#line 297 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.decl) = var_decl(I32::get(), (yyvsp[-1].var_defs), false, (yyloc));
  }
#line 1837 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 15: /* VarDecl: FLOAT VarDefList ';'  */
#line 298 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.decl) = var_decl(Float::get(), (yyvsp[-1].var_defs), false, (yyloc));
  }
#line 1843 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 16: /* VarDefList: VarDef  */
#line 302 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.var_defs) = ptr_list((yyvsp[0].var_def));
  }
#line 1849 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 17: /* VarDefList: VarDefList ',' VarDef  */
#line 303 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.var_defs) = append_ptr((yyvsp[-2].var_defs), (yyvsp[0].var_def));
  }
#line 1855 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 18: /* VarDef: IDENT  */
#line 307 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.var_def) =
      at(new VarDefAST(take((yyvsp[0].str)), {}, std::nullopt), (yyloc));
  }
#line 1861 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 19: /* VarDef: IDENT '=' InitVal  */
#line 308 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.var_def) = at(
      new VarDefAST(take((yyvsp[-2].str)), {}, opt_init((yyvsp[0].init))),
      (yyloc)
    );
  }
#line 1867 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 20: /* VarDef: IDENT ConstArrayDims  */
#line 309 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.var_def) = at(
      new VarDefAST(
        take((yyvsp[-1].str)), take((yyvsp[0].exprs)), std::nullopt
      ),
      (yyloc)
    );
  }
#line 1873 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 21: /* VarDef: IDENT ConstArrayDims '=' InitVal  */
#line 310 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.var_def) = at(
      new VarDefAST(
        take((yyvsp[-3].str)),
        take((yyvsp[-2].exprs)),
        opt_init((yyvsp[0].init))
      ),
      (yyloc)
    );
  }
#line 1881 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 22: /* ConstArrayDims: '[' ConstExp ']'  */
#line 316 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.exprs) = list((yyvsp[-1].expr));
  }
#line 1887 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 23: /* ConstArrayDims: ConstArrayDims '[' ConstExp ']'  */
#line 317 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.exprs) = append((yyvsp[-3].exprs), (yyvsp[-1].expr));
  }
#line 1893 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 24: /* InitVal: Exp  */
#line 321 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.init) = new InitVal(take((yyvsp[0].expr)));
  }
#line 1899 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 25: /* InitVal: '{' '}'  */
#line 322 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.init) =
      new InitVal(make_ast<InitListAST>((yyloc), std::vector<InitVal>()));
  }
#line 1905 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 26: /* InitVal: '{' InitValList '}'  */
#line 323 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.init) =
      new InitVal(make_ast<InitListAST>((yyloc), take((yyvsp[-1].inits))));
  }
#line 1911 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 27: /* InitValList: InitVal  */
#line 327 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.inits) = list((yyvsp[0].init));
  }
#line 1917 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 28: /* InitValList: InitValList ',' InitVal  */
#line 328 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.inits) = append((yyvsp[-2].inits), (yyvsp[0].init));
  }
#line 1923 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 29: /* FuncDef: INT IDENT '(' ')' Block  */
#line 332 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.func_def) = make_ast<FuncDefAST>(
                         (yyloc),
                         I32::get(),
                         take((yyvsp[-3].str)),
                         std::vector<std::unique_ptr<FuncParamAST>>(),
                         std::unique_ptr<BlockSAST>((yyvsp[0].block))
    )
                         .release();
  }
#line 1933 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 30: /* FuncDef: FLOAT IDENT '(' ')' Block  */
#line 337 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.func_def) = make_ast<FuncDefAST>(
                         (yyloc),
                         Float::get(),
                         take((yyvsp[-3].str)),
                         std::vector<std::unique_ptr<FuncParamAST>>(),
                         std::unique_ptr<BlockSAST>((yyvsp[0].block))
    )
                         .release();
  }
#line 1943 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 31: /* FuncDef: VOID IDENT '(' ')' Block  */
#line 342 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.func_def) = make_ast<FuncDefAST>(
                         (yyloc),
                         Void::get(),
                         take((yyvsp[-3].str)),
                         std::vector<std::unique_ptr<FuncParamAST>>(),
                         std::unique_ptr<BlockSAST>((yyvsp[0].block))
    )
                         .release();
  }
#line 1953 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 32: /* FuncDef: INT IDENT '(' FuncFParams ')' Block  */
#line 347 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.func_def) = make_ast<FuncDefAST>(
                         (yyloc),
                         I32::get(),
                         take((yyvsp[-4].str)),
                         take((yyvsp[-2].params)),
                         std::unique_ptr<BlockSAST>((yyvsp[0].block))
    )
                         .release();
  }
#line 1961 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 33: /* FuncDef: FLOAT IDENT '(' FuncFParams ')' Block  */
#line 350 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.func_def) = make_ast<FuncDefAST>(
                         (yyloc),
                         Float::get(),
                         take((yyvsp[-4].str)),
                         take((yyvsp[-2].params)),
                         std::unique_ptr<BlockSAST>((yyvsp[0].block))
    )
                         .release();
  }
#line 1969 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 34: /* FuncDef: VOID IDENT '(' FuncFParams ')' Block  */
#line 353 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.func_def) = make_ast<FuncDefAST>(
                         (yyloc),
                         Void::get(),
                         take((yyvsp[-4].str)),
                         take((yyvsp[-2].params)),
                         std::unique_ptr<BlockSAST>((yyvsp[0].block))
    )
                         .release();
  }
#line 1977 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 35: /* FuncFParams: FuncFParam  */
#line 359 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.params) = ptr_list((yyvsp[0].param));
  }
#line 1983 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 36: /* FuncFParams: FuncFParams ',' FuncFParam  */
#line 360 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.params) = append_ptr((yyvsp[-2].params), (yyvsp[0].param));
  }
#line 1989 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 37: /* FuncFParam: INT IDENT FuncParamDims  */
#line 364 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.param) =
      make_ast<FuncParamAST>(
        (yyloc), I32::get(), take((yyvsp[-1].str)), take((yyvsp[0].exprs))
      )
        .release();
  }
#line 1995 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 38: /* FuncFParam: FLOAT IDENT FuncParamDims  */
#line 365 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.param) =
      make_ast<FuncParamAST>(
        (yyloc), Float::get(), take((yyvsp[-1].str)), take((yyvsp[0].exprs))
      )
        .release();
  }
#line 2001 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 39: /* FuncParamDims: %empty  */
#line 369 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.exprs) = new std::vector<Expr>();
  }
#line 2007 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 40: /* FuncParamDims: '[' ']'  */
#line 370 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.exprs) = new std::vector<Expr>();
    (yyval.exprs)->push_back(take(number_expr(-1, (yyloc))));
  }
#line 2016 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 41: /* FuncParamDims: '[' ']' ConstArrayDims  */
#line 374 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    auto dims = take((yyvsp[0].exprs));
    dims.insert(dims.begin(), take(number_expr(-1, (yyloc))));
    (yyval.exprs) = new std::vector<Expr>(std::move(dims));
  }
#line 2026 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 42: /* Block: '{' '}'  */
#line 382 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.block) =
      make_ast<BlockSAST>((yyloc), std::vector<BlockItem>()).release();
  }
#line 2032 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 43: /* Block: '{' BlockItems '}'  */
#line 383 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.block) =
      make_ast<BlockSAST>((yyloc), take((yyvsp[-1].block_items))).release();
  }
#line 2038 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 44: /* BlockItems: BlockItem  */
#line 387 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.block_items) = list((yyvsp[0].block_item));
  }
#line 2044 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 45: /* BlockItems: BlockItems BlockItem  */
#line 388 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.block_items) =
      append((yyvsp[-1].block_items), (yyvsp[0].block_item));
  }
#line 2050 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 46: /* BlockItem: Decl  */
#line 392 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.block_item) = new BlockItem(take((yyvsp[0].decl)));
  }
#line 2056 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 47: /* BlockItem: Stmt  */
#line 393 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.block_item) = new BlockItem(take((yyvsp[0].stmt)));
  }
#line 2062 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 48: /* Stmt: LVal '=' Exp ';'  */
#line 397 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.stmt) = new Stmt(
      make_ast<AssignStmtAST>(
        (yyloc),
        std::unique_ptr<LvalAST>((yyvsp[-3].lval)),
        take((yyvsp[-1].expr))
      )
    );
  }
#line 2070 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 49: /* Stmt: Exp ';'  */
#line 400 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.stmt) =
      new Stmt(make_ast<ExprStmtAST>((yyloc), take((yyvsp[-1].expr))));
  }
#line 2076 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 50: /* Stmt: ';'  */
#line 401 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.stmt) = new Stmt(make_ast<ExprStmtAST>((yyloc)));
  }
#line 2082 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 51: /* Stmt: Block  */
#line 402 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.stmt) = new Stmt(std::unique_ptr<BlockSAST>((yyvsp[0].block)));
  }
#line 2088 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 52: /* Stmt: IF '(' Cond ')' Stmt  */
#line 403 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.stmt) = new Stmt(
      make_ast<IfStmtAST>(
        (yyloc), take((yyvsp[-2].expr)), take((yyvsp[0].stmt)), std::nullopt
      )
    );
  }
#line 2096 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 53: /* Stmt: IF '(' Cond ')' Stmt ELSE Stmt  */
#line 406 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.stmt) = new Stmt(
      make_ast<IfStmtAST>(
        (yyloc),
        take((yyvsp[-4].expr)),
        take((yyvsp[-2].stmt)),
        std::optional<Stmt>(take((yyvsp[0].stmt)))
      )
    );
  }
#line 2107 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 54: /* Stmt: WHILE '(' Cond ')' Stmt  */
#line 412 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.stmt) = new Stmt(
      make_ast<WhileStmtAST>(
        (yyloc), take((yyvsp[-2].expr)), take((yyvsp[0].stmt))
      )
    );
  }
#line 2115 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 55: /* Stmt: BREAK ';'  */
#line 415 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.stmt) = new Stmt(make_ast<BreakStmtAST>((yyloc)));
  }
#line 2121 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 56: /* Stmt: CONTINUE ';'  */
#line 416 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.stmt) = new Stmt(make_ast<ContinueStmtAST>((yyloc)));
  }
#line 2127 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 57: /* Stmt: RETURN ';'  */
#line 417 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.stmt) = new Stmt(make_ast<ReturnStmtAST>((yyloc), std::nullopt));
  }
#line 2133 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 58: /* Stmt: RETURN Exp ';'  */
#line 418 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.stmt) = new Stmt(
      make_ast<ReturnStmtAST>(
        (yyloc), std::optional<Expr>(take((yyvsp[-1].expr)))
      )
    );
  }
#line 2141 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 59: /* Exp: LOrExp  */
#line 424 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.expr) = (yyvsp[0].expr);
  }
#line 2147 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 60: /* Cond: LOrExp  */
#line 428 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.expr) = (yyvsp[0].expr);
  }
#line 2153 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 61: /* LVal: IDENT  */
#line 432 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.lval) =
      make_ast<LvalAST>((yyloc), take((yyvsp[0].str)), std::vector<Expr>())
        .release();
  }
#line 2159 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 62: /* LVal: IDENT ArrayIndices  */
#line 433 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.lval) =
      make_ast<LvalAST>((yyloc), take((yyvsp[-1].str)), take((yyvsp[0].exprs)))
        .release();
  }
#line 2165 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 63: /* ArrayIndices: '[' Exp ']'  */
#line 437 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.exprs) = list((yyvsp[-1].expr));
  }
#line 2171 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 64: /* ArrayIndices: ArrayIndices '[' Exp ']'  */
#line 438 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.exprs) = append((yyvsp[-3].exprs), (yyvsp[-1].expr));
  }
#line 2177 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 65: /* PrimaryExp: '(' Exp ')'  */
#line 442 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    set_expr_loc(*(yyvsp[-1].expr), (yyloc));
    (yyval.expr) = (yyvsp[-1].expr);
  }
#line 2183 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 66: /* PrimaryExp: LVal  */
#line 443 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.expr) = new Expr(std::unique_ptr<LvalAST>((yyvsp[0].lval)));
  }
#line 2189 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 67: /* PrimaryExp: INT_CONST  */
#line 444 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.expr) = number_expr((yyvsp[0].i32), (yyloc));
  }
#line 2195 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 68: /* PrimaryExp: FLOAT_CONST  */
#line 445 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.expr) = number_expr((yyvsp[0].f32), (yyloc));
  }
#line 2201 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 69: /* UnaryExp: PrimaryExp  */
#line 449 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.expr) = (yyvsp[0].expr);
  }
#line 2207 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 70: /* UnaryExp: IDENT '(' ')'  */
#line 450 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.expr) = call_expr((yyvsp[-2].str), {}, (yyloc));
  }
#line 2213 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 71: /* UnaryExp: IDENT '(' FuncArgList ')'  */
#line 451 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.expr) = call_expr((yyvsp[-3].str), take((yyvsp[-1].exprs)), (yyloc));
  }
#line 2219 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 72: /* UnaryExp: '+' UnaryExp  */
#line 452 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.expr) = unary_expr(UnaryOp::Pos, (yyvsp[0].expr), (yyloc));
  }
#line 2225 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 73: /* UnaryExp: '-' UnaryExp  */
#line 453 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.expr) = unary_expr(UnaryOp::Neg, (yyvsp[0].expr), (yyloc));
  }
#line 2231 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 74: /* UnaryExp: '!' UnaryExp  */
#line 454 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.expr) = unary_expr(UnaryOp::Not, (yyvsp[0].expr), (yyloc));
  }
#line 2237 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 75: /* FuncArgList: Exp  */
#line 458 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.exprs) = list((yyvsp[0].expr));
  }
#line 2243 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 76: /* FuncArgList: FuncArgList ',' Exp  */
#line 459 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.exprs) = append((yyvsp[-2].exprs), (yyvsp[0].expr));
  }
#line 2249 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 77: /* MulExp: UnaryExp  */
#line 463 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.expr) = (yyvsp[0].expr);
  }
#line 2255 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 78: /* MulExp: MulExp '*' UnaryExp  */
#line 464 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.expr) =
      binary_expr(BinaryOp::Mul, (yyvsp[-2].expr), (yyvsp[0].expr), (yyloc));
  }
#line 2261 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 79: /* MulExp: MulExp '/' UnaryExp  */
#line 465 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.expr) =
      binary_expr(BinaryOp::Div, (yyvsp[-2].expr), (yyvsp[0].expr), (yyloc));
  }
#line 2267 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 80: /* MulExp: MulExp '%' UnaryExp  */
#line 466 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.expr) =
      binary_expr(BinaryOp::Mod, (yyvsp[-2].expr), (yyvsp[0].expr), (yyloc));
  }
#line 2273 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 81: /* AddExp: MulExp  */
#line 470 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.expr) = (yyvsp[0].expr);
  }
#line 2279 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 82: /* AddExp: AddExp '+' MulExp  */
#line 471 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.expr) =
      binary_expr(BinaryOp::Add, (yyvsp[-2].expr), (yyvsp[0].expr), (yyloc));
  }
#line 2285 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 83: /* AddExp: AddExp '-' MulExp  */
#line 472 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.expr) =
      binary_expr(BinaryOp::Sub, (yyvsp[-2].expr), (yyvsp[0].expr), (yyloc));
  }
#line 2291 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 84: /* RelExp: AddExp  */
#line 476 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.expr) = (yyvsp[0].expr);
  }
#line 2297 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 85: /* RelExp: RelExp LT AddExp  */
#line 477 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.expr) =
      binary_expr(BinaryOp::Lt, (yyvsp[-2].expr), (yyvsp[0].expr), (yyloc));
  }
#line 2303 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 86: /* RelExp: RelExp GT AddExp  */
#line 478 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.expr) =
      binary_expr(BinaryOp::Gt, (yyvsp[-2].expr), (yyvsp[0].expr), (yyloc));
  }
#line 2309 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 87: /* RelExp: RelExp LE AddExp  */
#line 479 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.expr) =
      binary_expr(BinaryOp::Le, (yyvsp[-2].expr), (yyvsp[0].expr), (yyloc));
  }
#line 2315 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 88: /* RelExp: RelExp GE AddExp  */
#line 480 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.expr) =
      binary_expr(BinaryOp::Ge, (yyvsp[-2].expr), (yyvsp[0].expr), (yyloc));
  }
#line 2321 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 89: /* EqExp: RelExp  */
#line 484 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.expr) = (yyvsp[0].expr);
  }
#line 2327 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 90: /* EqExp: EqExp EQ RelExp  */
#line 485 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.expr) =
      binary_expr(BinaryOp::Eq, (yyvsp[-2].expr), (yyvsp[0].expr), (yyloc));
  }
#line 2333 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 91: /* EqExp: EqExp NE RelExp  */
#line 486 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.expr) =
      binary_expr(BinaryOp::Ne, (yyvsp[-2].expr), (yyvsp[0].expr), (yyloc));
  }
#line 2339 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 92: /* LAndExp: EqExp  */
#line 490 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.expr) = (yyvsp[0].expr);
  }
#line 2345 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 93: /* LAndExp: LAndExp AND EqExp  */
#line 491 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.expr) =
      binary_expr(BinaryOp::And, (yyvsp[-2].expr), (yyvsp[0].expr), (yyloc));
  }
#line 2351 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 94: /* LOrExp: LAndExp  */
#line 495 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.expr) = (yyvsp[0].expr);
  }
#line 2357 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 95: /* LOrExp: LOrExp OR LAndExp  */
#line 496 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.expr) =
      binary_expr(BinaryOp::Or, (yyvsp[-2].expr), (yyvsp[0].expr), (yyloc));
  }
#line 2363 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

  case 96: /* ConstExp: Exp  */
#line 500 "/home/anfsity/Project/Exodus/src/FE/sysy.y"
  {
    (yyval.expr) = (yyvsp[0].expr);
  }
#line 2369 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"
  break;

#line 2373 "/home/anfsity/Project/Exodus/build/src/sysy.tab.cpp"

  default:
    break;
  }
  /* User semantic actions sometimes alter yychar, and that requires
     that yytoken be updated with the new translation.  We take the
     approach of translating immediately before every use of yytoken.
     One alternative is translating here after every semantic action,
     but that translation would be missed if the semantic action invokes
     YYABORT, YYACCEPT, or YYERROR immediately after altering yychar or
     if it invokes YYBACKUP.  In the case of YYABORT or YYACCEPT, an
     incorrect destructor might then be invoked immediately.  In the
     case of YYERROR or YYBACKUP, subsequent parser actions might lead
     to an incorrect destructor call or verbose syntax error message
     before the lookahead is translated.  */
  YY_SYMBOL_PRINT(
    "-> $$ =", YY_CAST(yysymbol_kind_t, yyr1[yyn]), &yyval, &yyloc
  );

  YYPOPSTACK(yylen);
  yylen = 0;

  *++yyvsp = yyval;
  *++yylsp = yyloc;

  /* Now 'shift' the result of the reduction.  Determine what state
     that goes to, based on the state we popped back to and the rule
     number reduced by.  */
  {
    const int yylhs = yyr1[yyn] - YYNTOKENS;
    const int yyi = yypgoto[yylhs] + *yyssp;
    yystate =
      (0 <= yyi && yyi <= YYLAST && yycheck[yyi] == *yyssp ? yytable[yyi]
                                                           : yydefgoto[yylhs]);
  }

  goto yynewstate;

/*--------------------------------------.
| yyerrlab -- here on detecting error.  |
`--------------------------------------*/
yyerrlab:
  /* Make sure we have latest lookahead translation.  See comments at
     user semantic actions for why this is necessary.  */
  yytoken = yychar == YYEMPTY ? YYSYMBOL_YYEMPTY : YYTRANSLATE(yychar);
  /* If not already recovering from an error, report this error.  */
  if (!yyerrstatus) {
    ++yynerrs;
    yyerror(ast, YY_("syntax error"));
  }

  yyerror_range[1] = yylloc;
  if (yyerrstatus == 3) {
    /* If just tried and failed to reuse lookahead token after an
       error, discard it.  */

    if (yychar <= YYEOF) {
      /* Return failure if at end of input.  */
      if (yychar == YYEOF)
        YYABORT;
    } else {
      yydestruct("Error: discarding", yytoken, &yylval, &yylloc, ast);
      yychar = YYEMPTY;
    }
  }

  /* Else will try to reuse lookahead token after shifting the error
     token.  */
  goto yyerrlab1;

/*---------------------------------------------------.
| yyerrorlab -- error raised explicitly by YYERROR.  |
`---------------------------------------------------*/
yyerrorlab:
  /* Pacify compilers when the user code never invokes YYERROR and the
     label yyerrorlab therefore never appears in user code.  */
  if (0)
    YYERROR;
  ++yynerrs;

  /* Do not reclaim the symbols of the rule whose action triggered
     this YYERROR.  */
  YYPOPSTACK(yylen);
  yylen = 0;
  YY_STACK_PRINT(yyss, yyssp);
  yystate = *yyssp;
  goto yyerrlab1;

/*-------------------------------------------------------------.
| yyerrlab1 -- common code for both syntax error and YYERROR.  |
`-------------------------------------------------------------*/
yyerrlab1:
  yyerrstatus = 3; /* Each real token shifted decrements this.  */

  /* Pop stack until we find a state that shifts the error token.  */
  for (;;) {
    yyn = yypact[yystate];
    if (!yypact_value_is_default(yyn)) {
      yyn += YYSYMBOL_YYerror;
      if (0 <= yyn && yyn <= YYLAST && yycheck[yyn] == YYSYMBOL_YYerror) {
        yyn = yytable[yyn];
        if (0 < yyn)
          break;
      }
    }

    /* Pop the current state because it cannot handle the error token.  */
    if (yyssp == yyss)
      YYABORT;

    yyerror_range[1] = *yylsp;
    yydestruct(
      "Error: popping", YY_ACCESSING_SYMBOL(yystate), yyvsp, yylsp, ast
    );
    YYPOPSTACK(1);
    yystate = *yyssp;
    YY_STACK_PRINT(yyss, yyssp);
  }

  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  *++yyvsp = yylval;
  YY_IGNORE_MAYBE_UNINITIALIZED_END

  yyerror_range[2] = yylloc;
  ++yylsp;
  YYLLOC_DEFAULT(*yylsp, yyerror_range, 2);

  /* Shift the error token.  */
  YY_SYMBOL_PRINT("Shifting", YY_ACCESSING_SYMBOL(yyn), yyvsp, yylsp);

  yystate = yyn;
  goto yynewstate;

/*-------------------------------------.
| yyacceptlab -- YYACCEPT comes here.  |
`-------------------------------------*/
yyacceptlab:
  yyresult = 0;
  goto yyreturnlab;

/*-----------------------------------.
| yyabortlab -- YYABORT comes here.  |
`-----------------------------------*/
yyabortlab:
  yyresult = 1;
  goto yyreturnlab;

/*-----------------------------------------------------------.
| yyexhaustedlab -- YYNOMEM (memory exhaustion) comes here.  |
`-----------------------------------------------------------*/
yyexhaustedlab:
  yyerror(ast, YY_("memory exhausted"));
  yyresult = 2;
  goto yyreturnlab;

/*----------------------------------------------------------.
| yyreturnlab -- parsing is finished, clean up and return.  |
`----------------------------------------------------------*/
yyreturnlab:
  if (yychar != YYEMPTY) {
    /* Make sure we have latest lookahead translation.  See comments at
       user semantic actions for why this is necessary.  */
    yytoken = YYTRANSLATE(yychar);
    yydestruct("Cleanup: discarding lookahead", yytoken, &yylval, &yylloc, ast);
  }
  /* Do not reclaim the symbols of the rule whose action triggered
     this YYABORT or YYACCEPT.  */
  YYPOPSTACK(yylen);
  YY_STACK_PRINT(yyss, yyssp);
  while (yyssp != yyss) {
    yydestruct(
      "Cleanup: popping", YY_ACCESSING_SYMBOL(+*yyssp), yyvsp, yylsp, ast
    );
    YYPOPSTACK(1);
  }
#ifndef yyoverflow
  if (yyss != yyssa)
    YYSTACK_FREE(yyss);
#endif

  return yyresult;
}

#line 503 "/home/anfsity/Project/Exodus/src/FE/sysy.y"

void yyerror(CompUnitAST &ast, const char *s) {
  (void)ast;
#ifdef __DEBUG
  exodus::Log::log_error(
    "parse error at {}:{}: {}", yylloc.first_line, yylloc.first_column, s
  );
#endif
  std::cerr << "parse error at " << yylloc.first_line << ":"
            << yylloc.first_column << ": " << s << std::endl;
}
