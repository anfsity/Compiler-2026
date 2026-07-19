#pragma once

#include "../../high/ir.hpp"
#include "../../high/rewriter.hpp"
#include "../../high/scc.hpp"
#include "../../high/visitor.hpp"
#include "../AnalysisManager.hpp"
#include <unordered_map>
#include <vector>

namespace exodus::high_ir::opt {

using namespace exodus::opt;

class IPCP {
  Module *module;

public:
  explicit IPCP(Module *m) : module(m) {}

  auto run(Module &, ModuleAnalysisManager &) -> PreservedAnalysis;
};

} // namespace exodus::high_ir::opt
