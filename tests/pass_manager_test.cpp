#ifdef EXODUS_UNIT_TEST
#include "../src/opt/PassManager.hpp"
#include <cassert>
#include <iostream>

namespace {

struct CounterIR {
  int value = 0;
};

struct ChildIR {};

struct ChildAnalysis {
  using Result = int;
  static int runs;

  auto run(ChildIR &, exodus::opt::AnalysisManager<ChildIR> &) -> Result {
    ++runs;
    return runs;
  }
};

int ChildAnalysis::runs = 0;

struct IncrementUntil {
  int target = 0;

  auto run(CounterIR &ir, exodus::opt::AnalysisManager<CounterIR> &)
    -> exodus::opt::PreservedAnalysis {
    if (ir.value >= target)
      return exodus::opt::PreservedAnalysis::all();
    ++ir.value;
    return exodus::opt::PreservedAnalysis::none();
  }
};

struct PreserveSpecificAnalysisPass {
  auto run(CounterIR &, exodus::opt::AnalysisManager<CounterIR> &)
    -> exodus::opt::PreservedAnalysis {
    auto preserved = exodus::opt::PreservedAnalysis::none();
    preserved.preserve<ChildAnalysis>();
    return preserved;
  }
};

auto test_fixed_point_reports_stable_convergence() -> void {
  CounterIR ir;
  exodus::opt::AnalysisManager<CounterIR> am;
  exodus::opt::PassManager<CounterIR> pm;
  pm.add_pass(IncrementUntil{2}, "increment", "increment counter");

  auto result = pm.run_to_fixed_point(ir, am, 8);

  assert(ir.value == 2);
  assert(result.changed_any);
  assert(!result.changed_this_iteration);
  assert(result.stable);
  assert(!result.reached_iteration_limit);
  assert(result.iterations == 3);
  assert(!result.all_preserved());
  std::cout << "test_fixed_point_reports_stable_convergence passed!\n";
}

auto test_fixed_point_reports_iteration_limit() -> void {
  CounterIR ir;
  exodus::opt::AnalysisManager<CounterIR> am;
  exodus::opt::PassManager<CounterIR> pm;
  pm.add_pass(IncrementUntil{5}, "increment", "increment counter");

  auto result = pm.run_to_fixed_point(ir, am, 2);

  assert(ir.value == 2);
  assert(result.changed_any);
  assert(result.changed_this_iteration);
  assert(!result.stable);
  assert(result.reached_iteration_limit);
  assert(result.iterations == 2);
  std::cout << "test_fixed_point_reports_iteration_limit passed!\n";
}

auto test_fixed_point_zero_limit_is_not_stable() -> void {
  exodus::opt::FixedPointContext fixed_point(0);
  auto result = fixed_point.current_result();

  assert(!result.stable);
  assert(result.reached_iteration_limit);
  assert(!result.all_preserved());
  std::cout << "test_fixed_point_zero_limit_is_not_stable passed!\n";
}

auto test_pass_manager_intersects_specific_preservation() -> void {
  CounterIR ir;
  exodus::opt::AnalysisManager<CounterIR> am;
  exodus::opt::PassManager<CounterIR> pm;
  pm.add_pass(
    PreserveSpecificAnalysisPass{}, "preserve_specific", "preserve one analysis"
  );

  auto result = pm.run_with_result(ir, am);

  assert(result.changed);
  assert(!result.preserved.all_preserved());
  assert(result.preserved.is_presvd(typeid(ChildAnalysis)));
  std::cout << "test_pass_manager_intersects_specific_preservation passed!\n";
}

auto test_context_invalidates_child_analyses_and_instruments() -> void {
  CounterIR ir;
  ChildIR child;
  exodus::opt::AnalysisManager<CounterIR> am;
  exodus::opt::AnalysisManager<ChildIR> child_am;
  child_am.register_pass<ChildAnalysis>();
  assert(child_am.get_result<ChildAnalysis>(child) == 1);

  exodus::opt::PassManager<CounterIR> pm;
  pm.add_pass(IncrementUntil{1}, "increment", "increment counter");
  int instrumentation_count = 0;
  exodus::opt::PassOptions options;
  exodus::opt::PassContext<CounterIR> context(
    ir, am, options, [&](const std::string &, CounterIR &) {
      ++instrumentation_count;
    }
  );
  context.add_child_analysis_manager(child_am);
  auto result = pm.run_with_result(context);

  assert(result.changed);
  assert(instrumentation_count == 1);
  assert(child_am.get_result<ChildAnalysis>(child) == 2);
  std::cout
    << "test_context_invalidates_child_analyses_and_instruments passed!\n";
}

} // namespace

auto main() -> int {
  test_fixed_point_reports_stable_convergence();
  test_fixed_point_reports_iteration_limit();
  test_fixed_point_zero_limit_is_not_stable();
  test_pass_manager_intersects_specific_preservation();
  test_context_invalidates_child_analyses_and_instruments();
  return 0;
}
#endif
