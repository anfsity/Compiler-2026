#include "local_array_summary.hpp"

#include "../../base/getptr.hpp"
#include <algorithm>
#include <limits>
#include <unordered_set>
#include <utility>

namespace exodus::high_ir::opt {
namespace {

constexpr size_t max_summary_elements = 16'384;
constexpr size_t max_interpreter_iterations = 100'000;

auto defining_op(Value *value) -> Op * {
  if (!value || value->kind != ValueKind::OpResult)
    return nullptr;
  return static_cast<Op *>(static_cast<OpResult *>(value)->creator);
}

auto int_constant(Value *value) -> std::optional<int64_t> {
  if (!value || value->kind != ValueKind::Constant)
    return std::nullopt;
  const auto &data = static_cast<Constant *>(value)->val;
  if (!std::holds_alternative<int>(data))
    return std::nullopt;
  return std::get<int>(data);
}

auto checked_add(int64_t lhs, int64_t rhs) -> std::optional<int64_t> {
  int64_t result = 0;
  return __builtin_add_overflow(lhs, rhs, &result)
           ? std::nullopt
           : std::optional<int64_t>(result);
}

auto checked_sub(int64_t lhs, int64_t rhs) -> std::optional<int64_t> {
  int64_t result = 0;
  return __builtin_sub_overflow(lhs, rhs, &result)
           ? std::nullopt
           : std::optional<int64_t>(result);
}

auto checked_mul(int64_t lhs, int64_t rhs) -> std::optional<int64_t> {
  int64_t result = 0;
  return __builtin_mul_overflow(lhs, rhs, &result)
           ? std::nullopt
           : std::optional<int64_t>(result);
}

auto checked_i32(std::optional<int64_t> value) -> std::optional<int64_t> {
  if (
    !value || *value < std::numeric_limits<int32_t>::min() ||
    *value > std::numeric_limits<int32_t>::max()
  ) {
    return std::nullopt;
  }
  return value;
}

auto checked_type_size(const std::shared_ptr<Type> &type)
  -> std::optional<int64_t> {
  if (!type)
    return std::nullopt;
  if (type->is_i32() || type->is_f32() || type->is_bool() || type->is_ptr()) {
    return int64_t{4};
  }
  if (!type->is_array())
    return std::nullopt;
  auto array = std::static_pointer_cast<Array>(type);
  if (array->len < 0)
    return std::nullopt;
  auto base_size = checked_type_size(array->base);
  return base_size ? checked_mul(*base_size, array->len) : std::nullopt;
}

auto checked_index_scale(const ir::GetPtrStep &step) -> std::optional<int64_t> {
  if (!step.from_type)
    return std::nullopt;
  if (step.from_type->is_array()) {
    return checked_type_size(
      std::static_pointer_cast<Array>(step.from_type)->base
    );
  }
  if (step.from_type->is_ptr()) {
    return checked_type_size(
      std::static_pointer_cast<Ptr>(step.from_type)->target
    );
  }
  return checked_type_size(step.from_type);
}

auto i32_array_element_count(const std::shared_ptr<Type> &type)
  -> std::optional<size_t> {
  if (!type || !type->is_array())
    return std::nullopt;
  int64_t count = 1;
  auto current = type;
  while (current && current->is_array()) {
    auto array = std::static_pointer_cast<Array>(current);
    if (array->len <= 0)
      return std::nullopt;
    auto next = checked_mul(count, array->len);
    if (!next || *next > static_cast<int64_t>(max_summary_elements))
      return std::nullopt;
    count = *next;
    current = array->base;
  }
  if (!current || !current->is_i32())
    return std::nullopt;
  return static_cast<size_t>(count);
}

auto is_local_root(Value *value) -> bool {
  auto *op = defining_op(value);
  return op && op->code == OpCode::Alloca;
}

struct ResolvedPointer {
  Value *root = nullptr;
  std::optional<int64_t> offset;
};

auto resolve_pointer_impl(Value *value, std::unordered_set<Value *> &active)
  -> ResolvedPointer {
  if (!value || !value->type->is_ptr() || !active.insert(value).second)
    return {};
  auto finish = [&](ResolvedPointer result) {
    active.erase(value);
    return result;
  };

  if (
    value->kind == ValueKind::Argument || value->kind == ValueKind::GlobalVar
  ) {
    return finish({value, int64_t{0}});
  }
  auto *op = defining_op(value);
  if (!op)
    return finish({});
  if (op->code == OpCode::Alloca)
    return finish({value, int64_t{0}});
  if (
    op->code != OpCode::GetPtr || op->operands.empty() || !op->result ||
    !op->operands[0]->type->is_ptr() || !op->result->type->is_ptr()
  ) {
    return finish({});
  }

  auto plan = ir::analyze_getptr(
    op->operands[0]->type, op->result->type, op->operands.size() - 1
  );
  if (plan.reads_memory)
    return finish({});
  auto pointer = resolve_pointer_impl(op->operands[0], active);
  if (!pointer.root)
    return finish({});
  for (const auto &step : plan.steps) {
    if (step.kind != ir::GetPtrStep::Kind::Index)
      return finish({});
    auto scale = checked_index_scale(step);
    if (
      !scale || *scale != step.scale ||
      step.index_pos + 1 >= op->operands.size()
    )
      return finish({});
    if (!pointer.offset)
      continue;
    auto index = int_constant(op->operands[step.index_pos + 1]);
    auto scaled = index ? checked_mul(*index, *scale) : std::nullopt;
    pointer.offset =
      scaled ? checked_add(*pointer.offset, *scaled) : std::nullopt;
  }
  return finish(pointer);
}

auto resolve_pointer(Value *value) -> ResolvedPointer {
  std::unordered_set<Value *> active;
  return resolve_pointer_impl(value, active);
}

struct SymbolicPointer {
  Value *root = nullptr;
  int64_t constant_offset = 0;
  std::vector<LocalArrayIndexTerm> terms;
};

auto add_index_term(
  std::vector<LocalArrayIndexTerm> &terms, Value *value, int64_t coefficient
) -> bool {
  for (auto &term : terms) {
    if (term.value != value)
      continue;
    auto combined = checked_add(term.coefficient, coefficient);
    if (!combined)
      return false;
    term.coefficient = *combined;
    return true;
  }
  terms.push_back({value, coefficient});
  return true;
}

auto symbolic_pointer_impl(Value *value, std::unordered_set<Value *> &active)
  -> std::optional<SymbolicPointer> {
  if (!value || !value->type->is_ptr() || !active.insert(value).second)
    return std::nullopt;
  auto finish = [&](std::optional<SymbolicPointer> result) {
    active.erase(value);
    return result;
  };

  if (
    value->kind == ValueKind::Argument || value->kind == ValueKind::GlobalVar
  ) {
    return finish(SymbolicPointer{value, 0, {}});
  }
  auto *op = defining_op(value);
  if (!op)
    return finish(std::nullopt);
  if (op->code == OpCode::Alloca)
    return finish(SymbolicPointer{value, 0, {}});
  if (
    op->code != OpCode::GetPtr || op->operands.empty() || !op->result ||
    !op->operands[0]->type->is_ptr() || !op->result->type->is_ptr()
  ) {
    return finish(std::nullopt);
  }

  auto plan = ir::analyze_getptr(
    op->operands[0]->type, op->result->type, op->operands.size() - 1
  );
  if (plan.reads_memory)
    return finish(std::nullopt);
  auto pointer = symbolic_pointer_impl(op->operands[0], active);
  if (!pointer)
    return finish(std::nullopt);
  for (const auto &step : plan.steps) {
    auto scale = checked_index_scale(step);
    if (
      step.kind != ir::GetPtrStep::Kind::Index || !scale ||
      *scale != step.scale || *scale % 4 != 0 ||
      step.index_pos + 1 >= op->operands.size()
    ) {
      return finish(std::nullopt);
    }
    auto *index_value = op->operands[step.index_pos + 1];
    const auto element_scale = *scale / 4;
    if (auto index = int_constant(index_value)) {
      auto scaled = checked_mul(*index, element_scale);
      auto offset =
        scaled ? checked_add(pointer->constant_offset, *scaled) : std::nullopt;
      if (!offset)
        return finish(std::nullopt);
      pointer->constant_offset = *offset;
    } else if (!add_index_term(pointer->terms, index_value, element_scale)) {
      return finish(std::nullopt);
    }
  }
  pointer->terms.erase(
    std::remove_if(
      pointer->terms.begin(),
      pointer->terms.end(),
      [](const auto &term) { return term.coefficient == 0; }
    ),
    pointer->terms.end()
  );
  return finish(std::move(pointer));
}

auto symbolic_pointer(Value *value) -> std::optional<SymbolicPointer> {
  std::unordered_set<Value *> active;
  return symbolic_pointer_impl(value, active);
}

struct AbstractValue {
  enum class Kind : uint8_t { Integer, Pointer };
  Kind kind = Kind::Integer;
  int64_t integer = 0;
  ResolvedPointer pointer;

  static auto integer_value(int64_t value) -> AbstractValue {
    return {Kind::Integer, value, {}};
  }
  static auto pointer_value(ResolvedPointer value) -> AbstractValue {
    return {Kind::Pointer, 0, value};
  }
};

struct MemoryKey {
  Value *root = nullptr;
  int64_t offset = 0;

  auto operator==(const MemoryKey &other) const -> bool {
    return root == other.root && offset == other.offset;
  }
};

struct MemoryKeyHash {
  auto operator()(const MemoryKey &key) const -> size_t {
    return std::hash<Value *>{}(key.root) ^
           (std::hash<int64_t>{}(key.offset) << 1);
  }
};

struct CandidateState {
  Value *root = nullptr;
  std::vector<std::optional<int32_t>> values;
  std::unordered_set<Op *> write_ops;
  size_t boundary_index = 0;
  Op *boundary = nullptr;
};

class ConstantPrefixInterpreter {
public:
  explicit ConstantPrefixInterpreter(Function &function) {
    for (auto *op : function.body) {
      if (
        op->code == OpCode::Alloca && op->result && op->result->type->is_ptr()
      ) {
        auto target = std::static_pointer_cast<Ptr>(op->result->type)->target;
        if (auto count = i32_array_element_count(target)) {
          CandidateState state;
          state.root = op->result;
          state.values.resize(*count);
          candidates.emplace(op->result, std::move(state));
        }
      }
    }
  }

  auto evaluate(Function &function) -> void {
    size_t index = 0;
    for (auto *op : function.body) {
      auto saved_values = values;
      auto saved_memory = memory;
      auto saved_candidates = candidates;
      auto saved_iterations = iterations;
      current_top_index = index;
      current_top_op = op;
      nested_depth = 0;
      if (!evaluate_node(op)) {
        values = std::move(saved_values);
        memory = std::move(saved_memory);
        candidates = std::move(saved_candidates);
        iterations = saved_iterations;
        break;
      }
      ++index;
    }
  }

  auto states() const -> const std::unordered_map<Value *, CandidateState> & {
    return candidates;
  }

private:
  std::unordered_map<Value *, AbstractValue> values;
  std::unordered_map<MemoryKey, AbstractValue, MemoryKeyHash> memory;
  std::unordered_map<Value *, CandidateState> candidates;
  size_t iterations = 0;
  size_t current_top_index = 0;
  Op *current_top_op = nullptr;
  size_t nested_depth = 0;

  auto evaluate_value(Value *value) -> std::optional<AbstractValue> {
    if (!value)
      return std::nullopt;
    if (auto constant = int_constant(value))
      return AbstractValue::integer_value(*constant);
    if (
      (value->kind == ValueKind::Argument ||
       value->kind == ValueKind::GlobalVar) &&
      value->type->is_ptr()
    ) {
      return AbstractValue::pointer_value({value, int64_t{0}});
    }
    auto it = values.find(value);
    return it == values.end() ? std::nullopt
                              : std::optional<AbstractValue>(it->second);
  }

  auto evaluate_pointer(Value *value) -> std::optional<ResolvedPointer> {
    auto result = evaluate_value(value);
    if (!result || result->kind != AbstractValue::Kind::Pointer)
      return std::nullopt;
    return result->pointer;
  }

  auto evaluate_integer(Value *value) -> std::optional<int64_t> {
    auto result = evaluate_value(value);
    if (!result || result->kind != AbstractValue::Kind::Integer)
      return std::nullopt;
    return result->integer;
  }

  auto
  record_candidate_store(Op *op, const ResolvedPointer &pointer, int64_t value)
    -> bool {
    auto it = candidates.find(pointer.root);
    if (it == candidates.end())
      return true;
    if (
      !pointer.offset || *pointer.offset < 0 || *pointer.offset % 4 != 0 ||
      value < std::numeric_limits<int32_t>::min() ||
      value > std::numeric_limits<int32_t>::max()
    ) {
      return false;
    }
    auto index = static_cast<uint64_t>(*pointer.offset / 4);
    if (index >= it->second.values.size())
      return false;
    it->second.values[index] = static_cast<int32_t>(value);
    it->second.write_ops.insert(op);
    it->second.boundary_index = current_top_index;
    it->second.boundary = current_top_op;
    return true;
  }

  auto store_memory(const ResolvedPointer &pointer, AbstractValue value)
    -> bool {
    if (!is_local_root(pointer.root))
      return true;
    if (!pointer.offset)
      return false;
    memory[{pointer.root, *pointer.offset}] = std::move(value);
    return true;
  }

  auto evaluate_getptr(Op *op) -> bool {
    if (
      op->operands.empty() || !op->result || !op->operands[0]->type->is_ptr() ||
      !op->result->type->is_ptr()
    ) {
      return false;
    }
    auto pointer = evaluate_pointer(op->operands[0]);
    if (!pointer || !pointer->root || !pointer->offset)
      return false;
    auto plan = ir::analyze_getptr(
      op->operands[0]->type, op->result->type, op->operands.size() - 1
    );
    if (plan.reads_memory)
      return false;
    for (const auto &step : plan.steps) {
      if (step.kind != ir::GetPtrStep::Kind::Index)
        return false;
      auto scale = checked_index_scale(step);
      if (
        !scale || *scale != step.scale ||
        step.index_pos + 1 >= op->operands.size()
      )
        return false;
      auto index = evaluate_integer(op->operands[step.index_pos + 1]);
      auto scaled = index ? checked_mul(*index, *scale) : std::nullopt;
      auto offset =
        scaled ? checked_add(*pointer->offset, *scaled) : std::nullopt;
      if (!offset)
        return false;
      pointer->offset = *offset;
    }
    values[op->result] = AbstractValue::pointer_value(*pointer);
    return true;
  }

  auto evaluate_binary(Op *op) -> bool {
    if (op->operands.size() != 2 || !op->result)
      return false;
    auto lhs = evaluate_integer(op->operands[0]);
    auto rhs = evaluate_integer(op->operands[1]);
    if (!lhs || !rhs)
      return false;

    std::optional<int64_t> result;
    switch (op->code) {
    case OpCode::Add:
      result = checked_i32(checked_add(*lhs, *rhs));
      break;
    case OpCode::Sub:
      result = checked_i32(checked_sub(*lhs, *rhs));
      break;
    case OpCode::Mul:
      result = checked_i32(checked_mul(*lhs, *rhs));
      break;
    case OpCode::Div:
      if (
        *rhs == 0 || (*lhs == std::numeric_limits<int32_t>::min() && *rhs == -1)
      ) {
        return false;
      }
      result = *lhs / *rhs;
      break;
    case OpCode::Mod:
      if (
        *rhs == 0 || (*lhs == std::numeric_limits<int32_t>::min() && *rhs == -1)
      ) {
        return false;
      }
      result = *lhs % *rhs;
      break;
    case OpCode::Eq:
      result = *lhs == *rhs;
      break;
    case OpCode::Ne:
      result = *lhs != *rhs;
      break;
    case OpCode::Lt:
      result = *lhs < *rhs;
      break;
    case OpCode::Gt:
      result = *lhs > *rhs;
      break;
    case OpCode::Le:
      result = *lhs <= *rhs;
      break;
    case OpCode::Ge:
      result = *lhs >= *rhs;
      break;
    case OpCode::And:
      result = static_cast<int32_t>(*lhs) & static_cast<int32_t>(*rhs);
      break;
    case OpCode::Or:
      result = static_cast<int32_t>(*lhs) | static_cast<int32_t>(*rhs);
      break;
    case OpCode::Xor:
      result = static_cast<int32_t>(*lhs) ^ static_cast<int32_t>(*rhs);
      break;
    default:
      return false;
    }
    if (!result)
      return false;
    values[op->result] = AbstractValue::integer_value(*result);
    return true;
  }

  auto evaluate_memset(Op *op) -> bool {
    if (op->operands.size() != 3)
      return false;
    auto pointer = evaluate_pointer(op->operands[0]);
    auto count = evaluate_integer(op->operands[1]);
    auto value = evaluate_integer(op->operands[2]);
    if (
      !pointer || !pointer->root || !pointer->offset || !count || !value ||
      *count < 0 || static_cast<uint64_t>(*count) > max_summary_elements ||
      !op->operands[2]->type->is_i32()
    ) {
      return false;
    }
    for (int64_t i = 0; i < *count; ++i) {
      auto delta = checked_mul(i, int64_t{4});
      auto offset =
        delta ? checked_add(*pointer->offset, *delta) : std::nullopt;
      if (!offset)
        return false;
      ResolvedPointer element{pointer->root, *offset};
      if (!record_candidate_store(op, element, *value))
        return false;
      if (!store_memory(element, AbstractValue::integer_value(*value)))
        return false;
    }
    return true;
  }

  auto evaluate_plain_op(Op *op) -> bool {
    if (op->result)
      values.erase(op->result);
    switch (op->code) {
    case OpCode::Alloca:
      if (nested_depth != 0 || !op->result || !op->result->type->is_ptr())
        return false;
      values[op->result] =
        AbstractValue::pointer_value({op->result, int64_t{0}});
      return true;
    case OpCode::GetPtr:
      return evaluate_getptr(op);
    case OpCode::Load: {
      if (op->operands.size() != 1 || !op->result)
        return false;
      auto pointer = evaluate_pointer(op->operands[0]);
      if (!pointer || !pointer->root || !pointer->offset)
        return false;
      auto value = memory.find({pointer->root, *pointer->offset});
      if (value == memory.end())
        return false;
      values[op->result] = value->second;
      return true;
    }
    case OpCode::Store: {
      if (op->operands.size() != 2)
        return false;
      auto pointer = evaluate_pointer(op->operands[1]);
      auto value = evaluate_value(op->operands[0]);
      if (!pointer || !pointer->root || !value)
        return false;
      if (candidates.count(pointer->root)) {
        if (
          value->kind != AbstractValue::Kind::Integer ||
          !op->operands[0]->type->is_i32() ||
          !record_candidate_store(op, *pointer, value->integer)
        ) {
          return false;
        }
      }
      return store_memory(*pointer, *value);
    }
    case OpCode::Memset:
      return evaluate_memset(op);
    case OpCode::Add:
    case OpCode::Sub:
    case OpCode::Mul:
    case OpCode::Div:
    case OpCode::Mod:
    case OpCode::Eq:
    case OpCode::Ne:
    case OpCode::Lt:
    case OpCode::Gt:
    case OpCode::Le:
    case OpCode::Ge:
    case OpCode::And:
    case OpCode::Or:
    case OpCode::Xor:
      return evaluate_binary(op);
    case OpCode::ZExt: {
      if (op->operands.size() != 1 || !op->result)
        return false;
      auto value = evaluate_integer(op->operands[0]);
      if (!value)
        return false;
      values[op->result] = AbstractValue::integer_value(*value != 0);
      return true;
    }
    default:
      return false;
    }
  }

  auto evaluate_region(const Region &region) -> bool {
    ++nested_depth;
    for (auto *op : region) {
      if (!evaluate_node(op)) {
        --nested_depth;
        return false;
      }
    }
    --nested_depth;
    return true;
  }

  auto evaluate_node(Op *op) -> bool {
    if (op->code == OpCode::If) {
      if (op->operands.size() != 1)
        return false;
      auto condition = evaluate_integer(op->operands[0]);
      if (!condition)
        return false;
      auto &payload = std::get<IfPayload>(op->payload);
      if (*condition != 0)
        return evaluate_region(*payload.then_region);
      return !payload.else_region || evaluate_region(*payload.else_region);
    }
    if (op->code != OpCode::While)
      return evaluate_plain_op(op);

    auto &payload = std::get<WhilePayload>(op->payload);
    if (!payload.cond_region || !payload.loop_region)
      return false;
    for (;;) {
      if (++iterations > max_interpreter_iterations)
        return false;
      bool saw_condition = false;
      bool should_continue = false;
      ++nested_depth;
      for (auto *condition_op : *payload.cond_region) {
        if (condition_op->code == OpCode::Condition) {
          if (saw_condition || condition_op->operands.size() != 1) {
            --nested_depth;
            return false;
          }
          auto condition = evaluate_integer(condition_op->operands[0]);
          if (!condition) {
            --nested_depth;
            return false;
          }
          should_continue = *condition != 0;
          saw_condition = true;
        } else if (!evaluate_node(condition_op)) {
          --nested_depth;
          return false;
        }
      }
      --nested_depth;
      if (!saw_condition)
        return false;
      if (!should_continue)
        return true;
      if (!evaluate_region(*payload.loop_region))
        return false;
    }
  }
};

class CandidateUseAudit {
public:
  CandidateUseAudit(
    const std::unordered_map<Value *, CandidateState> &candidates,
    std::unordered_set<Value *> &invalid
  )
      : candidates(candidates), invalid(invalid) {}

  auto run(const Function &function) -> void {
    size_t top_index = 0;
    for (auto *op : function.body) {
      scan_op(op, top_index);
      ++top_index;
    }
  }

private:
  const std::unordered_map<Value *, CandidateState> &candidates;
  std::unordered_set<Value *> &invalid;

  auto candidate_root(Value *pointer) const -> Value * {
    if (!pointer || !pointer->type->is_ptr())
      return nullptr;
    auto resolved = resolve_pointer(pointer);
    return candidates.count(resolved.root) ? resolved.root : nullptr;
  }

  auto reject_pointer_operands(Op *op) -> void {
    for (auto *operand : op->operands) {
      if (auto *root = candidate_root(operand))
        invalid.insert(root);
    }
  }

  auto scan_op(Op *op, size_t top_index) -> void {
    switch (op->code) {
    case OpCode::GetPtr: {
      auto *root =
        op->operands.empty() ? nullptr : candidate_root(op->operands[0]);
      if (root) {
        auto plan = ir::analyze_getptr(
          op->operands[0]->type,
          op->result ? op->result->type : nullptr,
          op->operands.size() - 1
        );
        auto resolved =
          op->result ? resolve_pointer(op->result) : ResolvedPointer{};
        if (plan.reads_memory || resolved.root != root)
          invalid.insert(root);
      }
      for (size_t i = 1; i < op->operands.size(); ++i) {
        if (auto *index_root = candidate_root(op->operands[i]))
          invalid.insert(index_root);
      }
      break;
    }
    case OpCode::Load:
      if (!op->operands.empty()) {
        if (auto *root = candidate_root(op->operands[0])) {
          auto it = candidates.find(root);
          if (
            it == candidates.end() || top_index <= it->second.boundary_index ||
            !op->result || !op->result->type->is_i32()
          ) {
            invalid.insert(root);
          }
        }
      }
      break;
    case OpCode::Store: {
      if (!op->operands.empty()) {
        if (auto *root = candidate_root(op->operands[0]))
          invalid.insert(root);
      }
      if (op->operands.size() >= 2) {
        if (auto *root = candidate_root(op->operands[1])) {
          auto it = candidates.find(root);
          if (
            it == candidates.end() || !it->second.write_ops.count(op) ||
            top_index > it->second.boundary_index
          ) {
            invalid.insert(root);
          }
        }
      }
      break;
    }
    case OpCode::Memset:
      if (!op->operands.empty()) {
        if (auto *root = candidate_root(op->operands[0])) {
          auto it = candidates.find(root);
          if (
            it == candidates.end() || !it->second.write_ops.count(op) ||
            top_index > it->second.boundary_index
          ) {
            invalid.insert(root);
          }
        }
      }
      for (size_t i = 1; i < op->operands.size(); ++i) {
        if (auto *root = candidate_root(op->operands[i]))
          invalid.insert(root);
      }
      break;
    case OpCode::If:
    case OpCode::While:
      reject_pointer_operands(op);
      break;
    default:
      reject_pointer_operands(op);
      break;
    }

    if (op->code == OpCode::If) {
      const auto &payload = std::get<IfPayload>(op->payload);
      for (auto *nested : *payload.then_region)
        scan_op(nested, top_index);
      if (payload.else_region) {
        for (auto *nested : *payload.else_region)
          scan_op(nested, top_index);
      }
    } else if (op->code == OpCode::While) {
      const auto &payload = std::get<WhilePayload>(op->payload);
      for (auto *nested : *payload.cond_region)
        scan_op(nested, top_index);
      for (auto *nested : *payload.loop_region)
        scan_op(nested, top_index);
    }
  }
};

} // namespace

auto LocalArraySummary::segments() const
  -> std::vector<LocalArrayValueSegment> {
  std::vector<LocalArrayValueSegment> result;
  size_t begin = 0;
  while (begin < values.size()) {
    size_t end = begin + 1;
    while (end < values.size() && values[end] == values[begin])
      ++end;
    result.push_back({begin, end, values[begin]});
    begin = end;
  }
  return result;
}

auto LocalArraySummaryInfo::compute(Function &function) -> void {
  result.clear();
  ConstantPrefixInterpreter interpreter(function);
  interpreter.evaluate(function);

  std::unordered_map<Value *, CandidateState> complete;
  for (const auto &[root, state] : interpreter.states()) {
    if (
      state.boundary &&
      std::all_of(state.values.begin(), state.values.end(), [](const auto &v) {
        return v.has_value();
      })
    ) {
      complete.emplace(root, state);
    }
  }

  std::unordered_set<Value *> invalid;
  CandidateUseAudit(complete, invalid).run(function);
  for (const auto &[root, state] : complete) {
    if (invalid.count(root))
      continue;
    LocalArraySummary summary;
    summary.root = root;
    summary.initialization_boundary = state.boundary;
    summary.values.reserve(state.values.size());
    for (const auto &value : state.values)
      summary.values.push_back(*value);
    result.emplace(root, std::move(summary));
  }
}

auto LocalArraySummaryInfo::find(Value *root_or_address) const
  -> const LocalArraySummary * {
  auto pointer = resolve_pointer(root_or_address);
  auto it = result.find(pointer.root);
  return it == result.end() ? nullptr : &it->second;
}

auto LocalArraySummaryInfo::constant_at(
  Value *root, int64_t element_index
) const -> std::optional<int32_t> {
  if (element_index < 0)
    return std::nullopt;
  auto *summary = find(root);
  if (
    !summary || static_cast<uint64_t>(element_index) >= summary->values.size()
  )
    return std::nullopt;
  return summary->values[element_index];
}

auto LocalArraySummaryInfo::analyze_load(const Op &load) const
  -> std::optional<LocalArrayLoadAccess> {
  if (
    load.code != OpCode::Load || load.operands.size() != 1 || !load.result ||
    !load.result->type->is_i32()
  ) {
    return std::nullopt;
  }
  auto pointer = symbolic_pointer(load.operands[0]);
  if (!pointer)
    return std::nullopt;
  auto summary = result.find(pointer->root);
  if (summary == result.end())
    return std::nullopt;
  return LocalArrayLoadAccess{
    &summary->second, pointer->constant_offset, std::move(pointer->terms)
  };
}

auto LocalArraySummaryInfo::constant_for_load(const Op &load) const
  -> std::optional<int32_t> {
  auto access = analyze_load(load);
  if (!access || !access->terms.empty())
    return std::nullopt;
  if (
    access->constant_offset < 0 ||
    static_cast<uint64_t>(access->constant_offset) >=
      access->summary->values.size()
  ) {
    return std::nullopt;
  }
  return access->summary->values[access->constant_offset];
}

} // namespace exodus::high_ir::opt
