#pragma once

#include "../../mid/affine_loop.hpp"
#include "../../mid/cfg_editor.hpp"
#include <optional>
#include <unordered_map>

namespace exodus::mid_ir::opt {

class LoopIdiomRecognize {
public:
  explicit LoopIdiomRecognize(MidModule *m) : module(m) {}

  auto run(LinearFunction &func, exodus::opt::LinearFunctionAnalysisManager &am)
    -> exodus::opt::PreservedAnalysis;

private:
  struct Context {
    MidModule *module = nullptr;
    const AffineLoopInfo *affine_loops = nullptr;
    std::unordered_map<Op *, Block *> op_blocks;
    CFGEditor *cfg = nullptr;
  };

  MidModule *module;

  static auto build_op_block_map(Context &context, LinearFunction &func)
    -> void;
  auto sink_partial_store(LinearFunction &func, Context &context) -> bool;
  static auto
  can_move_store_across_branch(Op *store, Op *branch, const Context &context)
    -> bool;
  static auto is_side_effect_free(const Op *op) -> bool;
  static auto same_memory_access(Op *lhs, Op *rhs) -> bool;
  static auto match_contiguous_pointer(
    Value *pointer, const CountedLoopInfo &loop, const Context &context
  ) -> bool;
  static auto has_escaping_result(const Loop &loop, const Context &context)
    -> bool;
  static auto
  replace_single_store_loop(const Loop &loop, const Context &context) -> bool;
  auto forward_loop_carried_store(
    LinearFunction &func, const Loop &loop, const Context &context
  ) -> bool;
  static auto loop_executes_at_least_once(
    const Loop &loop, const CountedLoopInfo &counted, const Context &context
  ) -> bool;
  static auto match_relative_pointer(
    Value *pointer, Value *base, int offset, const Context &context
  ) -> bool;
  auto hoist_independent_store(
    LinearFunction &func, const Loop &loop, const Context &context
  ) -> bool;
  static auto get_initial_pointer(
    Value *pointer, const CountedLoopInfo &loop, const Context &context
  ) -> Value *;
  static auto memory_accesses_are_independent(
    Op *store, const Loop &loop, const CountedLoopInfo &counted
  ) -> bool;

  static auto integer_constant(Value *value) -> std::optional<int>;
  static auto is_byte_splat_constant(Value *value) -> bool;
  static auto reset_operands(Op *op, std::vector<Value *> operands) -> void;
};

} // namespace exodus::mid_ir::opt
