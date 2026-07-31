#include "guarded_bitwise_idiom.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace exodus::high_ir::opt {
namespace {

auto defining_op(Value *value) -> Op * {
  if (!value || value->kind != ValueKind::OpResult)
    return nullptr;
  return static_cast<Op *>(static_cast<OpResult *>(value)->creator);
}

auto int_constant(Value *value) -> std::optional<int> {
  if (!value || value->kind != ValueKind::Constant)
    return std::nullopt;
  auto *constant = static_cast<Constant *>(value);
  if (!std::holds_alternative<int>(constant->val))
    return std::nullopt;
  return std::get<int>(constant->val);
}

auto is_alloca(Value *value) -> bool {
  auto *op = defining_op(value);
  return op && op->code == OpCode::Alloca && op->result == value;
}

auto load_from(Value *value, Value *address, Op *&load) -> bool {
  load = defining_op(value);
  return load && load->code == OpCode::Load && load->result == value &&
         load->operands.size() == 1 && load->operands[0] == address;
}

auto match_binary(
  Value *value,
  OpCode code,
  Value *address,
  int constant,
  bool commutative,
  std::unordered_set<Op *> &used
) -> bool {
  auto *binary = defining_op(value);
  if (!binary || binary->code != code || binary->operands.size() != 2)
    return false;

  Op *load = nullptr;
  auto matches = [&](Value *lhs, Value *rhs) {
    return load_from(lhs, address, load) && int_constant(rhs) == constant;
  };
  if (!matches(binary->operands[0], binary->operands[1])) {
    if (!commutative || !matches(binary->operands[1], binary->operands[0]))
      return false;
  }
  used.insert(load);
  used.insert(binary);
  return true;
}

auto match_update_store(
  Op *store,
  Value *address,
  OpCode code,
  int constant,
  bool commutative,
  std::unordered_set<Op *> &used
) -> bool {
  if (
    !store || store->code != OpCode::Store || store->operands.size() != 2 ||
    store->operands[1] != address ||
    !match_binary(
      store->operands[0], code, address, constant, commutative, used
    )
  ) {
    return false;
  }
  used.insert(store);
  return true;
}

auto stores_to(const Region &region, Value *address) -> std::vector<Op *> {
  std::vector<Op *> stores;
  for (auto *op : region) {
    if (
      op->code == OpCode::Store && op->operands.size() == 2 &&
      op->operands[1] == address
    ) {
      stores.push_back(op);
    }
  }
  return stores;
}

auto collect_ops(const Region &region, std::unordered_set<Op *> &ops) -> void {
  for (auto *op : region) {
    ops.insert(op);
    if (op->code == OpCode::If) {
      const auto &payload = std::get<IfPayload>(op->payload);
      collect_ops(*payload.then_region, ops);
      if (payload.else_region)
        collect_ops(*payload.else_region, ops);
    } else if (op->code == OpCode::While) {
      const auto &payload = std::get<WhilePayload>(op->payload);
      collect_ops(*payload.cond_region, ops);
      collect_ops(*payload.loop_region, ops);
    }
  }
}

auto match_result_update(
  Op *if_op,
  Value *result_address,
  Value *power_address,
  std::unordered_set<Op *> &used
) -> bool {
  if (!if_op || if_op->code != OpCode::If || if_op->operands.size() != 1)
    return false;
  const auto &payload = std::get<IfPayload>(if_op->payload);
  if (payload.else_region || payload.then_region->size() != 4)
    return false;

  auto it = payload.then_region->begin();
  auto *result_load = *it++;
  auto *power_load = *it++;
  auto *add = *it++;
  auto *store = *it++;
  if (
    result_load->code != OpCode::Load || result_load->operands.size() != 1 ||
    result_load->operands[0] != result_address || !result_load->result ||
    power_load->code != OpCode::Load || power_load->operands.size() != 1 ||
    power_load->operands[0] != power_address || !power_load->result ||
    add->code != OpCode::Add || add->operands.size() != 2 || !add->result ||
    store->code != OpCode::Store || store->operands.size() != 2 ||
    store->operands[0] != add->result || store->operands[1] != result_address
  ) {
    return false;
  }
  const bool operands_match = (add->operands[0] == result_load->result &&
                               add->operands[1] == power_load->result) ||
                              (add->operands[1] == result_load->result &&
                               add->operands[0] == power_load->result);
  if (!operands_match)
    return false;
  used.insert(if_op);
  used.insert(result_load);
  used.insert(power_load);
  used.insert(add);
  used.insert(store);
  return true;
}

auto match_xor_condition(
  Op *if_op, Value *bit_a, Value *bit_b, std::unordered_set<Op *> &used
) -> bool {
  auto *compare = defining_op(if_op->operands[0]);
  if (!compare || compare->code != OpCode::Ne || compare->operands.size() != 2)
    return false;
  Op *lhs_load = nullptr;
  Op *rhs_load = nullptr;
  const bool matches = (load_from(compare->operands[0], bit_a, lhs_load) &&
                        load_from(compare->operands[1], bit_b, rhs_load)) ||
                       (load_from(compare->operands[0], bit_b, lhs_load) &&
                        load_from(compare->operands[1], bit_a, rhs_load));
  if (!matches)
    return false;
  used.insert(lhs_load);
  used.insert(rhs_load);
  used.insert(compare);
  return true;
}

auto match_eq_one(Value *value, Value *address, Op *&load, Op *&compare)
  -> bool {
  compare = defining_op(value);
  if (!compare || compare->code != OpCode::Eq || compare->operands.size() != 2)
    return false;
  return (load_from(compare->operands[0], address, load) &&
          int_constant(compare->operands[1]) == 1) ||
         (load_from(compare->operands[1], address, load) &&
          int_constant(compare->operands[0]) == 1);
}

auto match_and_condition_order(
  const Region &body,
  Op *result_if,
  Op *short_circuit_if,
  Value *first_bit,
  Value *second_bit,
  std::unordered_set<Op *> &used
) -> bool {
  Op *first_load = nullptr;
  Op *first_compare = nullptr;
  if (!match_eq_one(
        short_circuit_if->operands[0], first_bit, first_load, first_compare
      )) {
    return false;
  }
  const auto &short_payload = std::get<IfPayload>(short_circuit_if->payload);
  if (short_payload.else_region || short_payload.then_region->size() != 3)
    return false;
  auto inner = short_payload.then_region->begin();
  auto *second_load = *inner++;
  auto *second_compare = *inner++;
  auto *second_store = *inner++;
  Op *matched_second_load = nullptr;
  Op *matched_second_compare = nullptr;
  if (
    !match_eq_one(
      second_compare->result,
      second_bit,
      matched_second_load,
      matched_second_compare
    ) ||
    matched_second_load != second_load ||
    matched_second_compare != second_compare ||
    second_store->code != OpCode::Store || second_store->operands.size() != 2 ||
    second_store->operands[0] != second_compare->result
  ) {
    return false;
  }
  auto *bool_address = second_store->operands[1];
  if (!is_alloca(bool_address))
    return false;

  Op *seed_store = nullptr;
  for (auto *op : body) {
    if (
      op->code == OpCode::Store && op->operands.size() == 2 &&
      op->operands[0] == first_compare->result &&
      op->operands[1] == bool_address
    ) {
      if (seed_store)
        return false;
      seed_store = op;
    }
  }
  if (!seed_store)
    return false;

  auto *truth_compare = defining_op(result_if->operands[0]);
  if (
    !truth_compare || truth_compare->code != OpCode::Ne ||
    truth_compare->operands.size() != 2
  ) {
    return false;
  }
  Value *zext_value = nullptr;
  if (int_constant(truth_compare->operands[0]) == 0)
    zext_value = truth_compare->operands[1];
  else if (int_constant(truth_compare->operands[1]) == 0)
    zext_value = truth_compare->operands[0];
  else
    return false;
  auto *zext = defining_op(zext_value);
  if (!zext || zext->code != OpCode::ZExt || zext->operands.size() != 1)
    return false;
  Op *bool_load = nullptr;
  if (!load_from(zext->operands[0], bool_address, bool_load))
    return false;

  used.insert(defining_op(bool_address));
  used.insert(first_load);
  used.insert(first_compare);
  used.insert(seed_store);
  used.insert(short_circuit_if);
  used.insert(second_load);
  used.insert(second_compare);
  used.insert(second_store);
  used.insert(bool_load);
  used.insert(zext);
  used.insert(truth_compare);
  return true;
}

auto match_and_condition(
  const Region &body,
  Op *result_if,
  Value *bit_a,
  Value *bit_b,
  std::unordered_set<Op *> &used
) -> bool {
  Op *short_circuit_if = nullptr;
  for (auto *op : body) {
    if (op->code == OpCode::If && op != result_if) {
      if (short_circuit_if)
        return false;
      short_circuit_if = op;
    }
  }
  if (!short_circuit_if || short_circuit_if->operands.size() != 1)
    return false;
  auto attempt = used;
  if (
    match_and_condition_order(
      body, result_if, short_circuit_if, bit_a, bit_b, attempt
    )
  ) {
    used = std::move(attempt);
    return true;
  }
  attempt = used;
  if (
    match_and_condition_order(
      body, result_if, short_circuit_if, bit_b, bit_a, attempt
    )
  ) {
    used = std::move(attempt);
    return true;
  }
  return false;
}

auto add_operands(Op *op, std::initializer_list<Value *> operands) -> void {
  op->operands.assign(operands.begin(), operands.end());
  for (auto *operand : op->operands)
    operand->addUse(op);
}

auto make_result(Module *module, Op *op, const std::shared_ptr<Type> &type)
  -> void {
  op->result = module->ctx.make_value<OpResult>(type, op);
}

} // namespace

auto GuardedBitwiseIdiom::run(
  Module &current, exodus::opt::ModuleAnalysisManager &
) -> exodus::opt::PreservedAnalysis {
  bool changed = false;
  for (auto &function : current.functions) {
    if (function->is_decl)
      continue;
    if (auto kind = match(*function)) {
      specialize(*function, *kind);
      changed = true;
    }
  }
  return changed ? exodus::opt::PreservedAnalysis::none()
                 : exodus::opt::PreservedAnalysis::all();
}

auto GuardedBitwiseIdiom::match(Function &function) const
  -> std::optional<Kind> {
  auto type = std::static_pointer_cast<Func>(function.type);
  if (
    function.args.size() != 2 || !type->ret_type->is_i32() ||
    !function.args[0]->type->is_i32() || !function.args[1]->type->is_i32()
  ) {
    return std::nullopt;
  }

  std::vector<Value *> allocas;
  std::vector<Op *> top_stores;
  Op *loop = nullptr;
  Op *ret = nullptr;
  for (auto *op : function.body) {
    if (op->code == OpCode::Alloca && op->result)
      allocas.push_back(op->result);
    else if (op->code == OpCode::Store)
      top_stores.push_back(op);
    else if (op->code == OpCode::While) {
      if (loop)
        return std::nullopt;
      loop = op;
    } else if (op->code == OpCode::Load) {
      continue;
    } else if (op->code == OpCode::Ret) {
      if (ret)
        return std::nullopt;
      ret = op;
    } else {
      return std::nullopt;
    }
  }
  if (
    allocas.size() != 7 || top_stores.size() != 5 || !loop || !ret ||
    ret->operands.size() != 1
  ) {
    return std::nullopt;
  }

  std::array<Value *, 2> input_addresses{nullptr, nullptr};
  for (size_t index = 0; index < function.args.size(); ++index) {
    for (auto *store : top_stores) {
      if (
        store->operands.size() == 2 &&
        store->operands[0] == function.args[index]
      ) {
        if (input_addresses[index])
          return std::nullopt;
        input_addresses[index] = store->operands[1];
      }
    }
    if (!is_alloca(input_addresses[index]))
      return std::nullopt;
  }

  Value *length_address = nullptr;
  Value *result_address = nullptr;
  Value *power_address = nullptr;
  for (auto *store : top_stores) {
    if (store->operands.size() != 2)
      return std::nullopt;
    auto value = int_constant(store->operands[0]);
    if (!value)
      continue;
    auto **destination = *value == 32  ? &length_address
                         : *value == 0 ? &result_address
                         : *value == 1 ? &power_address
                                       : nullptr;
    if (!destination || *destination)
      return std::nullopt;
    *destination = store->operands[1];
  }
  if (
    !is_alloca(length_address) || !is_alloca(result_address) ||
    !is_alloca(power_address)
  ) {
    return std::nullopt;
  }

  std::vector<Value *> bit_addresses;
  for (auto *address : allocas) {
    if (
      address != input_addresses[0] && address != input_addresses[1] &&
      address != length_address && address != result_address &&
      address != power_address
    ) {
      bit_addresses.push_back(address);
    }
  }
  if (bit_addresses.size() != 2)
    return std::nullopt;

  Op *return_load = nullptr;
  if (!load_from(ret->operands[0], result_address, return_load))
    return std::nullopt;
  size_t top_loads = 0;
  for (auto *op : function.body)
    top_loads += op->code == OpCode::Load;
  if (top_loads != 1 || function.body.size() != 15)
    return std::nullopt;
  auto loop_position =
    std::find(function.body.begin(), function.body.end(), loop);
  if (
    loop_position == function.body.end() ||
    std::next(loop_position) == function.body.end() ||
    *std::next(loop_position) != return_load ||
    std::next(loop_position, 2) == function.body.end() ||
    *std::next(loop_position, 2) != ret ||
    std::next(loop_position, 3) != function.body.end()
  ) {
    return std::nullopt;
  }

  const auto &payload = std::get<WhilePayload>(loop->payload);
  if (payload.cond_region->size() != 3)
    return std::nullopt;
  auto cond_it = payload.cond_region->begin();
  auto *length_load = *cond_it++;
  auto *length_compare = *cond_it++;
  auto *condition = *cond_it++;
  if (
    length_load->code != OpCode::Load || length_load->operands.size() != 1 ||
    length_load->operands[0] != length_address || !length_load->result ||
    length_compare->code != OpCode::Ne ||
    length_compare->operands.size() != 2 ||
    length_compare->operands[0] != length_load->result ||
    int_constant(length_compare->operands[1]) != 0 ||
    condition->code != OpCode::Condition || condition->operands.size() != 1 ||
    condition->operands[0] != length_compare->result
  ) {
    return std::nullopt;
  }

  auto &body = *payload.loop_region;
  std::unordered_map<Op *, size_t> body_order;
  size_t body_index = 0;
  for (auto *op : body)
    body_order[op] = body_index++;
  auto precedes = [&](Op *lhs, Op *rhs) {
    auto lhs_position = body_order.find(lhs);
    auto rhs_position = body_order.find(rhs);
    return lhs_position != body_order.end() &&
           rhs_position != body_order.end() &&
           lhs_position->second < rhs_position->second;
  };
  auto ordered_update = [&](Op *store, Value *source_address) {
    if (!store || store->operands.empty())
      return false;
    auto *binary = defining_op(store->operands[0]);
    if (!binary || binary->operands.empty())
      return false;
    Op *load = nullptr;
    if (!load_from(binary->operands[0], source_address, load)) {
      if (
        binary->operands.size() != 2 ||
        !load_from(binary->operands[1], source_address, load)
      ) {
        return false;
      }
    }
    return precedes(load, binary) && precedes(binary, store);
  };
  std::unordered_set<Op *> common_used{length_load, length_compare, condition};
  std::array<Op *, 2> input_update_stores{nullptr, nullptr};
  for (size_t index = 0; index < 2; ++index) {
    auto input_updates = stores_to(body, input_addresses[index]);
    if (
      input_updates.size() != 1 || !match_update_store(
                                     input_updates.front(),
                                     input_addresses[index],
                                     OpCode::Div,
                                     2,
                                     false,
                                     common_used
                                   )
    ) {
      return std::nullopt;
    }
    input_update_stores[index] = input_updates.front();
  }
  auto power_updates = stores_to(body, power_address);
  auto length_updates = stores_to(body, length_address);
  if (
    power_updates.size() != 1 || length_updates.size() != 1 ||
    !match_update_store(
      power_updates.front(), power_address, OpCode::Mul, 2, true, common_used
    ) ||
    !match_update_store(
      length_updates.front(), length_address, OpCode::Sub, 1, false, common_used
    )
  ) {
    return std::nullopt;
  }

  auto try_kind =
    [&](Kind kind, Value *bit_a, Value *bit_b) -> std::optional<Kind> {
    auto used = common_used;
    std::array<Op *, 2> bit_stores_by_input{nullptr, nullptr};
    for (size_t index = 0; index < 2; ++index) {
      auto bit_stores = stores_to(body, index == 0 ? bit_a : bit_b);
      if (bit_stores.size() != 1)
        return std::nullopt;
      auto *input = input_addresses[index];
      if (
        bit_stores.front()->operands.size() != 2 ||
        !match_binary(
          bit_stores.front()->operands[0], OpCode::Mod, input, 2, false, used
        )
      ) {
        return std::nullopt;
      }
      used.insert(bit_stores.front());
      bit_stores_by_input[index] = bit_stores.front();
    }

    Op *result_if = nullptr;
    for (auto *op : body) {
      if (op->code != OpCode::If)
        continue;
      auto candidate_used = used;
      if (
        match_result_update(op, result_address, power_address, candidate_used)
      ) {
        if (result_if)
          return std::nullopt;
        result_if = op;
        used = std::move(candidate_used);
      }
    }
    if (!result_if || !stores_to(body, result_address).empty())
      return std::nullopt;

    bool condition_matches =
      kind == Kind::Xor
        ? match_xor_condition(result_if, bit_a, bit_b, used)
        : match_and_condition(body, result_if, bit_a, bit_b, used);
    if (!condition_matches)
      return std::nullopt;

    for (size_t index = 0; index < 2; ++index) {
      if (
        !ordered_update(bit_stores_by_input[index], input_addresses[index]) ||
        !ordered_update(input_update_stores[index], input_addresses[index]) ||
        !precedes(bit_stores_by_input[index], input_update_stores[index]) ||
        !precedes(input_update_stores[index], result_if)
      ) {
        return std::nullopt;
      }
    }
    if (
      !ordered_update(power_updates.front(), power_address) ||
      !ordered_update(length_updates.front(), length_address) ||
      !precedes(result_if, power_updates.front()) ||
      !precedes(power_updates.front(), length_updates.front())
    ) {
      return std::nullopt;
    }
    for (auto *op : body) {
      if (op->code == OpCode::If && op != result_if && !precedes(op, result_if))
        return std::nullopt;
    }

    std::unordered_set<Op *> all_ops;
    collect_ops(*payload.cond_region, all_ops);
    collect_ops(body, all_ops);
    if (all_ops != used)
      return std::nullopt;
    return kind;
  };

  for (auto kind : {Kind::And, Kind::Xor}) {
    if (auto matched = try_kind(kind, bit_addresses[0], bit_addresses[1]))
      return matched;
    if (auto matched = try_kind(kind, bit_addresses[1], bit_addresses[0]))
      return matched;
  }
  return std::nullopt;
}

auto GuardedBitwiseIdiom::specialize(Function &function, Kind kind) -> void {
  // Keep the cold, semantics-preserving loop fallback outlined. Inlining a
  // versioned function would duplicate that loop at every call site and erase
  // the code-size benefit of the fast path.
  function.no_inline = true;

  // Proof for the guarded path: for non-negative i32 operands, iteration k
  // (0 <= k <= 30) reads floor(a / 2^k) % 2 and floor(b / 2^k) % 2, then adds
  // the selected bit with weight 2^k. The sum is at most INT_MAX. At k = 31
  // both quotients are zero, so the wrapped power update cannot affect the
  // returned value. This relies on Exodus IR's two's-complement i32 arithmetic;
  // negative operands retain the original truncating div/rem loop.

  auto *zero = module->ctx.make_const(I32::get(), 0);
  auto *lhs_non_negative = module->ctx.make_op(OpCode::Ge);
  add_operands(lhs_non_negative, {function.args[0], zero});
  make_result(module, lhs_non_negative, Bool::get());

  auto *rhs_non_negative = module->ctx.make_op(OpCode::Ge);
  add_operands(rhs_non_negative, {function.args[1], zero});
  make_result(module, rhs_non_negative, Bool::get());

  auto *lhs_guard = module->ctx.make_op(OpCode::If);
  auto rhs_check = std::make_unique<Region>();
  lhs_guard->payload = IfPayload{std::move(rhs_check), std::nullopt};
  add_operands(lhs_guard, {lhs_non_negative->result});
  auto &rhs_region = *std::get<IfPayload>(lhs_guard->payload).then_region;
  rhs_region.push_back(rhs_non_negative);

  auto *rhs_guard = module->ctx.make_op(OpCode::If);
  auto fast = std::make_unique<Region>();
  rhs_guard->payload = IfPayload{std::move(fast), std::nullopt};
  add_operands(rhs_guard, {rhs_non_negative->result});
  auto &fast_region = *std::get<IfPayload>(rhs_guard->payload).then_region;

  auto *native =
    module->ctx.make_op(kind == Kind::And ? OpCode::And : OpCode::Xor);
  add_operands(native, {function.args[0], function.args[1]});
  make_result(module, native, I32::get());
  fast_region.push_back(native);

  auto *ret = module->ctx.make_op(OpCode::Ret);
  add_operands(ret, {native->result});
  fast_region.push_back(ret);
  rhs_region.push_back(rhs_guard);

  function.body.push_front(lhs_guard);
  function.body.push_front(lhs_non_negative);
}

} // namespace exodus::high_ir::opt
