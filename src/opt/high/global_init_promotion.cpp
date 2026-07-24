#include "global_init_promotion.hpp"

#include "../../base/getptr.hpp"
#include "../../high/effects.hpp"
#include "../../high/rewriter.hpp"
#include "../../high/visitor.hpp"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace exodus::high_ir::opt {
namespace {

struct Pointer {
  Value *root = nullptr;
  int64_t offset = 0;
};

struct AbstractValue {
  enum class Kind : uint8_t { Unknown, Integer, Pointer };
  Kind kind = Kind::Unknown;
  int64_t integer = 0;
  Pointer pointer;

  static auto integer_value(int64_t value) -> AbstractValue {
    return {Kind::Integer, value, {}};
  }
  static auto pointer_value(Pointer value) -> AbstractValue {
    return {Kind::Pointer, 0, value};
  }
};

auto as_int(const AbstractValue &value) -> std::optional<int64_t> {
  if (value.kind != AbstractValue::Kind::Integer)
    return std::nullopt;
  return value.integer;
}

auto as_pointer(const AbstractValue &value) -> std::optional<Pointer> {
  if (value.kind != AbstractValue::Kind::Pointer)
    return std::nullopt;
  return value.pointer;
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

auto int_constant(Value *value) -> std::optional<int64_t> {
  if (!value || value->kind != ValueKind::Constant)
    return std::nullopt;
  const auto &data = static_cast<Constant *>(value)->val;
  if (!std::holds_alternative<int>(data))
    return std::nullopt;
  return std::get<int>(data);
}

auto defining_op(Value *value) -> Op * {
  if (!value || value->kind != ValueKind::OpResult)
    return nullptr;
  return static_cast<Op *>(static_cast<OpResult *>(value)->creator);
}

auto global_from_zero_getptr(Value *value) -> GlobalAddr * {
  auto *getptr = defining_op(value);
  if (
    !getptr || getptr->code != OpCode::GetPtr || getptr->operands.size() != 2 ||
    getptr->operands[0]->kind != ValueKind::GlobalVar
  ) {
    return nullptr;
  }
  auto index = int_constant(getptr->operands[1]);
  if (!index || *index != 0)
    return nullptr;
  return static_cast<GlobalAddr *>(getptr->operands[0]);
}

auto is_i32_aggregate(const std::shared_ptr<Type> &type) -> bool {
  auto current = type;
  while (current && current->is_array())
    current = std::static_pointer_cast<Array>(current)->base;
  return current && current->is_i32();
}

auto element_count(const std::shared_ptr<Type> &type)
  -> std::optional<int64_t> {
  int64_t count = 1;
  auto current = type;
  while (current && current->is_array()) {
    auto array = std::static_pointer_cast<Array>(current);
    if (
      array->len < 0 ||
      __builtin_mul_overflow(count, static_cast<int64_t>(array->len), &count)
    )
      return std::nullopt;
    current = array->base;
  }
  return count;
}

class ConstantInitializer {
public:
  ConstantInitializer(GlobalAddr *target, std::shared_ptr<Type> target_type)
      : target(target), target_type(std::move(target_type)) {}

  auto evaluate(Function &function) -> bool {
    allowed_return = nullptr;
    if (!function.body.empty() && function.body.back()->code == OpCode::Ret) {
      allowed_return = function.body.back();
      if (!allowed_return->operands.empty())
        return false;
    }
    for (size_t i = 0; i < function.args.size(); ++i) {
      values[function.args[i]] =
        i == 0 ? AbstractValue::pointer_value({target, 0}) : AbstractValue{};
    }
    if (!evaluate_region(function.body))
      return false;
    if (stores.empty())
      return false;
    auto size = element_count(target_type);
    if (!size)
      return false;
    for (const auto &[offset, value] : stores) {
      if (offset < 0 || offset % 4 != 0 || offset / 4 >= *size)
        return false;
      (void)value;
    }
    return true;
  }

  auto initializer() const -> InitVal {
    return make_initializer(target_type, 0);
  }

private:
  GlobalAddr *target;
  std::shared_ptr<Type> target_type;
  std::unordered_map<Value *, AbstractValue> values;
  std::map<int64_t, int> stores;
  std::map<std::pair<Value *, int64_t>, AbstractValue> memory;
  size_t iterations = 0;
  Op *allowed_return = nullptr;

  auto evaluate_value(Value *value) -> std::optional<AbstractValue> {
    if (!value)
      return std::nullopt;
    if (auto constant = int_constant(value))
      return AbstractValue::integer_value(*constant);
    if (value->kind == ValueKind::GlobalVar)
      return AbstractValue{};
    if (auto it = values.find(value); it != values.end())
      return it->second.kind == AbstractValue::Kind::Unknown
               ? std::nullopt
               : std::optional<AbstractValue>(it->second);
    auto *op = defining_op(value);
    if (!op)
      return std::nullopt;
    if (!evaluate_op(op))
      return std::nullopt;
    auto it = values.find(value);
    if (it == values.end() || it->second.kind == AbstractValue::Kind::Unknown)
      return std::nullopt;
    return it->second;
  }

  auto evaluate_getptr(Op *op) -> std::optional<Pointer> {
    if (op->operands.empty())
      return std::nullopt;
    auto base = evaluate_value(op->operands[0]);
    if (!base || !as_pointer(*base))
      return std::nullopt;
    auto pointer = *as_pointer(*base);
    auto plan = ir::analyze_getptr(
      op->operands[0]->type, op->result->type, op->operands.size() - 1
    );
    for (const auto &step : plan.steps) {
      if (step.kind == ir::GetPtrStep::Kind::ImplicitLoad) {
        if (pointer.root == target)
          return std::nullopt;
        auto it = memory.find({pointer.root, pointer.offset});
        if (it == memory.end())
          return std::nullopt;
        auto loaded = as_pointer(it->second);
        if (!loaded)
          return std::nullopt;
        pointer = *loaded;
        continue;
      }

      if (step.index_pos + 1 >= op->operands.size())
        return std::nullopt;
      auto index_value = evaluate_value(op->operands[step.index_pos + 1]);
      if (!index_value)
        return std::nullopt;
      auto index = as_int(*index_value);
      if (!index)
        return std::nullopt;
      auto scaled = checked_mul(*index, step.scale);
      if (!scaled)
        return std::nullopt;
      auto offset = checked_add(pointer.offset, *scaled);
      if (!offset)
        return std::nullopt;
      pointer.offset = *offset;
    }
    return pointer;
  }

  auto evaluate_op(Op *op) -> bool {
    // Structured loops execute the same operation objects repeatedly.  Their
    // results must therefore be recomputed from the current abstract memory
    // instead of reusing a value produced by an earlier iteration.
    if (op->result)
      values.erase(op->result);

    switch (op->code) {
    case OpCode::Alloca:
      values[op->result] = AbstractValue::pointer_value({op->result, 0});
      return true;
    case OpCode::GetPtr: {
      auto pointer = evaluate_getptr(op);
      if (!pointer)
        return false;
      values[op->result] = AbstractValue::pointer_value(*pointer);
      return true;
    }
    case OpCode::Load: {
      if (op->operands.size() != 1)
        return false;
      auto pointer = evaluate_value(op->operands[0]);
      if (!pointer || !as_pointer(*pointer))
        return false;
      auto location = *as_pointer(*pointer);
      if (location.root == target)
        return false;
      auto it = memory.find({location.root, location.offset});
      if (it == memory.end())
        return false;
      values[op->result] = it->second;
      return true;
    }
    case OpCode::Store: {
      if (op->operands.size() != 2)
        return false;
      auto value = evaluate_value(op->operands[0]);
      auto pointer = evaluate_value(op->operands[1]);
      if (!value || !pointer || !as_pointer(*pointer))
        return false;
      auto location = *as_pointer(*pointer);
      if (location.root == target) {
        auto integer = as_int(*value);
        if (
          !integer || *integer < std::numeric_limits<int32_t>::min() ||
          *integer > std::numeric_limits<int32_t>::max()
        )
          return false;
        stores[location.offset] = static_cast<int>(*integer);
      } else {
        memory[{location.root, location.offset}] = *value;
      }
      return true;
    }
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
    case OpCode::Ge: {
      if (op->operands.size() != 2)
        return false;
      auto lhs = evaluate_value(op->operands[0]);
      auto rhs = evaluate_value(op->operands[1]);
      if (!lhs || !rhs || !as_int(*lhs) || !as_int(*rhs))
        return false;
      auto left = *as_int(*lhs);
      auto right = *as_int(*rhs);
      std::optional<int64_t> result;
      switch (op->code) {
      case OpCode::Add:
        result = checked_add(left, right);
        break;
      case OpCode::Sub:
        result = checked_sub(left, right);
        break;
      case OpCode::Mul:
        result = checked_mul(left, right);
        break;
      case OpCode::Div:
        if (
          right == 0 ||
          (left == std::numeric_limits<int32_t>::min() && right == -1)
        )
          return false;
        result = left / right;
        break;
      case OpCode::Mod:
        if (
          right == 0 ||
          (left == std::numeric_limits<int32_t>::min() && right == -1)
        )
          return false;
        result = left % right;
        break;
      case OpCode::Eq:
        result = left == right;
        break;
      case OpCode::Ne:
        result = left != right;
        break;
      case OpCode::Lt:
        result = left < right;
        break;
      case OpCode::Gt:
        result = left > right;
        break;
      case OpCode::Le:
        result = left <= right;
        break;
      case OpCode::Ge:
        result = left >= right;
        break;
      default:
        break;
      }
      if (
        !result || *result < std::numeric_limits<int32_t>::min() ||
        *result > std::numeric_limits<int32_t>::max()
      )
        return false;
      values[op->result] = AbstractValue::integer_value(*result);
      return true;
    }
    case OpCode::ZExt: {
      if (op->operands.size() != 1)
        return false;
      auto value = evaluate_value(op->operands[0]);
      if (!value || !as_int(*value))
        return false;
      values[op->result] = AbstractValue::integer_value(*as_int(*value));
      return true;
    }
    case OpCode::Call:
    case OpCode::Memset:
    case OpCode::FAdd:
    case OpCode::FSub:
    case OpCode::FMul:
    case OpCode::FDiv:
    case OpCode::I2F:
    case OpCode::F2I:
    case OpCode::And:
    case OpCode::Or:
    case OpCode::Xor:
    case OpCode::Shl:
    case OpCode::Shr:
    case OpCode::If:
    case OpCode::While:
    case OpCode::Break:
    case OpCode::Continue:
    case OpCode::Condition:
    case OpCode::Jump:
    case OpCode::Branch:
    case OpCode::Ret:
      return false;
    }
    return false;
  }

  auto evaluate_region(const Region &region) -> bool {
    for (auto *op : region) {
      switch (op->code) {
      case OpCode::If: {
        if (op->operands.size() != 1)
          return false;
        auto condition = evaluate_value(op->operands[0]);
        if (!condition || !as_int(*condition))
          return false;
        auto &payload = std::get<IfPayload>(op->payload);
        if (*as_int(*condition)) {
          if (!evaluate_region(*payload.then_region))
            return false;
        } else if (
          payload.else_region && !evaluate_region(*payload.else_region)
        ) {
          return false;
        }
        break;
      }
      case OpCode::While: {
        auto &payload = std::get<WhilePayload>(op->payload);
        if (
          payload.cond_region->empty() ||
          payload.cond_region->back()->code != OpCode::Condition
        )
          return false;
        for (;;) {
          if (++iterations > 1'000'000)
            return false;
          int condition = 0;
          bool saw_condition = false;
          for (auto *cond_op : *payload.cond_region) {
            if (cond_op->code == OpCode::Condition) {
              if (saw_condition || cond_op->operands.size() != 1)
                return false;
              auto value = evaluate_value(cond_op->operands[0]);
              if (!value || !as_int(*value))
                return false;
              condition = *as_int(*value) != 0;
              saw_condition = true;
            } else if (!evaluate_op(cond_op)) {
              return false;
            }
          }
          if (!saw_condition)
            return false;
          if (!condition)
            break;
          if (!evaluate_region(*payload.loop_region))
            return false;
        }
        break;
      }
      case OpCode::Ret:
        if (op != allowed_return)
          return false;
        break;
      default:
        if (!evaluate_op(op))
          return false;
        break;
      }
    }
    return true;
  }

  auto
  make_initializer(const std::shared_ptr<Type> &type, int64_t base_offset) const
    -> InitVal {
    if (!type->is_array()) {
      auto it = stores.find(base_offset);
      return {it == stores.end() ? InitVal{ZeroInit{}} : InitVal{it->second}};
    }
    auto array = std::static_pointer_cast<Array>(type);
    InitList list;
    auto stride = array->base->byte_size();
    for (int i = 0; i < array->len; ++i)
      list.values.push_back(
        make_initializer(array->base, base_offset + i * stride)
      );
    return {std::move(list)};
  }
};

struct CallCollector : RecursiveOpVisitor<CallCollector> {
  std::unordered_map<std::string, std::vector<std::pair<Function *, Op *>>>
    calls;
  Function *current = nullptr;

  using RecursiveOpVisitor<CallCollector>::visit;
  auto visit(Function &function) -> void {
    current = &function;
    RecursiveOpVisitor<CallCollector>::visit(function);
  }
  auto visit(Op *op, OpTag<OpCode::Call>) -> void {
    calls[std::get<CallPayload>(op->payload).func_name].push_back(
      {current, op}
    );
  }
};

auto op_uses_global_root(const Op &op, GlobalAddr *target) -> bool {
  bool derives_address_without_observing = false;
  if (
    op.code == OpCode::GetPtr && !op.operands.empty() && op.result &&
    op.operands[0]->type->is_ptr() && op.result->type->is_ptr()
  ) {
    derives_address_without_observing =
      !ir::analyze_getptr(
         op.operands[0]->type, op.result->type, op.operands.size() - 1
      )
         .reads_memory;
  }

  if (!derives_address_without_observing) {
    for (auto *operand : op.operands) {
      if (get_addr_root(operand) == target)
        return true;
    }
  }

  if (op.code == OpCode::If) {
    const auto &payload = std::get<IfPayload>(op.payload);
    for (auto *nested : *payload.then_region) {
      if (op_uses_global_root(*nested, target))
        return true;
    }
    if (payload.else_region) {
      for (auto *nested : *payload.else_region) {
        if (op_uses_global_root(*nested, target))
          return true;
      }
    }
  } else if (op.code == OpCode::While) {
    const auto &payload = std::get<WhilePayload>(op.payload);
    for (auto *nested : *payload.cond_region) {
      if (op_uses_global_root(*nested, target))
        return true;
    }
    for (auto *nested : *payload.loop_region) {
      if (op_uses_global_root(*nested, target))
        return true;
    }
  }

  return false;
}

auto target_is_unused_before_call(
  const Function &caller, const Op *call, GlobalAddr *target
) -> bool {
  auto call_it = std::find(caller.body.begin(), caller.body.end(), call);
  if (call_it == caller.body.end())
    return false;

  return std::none_of(caller.body.begin(), call_it, [target](const Op *op) {
    return op_uses_global_root(*op, target);
  });
}

auto op_references_global_root(const Op &op, GlobalAddr *target) -> bool {
  for (auto *operand : op.operands) {
    if (get_addr_root(operand) == target)
      return true;
  }

  if (op.code == OpCode::If) {
    const auto &payload = std::get<IfPayload>(op.payload);
    for (auto *nested : *payload.then_region) {
      if (op_references_global_root(*nested, target))
        return true;
    }
    if (payload.else_region) {
      for (auto *nested : *payload.else_region) {
        if (op_references_global_root(*nested, target))
          return true;
      }
    }
  } else if (op.code == OpCode::While) {
    const auto &payload = std::get<WhilePayload>(op.payload);
    for (auto *nested : *payload.cond_region) {
      if (op_references_global_root(*nested, target))
        return true;
    }
    for (auto *nested : *payload.loop_region) {
      if (op_references_global_root(*nested, target))
        return true;
    }
  }

  return false;
}

auto target_is_not_referenced_outside_caller(
  const Module &module, const Function &caller, GlobalAddr *target
) -> bool {
  for (const auto &function : module.functions) {
    if (function.get() == &caller || function->is_decl)
      continue;
    for (auto *op : function->body) {
      if (op_references_global_root(*op, target))
        return false;
    }
  }
  return true;
}

} // namespace

auto GlobalInitPromotion::run(Module &, exodus::opt::ModuleAnalysisManager &)
  -> exodus::opt::PreservedAnalysis {
  CallCollector collector;
  for (auto &function : module->functions) {
    if (!function->is_decl)
      collector.visit(*function);
  }

  bool changed = false;
  for (auto &function : module->functions) {
    if (function->is_decl)
      continue;
    auto it = collector.calls.find(function->name);
    if (it == collector.calls.end() || it->second.size() != 1)
      continue;
    auto [caller, call] = it->second.front();
    if (
      call->operands.size() != function->args.size() || function->args.empty()
    )
      continue;
    if (collector.calls.count(caller->name))
      continue;
    auto function_type = std::static_pointer_cast<Func>(function->type);
    if (
      !function_type->ret_type->is_void() ||
      (call->result && !call->result->users.empty())
    )
      continue;
    auto *target = global_from_zero_getptr(call->operands[0]);
    if (!target || target->type->is_ptr() == false)
      continue;
    if (!target_is_unused_before_call(*caller, call, target))
      continue;
    if (!target_is_not_referenced_outside_caller(*module, *caller, target))
      continue;
    auto global_it = std::find_if(
      module->globals.begin(),
      module->globals.end(),
      [target](const auto &global) { return global->addr == target; }
    );
    if (
      global_it == module->globals.end() ||
      !std::holds_alternative<ZeroInit>((*global_it)->init.data) ||
      !is_i32_aggregate((*global_it)->type)
    )
      continue;

    ConstantInitializer evaluator(
      target, std::static_pointer_cast<Ptr>(target->type)->target
    );
    if (!evaluator.evaluate(*function))
      continue;

    (*global_it)->init = evaluator.initializer();
    IRRewriter rewriter;
    rewriter.eraseOp(call);
    rewriter.finalize(*caller);
    changed = true;
  }
  return changed ? exodus::opt::PreservedAnalysis::none()
                 : exodus::opt::PreservedAnalysis::all();
}

} // namespace exodus::high_ir::opt
