#pragma once

#include "../../mid/effects.hpp"
#include "../../mid/loop.hpp"
#include "../../mid/memory.hpp"
#include "../../mid/rewriter.hpp"
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace exodus::mid_ir::opt {

class LICM {
public:
  explicit LICM(MidModule *m) : module(m) {}

  auto run(LinearFunction &func, exodus::opt::LinearFunctionAnalysisManager &am)
    -> exodus::opt::PreservedAnalysis;

private:
  MidModule *module;
  BasicAliasAnalysis alias_analysis;
  DomTree *dom = nullptr;
  std::unordered_map<Op *, Block *> op_blocks;
  std::unordered_set<Op *> invariant_ops;
  std::unordered_set<std::string> readnone_scalar_functions;
  bool changed = false;

  auto build_op_block_map(LinearFunction &func) -> void;
  auto process_loop(LinearFunction &func, Loop &loop) -> void;
  auto mark_and_hoist(LinearFunction &func, Loop &loop) -> void;
  auto sink_stores(LinearFunction &func, Loop &loop) -> void;

  auto can_hoist(Op *op, Block *block, const Loop &loop) const -> bool;
  auto operands_are_invariant(const Op &op, const Loop &loop) const -> bool;
  auto value_is_invariant(Value *value, const Loop &loop) const -> bool;
  auto block_dominates_all_exits(Block *block, const Loop &loop) const -> bool;
  auto value_dominates_block(Value *value, Block *block) const -> bool;
  auto
  has_aliasing_write(const Loop &loop, const MemoryLocation &location) const
    -> bool;
  auto has_interfering_access(
    const Loop &loop, const Op &store, const MemoryLocation &location
  ) const -> bool;
  auto exits_are_dedicated(const Loop &loop) const -> bool;
  auto hoist_set_fits_register_budget(const Loop &loop) const -> bool;
  auto loop_contains_call(const Loop &loop) const -> bool;
  auto live_in_pressure(const Loop &loop, bool floating) const -> size_t;
  auto has_non_call_loop_user(Value *value, const Loop &loop) const -> bool;
  auto has_user_outside_hoist_set(Value *value) const -> bool;

  auto get_getptr_read_locations(const Op &op) const
    -> std::vector<MemoryLocation>;
  auto move_to_preheader(Op *op, Block *from, Block *preheader) -> void;
  auto clone_store_to_exit(const Op &store, Block *exit) -> void;

  static auto is_safe_to_speculate(const Op &op) -> bool;
};

} // namespace exodus::mid_ir::opt
