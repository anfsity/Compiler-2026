#include "pure_call_loop_sink.hpp"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

namespace exodus::high_ir::opt {
namespace {

auto add_operands(Op *op, std::initializer_list<Value *> operands) -> void {
  op->operands.assign(operands.begin(), operands.end());
  for (auto *operand : op->operands)
    operand->addUse(op);
}

auto make_result(Module *module, Op *op, const std::shared_ptr<Type> &type)
  -> void {
  op->result = module->ctx.make_value<OpResult>(type, op);
}

auto is_pure_scalar_op(const Op &op) -> bool {
  if (
    op.code == OpCode::Call || op.code == OpCode::Store ||
    op.code == OpCode::Memset || op.code == OpCode::Load
  ) {
    return false;
  }
  auto effects = get_op_effects(op);
  return !effects.has_unknown_effect && !effects.has_control_effect &&
         !effects.reads_memory() && !effects.writes_memory();
}

} // namespace

auto PureCallLoopSink::run(Module &, exodus::opt::ModuleAnalysisManager &)
  -> exodus::opt::PreservedAnalysis {
  functions.clear();
  for (auto &function : module->functions)
    functions.emplace(function->name, function.get());
  summaries = get_function_effects(*module);

  bool changed = false;
  for (auto &function : module->functions) {
    if (!function->is_decl)
      changed |= simplify_function(*function);
  }
  return changed ? exodus::opt::PreservedAnalysis::none()
                 : exodus::opt::PreservedAnalysis::all();
}

auto PureCallLoopSink::simplify_function(Function &function) -> bool {
  return simplify_region(function.body);
}

auto PureCallLoopSink::simplify_region(Region &region) -> bool {
  bool changed = false;
  for (auto it = region.begin(); it != region.end();) {
    auto current = it++;
    auto *op = *current;
    if (op->code == OpCode::If) {
      auto &payload = std::get<IfPayload>(op->payload);
      changed |= simplify_region(*payload.then_region);
      if (payload.else_region)
        changed |= simplify_region(*payload.else_region);
      continue;
    }
    if (op->code != OpCode::While)
      continue;

    auto candidate = match(region, current);
    if (candidate && transform(region, current, *candidate)) {
      changed = true;
      continue;
    }

    auto &payload = std::get<WhilePayload>(op->payload);
    changed |= simplify_region(*payload.cond_region);
    changed |= simplify_region(*payload.loop_region);
  }
  return changed;
}

auto PureCallLoopSink::defining_op(Value *value) const -> Op * {
  if (!value || value->kind != ValueKind::OpResult)
    return nullptr;
  return static_cast<Op *>(static_cast<OpResult *>(value)->creator);
}

auto PureCallLoopSink::int_constant(Value *value) const -> std::optional<int> {
  if (!value || value->kind != ValueKind::Constant)
    return std::nullopt;
  auto *constant = static_cast<Constant *>(value);
  if (!std::holds_alternative<int>(constant->val))
    return std::nullopt;
  return std::get<int>(constant->val);
}

auto PureCallLoopSink::is_local_root(Value *value) const -> bool {
  auto *root = root_of(value);
  auto *op = defining_op(root);
  return op && op->code == OpCode::Alloca;
}

auto PureCallLoopSink::root_of(Value *value) const -> Value * {
  return get_addr_root(value);
}

auto PureCallLoopSink::is_in_region(const Region &region, Op *op) const
  -> bool {
  return std::find(region.begin(), region.end(), op) != region.end();
}

auto PureCallLoopSink::has_only_user(Value *value, Op *user) const -> bool {
  return value && value->users.size() == 1 && value->users.front() == user;
}

auto PureCallLoopSink::call_reads_are_disjoint(
  const Candidate &candidate, const OpEffects &call_effects
) const -> bool {
  if (
    call_effects.has_unknown_effect || call_effects.has_control_effect ||
    call_effects.writes_memory()
  )
    return false;
  std::vector<Value *> writes{
    candidate.counter_address, candidate.answer_address
  };
  for (auto *read : call_effects.reads) {
    auto *read_root = root_of(read);
    if (!read_root)
      return false;
    for (auto *write : writes) {
      auto *write_root = root_of(write);
      if (!write_root || write_root == read_root)
        return false;
    }
  }
  return true;
}

auto PureCallLoopSink::collect_dependency(
  Value *value,
  const Region &body,
  Candidate &candidate,
  std::unordered_set<Op *> &visiting
) -> bool {
  auto *definition = defining_op(value);
  if (!definition || !is_in_region(body, definition))
    return true;
  if (!visiting.insert(definition).second)
    return false;
  if (definition->code == OpCode::Load) {
    if (definition != candidate.iteration_counter_load)
      return false;
    candidate.moved_ops.insert(definition);
    visiting.erase(definition);
    return true;
  }
  if (!is_pure_scalar_op(*definition))
    return false;

  candidate.moved_ops.insert(definition);
  for (auto *operand : definition->operands) {
    if (!collect_dependency(operand, body, candidate, visiting))
      return false;
  }
  visiting.erase(definition);
  return true;
}

auto PureCallLoopSink::match(Region &parent, Region::iterator position)
  -> std::optional<Candidate> {
  auto *loop = *position;
  if (loop->code != OpCode::While || position == parent.begin())
    return std::nullopt;
  auto &payload = std::get<WhilePayload>(loop->payload);
  if (
    payload.cond_region->size() != 3 || payload.loop_region->size() < 4 ||
    payload.cond_region->back()->code != OpCode::Condition
  ) {
    return std::nullopt;
  }

  auto cond_it = payload.cond_region->begin();
  auto *counter_load = *cond_it++;
  auto *compare = *cond_it++;
  auto *condition = *cond_it++;
  if (
    counter_load->code != OpCode::Load || counter_load->operands.size() != 1 ||
    compare->code != OpCode::Ne || compare->operands.size() != 2 ||
    compare->operands[0] != counter_load->result ||
    !int_constant(compare->operands[1]) ||
    *int_constant(compare->operands[1]) != 0 ||
    condition->code != OpCode::Condition || condition->operands.size() != 1 ||
    condition->operands[0] != compare->result ||
    !has_only_user(counter_load->result, compare) ||
    !has_only_user(compare->result, condition)
  ) {
    return std::nullopt;
  }

  auto *counter_address = counter_load->operands[0];
  if (
    !is_local_root(counter_address) ||
    root_of(counter_address) != counter_address
  ) {
    return std::nullopt;
  }

  Value *initial_counter = nullptr;
  Op *initial_store = nullptr;
  for (auto it = parent.begin(); it != position; ++it) {
    auto *op = *it;
    if (
      op->code != OpCode::Store || op->operands.size() != 2 ||
      op->operands[1] != counter_address
    ) {
      continue;
    }
    if (initial_store)
      return std::nullopt;
    initial_store = op;
    initial_counter = op->operands[0];
  }
  if (!initial_store || !initial_counter || !initial_counter->type->is_i32())
    return std::nullopt;

  auto &body = *payload.loop_region;
  auto tail_store_it = std::prev(body.end());
  auto tail_sub_it = std::prev(tail_store_it);
  auto tail_load_it = std::prev(tail_sub_it);
  auto *tail_store = *tail_store_it;
  auto *tail_sub = *tail_sub_it;
  auto *tail_load = *tail_load_it;
  if (
    tail_load->code != OpCode::Load || tail_load->operands.size() != 1 ||
    tail_load->operands[0] != counter_address ||
    tail_sub->code != OpCode::Sub || tail_sub->operands.size() != 2 ||
    tail_store->code != OpCode::Store || tail_store->operands.size() != 2 ||
    tail_store->operands[0] != tail_sub->result ||
    tail_store->operands[1] != counter_address ||
    !has_only_user(tail_load->result, tail_sub) ||
    !has_only_user(tail_sub->result, tail_store)
  ) {
    return std::nullopt;
  }
  if (
    tail_sub->operands[0] != tail_load->result ||
    !int_constant(tail_sub->operands[1]) ||
    *int_constant(tail_sub->operands[1]) != 1
  ) {
    return std::nullopt;
  }

  std::vector<Op *> calls;
  for (auto *op : body) {
    if (op->code == OpCode::Call)
      calls.push_back(op);
    if (op->code == OpCode::If || op->code == OpCode::While)
      return std::nullopt;
  }
  if (calls.size() != 1)
    return std::nullopt;
  auto *call = calls.front();
  if (!call->result || !call->result->type->is_i32())
    return std::nullopt;

  const auto &call_payload = std::get<CallPayload>(call->payload);
  auto function_it = functions.find(call_payload.func_name);
  if (function_it == functions.end() || function_it->second->is_decl)
    return std::nullopt;
  auto summary_it = summaries.find(function_it->second);
  if (summary_it == summaries.end())
    return std::nullopt;
  auto call_effects =
    get_call_effects(*call, *function_it->second, summary_it->second);

  Candidate candidate;
  candidate.loop = loop;
  candidate.call = call;
  candidate.counter_address = counter_address;
  candidate.initial_counter = initial_counter;

  size_t counter_operands = 0;
  for (auto *operand : call->operands) {
    auto *definition = defining_op(operand);
    if (
      !definition || definition->code != OpCode::Load ||
      !is_in_region(body, definition) || definition->operands.size() != 1 ||
      definition->operands[0] != counter_address
    ) {
      continue;
    }
    candidate.iteration_counter_load = definition;
    ++counter_operands;
  }
  if (
    counter_operands != 1 || !candidate.iteration_counter_load ||
    !has_only_user(candidate.iteration_counter_load->result, call)
  ) {
    return std::nullopt;
  }

  if (
    !call->result || call->result->users.size() != 1 ||
    call->result->users.front() == nullptr
  ) {
    return std::nullopt;
  }
  auto *mod = static_cast<Op *>(call->result->users.front());
  if (
    mod->code != OpCode::Mod || mod->operands.size() != 2 ||
    mod->operands[0] != call->result || !mod->result ||
    mod->result->users.size() != 1
  ) {
    return std::nullopt;
  }
  auto *answer_store = static_cast<Op *>(mod->result->users.front());
  if (
    answer_store->code != OpCode::Store || answer_store->operands.size() != 2 ||
    answer_store->operands[0] != mod->result
  ) {
    return std::nullopt;
  }
  candidate.mod = mod;
  candidate.modulus = mod->operands[1];
  candidate.answer_address = answer_store->operands[1];
  if (
    !candidate.modulus || !candidate.modulus->type->is_i32() ||
    is_in_region(body, defining_op(candidate.modulus)) ||
    !is_local_root(candidate.answer_address) ||
    root_of(candidate.answer_address) != candidate.answer_address
  ) {
    return std::nullopt;
  }

  if (
    !call_reads_are_disjoint(candidate, call_effects) ||
    !has_only_user(call->result, mod) ||
    !has_only_user(mod->result, answer_store)
  ) {
    return std::nullopt;
  }

  // The call slice must be a contiguous prefix, followed only by the
  // countdown update.  This preserves every observable operation order while
  // allowing the slice to be selected by the runtime guard.
  std::unordered_set<Op *> visiting;
  candidate.moved_ops = {call, mod, answer_store};
  for (auto *operand : call->operands) {
    if (!collect_dependency(operand, body, candidate, visiting))
      return std::nullopt;
  }
  for (auto *op : {tail_load, tail_sub, tail_store})
    candidate.moved_ops.erase(op);

  if (candidate.moved_ops.empty())
    return std::nullopt;
  auto first_moved = body.begin();
  for (; first_moved != body.end() && !candidate.moved_ops.count(*first_moved);
       ++first_moved) {
  }
  if (first_moved == body.end())
    return std::nullopt;
  auto last_moved = first_moved;
  while (last_moved != body.end() && candidate.moved_ops.count(*last_moved))
    ++last_moved;
  if (
    last_moved == body.end() ||
    std::distance(first_moved, last_moved) + 3 != static_cast<long>(body.size())
  ) {
    return std::nullopt;
  }
  if (*std::prev(last_moved) != answer_store)
    return std::nullopt;
  for (auto it = body.begin(); it != last_moved; ++it) {
    if (!candidate.moved_ops.count(*it))
      return std::nullopt;
  }
  if (
    std::distance(last_moved, body.end()) != 3 || *last_moved != tail_load ||
    *std::next(last_moved) != tail_sub ||
    *std::next(last_moved, 2) != tail_store
  ) {
    return std::nullopt;
  }

  for (auto *op : body) {
    if (candidate.moved_ops.count(op))
      continue;
    if (op != tail_load && op != tail_sub && op != tail_store)
      return std::nullopt;
  }

  // The answer slot is local state.  It must not be read by the loop or have
  // another store whose value would be observed on the fast path.
  for (auto *op : body) {
    for (auto *operand : op->operands) {
      if (operand == candidate.answer_address && op != answer_store)
        return std::nullopt;
    }
  }
  for (auto *user_base : candidate.answer_address->users) {
    auto *user = static_cast<Op *>(user_base);
    if (user->code == OpCode::Store && user != answer_store)
      return std::nullopt;
  }

  for (auto *user_base : candidate.counter_address->users) {
    auto *user = static_cast<Op *>(user_base);
    if (
      user != initial_store && user != counter_load &&
      user != candidate.iteration_counter_load && user != tail_load &&
      user != tail_store
    ) {
      return std::nullopt;
    }
  }

  return candidate;
}

auto PureCallLoopSink::clone_invariant_value(
  Value *value,
  const Region &body,
  const Candidate &candidate,
  std::unordered_map<Value *, Value *> &cache,
  Region &destination
) -> Value * {
  if (!value)
    return nullptr;
  if (value == candidate.iteration_counter_load->result)
    return module->ctx.make_const(I32::get(), 1);
  if (auto it = cache.find(value); it != cache.end())
    return it->second;

  auto *definition = defining_op(value);
  if (!definition || !is_in_region(body, definition))
    return value;
  if (!candidate.moved_ops.count(definition) || !is_pure_scalar_op(*definition))
    return nullptr;
  if (definition->code == OpCode::Load)
    return nullptr;

  auto *clone = module->ctx.make_op(definition->code);
  for (auto *operand : definition->operands) {
    auto *mapped =
      clone_invariant_value(operand, body, candidate, cache, destination);
    if (!mapped)
      return nullptr;
    clone->operands.push_back(mapped);
    mapped->addUse(clone);
  }
  if (!definition->result)
    return nullptr;
  make_result(module, clone, definition->result->type);
  destination.push_back(clone);
  cache[value] = clone->result;
  return clone->result;
}

auto PureCallLoopSink::transform(
  Region &parent, Region::iterator position, Candidate &candidate
) -> bool {
  auto *loop = candidate.loop;
  auto &payload = std::get<WhilePayload>(loop->payload);
  auto &body = *payload.loop_region;

  auto *positive = module->ctx.make_op(OpCode::Gt);
  auto *zero = module->ctx.make_const(I32::get(), 0);
  add_operands(positive, {candidate.initial_counter, zero});
  make_result(module, positive, Bool::get());

  auto versioned = module->ctx.make_op(OpCode::If);
  auto fast_region = std::make_unique<Region>();
  Region fallback_region;
  versioned->payload =
    IfPayload{std::move(fast_region), std::move(fallback_region)};
  versioned->operands = {positive->result};
  positive->result->addUse(versioned);
  auto &final_body = *std::get<IfPayload>(versioned->payload).then_region;

  std::unordered_map<Value *, Value *> cache;
  std::vector<Value *> final_operands;
  final_operands.reserve(candidate.call->operands.size());
  for (auto *operand : candidate.call->operands) {
    auto *mapped =
      clone_invariant_value(operand, body, candidate, cache, final_body);
    if (!mapped)
      return false;
    final_operands.push_back(mapped);
  }

  auto *final_call = module->ctx.make_op(
    OpCode::Call,
    CallPayload{std::get<CallPayload>(candidate.call->payload).func_name}
  );
  final_call->operands = std::move(final_operands);
  for (auto *operand : final_call->operands)
    operand->addUse(final_call);
  make_result(module, final_call, candidate.call->result->type);
  final_body.push_back(final_call);

  auto *final_mod = module->ctx.make_op(OpCode::Mod);
  add_operands(final_mod, {final_call->result, candidate.modulus});
  make_result(module, final_mod, candidate.mod->result->type);
  final_body.push_back(final_mod);

  auto *final_store = module->ctx.make_op(OpCode::Store);
  add_operands(final_store, {final_mod->result, candidate.answer_address});
  final_body.push_back(final_store);

  auto *final_counter_store = module->ctx.make_op(OpCode::Store);
  add_operands(final_counter_store, {zero, candidate.counter_address});
  final_body.push_back(final_counter_store);

  auto after_loop = std::next(position);
  auto &fallback_body = *std::get<IfPayload>(versioned->payload).else_region;
  fallback_body.splice(fallback_body.end(), parent, position);
  parent.insert(after_loop, positive);
  parent.insert(after_loop, versioned);
  return true;
}

} // namespace exodus::high_ir::opt
