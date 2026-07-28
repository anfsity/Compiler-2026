#pragma once

#include "../../high/effects.hpp"
#include "../../high/ir.hpp"
#include "../AnalysisManager.hpp"
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace exodus::high_ir::opt {

class PureCallLoopSink {
public:
  explicit PureCallLoopSink(Module *module) : module(module) {}

  auto run(Module &, exodus::opt::ModuleAnalysisManager &)
    -> exodus::opt::PreservedAnalysis;

private:
  struct Candidate {
    Op *loop = nullptr;
    Op *iteration_counter_load = nullptr;
    Op *call = nullptr;
    Op *mod = nullptr;
    Value *counter_address = nullptr;
    Value *initial_counter = nullptr;
    Value *answer_address = nullptr;
    Value *modulus = nullptr;
    std::unordered_set<Op *> moved_ops;
  };

  Module *module;
  std::unordered_map<std::string, Function *> functions;
  std::unordered_map<Function *, OpEffects> summaries;

  auto simplify_function(Function &function) -> bool;
  auto simplify_region(Region &region) -> bool;
  auto match(Region &parent, Region::iterator position)
    -> std::optional<Candidate>;
  auto
  transform(Region &parent, Region::iterator position, Candidate &candidate)
    -> bool;

  auto collect_dependency(
    Value *value,
    const Region &body,
    Candidate &candidate,
    std::unordered_set<Op *> &visiting
  ) -> bool;
  auto clone_invariant_value(
    Value *value,
    const Region &body,
    const Candidate &candidate,
    std::unordered_map<Value *, Value *> &cache,
    Region &destination
  ) -> Value *;
  auto is_local_root(Value *value) const -> bool;
  auto defining_op(Value *value) const -> Op *;
  auto int_constant(Value *value) const -> std::optional<int>;
  auto is_in_region(const Region &region, Op *op) const -> bool;
  auto root_of(Value *value) const -> Value *;
  auto has_only_user(Value *value, Op *user) const -> bool;
  auto call_reads_are_disjoint(
    const Candidate &candidate, const OpEffects &call_effects
  ) const -> bool;
};

} // namespace exodus::high_ir::opt
