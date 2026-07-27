#pragma once

#include "../../mid/affine_loop.hpp"
#include <optional>
#include <unordered_map>

namespace exodus::mid_ir::opt {

class LoopIdiomRecognize {
public:
  explicit LoopIdiomRecognize(MidModule *m) : module(m) {}

  auto run(LinearFunction &func, exodus::opt::LinearFunctionAnalysisManager &am)
    -> exodus::opt::PreservedAnalysis;

private:
  MidModule *module;
  const AffineLoopInfo *affine_loops = nullptr;
  std::unordered_map<Op *, Block *> op_blocks;

  auto build_op_block_map(LinearFunction &func) -> void;
  auto
  match_contiguous_pointer(Value *pointer, const CountedLoopInfo &loop) const
    -> bool;
  auto has_escaping_result(const Loop &loop) const -> bool;
  auto replace_single_store_loop(const Loop &loop) -> bool;
  auto hoist_independent_store(LinearFunction &func, const Loop &loop) -> bool;
  auto get_initial_pointer(Value *pointer, const CountedLoopInfo &loop) const
    -> Value *;
  auto memory_accesses_are_independent(
    Op *store, const Loop &loop, const CountedLoopInfo &counted
  ) const -> bool;

  static auto integer_constant(Value *value) -> std::optional<int>;
  static auto is_byte_splat_constant(Value *value) -> bool;
  static auto reset_operands(Op *op, std::vector<Value *> operands) -> void;
  static auto renumber_blocks(LinearFunction &func) -> void;
};

} // namespace exodus::mid_ir::opt
