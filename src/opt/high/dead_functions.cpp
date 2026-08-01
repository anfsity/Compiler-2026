#include "dead_functions.hpp"

#include "../../high/scc.hpp"
#include <algorithm>
#include <unordered_set>
#include <vector>

namespace exodus::high_ir::opt {

auto DeadFunctions::run(Module &, exodus::opt::ModuleAnalysisManager &)
  -> exodus::opt::PreservedAnalysis {
  Function *entry = nullptr;
  for (const auto &function : module->functions) {
    if (function->name != "main" || function->is_decl)
      continue;
    if (entry)
      return exodus::opt::PreservedAnalysis::all();
    entry = function.get();
  }
  if (!entry)
    return exodus::opt::PreservedAnalysis::all();

  CallGraph call_graph(*module);
  const auto &nodes = call_graph.getNodes();
  std::unordered_set<Function *> reachable;
  std::vector<Function *> worklist{entry};
  while (!worklist.empty()) {
    auto *function = worklist.back();
    worklist.pop_back();
    if (!reachable.insert(function).second)
      continue;
    auto node = nodes.find(function);
    if (node == nodes.end())
      return exodus::opt::PreservedAnalysis::all();
    worklist.insert(
      worklist.end(), node->second.callees.begin(), node->second.callees.end()
    );
  }

  auto old_size = module->functions.size();
  module->functions.erase(
    std::remove_if(
      module->functions.begin(),
      module->functions.end(),
      [&reachable](const auto &function) {
        return !function->is_decl && !reachable.count(function.get());
      }
    ),
    module->functions.end()
  );
  return module->functions.size() != old_size
           ? exodus::opt::PreservedAnalysis::none()
           : exodus::opt::PreservedAnalysis::all();
}

} // namespace exodus::high_ir::opt
