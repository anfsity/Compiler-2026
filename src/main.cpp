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
#include "low/riscv/isel.hpp"
#include "low/riscv/machine_printer.hpp"
#include "mid/dom.hpp"
#include "mid/flatten.hpp"
#include "mid/ir_printer.hpp"
#include "opt/AnalysisManager.hpp"
#include "opt/PassBuilder.hpp"
#include "opt/PassManager.hpp"

using namespace exodus::ast;
using namespace exodus::high_ir;
using namespace exodus::mid_ir;
using namespace exodus::low_ir;
using namespace exodus::opt;

namespace exodus::high_ir::opt {
auto register_passes() -> void;
}
namespace exodus::mid_ir::opt {
auto register_passes() -> void;
}

extern FILE *yyin;
extern int yyparse(CompUnitAST &ast);

struct Compiler {
  struct Options {
    std::string input_file;
    std::vector<std::string> pass_names;
    bool print_ir_after_all = false;
  };

  auto run(int argc, char **argv) -> int {
    exodus::high_ir::opt::register_passes();
    exodus::mid_ir::opt::register_passes();

    if (!parse_args(argc, argv)) {
      return 1;
    }

    if (!run_frontend()) {
      return 1;
    }

    run_high_opt();

    verify_high_ir();
    run_lowering();
    run_mid_opt();
    run_isel();

    print_final_ir();

    return 0;
  }

private:
  Options options;
  std::unique_ptr<Module> module;
  std::unique_ptr<MidModule> mid_module;
  std::vector<std::unique_ptr<MachineFunction>> machine_functions;

  auto parse_args(int argc, char **argv) -> bool {
    for (int i = 1; i < argc; ++i) {
      std::string arg = argv[i];
      if (arg.size() > 2 && arg.substr(0, 2) == "-O") {
        options.pass_names.push_back(arg.substr(2));
      } else if (arg == "-print-ir-after-all") {
        options.print_ir_after_all = true;
      } else {
        options.input_file = arg;
      }
    }

    if (options.input_file.empty()) {
      fmt::print(
        stderr,
        "Usage: {} <input_file> [-Opass1 -Opass2 ...] [-print-ir-after-all]\n",
        argv[0]
      );
      return false;
    }
    return true;
  }

  auto run_frontend() -> bool {
    yyin = fopen(options.input_file.c_str(), "r");
    if (!yyin) {
      fmt::print(stderr, "Error: Could not open file {}\n", options.input_file);
      return false;
    }

    CompUnitAST ast;
    if (yyparse(ast) != 0) {
      fmt::print(stderr, "Error: Parsing failed.\n");
      return false;
    }

    IRBuilder builder(nullptr);
    module = builder.build(ast);
    return true;
  }

  template <typename T>
  auto instrument(const std::string &name, const T &unit) -> void {
    if (!options.print_ir_after_all)
      return;

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

  auto run_high_opt() -> void {
    PassBuilder pb(module.get());
    FunctionAnalysisManager fam;
    ModuleAnalysisManager mam;

    auto instrumentation = [&](const std::string &name, const auto &unit) {
      this->instrument(name, unit);
    };

    if (options.pass_names.empty()) {
      auto fpm = pb.build_function_pipeline();
      auto mpm = pb.build_module_pipeline();
      fpm.set_after_pass_callback(instrumentation);
      mpm.set_after_pass_callback(instrumentation);

      for (auto &f : module->functions) {
        if (!f->is_decl)
          fpm.run(*f, fam);
      }
      mpm.run(*module, mam);
      for (auto &f : module->functions) {
        if (!f->is_decl)
          fpm.run(*f, fam);
      }
    } else {
      for (const auto &name : options.pass_names) {
        if (pb.is_function_pass(name)) {
          auto pass = pb.create_function_pass(name);
          for (auto &f : module->functions) {
            if (!f->is_decl) {
              pass.run(*f, fam);
              instrument(name, *f);
            }
          }
        } else if (pb.is_module_pass(name)) {
          auto pass = pb.create_module_pass(name);
          pass.run(*module, mam);
          instrument(name, *module);
        }
      }
    }
  }

  auto verify_high_ir() -> bool {
    Verifier verifier;
    if (!verifier.verify(*module)) {
      fmt::print(stderr, "Verifier: IR is invalid in module!\n");
      return false;
    }
    return true;
  }

  auto run_lowering() -> void {
    Flattener flattener(module.get());
    mid_module = flattener.flatten();
    instrument("flatten", *mid_module);
  }

  auto run_mid_opt() -> void {
    PassBuilder pb(module.get(), mid_module.get());
    LinearFunctionAnalysisManager lfam;
    lfam.register_pass<DominanceAnalysis>();

    auto instrumentation = [&](const std::string &name, const auto &unit) {
      this->instrument(name, unit);
    };

    if (options.pass_names.empty()) {
      auto lfpm = pb.build_linear_function_pipeline();
      lfpm.set_after_pass_callback(instrumentation);
      for (auto &f : mid_module->functions) {
        if (!f->is_decl) {
          lfpm.run(*f, lfam);
        }
      }
    } else {
      for (const auto &name : options.pass_names) {
        if (pb.is_linear_function_pass(name)) {
          auto pass = pb.create_linear_function_pass(name);
          for (auto &f : mid_module->functions) {
            if (!f->is_decl) {
              pass.run(*f, lfam);
              instrument(name, *f);
            }
          }
        }
      }
    }
  }

  auto run_isel() -> void {
    for (auto &f : mid_module->functions) {
      if (!f->is_decl) {
        machine_functions.push_back(exodus::riscv::lower_function(*f));
      }
    }
  }

  auto print_final_ir() -> void {
    fmt::print("\n--- Final High IR ---\n");
    IRPrinter hprinter;
    fmt::print("{}\n", hprinter.dump(*module));

    fmt::print("\n--- Final Mid IR ---\n");
    LinearIRPrinter mprinter;
    fmt::print("{}\n", mprinter.dump(*mid_module));

    fmt::print("\n--- Final Machine IR (RISC-V) ---\n");
    exodus::riscv::MachinePrinter machine_printer;
    fmt::print(
      "{}\n", machine_printer.to_string(*mid_module, machine_functions)
    );
  }
};

auto main(int argc, char **argv) -> int {
  Compiler compiler;
  return compiler.run(argc, argv);
}
