#pragma once

#include "../../high/ir.hpp"
#include "../AnalysisManager.hpp"
#include <optional>

namespace exodus::high_ir::opt {

class GuardedBitwiseIdiom {
public:
  explicit GuardedBitwiseIdiom(Module *module) : module(module) {}

  auto run(Module &, exodus::opt::ModuleAnalysisManager &)
    -> exodus::opt::PreservedAnalysis;

private:
  enum class Kind { And, Xor };

  Module *module;

  auto match(Function &function) const -> std::optional<Kind>;
  auto specialize(Function &function, Kind kind) -> void;
};

} // namespace exodus::high_ir::opt
