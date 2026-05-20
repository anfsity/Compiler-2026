#include <cstdio>
#include <memory>
#include <string>

#include "../include/fmt/base.h"
#include "../include/high/ast_base.hpp"
#include "../include/high/ir.hpp"
#include "../include/high/ir_builder.hpp"
#include "../include/high/ir_printer.hpp"
#include "../include/mid/flatten.hpp"

using namespace exodus;
using namespace exodus::ast;
using namespace exodus::high_ir;
using namespace exodus::mid_ir;

extern FILE *yyin;
extern int yyparse(CompUnitAST &ast);

int main(int argc, char **argv) {
  if (argc > 1) {
    yyin = fopen(argv[1], "r");
    if (!yyin) {
      fmt::print(stderr, "Error: Could not open file {}\n", argv[1]);
      return 1;
    }
  }

  CompUnitAST ast;
  if (yyparse(ast) != 0) {
    fmt::print(stderr, "Error: Parsing failed.\n");
    return 1;
  }

  IRBuilder builder(nullptr);
  auto module = builder.build(ast);

  Flattener flattener(module.get());
  auto mid_module = flattener.flatten();

  IRPrinter printer;
  fmt::print("{}\n", printer.dump(*module));

  // LinearIRPrinter printer;
  // fmt::print("{}\n", printer.dump(*mid_module));

  return 0;
}
