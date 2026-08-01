#pragma once

#include "AnalysisManager.hpp"
#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace exodus::opt {

// Options shared by explicit and default pipelines.  The context deliberately
// owns policy only; pass-specific configuration remains in the pass itself.
struct PassOptions {
  unsigned opt_level = 2;
  size_t max_fixed_point_iterations = 8;
  bool debug_verify = false;
};

struct PassDiagnostics {
  std::function<void(const std::string &)> report;

  auto emit(const std::string &message) const -> void {
    if (report)
      report(message);
  }
};

template <typename IRUnitT>
class PassContext {
public:
  using AnalysisManagerT = AnalysisManager<IRUnitT>;
  using Instrumentation = std::function<void(const std::string &, IRUnitT &)>;
  using DebugVerifier = std::function<bool(IRUnitT &)>;

  PassContext(
    IRUnitT &ir,
    AnalysisManagerT &analysis,
    PassOptions options = {},
    Instrumentation instrumentation = {},
    PassDiagnostics diagnostics = {},
    DebugVerifier verifier = {}
  )
      : ir_unit(&ir), analysis_manager(&analysis), options(options),
        instrumentation(std::move(instrumentation)),
        diagnostics(std::move(diagnostics)), verifier(std::move(verifier)) {}

  auto ir() const -> IRUnitT & { return *ir_unit; }
  auto analysis() const -> AnalysisManagerT & { return *analysis_manager; }
  auto get_options() const -> const PassOptions & { return options; }
  auto get_diagnostics() const -> const PassDiagnostics & {
    return diagnostics;
  }

  template <typename ChildIRUnitT>
  auto add_child_analysis_manager(AnalysisManager<ChildIRUnitT> &child_analysis)
    -> void {
    child_invalidators.emplace_back([&child_analysis]() {
      child_analysis.clear();
    });
  }

  auto after_pass(const std::string &name) -> void {
    if (instrumentation)
      instrumentation(name, *ir_unit);
  }

  auto invalidate(const PreservedAnalysis &preserved) -> void {
    analysis_manager->invalidate(*ir_unit, preserved);
    if (preserved.all_preserved())
      return;
    for (auto &invalidate_child : child_invalidators)
      invalidate_child();
  }

  auto verify(const std::string &name) -> bool {
    if (!options.debug_verify || !verifier)
      return true;
    if (verifier(*ir_unit))
      return true;
    diagnostics.emit("debug verifier failed after pass: " + name);
    return false;
  }

private:
  IRUnitT *ir_unit = nullptr;
  AnalysisManagerT *analysis_manager = nullptr;
  PassOptions options;
  Instrumentation instrumentation;
  PassDiagnostics diagnostics;
  DebugVerifier verifier;
  std::vector<std::function<void()>> child_invalidators;
};

} // namespace exodus::opt
