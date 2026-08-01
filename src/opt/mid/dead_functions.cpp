#include "dead_functions.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace exodus::mid_ir::opt {

auto DeadFunctions::run(
  MidModule &module, exodus::opt::AnalysisManager<MidModule> &
) -> exodus::opt::PreservedAnalysis {
  return eliminate_dead_functions(module)
           ? exodus::opt::PreservedAnalysis::none()
           : exodus::opt::PreservedAnalysis::all();
}

auto eliminate_dead_functions(MidModule &module) -> bool {
  std::unordered_map<std::string, LinearFunction *> definitions;
  LinearFunction *entry = nullptr;
  for (const auto &function : module.functions) {
    if (function->is_decl)
      continue;
    if (!definitions.emplace(function->name, function.get()).second)
      return false;
    if (function->name == "main") {
      if (entry)
        return false;
      entry = function.get();
    }
  }
  if (!entry)
    return false;

  std::unordered_set<LinearFunction *> reachable;
  std::vector<LinearFunction *> worklist{entry};
  while (!worklist.empty()) {
    LinearFunction *function = worklist.back();
    worklist.pop_back();
    if (!reachable.insert(function).second)
      continue;

    for (const auto &block : function->blocks) {
      for (auto *op : block->insts) {
        if (op->code != OpCode::Call)
          continue;
        const auto &name = std::get<CallPayload>(op->payload).func_name;
        auto callee = definitions.find(name);
        if (callee != definitions.end())
          worklist.push_back(callee->second);
      }
    }
  }

  auto old_size = module.functions.size();
  module.functions.erase(
    std::remove_if(
      module.functions.begin(),
      module.functions.end(),
      [&](const auto &function) {
        return !function->is_decl && !reachable.count(function.get());
      }
    ),
    module.functions.end()
  );
  return module.functions.size() != old_size;
}

} // namespace exodus::mid_ir::opt
