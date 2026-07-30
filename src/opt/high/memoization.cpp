#include "memoization.hpp"

#include "../../high/effects.hpp"
#include "../../high/scc.hpp"
#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace exodus::high_ir::opt {
namespace {

constexpr size_t MaxCacheEntries = (1 << 12);
constexpr size_t MinCacheEntries = (1 << 4);
constexpr size_t MinScopedCacheEntries = (1 << 10);
// Bound both slot count and statically allocated key/value words.
constexpr size_t MaxCacheWords = (1 << 14);
// The cache stores and compares every explicit argument.  Memoizing a very
// high-arity function expands the function body and every return path by the
// arity, while offering little reuse.  Keep memoization focused on small
// computational functions such as h-1-03::fun.
constexpr size_t MaxMemoizedArguments = 16;

struct CacheInfo {
  size_t size = 0;
  GlobalAddr *used = nullptr;
  GlobalAddr *value = nullptr;
  std::vector<GlobalAddr *> keys;
  std::vector<GlobalAddr *> context_globals;
  std::vector<GlobalAddr *> context_keys;

  Value *used_ptr = nullptr;
  Value *value_ptr = nullptr;
  std::vector<Value *> key_ptrs;
  std::vector<Value *> context_key_ptrs;
  std::vector<Value *> context_values;
  Value *one = nullptr;
};

auto make_op(
  Module &module,
  OpCode code,
  const std::shared_ptr<Type> &result_type,
  const std::vector<Value *> &operands,
  Op::Payload payload = EmptyPayload{}
) -> Op * {
  auto *op = module.ctx.make_op(code, std::move(payload));
  op->operands = operands;
  for (auto *operand : op->operands) {
    if (operand)
      operand->addUse(op);
  }
  if (result_type)
    op->result = module.ctx.make_value<OpResult>(result_type, op);
  return op;
}

auto make_getptr(Module &module, Value *base, Value *index) -> Op * {
  return make_op(module, OpCode::GetPtr, I32::get()->ptr_to(), {base, index});
}

auto make_load(
  Module &module, Value *address, const std::shared_ptr<Type> &type
) -> Op * {
  return make_op(module, OpCode::Load, type, {address});
}

auto make_store(Module &module, Value *value, Value *address) -> Op * {
  return make_op(module, OpCode::Store, nullptr, {value, address});
}

auto make_binary(
  Module &module,
  OpCode code,
  const std::shared_ptr<Type> &type,
  Value *lhs,
  Value *rhs
) -> Op * {
  return make_op(module, code, type, {lhs, rhs});
}

auto make_if(Module &module, Value *condition) -> Op * {
  IfPayload payload;
  payload.then_region = std::make_unique<Region>();
  auto *op =
    make_op(module, OpCode::If, nullptr, {condition}, std::move(payload));
  return op;
}

auto global_name_exists(const Module &module, const std::string &name) -> bool {
  return std::any_of(
    module.globals.begin(), module.globals.end(), [&](const auto &global) {
      return global->name == name;
    }
  );
}

auto add_cache_global(
  Module &module,
  const std::string &name,
  const std::shared_ptr<Type> &element_type,
  size_t cache_size
) -> GlobalAddr * {
  auto global = std::make_unique<GlobalVar>();
  global->name = name;
  global->type = Array::get(element_type, static_cast<int>(cache_size));
  global->init = {ZeroInit{}};
  global->addr =
    module.ctx.make_value<GlobalAddr>(global->type->ptr_to(), name);
  auto *address = global->addr;
  module.globals.push_back(std::move(global));
  return address;
}

auto cache_name_exists(
  const Module &module,
  const std::string &base,
  size_t argument_count, // NOLINT
  size_t context_count
) -> bool {
  if (
    global_name_exists(module, base + "_used") ||
    global_name_exists(module, base + "_value")
  )
    return true;
  for (size_t i = 0; i < argument_count; ++i) {
    if (global_name_exists(module, base + "_key" + std::to_string(i)))
      return true;
  }
  for (size_t i = 0; i < context_count; ++i) {
    if (global_name_exists(module, base + "_context" + std::to_string(i)))
      return true;
  }
  return false;
}

auto create_cache(
  Module &module,
  const Function &function,
  const std::vector<GlobalAddr *> &context_globals,
  size_t cache_size
) -> CacheInfo {
  std::string base = "__exodus_memo_" + function.name;
  int suffix = 0;
  while (cache_name_exists(
    module, base, function.args.size(), context_globals.size()
  )) {
    base = "__exodus_memo_" + function.name + "_" + std::to_string(++suffix);
  }

  CacheInfo cache;
  cache.size = cache_size;
  cache.used = add_cache_global(module, base + "_used", I32::get(), cache_size);
  cache.value =
    add_cache_global(module, base + "_value", I32::get(), cache_size);
  cache.keys.reserve(function.args.size());
  for (size_t i = 0; i < function.args.size(); ++i) {
    cache.keys.push_back(add_cache_global(
      module, base + "_key" + std::to_string(i), I32::get(), cache_size
    ));
  }
  cache.context_globals = context_globals;
  cache.context_keys.reserve(context_globals.size());
  for (size_t i = 0; i < context_globals.size(); ++i) {
    cache.context_keys.push_back(add_cache_global(
      module, base + "_context" + std::to_string(i), I32::get(), cache_size
    ));
  }
  cache.one = module.ctx.make_const(I32::get(), 1);
  return cache;
}

auto get_global_dependencies(const OpEffects &effects)
  -> std::vector<GlobalAddr *> {
  std::vector<GlobalAddr *> dependencies;
  dependencies.reserve(effects.reads.size());
  for (auto *location : effects.reads) {
    // A scalar global can be part of the memoization context.  A pointer into
    // an aggregate is not a value dependency: another store may change any
    // element without changing the pointer itself.
    if (!location || location->kind != ValueKind::GlobalVar)
      return {};
    auto *global = static_cast<GlobalAddr *>(location);
    if (
      !global->type->is_ptr() ||
      !std::static_pointer_cast<Ptr>(global->type)->target->is_i32()
    )
      return {};
    dependencies.push_back(global);
  }
  std::sort(
    dependencies.begin(),
    dependencies.end(),
    [](const auto *lhs, const auto *rhs) { return lhs->name < rhs->name; }
  );
  return dependencies;
}

auto build_lookup(Module &module, const Function &function, CacheInfo &cache)
  -> Region {
  Region lookup;
  auto *zero = module.ctx.make_const(I32::get(), 0);
  // Hashing only selects a slot. Exact argument/context comparisons below
  // remain the correctness check for a hit.
  Value *hash = zero;
  bool first_key = true;
  auto fold_key = [&](Value *key) {
    if (first_key) {
      hash = key;
    } else {
      auto *scaled = make_binary(
        module,
        OpCode::Mul,
        I32::get(),
        hash,
        module.ctx.make_const(I32::get(), 33)
      );
      lookup.push_back(scaled);
      hash = scaled->result;
      auto *mixed = make_binary(module, OpCode::Xor, I32::get(), hash, key);
      lookup.push_back(mixed);
      hash = mixed->result;
    }
    first_key = false;
  };

  for (auto *argument : function.args)
    fold_key(argument);

  cache.context_key_ptrs.reserve(cache.context_keys.size());
  cache.context_values.reserve(cache.context_keys.size());
  for (size_t i = 0; i < cache.context_keys.size(); ++i) {
    auto *value = make_load(module, cache.context_globals[i], I32::get());
    cache.context_values.push_back(value->result);
    lookup.push_back(value);
    fold_key(value->result);
  }

  auto *mask =
    module.ctx.make_const(I32::get(), static_cast<int>(cache.size - 1));
  auto *index_op = make_binary(module, OpCode::And, I32::get(), hash, mask);
  lookup.push_back(index_op);
  Value *index = index_op->result;

  auto *used_ptr = make_getptr(module, cache.used, index);
  cache.used_ptr = used_ptr->result;
  lookup.push_back(used_ptr);
  auto *value_ptr = make_getptr(module, cache.value, index);
  cache.value_ptr = value_ptr->result;
  lookup.push_back(value_ptr);

  cache.key_ptrs.reserve(cache.keys.size());
  for (auto *key : cache.keys) {
    auto *key_ptr = make_getptr(module, key, index);
    cache.key_ptrs.push_back(key_ptr->result);
    lookup.push_back(key_ptr);
  }

  for (size_t i = 0; i < cache.context_keys.size(); ++i) {
    auto *key = cache.context_keys[i];
    auto *key_ptr = make_getptr(module, key, index);
    cache.context_key_ptrs.push_back(key_ptr->result);
    lookup.push_back(key_ptr);
  }

  auto *used = make_load(module, cache.used_ptr, I32::get());
  lookup.push_back(used);
  auto *hit = make_binary(module, OpCode::Ne, Bool::get(), used->result, zero);
  lookup.push_back(hit);

  auto *outer_if = make_if(module, hit->result);
  lookup.push_back(outer_if);
  Region *current = std::get<IfPayload>(outer_if->payload).then_region.get();

  for (size_t i = 0; i < function.args.size(); ++i) {
    auto *key = make_load(module, cache.key_ptrs[i], I32::get());
    current->push_back(key);
    auto *same = make_binary(
      module, OpCode::Eq, Bool::get(), key->result, function.args[i]
    );
    current->push_back(same);
    auto *next_if = make_if(module, same->result);
    current->push_back(next_if);
    current = std::get<IfPayload>(next_if->payload).then_region.get();
  }

  for (size_t i = 0; i < cache.context_keys.size(); ++i) {
    auto *key = make_load(module, cache.context_key_ptrs[i], I32::get());
    current->push_back(key);
    auto *same = make_binary(
      module, OpCode::Eq, Bool::get(), key->result, cache.context_values[i]
    );
    current->push_back(same);
    auto *next_if = make_if(module, same->result);
    current->push_back(next_if);
    current = std::get<IfPayload>(next_if->payload).then_region.get();
  }

  auto *cached = make_load(module, cache.value_ptr, I32::get());
  current->push_back(cached);
  auto *ret = make_op(module, OpCode::Ret, nullptr, {cached->result});
  current->push_back(ret);
  return lookup;
}

auto instrument_returns(
  Module &module,
  Region &region,
  const Function &function,
  const CacheInfo &cache
) -> void {
  for (auto it = region.begin(); it != region.end(); ++it) {
    auto *op = *it;
    if (op->code == OpCode::If) {
      auto &payload = std::get<IfPayload>(op->payload);
      instrument_returns(module, *payload.then_region, function, cache);
      if (payload.else_region)
        instrument_returns(module, *payload.else_region, function, cache);
      continue;
    }
    if (op->code == OpCode::While) {
      auto &payload = std::get<WhilePayload>(op->payload);
      instrument_returns(module, *payload.cond_region, function, cache);
      instrument_returns(module, *payload.loop_region, function, cache);
      continue;
    }
    if (op->code != OpCode::Ret || op->operands.empty())
      continue;

    std::vector<Op *> stores;
    stores.reserve(cache.key_ptrs.size() + cache.context_key_ptrs.size() + 2);
    stores.push_back(make_store(module, op->operands.front(), cache.value_ptr));
    for (size_t i = 0; i < function.args.size(); ++i) {
      stores.push_back(make_store(module, function.args[i], cache.key_ptrs[i]));
    }
    for (size_t i = 0; i < cache.context_key_ptrs.size(); ++i) {
      stores.push_back(
        make_store(module, cache.context_values[i], cache.context_key_ptrs[i])
      );
    }
    stores.push_back(make_store(module, cache.one, cache.used_ptr));
    for (auto *store : stores)
      region.insert(it, store);
  }
}

struct CostEstimate {
  size_t body_cost = 0;
  size_t recursive_calls = 0;
  size_t return_paths = 0;
};

auto estimate_region(
  const Region &region, const std::string &function_name, CostEstimate &estimate
) -> void {
  for (auto *op : region) {
    ++estimate.body_cost;
    switch (op->code) {
    case OpCode::Div:
    case OpCode::Mod:
    case OpCode::FDiv:
      estimate.body_cost += 7;
      break;
    case OpCode::Call:
      estimate.body_cost += 3;
      if (
        std::holds_alternative<CallPayload>(op->payload) &&
        std::get<CallPayload>(op->payload).func_name == function_name
      )
        ++estimate.recursive_calls;
      break;
    case OpCode::Load:
    case OpCode::Store:
    case OpCode::GetPtr:
    case OpCode::If:
    case OpCode::While:
      estimate.body_cost += 2;
      break;
    default:
      break;
    }

    if (op->code == OpCode::Ret) {
      ++estimate.return_paths;
    } else if (op->code == OpCode::If) {
      const auto &payload = std::get<IfPayload>(op->payload);
      estimate_region(*payload.then_region, function_name, estimate);
      if (payload.else_region)
        estimate_region(*payload.else_region, function_name, estimate);
    } else if (op->code == OpCode::While) {
      const auto &payload = std::get<WhilePayload>(op->payload);
      estimate_region(*payload.cond_region, function_name, estimate);
      estimate_region(*payload.loop_region, function_name, estimate);
    }
  }
}

struct MemoizationPlan {
  std::vector<GlobalAddr *> context_globals;
  size_t cache_size = 0;
  bool reset_on_entry = false;
};

auto next_power_of_two(size_t value) -> size_t {
  size_t result = 1;
  while (result < value && result < MaxCacheEntries)
    result <<= 1;
  return result;
}

auto choose_cache_size(
  size_t body_cost, size_t recursive_calls, size_t key_count
) -> size_t {
  const auto reuse_score = body_cost * std::max<size_t>(1, recursive_calls);
  auto desired = std::max(MinCacheEntries, reuse_score / 4);
  desired = std::min(desired, MaxCacheEntries);
  auto slots = next_power_of_two(desired);

  // Every slot contains used/value plus one word per identity key.
  const auto fields_per_slot = key_count + 2;
  auto footprint_limit = MaxCacheWords / std::max<size_t>(1, fields_per_slot);
  footprint_limit = std::max<size_t>(MinCacheEntries, footprint_limit);
  footprint_limit = std::min(footprint_limit, MaxCacheEntries);
  while (slots > footprint_limit)
    slots >>= 1;
  return std::max(slots, MinCacheEntries);
}

auto choose_scoped_cache_size(size_t key_count) -> size_t {
  const auto fields_per_slot = key_count + 2;
  auto footprint_limit = MaxCacheWords / std::max<size_t>(1, fields_per_slot);
  footprint_limit = std::min(footprint_limit, MaxCacheEntries);
  auto slots = size_t{1};
  while ((slots << 1) <= footprint_limit)
    slots <<= 1;
  return std::max(std::min(slots, MinScopedCacheEntries), MinCacheEntries);
}

auto is_memoizable(const Function &function, const OpEffects &effects)
  -> std::optional<MemoizationPlan> {
  if (
    function.is_decl || function.is_memoized || !function.type ||
    !function.type->is_func() || function.args.size() > MaxMemoizedArguments
  )
    return std::nullopt;

  auto function_type = std::static_pointer_cast<Func>(function.type);
  if (!function_type->ret_type->is_i32())
    return std::nullopt;
  for (auto *arg : function.args) {
    if (!arg->type->is_i32())
      return std::nullopt;
  }

  if (
    effects.has_unknown_effect || !effects.writes.empty() ||
    effects.has_unique_identity
  )
    return std::nullopt;

  // Read-only scalar globals can be represented directly as context keys.
  // Other read locations, including aggregate elements, cannot: hashing their
  // pointer would not capture the memory contents.  They are nevertheless
  // stable during one invocation because the effect summary above proves that
  // the complete recursive computation performs no writes.  Such functions
  // use an invocation-scoped cache that is cleared by a non-recursive wrapper.
  auto dependencies = get_global_dependencies(effects);
  const bool reset_on_entry = dependencies.size() != effects.reads.size();
  if (reset_on_entry)
    dependencies.clear();

  CostEstimate estimate;
  estimate_region(function.body, function.name, estimate);
  if (estimate.recursive_calls == 0 || estimate.return_paths == 0)
    return std::nullopt;

  const auto key_count = function.args.size() + dependencies.size();
  // A lookup performs hashing, address formation, loads, and one comparison
  // per key. Every return path also pays for the complete cache store.
  const auto hash_ops = key_count == 0 ? 0 : key_count * 2 - 1;
  const auto lookup_ops =
    hash_ops + (key_count + 2) + (key_count + 2) + key_count + 2;
  const auto store_ops = key_count + 2;
  const auto generated_ops = lookup_ops + estimate.return_paths * store_ops;
  const auto recursive_work = estimate.body_cost * estimate.recursive_calls;

  if (reset_on_entry) {
    // Invocation scoping is intended for branching recurrences where reuse
    // can occur across sibling recursive subtrees.  Exact key comparisons
    // preserve semantics even when no reuse occurs; this cost gate avoids
    // adding the lookup to linear recursion or bodies cheaper than the lookup.
    if (
      estimate.recursive_calls < 2 || estimate.body_cost <= lookup_ops ||
      recursive_work < generated_ops
    )
      return std::nullopt;
  } else {
    // Require enough recursive work to amortize both the lookup and the stores,
    // and keep generated cache IR below one third of the estimated body. This
    // rejects cheap recursive wrappers while allowing expensive computations.
    if (
      estimate.body_cost <= lookup_ops ||
      generated_ops * 3 > estimate.body_cost ||
      recursive_work < generated_ops * 3
    )
      return std::nullopt;
  }

  return MemoizationPlan{
    std::move(dependencies),
    reset_on_entry ? choose_scoped_cache_size(function.args.size())
                   : choose_cache_size(
                       estimate.body_cost, estimate.recursive_calls, key_count
                     ),
    reset_on_entry
  };
}

auto rewrite_self_calls(
  Region &region, const std::string &old_name, const std::string &helper_name
) -> void {
  for (auto *op : region) {
    if (op->code == OpCode::Call) {
      auto &payload = std::get<CallPayload>(op->payload);
      if (payload.func_name == old_name)
        payload.func_name = helper_name;
    } else if (op->code == OpCode::If) {
      auto &payload = std::get<IfPayload>(op->payload);
      rewrite_self_calls(*payload.then_region, old_name, helper_name);
      if (payload.else_region)
        rewrite_self_calls(*payload.else_region, old_name, helper_name);
    } else if (op->code == OpCode::While) {
      auto &payload = std::get<WhilePayload>(op->payload);
      rewrite_self_calls(*payload.cond_region, old_name, helper_name);
      rewrite_self_calls(*payload.loop_region, old_name, helper_name);
    }
  }
}

auto unique_function_name(const Module &module, const std::string &base)
  -> std::string {
  auto exists = [&](const std::string &name) {
    return std::any_of(
      module.functions.begin(), module.functions.end(), [&](const auto &func) {
        return func->name == name;
      }
    );
  };
  auto name = base;
  for (size_t suffix = 0; exists(name); ++suffix)
    name = base + "_" + std::to_string(suffix + 1);
  return name;
}

auto wrap_scoped_memoized_function(
  Module &module,
  Function &helper,
  CacheInfo &cache,
  const std::string &public_name
) -> void {
  const auto helper_name =
    unique_function_name(module, "__exodus_memo_body_" + public_name);
  helper.name = helper_name;
  rewrite_self_calls(helper.body, public_name, helper_name);

  auto wrapper = std::make_unique<Function>();
  wrapper->name = public_name;
  wrapper->type = helper.type;
  wrapper->args.reserve(helper.args.size());
  for (size_t index = 0; index < helper.args.size(); ++index) {
    wrapper->args.push_back(module.ctx.make_value<Argument>(
      helper.args[index]->type, static_cast<int>(index)
    ));
  }

  auto *zero = module.ctx.make_const(I32::get(), 0);
  auto *used_begin = make_getptr(module, cache.used, zero);
  wrapper->body.push_back(used_begin);
  wrapper->body.push_back(make_op(
    module,
    OpCode::Memset,
    nullptr,
    {used_begin->result,
     module.ctx.make_const(I32::get(), static_cast<int>(cache.size)),
     zero}
  ));
  auto *call = make_op(
    module,
    OpCode::Call,
    std::static_pointer_cast<Func>(helper.type)->ret_type,
    std::vector<Value *>(wrapper->args.begin(), wrapper->args.end()),
    CallPayload{helper_name}
  );
  wrapper->body.push_back(call);
  wrapper->body.push_back(
    make_op(module, OpCode::Ret, nullptr, {call->result})
  );
  module.functions.push_back(std::move(wrapper));
}

} // namespace

auto Memoization::run(
  Module &m, exodus::opt::ModuleAnalysisManager & /* am */
) -> exodus::opt::PreservedAnalysis {
  auto effects = get_function_effects(m);
  CallGraph call_graph(m);
  bool changed = false;

  struct Candidate {
    Function *function = nullptr;
    MemoizationPlan plan;
  };
  std::vector<Candidate> candidates;
  candidates.reserve(m.functions.size());

  for (auto &function_ptr : m.functions) {
    auto &function = *function_ptr;
    auto effect_it = effects.find(&function);
    if (effect_it == effects.end() || !call_graph.isRecursive(&function))
      continue;

    auto plan = is_memoizable(function, effect_it->second);
    if (!plan)
      continue;

    if (plan->reset_on_entry) {
      const bool singleton_scc = std::any_of(
        call_graph.getSCCs().begin(),
        call_graph.getSCCs().end(),
        [&](const auto &scc) {
          return scc.size() == 1 && scc.front() == &function;
        }
      );
      // A wrapper around one member of a mutually recursive SCC could be
      // re-entered by another member and clear a still-active invocation's
      // cache. Keep the original SCC unless one wrapper encloses all recursion.
      if (!singleton_scc)
        continue;
    }

    candidates.push_back({&function, std::move(*plan)});
  }

  for (auto &candidate : candidates) {
    auto &function = *candidate.function;
    const auto public_name = function.name;
    auto cache = create_cache(
      m, function, candidate.plan.context_globals, candidate.plan.cache_size
    );
    auto lookup = build_lookup(m, function, cache);
    instrument_returns(m, function.body, function, cache);
    function.body.splice(function.body.begin(), lookup);
    function.is_memoized = true;
    if (candidate.plan.reset_on_entry)
      wrap_scoped_memoized_function(m, function, cache, public_name);
    changed = true;
  }

  return changed ? exodus::opt::PreservedAnalysis::none()
                 : exodus::opt::PreservedAnalysis::all();
}

} // namespace exodus::high_ir::opt
