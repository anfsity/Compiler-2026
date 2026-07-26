#pragma once

#include "../../mid/loop.hpp"
#include <optional>
#include <unordered_map>

namespace exodus::mid_ir::opt {

class LoopIdiomRecognize {
public:
  explicit LoopIdiomRecognize(MidModule * /* module */) {}

  auto run(LinearFunction &func, exodus::opt::LinearFunctionAnalysisManager &am)
    -> exodus::opt::PreservedAnalysis;

private:
  struct CountedLoop {
    Block *preheader = nullptr;
    Block *header = nullptr;
    Block *latch = nullptr;
    Block *exit = nullptr;
    Op *induction_phi = nullptr;
    Value *bound = nullptr;
  };

  std::unordered_map<Op *, Block *> op_blocks;

  auto build_op_block_map(LinearFunction &func) -> void;
  auto match_counted_loop(const Loop &loop) const -> std::optional<CountedLoop>;
  auto match_contiguous_pointer(Value *pointer, const CountedLoop &loop) const
    -> bool;
  auto has_escaping_result(const Loop &loop) const -> bool;
  auto replace_single_store_loop(const Loop &loop) -> bool;

  static auto integer_constant(Value *value) -> std::optional<int>;
  static auto is_byte_splat_constant(Value *value) -> bool;
  static auto reset_operands(Op *op, std::vector<Value *> operands) -> void;
};

} // namespace exodus::mid_ir::opt
