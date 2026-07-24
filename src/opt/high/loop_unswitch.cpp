#include "loop_unswitch.hpp"

#include "../../base/getptr.hpp"
#include "../../high/cloner.hpp"
#include "../../high/effects.hpp"
#include <algorithm>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace exodus::high_ir::opt {
namespace {

constexpr int MaxTripCount = 8;
constexpr size_t MaxLoopOps = 96;

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

auto contains_op(const Region &region, const Op *target) -> bool {
  return std::find(region.begin(), region.end(), target) != region.end();
}

auto count_ops(const Region &region) -> size_t {
  size_t count = 0;
  for (auto *op : region) {
    ++count;
    if (op->code == OpCode::If) {
      const auto &payload = std::get<IfPayload>(op->payload);
      count += count_ops(*payload.then_region);
      if (payload.else_region)
        count += count_ops(*payload.else_region);
    } else if (op->code == OpCode::While) {
      const auto &payload = std::get<WhilePayload>(op->payload);
      count += count_ops(*payload.cond_region);
      count += count_ops(*payload.loop_region);
    }
  }
  return count;
}

auto has_nested_loop_or_exit(const Region &region) -> bool {
  for (auto *op : region) {
    if (
      op->code == OpCode::While || op->code == OpCode::Break ||
      op->code == OpCode::Continue || op->code == OpCode::Ret
    )
      return true;
    if (op->code == OpCode::If) {
      const auto &payload = std::get<IfPayload>(op->payload);
      if (has_nested_loop_or_exit(*payload.then_region))
        return true;
      if (payload.else_region && has_nested_loop_or_exit(*payload.else_region))
        return true;
    }
  }
  return false;
}

struct SmallCountedLoop {
  Op *loop;
  Op *counter_alloca;
  Op *initial_store;
  Value *counter_address;
};

auto region_contains(const Region &region, const Op *target) -> bool {
  for (auto *op : region) {
    if (op == target)
      return true;
    if (op->code == OpCode::If) {
      const auto &payload = std::get<IfPayload>(op->payload);
      if (region_contains(*payload.then_region, target))
        return true;
      if (payload.else_region && region_contains(*payload.else_region, target))
        return true;
    } else if (op->code == OpCode::While) {
      const auto &payload = std::get<WhilePayload>(op->payload);
      if (
        region_contains(*payload.cond_region, target) ||
        region_contains(*payload.loop_region, target)
      )
        return true;
    }
  }
  return false;
}

auto match_small_counted_loop(Region &parent, Region::iterator position)
  -> std::optional<SmallCountedLoop> {
  auto *loop = *position;
  if (loop->code != OpCode::While)
    return std::nullopt;
  auto &payload = std::get<WhilePayload>(loop->payload);
  if (
    payload.cond_region->empty() || payload.loop_region->size() < 3 ||
    has_nested_loop_or_exit(*payload.loop_region) ||
    count_ops(*payload.cond_region) + count_ops(*payload.loop_region) >
      MaxLoopOps
  )
    return std::nullopt;

  auto *condition = payload.cond_region->back();
  if (condition->code != OpCode::Condition || condition->operands.size() != 1)
    return std::nullopt;
  auto *compare = defining_op(condition->operands[0]);
  if (
    !compare || compare->code != OpCode::Lt || compare->operands.size() != 2 ||
    !contains_op(*payload.cond_region, compare)
  )
    return std::nullopt;
  auto trip_count = int_constant(compare->operands[1]);
  if (!trip_count || *trip_count < 2 || *trip_count > MaxTripCount)
    return std::nullopt;
  auto *condition_load = defining_op(compare->operands[0]);
  if (
    !condition_load || condition_load->code != OpCode::Load ||
    condition_load->operands.size() != 1 ||
    !contains_op(*payload.cond_region, condition_load)
  )
    return std::nullopt;
  auto *counter_address = condition_load->operands[0];
  auto *counter_alloca = defining_op(counter_address);
  if (!counter_alloca || counter_alloca->code != OpCode::Alloca)
    return std::nullopt;
  if (!contains_op(parent, counter_alloca))
    return std::nullopt;

  Op *initial_store = nullptr;
  for (auto it = parent.begin(); it != position; ++it) {
    auto *op = *it;
    if (
      op->code == OpCode::Store && op->operands.size() == 2 &&
      op->operands[1] == counter_address
    )
      initial_store = op;
  }
  if (!initial_store || int_constant(initial_store->operands[0]) != 0)
    return std::nullopt;

  auto tail_store_position = std::prev(payload.loop_region->end());
  auto tail_add_position = std::prev(tail_store_position);
  auto tail_load_position = std::prev(tail_add_position);
  auto *tail_store = *tail_store_position;
  auto *tail_add = *tail_add_position;
  auto *tail_load = *tail_load_position;
  if (
    tail_load->code != OpCode::Load || tail_load->operands.size() != 1 ||
    tail_load->operands[0] != counter_address ||
    tail_add->code != OpCode::Add || tail_add->operands.size() != 2 ||
    tail_store->code != OpCode::Store || tail_store->operands.size() != 2 ||
    tail_store->operands[0] != tail_add->result ||
    tail_store->operands[1] != counter_address
  )
    return std::nullopt;
  auto left = int_constant(tail_add->operands[0]);
  auto right = int_constant(tail_add->operands[1]);
  auto *nonconstant = left ? tail_add->operands[1] : tail_add->operands[0];
  auto step = left ? left : right;
  if (!step || *step != 1 || nonconstant != tail_load->result)
    return std::nullopt;

  for (auto *user_base : counter_address->users) {
    auto *user = static_cast<Op *>(user_base);
    if (user->code != OpCode::Store && user->code != OpCode::Load)
      return std::nullopt;
    if (
      user->code == OpCode::Store && user != initial_store && user != tail_store
    )
      return std::nullopt;
    if (
      user != initial_store && !region_contains(*payload.cond_region, user) &&
      !region_contains(*payload.loop_region, user)
    )
      return std::nullopt;
  }
  return SmallCountedLoop{loop, counter_alloca, initial_store, counter_address};
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

class InvariantClosure {
public:
  InvariantClosure(
    Op &loop,
    Op &branch,
    const std::unordered_map<std::string, Function *> &functions,
    const std::unordered_map<Function *, OpEffects> &summaries
  )
      : loop(loop), branch(branch), functions(functions), summaries(summaries) {
    auto &payload = std::get<WhilePayload>(loop.payload);
    collect_ops(*payload.cond_region, loop_ops);
    collect_ops(*payload.loop_region, loop_ops);
    size_t index = 0;
    for (auto *op : *payload.loop_region)
      direct_order.emplace(op, index++);
  }

  auto build(Value *condition) -> std::optional<std::vector<Op *>> {
    closure.clear();
    visiting.clear();
    if (!collect_value(condition))
      return std::nullopt;
    std::vector<Op *> ordered(closure.begin(), closure.end());
    std::sort(ordered.begin(), ordered.end(), [&](Op *lhs, Op *rhs) {
      return direct_order.at(lhs) < direct_order.at(rhs);
    });
    return ordered;
  }

private:
  Op &loop;
  Op &branch;
  const std::unordered_map<std::string, Function *> &functions;
  const std::unordered_map<Function *, OpEffects> &summaries;
  std::unordered_set<Op *> loop_ops;
  std::unordered_map<Op *, size_t> direct_order;
  std::unordered_set<Op *> closure;
  std::unordered_set<Op *> visiting;

  auto is_direct_local_address(Value *address) const -> bool {
    auto *root = get_addr_root(address);
    auto *root_op = defining_op(root);
    if (!root_op || root_op->code != OpCode::Alloca)
      return false;
    auto *address_op = defining_op(address);
    if (!address_op || address_op->code == OpCode::Alloca)
      return true;
    if (
      address_op->code != OpCode::GetPtr || address_op->operands.empty() ||
      !address_op->result
    )
      return false;
    return !ir::analyze_getptr(
              address_op->operands[0]->type,
              address_op->result->type,
              address_op->operands.size() - 1
    )
              .reads_memory;
  }

  auto write_may_alias(Value *address, Value *load_root) const -> bool {
    auto *write_root = get_addr_root(address);
    if (!write_root || write_root == load_root)
      return true;
    if (
      write_root->kind == ValueKind::GlobalVar &&
      load_root->kind == ValueKind::GlobalVar
    )
      return false;
    if (is_direct_local_address(address))
      return false;
    return true;
  }

  auto call_preserves_location(const Op &call, Value *load_root) const -> bool {
    const auto &name = std::get<CallPayload>(call.payload).func_name;
    auto function = functions.find(name);
    if (function == functions.end() || function->second->is_decl)
      return false;
    auto function_type = std::static_pointer_cast<Func>(function->second->type);
    if (
      std::any_of(
        function_type->params.begin(),
        function_type->params.end(),
        [](const auto &type) { return type->is_ptr(); }
      )
    )
      return false;
    auto summary = summaries.find(function->second);
    if (summary == summaries.end() || summary->second.has_unknown_effect)
      return false;
    return std::all_of(
      summary->second.writes.begin(),
      summary->second.writes.end(),
      [load_root](Value *write) {
        auto *root = get_addr_root(write);
        return root && root->kind == ValueKind::GlobalVar &&
               load_root->kind == ValueKind::GlobalVar && root != load_root;
      }
    );
  }

  auto region_preserves_location(const Region &region, Value *load_root) const
    -> bool {
    for (auto *op : region) {
      if (op->code == OpCode::Store) {
        if (
          op->operands.size() != 2 ||
          write_may_alias(op->operands[1], load_root)
        )
          return false;
      } else if (op->code == OpCode::Memset) {
        if (op->operands.empty() || write_may_alias(op->operands[0], load_root))
          return false;
      } else if (
        op->code == OpCode::Call && !call_preserves_location(*op, load_root)
      ) {
        return false;
      }

      if (op->code == OpCode::If) {
        const auto &payload = std::get<IfPayload>(op->payload);
        if (!region_preserves_location(*payload.then_region, load_root))
          return false;
        if (
          payload.else_region &&
          !region_preserves_location(*payload.else_region, load_root)
        )
          return false;
      } else if (op->code == OpCode::While) {
        const auto &payload = std::get<WhilePayload>(op->payload);
        if (
          !region_preserves_location(*payload.cond_region, load_root) ||
          !region_preserves_location(*payload.loop_region, load_root)
        )
          return false;
      }
    }
    return true;
  }

  auto collect_value(Value *value) -> bool {
    if (
      !value || value->kind == ValueKind::Constant ||
      value->kind == ValueKind::Argument || value->kind == ValueKind::GlobalVar
    )
      return value != nullptr;
    auto *op = defining_op(value);
    if (!op)
      return false;
    if (!loop_ops.count(op))
      return true;
    auto order = direct_order.find(op);
    auto branch_order = direct_order.find(&branch);
    if (
      order == direct_order.end() || branch_order == direct_order.end() ||
      order->second >= branch_order->second
    )
      return false;
    if (closure.count(op))
      return true;
    if (!visiting.insert(op).second)
      return false;

    bool safe = false;
    if (op->code == OpCode::Load && op->operands.size() == 1) {
      auto *root = get_addr_root(op->operands[0]);
      auto &payload = std::get<WhilePayload>(loop.payload);
      safe = root &&
             (root->kind == ValueKind::GlobalVar ||
              is_direct_local_address(op->operands[0])) &&
             region_preserves_location(*payload.cond_region, root) &&
             region_preserves_location(*payload.loop_region, root) &&
             collect_value(op->operands[0]);
    } else {
      auto effects = get_op_effects(*op);
      safe = effects.speculatable &&
             std::all_of(
               op->operands.begin(), op->operands.end(), [&](Value *operand) {
                 return collect_value(operand);
               }
             );
    }
    visiting.erase(op);
    if (!safe)
      return false;
    closure.insert(op);
    return true;
  }
};

auto unlink_region(Region &region) -> void {
  for (auto *op : region) {
    if (op->code == OpCode::If) {
      auto &payload = std::get<IfPayload>(op->payload);
      unlink_region(*payload.then_region);
      if (payload.else_region)
        unlink_region(*payload.else_region);
    } else if (op->code == OpCode::While) {
      auto &payload = std::get<WhilePayload>(op->payload);
      unlink_region(*payload.cond_region);
      unlink_region(*payload.loop_region);
    }
    for (auto *operand : op->operands) {
      if (operand)
        operand->rmUse(op);
    }
  }
}

auto is_counter_only_region(const Region &region, Value *counter_address)
  -> bool {
  for (auto *op : region) {
    if (op->code == OpCode::Load) {
      if (op->operands.size() != 1 || op->operands[0] != counter_address)
        return false;
      continue;
    }
    if (op->code == OpCode::Store) {
      if (op->operands.size() != 2 || op->operands[1] != counter_address)
        return false;
      continue;
    }
    if (op->code == OpCode::Condition)
      continue;
    if (
      op->code == OpCode::If || op->code == OpCode::While ||
      op->code == OpCode::Call || op->code == OpCode::Memset ||
      op->code == OpCode::Ret || op->code == OpCode::Break ||
      op->code == OpCode::Continue || op->code == OpCode::Jump ||
      op->code == OpCode::Branch
    )
      return false;
    if (!get_op_effects(*op).speculatable)
      return false;
  }
  return true;
}

auto remove_counter_only_loop(
  Region &parent, Region::iterator position, const SmallCountedLoop &match
) -> void {
  auto &payload = std::get<WhilePayload>(match.loop->payload);
  unlink_region(*payload.cond_region);
  unlink_region(*payload.loop_region);
  for (auto *operand : match.initial_store->operands)
    operand->rmUse(match.initial_store);

  auto alloca_position =
    std::find(parent.begin(), parent.end(), match.counter_alloca);
  auto init_position =
    std::find(parent.begin(), parent.end(), match.initial_store);
  parent.erase(alloca_position);
  parent.erase(init_position);
  parent.erase(position);
}

auto make_alloca(Module &module, const std::shared_ptr<Type> &type) -> Op * {
  auto *alloca = module.ctx.make_op(OpCode::Alloca);
  alloca->result = module.ctx.make_value<OpResult>(type, alloca);
  return alloca;
}

auto make_store(Module &module, Value *value, Value *address) -> Op * {
  auto *store = module.ctx.make_op(OpCode::Store);
  store->operands = {value, address};
  value->addUse(store);
  address->addUse(store);
  return store;
}

auto unswitch_loop(
  Module &module,
  Region &parent,
  Region::iterator position,
  const SmallCountedLoop &counted,
  Op &branch,
  const std::vector<Op *> &closure
) -> void {
  auto *loop = *position;
  auto &payload = std::get<WhilePayload>(loop->payload);
  for (auto *op : closure) {
    auto it =
      std::find(payload.loop_region->begin(), payload.loop_region->end(), op);
    parent.splice(position, *payload.loop_region, it);
  }

  auto *true_value = module.ctx.make_const(Bool::get(), 1);
  auto *false_value = module.ctx.make_const(Bool::get(), 0);
  auto *true_alloca = make_alloca(module, counted.counter_address->type);
  auto *false_alloca = make_alloca(module, counted.counter_address->type);
  auto *zero = module.ctx.make_zero(
    std::static_pointer_cast<Ptr>(counted.counter_address->type)->target
  );
  auto *true_init = make_store(module, zero, true_alloca->result);
  auto *false_init = make_store(module, zero, false_alloca->result);
  IRCloner true_cloner(&module.ctx);
  true_cloner.value_map[branch.operands[0]] = true_value;
  true_cloner.value_map[counted.counter_address] = true_alloca->result;
  auto *true_loop = true_cloner.cloneOp(loop);
  IRCloner false_cloner(&module.ctx);
  false_cloner.value_map[branch.operands[0]] = false_value;
  false_cloner.value_map[counted.counter_address] = false_alloca->result;
  auto *false_loop = false_cloner.cloneOp(loop);

  auto then_region = std::make_unique<Region>();
  then_region->push_back(true_alloca);
  then_region->push_back(true_init);
  then_region->push_back(true_loop);
  Region else_region;
  else_region.push_back(false_alloca);
  else_region.push_back(false_init);
  else_region.push_back(false_loop);
  auto *outer_if = module.ctx.make_op(
    OpCode::If, IfPayload{std::move(then_region), std::move(else_region)}
  );
  outer_if->operands = {branch.operands[0]};
  branch.operands[0]->addUse(outer_if);

  unlink_region(*payload.cond_region);
  unlink_region(*payload.loop_region);
  for (auto *operand : counted.initial_store->operands)
    operand->rmUse(counted.initial_store);
  auto alloca_position =
    std::find(parent.begin(), parent.end(), counted.counter_alloca);
  auto init_position =
    std::find(parent.begin(), parent.end(), counted.initial_store);
  parent.erase(alloca_position);
  parent.erase(init_position);
  *position = outer_if;
}

class Unswitcher {
public:
  explicit Unswitcher(Module &module) : module(module) {
    for (auto &function : module.functions)
      functions.emplace(function->name, function.get());
    summaries = get_function_effects(module);
  }

  auto run(Function &function) -> bool { return unswitch_one(function.body); }

private:
  Module &module;
  std::unordered_map<std::string, Function *> functions;
  std::unordered_map<Function *, OpEffects> summaries;

  auto unswitch_one(Region &region) -> bool {
    for (auto it = region.begin(); it != region.end(); ++it) {
      auto *op = *it;
      if (op->code == OpCode::If) {
        auto &payload = std::get<IfPayload>(op->payload);
        if (unswitch_one(*payload.then_region))
          return true;
        if (payload.else_region && unswitch_one(*payload.else_region))
          return true;
        continue;
      }
      if (op->code != OpCode::While)
        continue;

      auto &payload = std::get<WhilePayload>(op->payload);
      if (
        unswitch_one(*payload.cond_region) || unswitch_one(*payload.loop_region)
      )
        return true;
      auto counted = match_small_counted_loop(region, it);
      if (!counted)
        continue;
      if (
        is_counter_only_region(
          *payload.cond_region, counted->counter_address
        ) &&
        is_counter_only_region(*payload.loop_region, counted->counter_address)
      ) {
        remove_counter_only_loop(region, it, *counted);
        return true;
      }
      for (auto *candidate : *payload.loop_region) {
        if (
          candidate->code != OpCode::If || candidate->operands.size() != 1 ||
          candidate->operands[0]->kind == ValueKind::Constant
        )
          continue;
        InvariantClosure closure(*op, *candidate, functions, summaries);
        auto ops = closure.build(candidate->operands[0]);
        if (!ops)
          continue;
        unswitch_loop(module, region, it, *counted, *candidate, *ops);
        return true;
      }
    }
    return false;
  }
};

} // namespace

auto LoopUnswitch::run(
  Function &function, exodus::opt::FunctionAnalysisManager &
) -> exodus::opt::PreservedAnalysis {
  Unswitcher unswitcher(*module);
  return unswitcher.run(function) ? exodus::opt::PreservedAnalysis::none()
                                  : exodus::opt::PreservedAnalysis::all();
}

} // namespace exodus::high_ir::opt
