#pragma once

#include "dom.hpp"
#include "ir.hpp"
#include "rewriter.hpp"
#include <optional>
#include <unordered_map>
#include <vector>

namespace exodus::mid_ir {

struct Expression {
  OpCode code;
  Type *type = nullptr;
  std::vector<uint32_t> operands;

  auto operator==(const Expression &other) const -> bool {
    return code == other.code && type == other.type &&
           operands == other.operands;
  }
};

struct ExpressionHash {
  auto operator()(const Expression &expression) const -> size_t {
    size_t hash = std::hash<int>{}(static_cast<int>(expression.code));
    hash ^= std::hash<Type *>{}(expression.type) + (hash << 6) + (hash >> 2);
    for (auto operand : expression.operands) {
      hash ^= std::hash<uint32_t>{}(operand) + (hash << 6) + (hash >> 2);
    }
    return hash;
  }
};

class GVN {
  using ValueNumber = uint32_t;

  MidModule *module;
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
  auto visit(Block *block) -> void;
  auto process_op(
    Op *op,
    std::unordered_map<Expression, Value *, ExpressionHash> &loads,
    std::vector<Expression> &inserted
  ) -> void;
  auto number_value(Value *value) -> ValueNumber;
  auto build_expression(Op *op) -> std::optional<Expression>;
  auto simplify(Op *op, const std::vector<ValueNumber> &operands) -> Value *;

  static auto is_pure_opcode(OpCode code) -> bool;
  static auto is_memory_barrier(OpCode code) -> bool;
  static auto is_commutative(OpCode code) -> bool;
};

} // namespace exodus::mid_ir
