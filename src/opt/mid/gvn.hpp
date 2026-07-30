#pragma once

#include "../../mid/dom.hpp"
#include "../../mid/ir.hpp"
#include "../../mid/memory.hpp"
#include "../../mid/rewriter.hpp"
#include <optional>
#include <unordered_map>
#include <vector>

namespace exodus::mid_ir::opt {

struct Expression {
  OpCode code;
  Type *type = nullptr;
  std::vector<uint32_t> operands;
  Type *getptr_layout_type = nullptr;

  auto operator==(const Expression &other) const -> bool {
    return code == other.code && type == other.type &&
           operands == other.operands &&
           getptr_layout_type == other.getptr_layout_type;
  }
};

struct ExpressionHash {
  auto operator()(const Expression &expression) const -> size_t {
    size_t hash = std::hash<int>{}(static_cast<int>(expression.code));
    hash ^= std::hash<Type *>{}(expression.type) + (hash << 6) + (hash >> 2);
    for (auto operand : expression.operands) {
      hash ^= std::hash<uint32_t>{}(operand) + (hash << 6) + (hash >> 2);
    }
    hash ^= std::hash<Type *>{}(expression.getptr_layout_type) + (hash << 6) +
            (hash >> 2);
    return hash;
  }
};

class GVN {
  using ValueNumber = uint32_t;

  struct MemoryState {
    struct LoadFact {
      Value *value = nullptr;
      MemoryLocation location;
    };
    struct StoredValueFact {
      Value *value = nullptr;
      MemoryLocation location;
    };
    struct StoreFact {
      Op *operation = nullptr;
      MemoryLocation location;
    };

    std::unordered_map<Expression, LoadFact, ExpressionHash> loads;
    std::unordered_map<ValueNumber, StoredValueFact> stored_values;
    std::unordered_map<ValueNumber, StoreFact> pending_stores;

    auto clear() -> void {
      loads.clear();
      stored_values.clear();
      pending_stores.clear();
    }
  };

  MidModule *module;
  BasicAliasAnalysis alias_analysis;
  MidIRRewriter rewriter;
  DomTree *dom = nullptr;
  std::unordered_map<Value *, ValueNumber> value_numbers;
  std::unordered_map<std::string, ValueNumber> constant_numbers;
  std::unordered_map<Expression, Value *, ExpressionHash> available;
  bool changed = false;
  ValueNumber next_number = 0;

public:
  explicit GVN(MidModule *m) : module(m) {}

  auto run(LinearFunction &func, exodus::opt::LinearFunctionAnalysisManager &am)
    -> exodus::opt::PreservedAnalysis;

private:
  auto visit(Block *block, MemoryState state) -> void;
  auto prepare_inherited_state(Block *block, MemoryState &state) -> void;
  auto process_op(Op *op, MemoryState &state, std::vector<Expression> &inserted)
    -> void;
  auto invalidate_for_write(
    MemoryState &state, const std::optional<MemoryLocation> &location
  ) -> void;
  auto observe_read(
    MemoryState &state, const std::optional<MemoryLocation> &location
  ) -> void;
  auto number_value(Value *value) -> ValueNumber;
  auto build_expression(Op *op) -> std::optional<Expression>;
  auto simplify(Op *op, const std::vector<ValueNumber> &operands) -> Value *;

  static auto is_pure_opcode(OpCode code) -> bool;
  static auto reads_memory_through_getptr(const Op *op) -> bool;
  static auto is_memory_barrier(const Op *op) -> bool;
  static auto is_commutative(OpCode code) -> bool;
};

} // namespace exodus::mid_ir::opt
