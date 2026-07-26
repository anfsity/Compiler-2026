#include "loop_recurrence_simplify.hpp"

#include "../../base/integer_range.hpp"
#include "../../high/effects.hpp"
#include <algorithm>
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
  const auto &data = static_cast<Constant *>(value)->val;
  if (!std::holds_alternative<int>(data))
    return std::nullopt;
  return std::get<int>(data);
}

auto load_address(Value *value) -> Value * {
  auto *load = defining_op(value);
  if (!load || load->code != OpCode::Load || load->operands.size() != 1)
    return nullptr;
  return load->operands[0];
}

auto make_op(
  Module *module,
  OpCode code,
  const std::shared_ptr<Type> &type,
  std::initializer_list<Value *> operands
) -> Op * {
  auto *op = module->ctx.make_op(code);
  op->operands.assign(operands.begin(), operands.end());
  for (auto *operand : op->operands)
    operand->addUse(op);
  op->result = module->ctx.make_value<OpResult>(type, op);
  return op;
}

auto make_store(Module *module, Value *value, Value *address) -> Op * {
  auto *store = module->ctx.make_op(OpCode::Store);
  store->operands = {value, address};
  value->addUse(store);
  address->addUse(store);
  return store;
}

auto may_write_memory(const Op &op) -> bool {
  auto effects = get_op_effects(op);
  if (op.code == OpCode::If) {
    const auto &payload = std::get<IfPayload>(op.payload);
    effects.merge(get_region_effects(*payload.then_region));
    if (payload.else_region)
      effects.merge(get_region_effects(*payload.else_region));
  } else if (op.code == OpCode::While) {
    const auto &payload = std::get<WhilePayload>(op.payload);
    effects.merge(get_region_effects(*payload.cond_region));
    effects.merge(get_region_effects(*payload.loop_region));
  }
  return effects.writes_memory();
}

} // namespace

auto LoopRecurrenceSimplify::run(
  Function &function, exodus::opt::FunctionAnalysisManager &
) -> exodus::opt::PreservedAnalysis {
  return simplify_region(function.body) ? exodus::opt::PreservedAnalysis::none()
                                        : exodus::opt::PreservedAnalysis::all();
}

auto LoopRecurrenceSimplify::simplify_region(Region &region) -> bool {
  bool changed = false;
  for (auto it = region.begin(); it != region.end(); ++it) {
    auto *op = *it;
    if (op->code == OpCode::If) {
      auto &payload = std::get<IfPayload>(op->payload);
      changed |= simplify_region(*payload.then_region);
      if (payload.else_region)
        changed |= simplify_region(*payload.else_region);
      continue;
    }
    if (op->code != OpCode::While)
      continue;

    if (!guarded_fallbacks.count(op)) {
      if (auto match = match_modular_recurrence(region, it, op)) {
        replace_with_guarded_closed_form(region, it, op, *match);
        changed = true;
        continue;
      }
    }

    auto &payload = std::get<WhilePayload>(op->payload);
    changed |= simplify_region(*payload.cond_region);
    changed |= simplify_region(*payload.loop_region);
  }
  return changed;
}

auto LoopRecurrenceSimplify::match_modular_recurrence(
  Region &parent, Region::iterator position, Op *loop
) const -> std::optional<ModularRecurrence> {
  if (!loop || loop->code != OpCode::While)
    return std::nullopt;
  auto &payload = std::get<WhilePayload>(loop->payload);
  if (payload.cond_region->size() != 3)
    return std::nullopt;

  auto cond_it = payload.cond_region->begin();
  auto *counter_load = *cond_it++;
  auto *compare = *cond_it++;
  auto *condition = *cond_it;
  if (
    counter_load->code != OpCode::Load || counter_load->operands.size() != 1 ||
    compare->code != OpCode::Lt || compare->operands.size() != 2 ||
    compare->operands[0] != counter_load->result ||
    condition->code != OpCode::Condition || condition->operands.size() != 1 ||
    condition->operands[0] != compare->result
  ) {
    return std::nullopt;
  }
  auto *counter_address = counter_load->operands[0];
  auto *counter_alloca = defining_op(counter_address);
  if (!counter_alloca || counter_alloca->code != OpCode::Alloca)
    return std::nullopt;

  Value *trip_count = compare->operands[1];
  auto *trip_mod = defining_op(trip_count);
  if (
    !trip_mod || trip_mod->code != OpCode::Mod || trip_mod->operands.size() != 2
  ) {
    return std::nullopt;
  }
  auto trip_modulus = int_constant(trip_mod->operands[1]);
  Value *initial_state = trip_mod->operands[0];
  Value *state_address = load_address(initial_state);
  if (!trip_modulus || *trip_modulus <= 0 || !state_address)
    return std::nullopt;
  if (std::find(parent.begin(), position, trip_mod) == position)
    return std::nullopt;

  bool saw_counter_init = false;
  bool saw_initial_state = false;
  std::unordered_set<Op *> counter_initializers;
  for (auto it = parent.begin(); it != position; ++it) {
    auto *op = *it;
    saw_initial_state |= op == defining_op(initial_state);
    if (
      op->code == OpCode::Store && op->operands.size() == 2 &&
      op->operands[1] == counter_address
    ) {
      auto value = int_constant(op->operands[0]);
      if (!value || *value != 0)
        return std::nullopt;
      saw_counter_init = true;
      counter_initializers.insert(op);
    }
    if (saw_initial_state) {
      if (!counter_initializers.count(op) && may_write_memory(*op))
        return std::nullopt;
    }
  }
  if (!saw_counter_init || !saw_initial_state)
    return std::nullopt;

  auto &body = *payload.loop_region;
  if (body.size() != 7 && body.size() != 9)
    return std::nullopt;
  std::unordered_set<Op *> body_ops(body.begin(), body.end());
  Op *state_add = nullptr;
  Op *state_mod = nullptr;
  Op *final_state_store = nullptr;
  Op *counter_add = nullptr;
  Op *counter_store = nullptr;
  std::vector<Op *> state_store_ops;
  size_t state_stores = 0;
  size_t counter_stores = 0;

  for (auto *op : body) {
    switch (op->code) {
    case OpCode::Load:
      if (
        op->operands.size() != 1 ||
        (op->operands[0] != state_address && op->operands[0] != counter_address)
      ) {
        return std::nullopt;
      }
      break;
    case OpCode::Add: {
      if (op->operands.size() != 2)
        return std::nullopt;
      auto lhs = int_constant(op->operands[0]);
      auto rhs = int_constant(op->operands[1]);
      auto *nonconstant = lhs ? op->operands[1] : op->operands[0];
      auto constant = lhs ? lhs : rhs;
      auto *address = load_address(nonconstant);
      if (!constant || !address || !body_ops.count(defining_op(nonconstant))) {
        return std::nullopt;
      }
      if (address == state_address) {
        if (state_add)
          return std::nullopt;
        state_add = op;
      } else if (address == counter_address && *constant == 1) {
        if (counter_add)
          return std::nullopt;
        counter_add = op;
      } else {
        return std::nullopt;
      }
      break;
    }
    case OpCode::Mod:
      if (state_mod || op->operands.size() != 2)
        return std::nullopt;
      state_mod = op;
      break;
    case OpCode::Store:
      if (op->operands.size() != 2)
        return std::nullopt;
      if (op->operands[1] == state_address) {
        ++state_stores;
        state_store_ops.push_back(op);
        if (
          defining_op(op->operands[0]) &&
          defining_op(op->operands[0])->code == OpCode::Mod
        ) {
          final_state_store = op;
        }
      } else if (op->operands[1] == counter_address) {
        ++counter_stores;
        counter_store = op;
      } else {
        return std::nullopt;
      }
      break;
    default:
      return std::nullopt;
    }
  }

  if (
    !state_add || !state_mod || !final_state_store || !counter_add ||
    !counter_store || counter_store->operands[0] != counter_add->result ||
    counter_stores != 1 || (state_stores != 1 && state_stores != 2) ||
    final_state_store->operands[0] != state_mod->result
  ) {
    return std::nullopt;
  }

  for (auto *user_base : counter_address->users) {
    auto *user = static_cast<Op *>(user_base);
    if (
      !counter_initializers.count(user) && !body_ops.count(user) &&
      user != counter_load
    )
      return std::nullopt;
  }

  auto step = int_constant(
    int_constant(state_add->operands[0]) ? state_add->operands[0]
                                         : state_add->operands[1]
  );
  if (!step || *step < 0)
    return std::nullopt;
  auto state_modulus = int_constant(state_mod->operands[1]);
  if (!state_modulus || *state_modulus <= 0)
    return std::nullopt;

  auto *mod_input = state_mod->operands[0];
  Op *state_add_store = nullptr;
  for (auto *store : state_store_ops) {
    if (store == final_state_store)
      continue;
    if (state_add_store || store->operands[0] != state_add->result)
      return std::nullopt;
    state_add_store = store;
  }

  if (
    (mod_input == state_add->result && state_stores != 1) ||
    (mod_input != state_add->result && (state_stores != 2 || !state_add_store))
  ) {
    return std::nullopt;
  }

  if (mod_input != state_add->result) {
    auto *reload = defining_op(mod_input);
    if (
      !reload || reload->code != OpCode::Load ||
      reload->operands[0] != state_address || !body_ops.count(reload)
    ) {
      return std::nullopt;
    }
    bool stored_add_before_reload = false;
    for (auto *candidate : body) {
      if (candidate == reload)
        break;
      stored_add_before_reload |= candidate->code == OpCode::Store &&
                                  candidate->operands.size() == 2 &&
                                  candidate->operands[0] == state_add->result &&
                                  candidate->operands[1] == state_address;
    }
    if (!stored_add_before_reload)
      return std::nullopt;
  }

  using exodus::analysis::IntegerRange;
  IntegerRange state_range{0, static_cast<int64_t>(*state_modulus) - 1};
  IntegerRange trip_range{0, static_cast<int64_t>(*trip_modulus) - 1};
  auto delta = IntegerRange::multiply(trip_range, {*step, *step});
  auto closed = delta ? IntegerRange::add(state_range, *delta) : std::nullopt;
  auto one_step = IntegerRange::add(state_range, {*step, *step});
  if (!closed || !one_step || !closed->fits_i32() || !one_step->fits_i32())
    return std::nullopt;

  return ModularRecurrence{
    initial_state,
    trip_count,
    state_address,
    *step,
    *state_modulus,
  };
}

auto LoopRecurrenceSimplify::replace_with_guarded_closed_form(
  Region &parent,
  Region::iterator position,
  Op *loop,
  const ModularRecurrence &match
) -> void {
  auto *zero = module->ctx.make_const(I32::get(), 0);
  auto *upper = module->ctx.make_const(I32::get(), match.state_modulus - 1);
  auto *lower_check =
    make_op(module, OpCode::Ge, Bool::get(), {match.initial_state, zero});
  auto *upper_check =
    make_op(module, OpCode::Le, Bool::get(), {match.initial_state, upper});
  auto *lower_i32 =
    make_op(module, OpCode::ZExt, I32::get(), {lower_check->result});
  auto *upper_i32 =
    make_op(module, OpCode::ZExt, I32::get(), {upper_check->result});
  auto *both = make_op(
    module, OpCode::And, I32::get(), {lower_i32->result, upper_i32->result}
  );
  auto *guard = make_op(module, OpCode::Ne, Bool::get(), {both->result, zero});

  parent.insert(position, lower_check);
  parent.insert(position, upper_check);
  parent.insert(position, lower_i32);
  parent.insert(position, upper_i32);
  parent.insert(position, both);
  parent.insert(position, guard);

  auto *step = module->ctx.make_const(I32::get(), match.step);
  auto *modulus = module->ctx.make_const(I32::get(), match.state_modulus);
  auto *delta =
    make_op(module, OpCode::Mul, I32::get(), {match.trip_count, step});
  auto *sum = make_op(
    module, OpCode::Add, I32::get(), {match.initial_state, delta->result}
  );
  auto *reduced =
    make_op(module, OpCode::Mod, I32::get(), {sum->result, modulus});
  auto *store = make_store(module, reduced->result, match.state_address);

  auto then_region = std::make_unique<Region>();
  then_region->push_back(delta);
  then_region->push_back(sum);
  then_region->push_back(reduced);
  then_region->push_back(store);
  Region fallback;
  fallback.push_back(loop);
  guarded_fallbacks.insert(loop);

  auto *guarded = module->ctx.make_op(
    OpCode::If, IfPayload{std::move(then_region), std::move(fallback)}
  );
  guarded->operands = {guard->result};
  guard->result->addUse(guarded);
  *position = guarded;
}

} // namespace exodus::high_ir::opt
