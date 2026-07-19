#pragma once

#include "../../high/cloner.hpp"
#include "../../high/ir.hpp"
#include "../../high/rewriter.hpp"
#include "../../high/scc.hpp"
#include "../../high/visitor.hpp"
#include "../AnalysisManager.hpp"
#include "cost_model.hpp"
#include "function_analysis.hpp"
#include <cstdint>
#include <iterator>
#include <unordered_map>

namespace exodus::high_ir::opt {

using namespace exodus::high_ir;
using namespace exodus::opt;

// --- Inliner Pass ---
class Inliner {
  Module *m;
  std::unordered_map<std::string, Function *> func_map;
  std::unordered_map<Function *, OpEffects> function_effects;
  std::unordered_map<Function *, size_t> call_counts;
  CallGraph call_graph;

public:
  Inliner(Module *_m);

  auto run(Module &, ModuleAnalysisManager &) -> PreservedAnalysis;

private:
  enum class RetStatus : uint8_t { None, Safe, Unsafe };

  static auto getRetStatus(const Region &r) -> RetStatus;

  auto runOnFunction(Function &f) -> bool;
  auto tryInlineInRegion(Region &r, Function &caller, int depth) -> bool;
  auto shouldInline(Op &call_op, Function &callee, Function &, int depth)
    -> bool;
  auto inlineCall(Region &r, Region::iterator it, Op *call_op, Function &callee)
    -> bool;
};

} // namespace exodus::high_ir::opt
