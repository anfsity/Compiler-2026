#include "memoization.hpp"

#include "../../high/effects.hpp"
#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace exodus::high_ir::opt {
namespace {

constexpr int CacheSize = (1 << 12);

struct CacheInfo {
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
  const std::shared_ptr<Type> &element_type
) -> GlobalAddr * {
  auto global = std::make_unique<GlobalVar>();
  global->name = name;
  global->type = Array::get(element_type, CacheSize);
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
  const std::vector<GlobalAddr *> &context_globals
) -> CacheInfo {
  std::string base = "__exodus_memo_" + function.name;
  int suffix = 0;
  while (cache_name_exists(
    module, base, function.args.size(), context_globals.size()
  )) {
    base = "__exodus_memo_" + function.name + "_" + std::to_string(++suffix);
  }

  CacheInfo cache;
  cache.used = add_cache_global(module, base + "_used", I32::get());
  cache.value = add_cache_global(module, base + "_value", I32::get());
  cache.keys.reserve(function.args.size());
  for (size_t i = 0; i < function.args.size(); ++i) {
    cache.keys.push_back(
      add_cache_global(module, base + "_key" + std::to_string(i), I32::get())
    );
  }
  cache.context_globals = context_globals;
  cache.context_keys.reserve(context_globals.size());
  for (size_t i = 0; i < context_globals.size(); ++i) {
    cache.context_keys.push_back(add_cache_global(
      module, base + "_context" + std::to_string(i), I32::get()
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
  Value *index = zero;
  if (!function.args.empty()) {
    auto *mask = module.ctx.make_const(I32::get(), CacheSize - 1);
    auto *hash =
      make_binary(module, OpCode::And, I32::get(), function.args.front(), mask);
    lookup.push_back(hash);
    index = hash->result;
  }

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

  cache.context_key_ptrs.reserve(cache.context_keys.size());
  cache.context_values.reserve(cache.context_keys.size());
  for (size_t i = 0; i < cache.context_keys.size(); ++i) {
    auto *key = cache.context_keys[i];
    auto *key_ptr = make_getptr(module, key, index);
    cache.context_key_ptrs.push_back(key_ptr->result);
    lookup.push_back(key_ptr);

    auto *value = make_load(module, cache.context_globals[i], I32::get());
    cache.context_values.push_back(value->result);
    lookup.push_back(value);
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

auto is_memoizable(const Function &function, const OpEffects &effects)
  -> std::optional<std::vector<GlobalAddr *>> {
  if (
    function.is_decl || function.is_memoized || !function.type ||
    !function.type->is_func()
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

  // Read-only scalar globals are stable context dependencies.  In particular,
  // DU analysis of h-1-03 shows no store to lim in the loop containing the
  // calls, so all iterations share the same context key.  Keeping the value
  // in the key also makes the transformation correct if a later call site
  // observes a different value.
  auto dependencies = get_global_dependencies(effects);
  if (dependencies.size() != effects.reads.size())
    return std::nullopt;
  return dependencies;
}

} // namespace

auto Memoization::run(
  Module &m, exodus::opt::ModuleAnalysisManager & /* am */
) -> exodus::opt::PreservedAnalysis {
  auto effects = get_function_effects(m);
  bool changed = false;

  for (auto &function_ptr : m.functions) {
    auto &function = *function_ptr;
    auto effect_it = effects.find(&function);
    if (effect_it == effects.end())
      continue;

    auto context_keys = is_memoizable(function, effect_it->second);
    if (!context_keys)
      continue;

    auto cache = create_cache(m, function, *context_keys);
    auto lookup = build_lookup(m, function, cache);
    instrument_returns(m, function.body, function, cache);
    function.body.splice(function.body.begin(), lookup);
    function.is_memoized = true;
    changed = true;
  }

  return changed ? exodus::opt::PreservedAnalysis::none()
                 : exodus::opt::PreservedAnalysis::all();
}

} // namespace exodus::high_ir::opt
