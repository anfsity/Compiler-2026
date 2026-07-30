#pragma once

#include "../../mid/ir.hpp"
#include "../AnalysisManager.hpp"
#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace exodus::mid_ir::opt {

class Inliner {
  struct Cost {
    size_t ops = 0;
    size_t blocks = 0;
    size_t calls = 0;
    size_t external_calls = 0;
    size_t writes = 0;
    size_t allocas = 0;
    size_t returns = 0;
    size_t loops = 0;
    unsigned max_loop_depth = 0;
  };

  MidModule *module;
  size_t inline_serial = 0;
  std::unordered_map<std::string, LinearFunction *> functions;
  std::unordered_map<LinearFunction *, size_t> call_counts;
  std::unordered_map<LinearFunction *, std::unordered_set<LinearFunction *>>
    edges;
  std::unordered_set<LinearFunction *> recursive_functions;

public:
  explicit Inliner(MidModule *m) : module(m) {}

  auto run(LinearFunction &func, exodus::opt::LinearFunctionAnalysisManager &am)
    -> exodus::opt::PreservedAnalysis;

private:
  auto rebuild_call_graph() -> void;
  auto cost(LinearFunction &func) const -> Cost;
  auto should_inline(
    const LinearFunction &caller,
    LinearFunction &callee,
    const Op &call,
    const Cost &callee_cost,
    unsigned loop_depth
  ) const -> bool;
  auto
  exposes_noalias_specialization(LinearFunction &callee, const Op &call) const
    -> bool;
  auto validate_callee(const LinearFunction &callee, const Op &call) const
    -> bool;
  auto inline_call(
    LinearFunction &caller,
    Block &call_block,
    std::list<Op *>::iterator call_it,
    LinearFunction &callee
  ) -> void;

  static auto rebuild_cfg(LinearFunction &func) -> void;
  static auto renumber_blocks(LinearFunction &func) -> void;
};

} // namespace exodus::mid_ir::opt
