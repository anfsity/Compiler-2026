#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "../3rd-party/fmt/base.h"
#include "high/ast_base.hpp"
#include "high/ir.hpp"
#include "high/ir_builder.hpp"
#include "high/ir_printer.hpp"
#include "high/verifier.hpp"
#include "mid/dom.hpp"
#include "mid/flatten.hpp"
#include "mid/ir_printer.hpp"
#include "opt/AnalysisManager.hpp"
#include "opt/PassBuilder.hpp"
#include "opt/PassManager.hpp"

using namespace exodus::ast;
using namespace exodus::high_ir;
using namespace exodus::mid_ir;
using namespace exodus::opt;

namespace exodus::high_ir::opt {
void registerPasses();
}
namespace exodus::mid_ir::opt {
void registerPasses();
}

extern FILE *yyin;
extern int yyparse(CompUnitAST &ast);

auto main(int argc, char **argv) -> int {
  exodus::high_ir::opt::registerPasses();
  exodus::mid_ir::opt::registerPasses();

  std::string input_file;
  std::vector<std::string> pass_names;
  bool print_ir_after_all = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg.size() > 2 && arg.substr(0, 2) == "-O") {
      pass_names.push_back(arg.substr(2));
    } else if (arg == "-print-ir-after-all") {
      print_ir_after_all = true;
    } else {
      input_file = arg;
    }
  }

  if (input_file.empty()) {
    fmt::print(
      stderr,
      "Usage: {} <input_file> [-Opass1 -Opass2 ...] [-print-ir-after-all]\n",
      argv[0]
    );
    return 1;
  }

  yyin = fopen(input_file.c_str(), "r");
  if (!yyin) {
    fmt::print(stderr, "Error: Could not open file {}\n", input_file);
    return 1;
  }

  CompUnitAST ast;
  if (yyparse(ast) != 0) {
    fmt::print(stderr, "Error: Parsing failed.\n");
    return 1;
  }

  IRBuilder builder(nullptr);
  auto module = builder.build(ast);

  Verifier verifier;

  // --- Optimization Phase ---
  PassBuilder pb(module.get());
  FunctionAnalysisManager fam;
  ModuleAnalysisManager mam;

  auto instrumentation = [&](const std::string &name, auto &unit) {
    if (print_ir_after_all) {
      using T = std::decay_t<decltype(unit)>;
      if constexpr (std::is_same_v<T, Function> || std::is_same_v<T, Module>) {
        static IRPrinter p;
        if constexpr (std::is_same_v<T, Function>) {
          fmt::print(
            "*** IR after {} (High IR Function: {}) ***\n", name, unit.name
          );
        } else {
          fmt::print("*** IR after {} (High IR Module) ***\n", name);
        }
        fmt::print("{}\n", p.dump(unit));
      } else if constexpr (
        std::is_same_v<T, LinearFunction> || std::is_same_v<T, MidModule>
      ) {
        static LinearIRPrinter p;
        if constexpr (std::is_same_v<T, LinearFunction>) {
          fmt::print(
            "*** IR after {} (Mid IR Function: {}) ***\n", name, unit.name
          );
        } else {
          fmt::print("*** IR after {} (Mid IR Module) ***\n", name);
        }
        fmt::print("{}\n", p.dump(unit));
      }
    }
  };

  auto run_full_pipeline = [&]() {
    auto fpm = pb.buildFunctionPipeline();
    auto mpm = pb.buildModulePipeline();
    fpm.setAfterPassCallback(instrumentation);
    mpm.setAfterPassCallback(instrumentation);

    for (auto &f : module->functions) {
      if (!f->is_decl)
        fpm.run(*f, fam);
    }
    mpm.run(*module, mam);
    for (auto &f : module->functions) {
      if (!f->is_decl)
        fpm.run(*f, fam);
    }
  };

  if (pass_names.empty()) {
    run_full_pipeline();
  } else {
    for (const auto &name : pass_names) {
      if (pb.isFunctionPass(name)) {
        auto pass = pb.createFunctionPass(name);
        for (auto &f : module->functions) {
          if (!f->is_decl) {
            pass.run(*f, fam);
            instrumentation(name, *f);
          }
        }
      } else if (pb.isModulePass(name)) {
        auto pass = pb.createModulePass(name);
        pass.run(*module, mam);
        instrumentation(name, *module);
      } else if (!pb.isLinearFunctionPass(name)) {
        fmt::print(stderr, "Warning: Unknown pass '{}'\n", name);
      }
    }
  }

  if (!verifier.verify(*module)) {
    fmt::print(stderr, "Verifier: IR is invalid in module!\n");
  }

  // --- Lowering Phase ---
  Flattener flattener(module.get());
  auto mid_module = flattener.flatten();

  // --- Mid IR Optimization Phase ---
  pb = PassBuilder(module.get(), mid_module.get());
  LinearFunctionAnalysisManager lfam;
  lfam.registerPass<DominanceAnalysis>();

  auto run_full_mid_pipeline = [&]() {
    auto lfpm = pb.buildLinearFunctionPipeline();
    lfpm.setAfterPassCallback(instrumentation);
    for (auto &f : mid_module->functions) {
      if (!f->is_decl) {
        lfpm.run(*f, lfam);
      }
    }
  };

  if (pass_names.empty()) {
    run_full_mid_pipeline();
  } else {
    for (const auto &name : pass_names) {
      if (pb.isLinearFunctionPass(name)) {
        auto pass = pb.createLinearFunctionPass(name);
        for (auto &f : mid_module->functions) {
          if (!f->is_decl) {
            pass.run(*f, lfam);
            instrumentation(name, *f);
          }
        }
      }
    }
  }

  fmt::print("\n--- Final High IR ---\n");
  IRPrinter hprinter;
  fmt::print("{}\n", hprinter.dump(*module));

  fmt::print("\n--- Final Mid IR ---\n");
  LinearIRPrinter mprinter;
  fmt::print("{}\n", mprinter.dump(*mid_module));

  return 0;
}
