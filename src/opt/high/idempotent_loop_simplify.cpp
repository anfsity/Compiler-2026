#include "idempotent_loop_simplify.hpp"

#include "../../base/getptr.hpp"
#include "../../high/effects.hpp"
#include "../../high/visitor.hpp"
#include <algorithm>
#include <memory>
#include <optional>
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
  const auto &data = static_cast<Constant *>(value)->val;
  if (!std::holds_alternative<int>(data))
    return std::nullopt;
  return std::get<int>(data);
}

auto has_only_user(Value *value, Op *expected) -> bool {
  return value && value->users.size() == 1 && value->users.front() == expected;
}

auto is_local_root(Value *value) -> bool {
  auto *op = defining_op(value);
  return op && op->code == OpCode::Alloca;
}

class PointerRootAnalysis {
public:
  explicit PointerRootAnalysis(Function &function) {
    collect_region(function.body);
    for (const auto &[root, stores] : pointer_store_ops) {
      if (stores.size() != 1 || invalid_slots.count(root))
        continue;
      auto alloca_position = std::find(
        function.body.begin(), function.body.end(), defining_op(root)
      );
      if (alloca_position == function.body.end())
        continue;
      auto store_position = std::next(alloca_position);
      if (
        store_position == function.body.end() ||
        *store_position != stores.front()
      )
        continue;
      pointer_stores[root].push_back(stores.front()->operands[0]);
    }
  }

  auto resolve(Value *value) const -> Value * {
    std::unordered_set<Value *> visiting;
    return resolve_impl(value, visiting);
  }

private:
  std::unordered_map<Value *, std::vector<Value *>> pointer_stores;
  std::unordered_map<Value *, std::vector<Op *>> pointer_store_ops;
  std::unordered_set<Value *> invalid_slots;

  auto collect_region(const Region &region) -> void {
    for (auto *op : region) {
      if (op->code == OpCode::Store && op->operands.size() == 2) {
        if (op->operands[0]->type->is_ptr()) {
          auto *escaped_root = get_addr_root(op->operands[0]);
          if (is_local_root(escaped_root))
            invalid_slots.insert(escaped_root);
        }
        auto *address = op->operands[1];
        auto *root = get_addr_root(address);
        if (is_local_root(root)) {
          if (address == root && op->operands[0]->type->is_ptr())
            pointer_store_ops[root].push_back(op);
          else if (op->operands[0]->type->is_ptr())
            invalid_slots.insert(root);
        }
      } else if (op->code == OpCode::Memset && !op->operands.empty()) {
        auto *root = get_addr_root(op->operands[0]);
        if (is_local_root(root))
          invalid_slots.insert(root);
      } else if (op->code == OpCode::Call) {
        for (auto *operand : op->operands) {
          auto *root = get_addr_root(operand);
          if (is_local_root(root))
            invalid_slots.insert(root);
        }
      } else if (op->code == OpCode::Ret) {
        for (auto *operand : op->operands) {
          auto *root = get_addr_root(operand);
          if (is_local_root(root))
            invalid_slots.insert(root);
        }
      }

      if (op->code == OpCode::If) {
        const auto &payload = std::get<IfPayload>(op->payload);
        collect_region(*payload.then_region);
        if (payload.else_region)
          collect_region(*payload.else_region);
      } else if (op->code == OpCode::While) {
        const auto &payload = std::get<WhilePayload>(op->payload);
        collect_region(*payload.cond_region);
        collect_region(*payload.loop_region);
      }
    }
  }

  auto stored_pointer(Value *address) const -> Value * {
    auto *root = get_addr_root(address);
    if (address != root || !is_local_root(root) || invalid_slots.count(root))
      return nullptr;
    auto it = pointer_stores.find(root);
    if (it == pointer_stores.end() || it->second.size() != 1)
      return nullptr;
    return it->second.front();
  }

  auto resolve_impl(Value *value, std::unordered_set<Value *> &visiting) const
    -> Value * {
    if (!value || !value->type->is_ptr())
      return nullptr;
    if (
      value->kind == ValueKind::Argument || value->kind == ValueKind::GlobalVar
    ) {
      return value;
    }
    if (!visiting.insert(value).second)
      return nullptr;

    auto *op = defining_op(value);
    Value *result = nullptr;
    if (op && op->code == OpCode::Alloca) {
      result = value;
    } else if (op && op->code == OpCode::Load && op->operands.size() == 1) {
      result = resolve_impl(stored_pointer(op->operands[0]), visiting);
    } else if (op && op->code == OpCode::GetPtr && !op->operands.empty()) {
      auto plan = ir::analyze_getptr(
        op->operands[0]->type, op->result->type, op->operands.size() - 1
      );
      bool saw_index = false;
      Value *root = nullptr;
      for (const auto &step : plan.steps) {
        if (step.kind == ir::GetPtrStep::Kind::Index) {
          saw_index = true;
          if (!root)
            root = resolve_impl(op->operands[0], visiting);
          continue;
        }
        if (saw_index || root) {
          root = nullptr;
          break;
        }
        root = resolve_impl(stored_pointer(op->operands[0]), visiting);
        if (!root)
          break;
      }
      if (plan.steps.empty())
        root = resolve_impl(op->operands[0], visiting);
      result = root;
    }

    visiting.erase(value);
    return result;
  }
};

struct RootEffects {
  std::unordered_set<Value *> reads;
  std::unordered_set<Value *> writes;
};

class RootEffectCollector {
public:
  RootEffectCollector(
    const PointerRootAnalysis &pointer_roots,
    const std::unordered_map<std::string, Function *> &functions,
    const std::unordered_map<Function *, OpEffects> &summaries,
    const std::unordered_set<Value *> &ignored_roots
  )
      : pointer_roots(pointer_roots), functions(functions),
        summaries(summaries), ignored_roots(ignored_roots) {}

  auto collect(const Region &region) -> std::optional<RootEffects> {
    RootEffects effects;
    return collect_region(region, effects) ? std::optional(effects)
                                           : std::nullopt;
  }

private:
  const PointerRootAnalysis &pointer_roots;
  const std::unordered_map<std::string, Function *> &functions;
  const std::unordered_map<Function *, OpEffects> &summaries;
  const std::unordered_set<Value *> &ignored_roots;

  auto
  add_location(std::unordered_set<Value *> &locations, Value *address) const
    -> bool {
    auto *root = pointer_roots.resolve(address);
    if (!root)
      return false;
    if (!ignored_roots.count(root))
      locations.insert(root);
    return true;
  }

  auto collect_region(const Region &region, RootEffects &effects) -> bool {
    for (auto *op : region) {
      switch (op->code) {
      case OpCode::Load:
        if (
          op->operands.size() != 1 ||
          !add_location(effects.reads, op->operands[0])
        )
          return false;
        break;
      case OpCode::Store:
        if (
          op->operands.size() != 2 ||
          !add_location(effects.writes, op->operands[1])
        )
          return false;
        if (
          op->operands[0]->type->is_ptr() &&
          !is_local_root(pointer_roots.resolve(op->operands[1]))
        )
          return false;
        break;
      case OpCode::Memset:
        if (
          op->operands.empty() || !add_location(effects.writes, op->operands[0])
        )
          return false;
        break;
      case OpCode::GetPtr:
        if (op->result && !pointer_roots.resolve(op->result))
          return false;
        if (
          !op->operands.empty() && op->result &&
          ir::analyze_getptr(
            op->operands[0]->type, op->result->type, op->operands.size() - 1
          )
            .reads_memory &&
          !add_location(effects.reads, op->operands[0])
        )
          return false;
        break;
      case OpCode::Call: {
        const auto &name = std::get<CallPayload>(op->payload).func_name;
        auto function = functions.find(name);
        if (function == functions.end() || function->second->is_decl)
          return false;
        auto function_type =
          std::static_pointer_cast<Func>(function->second->type);
        if (
          std::any_of(
            function_type->params.begin(),
            function_type->params.end(),
            [](const auto &type) { return type->is_ptr(); }
          )
        )
          return false;
        auto summary = summaries.find(function->second);
        if (
          summary == summaries.end() || summary->second.has_unknown_effect ||
          summary->second.has_control_effect ||
          !summary->second.reads.empty() || !summary->second.writes.empty()
        )
          return false;
        break;
      }
      case OpCode::Ret:
      case OpCode::Break:
      case OpCode::Continue:
      case OpCode::Jump:
      case OpCode::Branch:
        return false;
      default:
        break;
      }

      if (op->code == OpCode::If) {
        const auto &payload = std::get<IfPayload>(op->payload);
        if (!collect_region(*payload.then_region, effects))
          return false;
        if (
          payload.else_region && !collect_region(*payload.else_region, effects)
        )
          return false;
      } else if (op->code == OpCode::While) {
        const auto &payload = std::get<WhilePayload>(op->payload);
        if (
          !collect_region(*payload.cond_region, effects) ||
          !collect_region(*payload.loop_region, effects)
        )
          return false;
      }
    }
    return true;
  }
};

struct CallSite {
  Function *caller;
  Op *call;
};

struct CallCollector : RecursiveOpVisitor<CallCollector> {
  const std::unordered_map<std::string, Function *> &functions; // NOLINT
  std::unordered_map<Function *, std::vector<CallSite>> calls;
  Function *current = nullptr;

  explicit CallCollector(
    const std::unordered_map<std::string, Function *> &functions
  )
      : functions(functions) {}

  using RecursiveOpVisitor<CallCollector>::visit;
  auto visit(Function &function) -> void {
    current = &function;
    RecursiveOpVisitor<CallCollector>::visit(function);
  }
  auto visit(Op *op, OpTag<OpCode::Call>) -> void {
    const auto &name = std::get<CallPayload>(op->payload).func_name;
    auto function = functions.find(name);
    if (function != functions.end() && !function->second->is_decl)
      calls[function->second].push_back({current, op});
  }
};

struct CountedLoop {
  Op *loop;
  Op *counter_load;
  Op *compare;
  Op *condition;
  Op *tail_load;
  Op *tail_add;
  Op *tail_store;
  Value *counter_address;
};

auto contains_op(const Region &region, const Op *target) -> bool {
  return std::find(region.begin(), region.end(), target) != region.end();
}

auto match_counted_loop(Region &parent, Region::iterator position)
  -> std::optional<CountedLoop> {
  auto *loop = *position;
  if (loop->code != OpCode::While || position == parent.begin())
    return std::nullopt;
  auto &payload = std::get<WhilePayload>(loop->payload);
  if (payload.cond_region->empty() || payload.loop_region->size() < 3)
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
  auto *counter_load = defining_op(compare->operands[0]);
  if (
    !counter_load || counter_load->code != OpCode::Load ||
    counter_load->operands.size() != 1 ||
    !contains_op(*payload.cond_region, counter_load)
  )
    return std::nullopt;
  auto *counter_address = counter_load->operands[0];
  auto *counter_alloca = defining_op(counter_address);
  if (!counter_alloca || counter_alloca->code != OpCode::Alloca)
    return std::nullopt;

  auto init_position = std::prev(position);
  auto *init = *init_position;
  if (
    init->code != OpCode::Store || init->operands.size() != 2 ||
    init->operands[1] != counter_address || int_constant(init->operands[0]) != 0
  )
    return std::nullopt;
  if (std::find(parent.begin(), init_position, counter_alloca) == init_position)
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

  if (
    !has_only_user(counter_load->result, compare) ||
    !has_only_user(compare->result, condition) ||
    !has_only_user(tail_load->result, tail_add) ||
    !has_only_user(tail_add->result, tail_store)
  ) {
    return std::nullopt;
  }

  std::unordered_set<Op *> allowed_users{
    init, counter_load, tail_load, tail_store
  };
  for (auto *user_base : counter_address->users) {
    auto *user = static_cast<Op *>(user_base);
    if (!allowed_users.count(user))
      return std::nullopt;
  }
  if (counter_address->users.size() != allowed_users.size())
    return std::nullopt;

  return CountedLoop{
    loop,
    counter_load,
    compare,
    condition,
    tail_load,
    tail_add,
    tail_store,
    counter_address,
  };
}

auto map_root_at_call(
  Value *root,
  const Function &callee,
  const CallSite &site,
  const std::unordered_map<Function *, std::unique_ptr<PointerRootAnalysis>>
    &pointer_roots
) -> Value * {
  if (!root)
    return nullptr;
  if (root->kind == ValueKind::GlobalVar)
    return root;
  if (is_local_root(root))
    return root;
  if (root->kind != ValueKind::Argument)
    return nullptr;
  auto *argument = static_cast<Argument *>(root);
  if (
    argument->idx < 0 ||
    static_cast<size_t>(argument->idx) >= callee.args.size() ||
    static_cast<size_t>(argument->idx) >= site.call->operands.size()
  )
    return nullptr;
  auto analysis = pointer_roots.find(site.caller);
  if (analysis == pointer_roots.end())
    return nullptr;
  return analysis->second->resolve(site.call->operands[argument->idx]);
}

auto is_concrete_object(Value *root) -> bool {
  return root && (root->kind == ValueKind::GlobalVar || is_local_root(root));
}

auto prove_disjoint_at_calls(
  const Function &function,
  const std::unordered_set<Value *> &reads,
  const std::unordered_set<Value *> &writes,
  const std::vector<CallSite> &calls,
  const std::unordered_map<Function *, std::unique_ptr<PointerRootAnalysis>>
    &pointer_roots
) -> bool {
  if (calls.empty())
    return false;
  for (auto *read : reads) {
    for (auto *write : writes) {
      if (read == write)
        return false;
      if (
        read->kind == ValueKind::GlobalVar &&
        write->kind == ValueKind::GlobalVar
      )
        continue;
      for (const auto &site : calls) {
        auto *actual_read =
          map_root_at_call(read, function, site, pointer_roots);
        auto *actual_write =
          map_root_at_call(write, function, site, pointer_roots);
        if (
          !is_concrete_object(actual_read) ||
          !is_concrete_object(actual_write) || actual_read == actual_write
        )
          return false;
      }
    }
  }
  return true;
}

auto unlink_op(Op *op) -> void {
  for (auto *operand : op->operands) {
    if (operand)
      operand->rmUse(op);
  }
}

auto collect_local_allocas(
  const Region &region, std::unordered_set<Value *> &roots
) -> void {
  for (auto *op : region) {
    if (op->code == OpCode::Alloca && op->result)
      roots.insert(op->result);
    if (op->code == OpCode::If) {
      const auto &payload = std::get<IfPayload>(op->payload);
      collect_local_allocas(*payload.then_region, roots);
      if (payload.else_region)
        collect_local_allocas(*payload.else_region, roots);
    } else if (op->code == OpCode::While) {
      const auto &payload = std::get<WhilePayload>(op->payload);
      collect_local_allocas(*payload.cond_region, roots);
      collect_local_allocas(*payload.loop_region, roots);
    }
  }
}

auto contains_alloca(const Region &region) -> bool {
  return std::any_of(region.begin(), region.end(), [](const Op *op) {
    return op->code == OpCode::Alloca;
  });
}

auto replace_with_guarded_body(
  Module &module,
  Region &parent,
  Region::iterator position,
  const CountedLoop &match
) -> void {
  auto &payload = std::get<WhilePayload>(match.loop->payload);
  auto *zero = module.ctx.make_zero(match.counter_load->result->type);
  match.counter_load->result->rmUse(match.compare);
  match.compare->operands[0] = zero;
  zero->addUse(match.compare);

  auto counter_load_position = std::find(
    payload.cond_region->begin(), payload.cond_region->end(), match.counter_load
  );
  unlink_op(match.counter_load);
  payload.cond_region->erase(counter_load_position);
  unlink_op(match.condition);
  payload.cond_region->pop_back();

  for (auto *op : {match.tail_store, match.tail_add, match.tail_load})
    unlink_op(op);
  auto tail_begin = std::prev(payload.loop_region->end(), 3);
  payload.loop_region->erase(tail_begin, payload.loop_region->end());

  parent.splice(position, *payload.cond_region);
  auto then_region = std::make_unique<Region>();
  then_region->splice(then_region->end(), *payload.loop_region);
  auto *guarded = module.ctx.make_op(
    OpCode::If, IfPayload{std::move(then_region), std::nullopt}
  );
  guarded->operands = {match.compare->result};
  match.compare->result->addUse(guarded);
  *position = guarded;
}

class Simplifier {
public:
  Simplifier(
    Module &module,
    const std::unordered_map<std::string, Function *> &functions,
    const std::unordered_map<Function *, OpEffects> &summaries,
    const std::unordered_map<Function *, std::vector<CallSite>> &calls,
    const std::unordered_map<Function *, std::unique_ptr<PointerRootAnalysis>>
      &pointer_roots
  )
      : module(module), functions(functions), summaries(summaries),
        calls(calls), pointer_roots(pointer_roots) {}

  auto run(Function &function) -> bool {
    auto roots = pointer_roots.find(&function);
    if (roots == pointer_roots.end())
      return false;
    return simplify_region(function, function.body, *roots->second);
  }

private:
  Module &module;
  const std::unordered_map<std::string, Function *> &functions;
  const std::unordered_map<Function *, OpEffects> &summaries;
  const std::unordered_map<Function *, std::vector<CallSite>> &calls;
  const std::unordered_map<Function *, std::unique_ptr<PointerRootAnalysis>>
    &pointer_roots;

  auto simplify_region(
    Function &function, Region &region, const PointerRootAnalysis &roots
  ) -> bool {
    bool changed = false;
    for (auto it = region.begin(); it != region.end(); ++it) {
      auto *op = *it;
      if (op->code == OpCode::If) {
        auto &payload = std::get<IfPayload>(op->payload);
        changed |= simplify_region(function, *payload.then_region, roots);
        if (payload.else_region)
          changed |= simplify_region(function, *payload.else_region, roots);
        continue;
      }
      if (op->code != OpCode::While)
        continue;

      auto match = match_counted_loop(region, it);
      if (match && can_collapse(function, *match, roots)) {
        replace_with_guarded_body(module, region, it, *match);
        changed = true;
        auto &payload = std::get<IfPayload>((*it)->payload);
        changed |= simplify_region(function, *payload.then_region, roots);
        continue;
      }

      auto &payload = std::get<WhilePayload>(op->payload);
      changed |= simplify_region(function, *payload.cond_region, roots);
      changed |= simplify_region(function, *payload.loop_region, roots);
    }
    return changed;
  }

  auto can_collapse(
    Function &function,
    const CountedLoop &match,
    const PointerRootAnalysis &roots
  ) const -> bool {
    auto &payload = std::get<WhilePayload>(match.loop->payload);
    if (contains_alloca(*payload.cond_region))
      return false;
    std::unordered_set<Value *> ignored_roots{match.counter_address};
    collect_local_allocas(*payload.loop_region, ignored_roots);
    RootEffectCollector collector(roots, functions, summaries, ignored_roots);
    auto condition_effects = collector.collect(*payload.cond_region);
    auto body_effects = collector.collect(*payload.loop_region);
    if (
      !condition_effects || !condition_effects->writes.empty() ||
      !body_effects || body_effects->writes.empty()
    )
      return false;

    std::unordered_set<Value *> reads = body_effects->reads;
    reads.insert(
      condition_effects->reads.begin(), condition_effects->reads.end()
    );
    auto call_it = calls.find(&function);
    if (call_it == calls.end())
      return false;
    return prove_disjoint_at_calls(
      function, reads, body_effects->writes, call_it->second, pointer_roots
    );
  }
};

} // namespace

auto IdempotentLoopSimplify::run(Module &, exodus::opt::ModuleAnalysisManager &)
  -> exodus::opt::PreservedAnalysis {
  std::unordered_map<std::string, Function *> functions;
  std::unordered_map<Function *, std::unique_ptr<PointerRootAnalysis>>
    pointer_roots;
  for (auto &function : module->functions) {
    functions.emplace(function->name, function.get());
    if (!function->is_decl) {
      pointer_roots.emplace(
        function.get(), std::make_unique<PointerRootAnalysis>(*function)
      );
    }
  }

  CallCollector collector(functions);
  for (auto &function : module->functions) {
    if (!function->is_decl)
      collector.visit(*function);
  }

  auto summaries = get_function_effects(*module);
  Simplifier simplifier(
    *module, functions, summaries, collector.calls, pointer_roots
  );
  bool changed = false;
  for (auto &function : module->functions) {
    if (!function->is_decl)
      changed |= simplifier.run(*function);
  }
  return changed ? exodus::opt::PreservedAnalysis::none()
                 : exodus::opt::PreservedAnalysis::all();
}

} // namespace exodus::high_ir::opt
