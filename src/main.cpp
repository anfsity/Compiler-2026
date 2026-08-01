#include <fstream>
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
#include "low/riscv/asm_printer.hpp"
#include "low/riscv/isel.hpp"
#include "low/riscv/machine_printer.hpp"
#include "low/riscv/reg_alloca.hpp"
#include "mid/affine_loop.hpp"
#include "mid/dom.hpp"
#include "mid/flatten.hpp"
#include "mid/ir_printer.hpp"
#include "mid/loop.hpp"
#include "opt/AnalysisManager.hpp"
#include "opt/PassBuilder.hpp"
#include "opt/PassContext.hpp"
#include "opt/PassManager.hpp"
#include "opt/high/local_array_summary.hpp"
#include "opt/mid/polyhedral.hpp"
#include "opt/mid/scalar_evolution.hpp"
#include "opt/pipeline_builder.hpp"

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
    std::string output_file;
    std::vector<std::string> pass_names;
    unsigned opt_level = 2;
    bool print_ir_after_all = false;
    bool dump_ra = false;
    bool emit_ra = false;
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

    if (!print_final_ir()) {
      return 1;
    }

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
      if (arg == "-S") {
        continue;
      } else if (arg == "-o") {
        options.output_file = argv[++i];
        continue;
      } else if (arg.size() > 2 && arg.substr(0, 2) == "-O") {
        auto value = arg.substr(2);
        bool numeric = !value.empty();
        for (auto ch : value)
          numeric = numeric && ch >= '0' && ch <= '9';
        if (numeric)
          options.opt_level = static_cast<unsigned>(std::stoul(value));
        else
          options.pass_names.push_back(std::move(value));
      } else if (arg == "-print-ir-after-all") {
        options.print_ir_after_all = true;
      } else if (arg == "-dump-ra") {
        options.dump_ra = true;
      } else if (arg == "-emit-ra") {
        options.emit_ra = true;
      } else {
        options.input_file = arg;
      }
    }

    if (options.input_file.empty()) {
      fmt::print(
        stderr,
        "Usage: {} <input_file> [-S] [-o <output_file>] [-O1] "
        "[-Opass1 -Opass2 ...] [-print-ir-after-all] [-dump-ra] [-emit-ra]\n",
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
    PipelineBuilder pipeline_builder(module.get());
    FunctionAnalysisManager fam;
    fam.register_pass<exodus::high_ir::opt::LocalArraySummaryAnalysis>();
    ModuleAnalysisManager mam;

    PassOptions pass_options;
    pass_options.opt_level = options.opt_level;
    auto function_instrumentation =
      [&](const std::string &name, Function &function) {
        this->instrument(name, function);
      };
    auto module_instrumentation = [&](const std::string &name, Module &mod) {
      this->instrument(name, mod);
    };
    PassDiagnostics diagnostics;

    if (options.pass_names.empty()) {
      auto fpm = pipeline_builder.build_function_pipeline(options.opt_level);
      auto mpm = pipeline_builder.build_module_pipeline(options.opt_level);
      FixedPointContext fixed_point(pass_options.max_fixed_point_iterations);
      while (fixed_point.can_run_iteration()) {
        bool changed = false;
        auto preserved = PreservedAnalysis::all();
        for (auto &f : module->functions) {
          if (f->is_decl)
            continue;
          PassContext<Function> context(
            *f, fam, pass_options, function_instrumentation, diagnostics
          );
          auto result = fpm.run_to_fixed_point(
            context, pass_options.max_fixed_point_iterations
          );
          changed = changed || result.changed_any;
          preserved.intersect(result.preserved);
        }
        PassContext<Module> context(
          *module, mam, pass_options, module_instrumentation, diagnostics
        );
        context.add_child_analysis_manager(fam);
        auto result = mpm.run_with_result(context);
        changed = changed || result.changed;
        preserved.intersect(result.preserved);
        fixed_point.record_iteration({changed, preserved});
        if (!changed)
          break;
      }
    } else {
      for (const auto &name : options.pass_names) {
        if (pb.is_function_pass(name)) {
          auto pass = pb.create_function_pass(name);
          for (auto &f : module->functions) {
            if (!f->is_decl) {
              PassContext<Function> context(
                *f, fam, pass_options, function_instrumentation, diagnostics
              );
              pass.run(context);
            }
          }
        } else if (pb.is_module_pass(name)) {
          auto pass = pb.create_module_pass(name);
          PassContext<Module> context(
            *module, mam, pass_options, module_instrumentation, diagnostics
          );
          context.add_child_analysis_manager(fam);
          pass.run(context);
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
    PipelineBuilder pipeline_builder(module.get(), mid_module.get());
    LinearFunctionAnalysisManager lfam;
    lfam.register_pass<DominanceAnalysis>();
    lfam.register_pass<LoopAnalysis>();
    lfam.register_pass<AffineLoopAnalysis>();
    lfam.register_pass<ScalarEvolutionAnalysis>();
    lfam.register_pass(PolyhedralAnalysis{mid_module.get()});
    MidModuleAnalysisManager mmam;

    PassOptions pass_options;
    pass_options.opt_level = options.opt_level;
    auto function_instrumentation =
      [&](const std::string &name, LinearFunction &function) {
        this->instrument(name, function);
      };
    auto module_instrumentation = [&](
                                    const std::string &name, MidModule &module
                                  ) { this->instrument(name, module); };
    PassDiagnostics diagnostics;

    if (options.pass_names.empty()) {
      auto lfpm =
        pipeline_builder.build_linear_function_pipeline(options.opt_level);
      auto mmpm = pipeline_builder.build_mid_module_pipeline(options.opt_level);
      FixedPointContext fixed_point(pass_options.max_fixed_point_iterations);
      while (fixed_point.can_run_iteration()) {
        bool changed = false;
        auto preserved = PreservedAnalysis::all();
        for (auto &f : mid_module->functions) {
          if (f->is_decl)
            continue;
          PassContext<LinearFunction> context(
            *f, lfam, pass_options, function_instrumentation, diagnostics
          );
          auto result = lfpm.run_to_fixed_point(
            context, pass_options.max_fixed_point_iterations
          );
          changed = changed || result.changed_any;
          preserved.intersect(result.preserved);
        }
        PassContext<MidModule> context(
          *mid_module, mmam, pass_options, module_instrumentation, diagnostics
        );
        context.add_child_analysis_manager(lfam);
        auto module_result = mmpm.run_with_result(context);
        changed = changed || module_result.changed;
        preserved.intersect(module_result.preserved);
        fixed_point.record_iteration({changed, preserved});
        if (!changed)
          break;
      }
    } else {
      for (const auto &name : options.pass_names) {
        if (pb.is_linear_function_pass(name)) {
          auto pass = pb.create_linear_function_pass(name);
          for (auto &f : mid_module->functions) {
            if (!f->is_decl) {
              PassContext<LinearFunction> context(
                *f, lfam, pass_options, function_instrumentation, diagnostics
              );
              pass.run(context);
            }
          }
        } else if (pb.is_mid_module_pass(name)) {
          auto pass = pb.create_mid_module_pass(name);
          PassContext<MidModule> context(
            *mid_module, mmam, pass_options, module_instrumentation, diagnostics
          );
          context.add_child_analysis_manager(lfam);
          pass.run(context);
        }
      }
    }
  }

  auto run_isel() -> void {
    for (auto &f : mid_module->functions) {
      if (!f->is_decl) {
        auto mf = exodus::riscv::lower_function(*f);
        exodus::riscv::run_ra(*mf, options.dump_ra, true);
        machine_functions.push_back(std::move(mf));
      }
    }
  }

  auto print_final_ir() -> bool {
    exodus::riscv::AsmPrinter asm_printer;
    auto fin_asm = asm_printer.to_string(*mid_module, machine_functions);

    if (!options.output_file.empty()) {
      std::ofstream out(options.output_file);
      if (!out) {
        fmt::print(
          stderr, "Error: Could not open output file {}\n", options.output_file
        );
        return false;
      }
      out << fin_asm << '\n';
      return true;
    }

    fmt::print("{}\n", fin_asm);
    return true;
  }
};

auto main(int argc, char **argv) -> int {
  Compiler compiler;
  return compiler.run(argc, argv);
}
