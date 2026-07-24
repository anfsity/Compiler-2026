#pragma once

#include "../../high/ir.hpp"
#include "../AnalysisManager.hpp"
#include <unordered_set>

namespace exodus::high_ir::opt {

class LoopRecurrenceSimplify {
public:
  explicit LoopRecurrenceSimplify(Module *m) : module(m) {}

  auto run(Function &function, exodus::opt::FunctionAnalysisManager &)
    -> exodus::opt::PreservedAnalysis;

private:
  struct ModularRecurrence {
    Value *initial_state = nullptr;
    Value *trip_count = nullptr;
    Value *state_address = nullptr;
    int step = 0;
    int state_modulus = 0;
  };

  Module *module;
  std::unordered_set<Op *> guarded_fallbacks;

  auto simplify_region(Region &region) -> bool;
  auto match_modular_recurrence(
    Region &parent, Region::iterator position, Op *loop
  ) const -> std::optional<ModularRecurrence>;
  auto replace_with_guarded_closed_form(
    Region &parent,
    Region::iterator position,
    Op *loop,
    const ModularRecurrence &match
  ) -> void;
};

} // namespace exodus::high_ir::opt
