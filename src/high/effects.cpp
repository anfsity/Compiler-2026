#include "effects.hpp"

#include "../base/getptr.hpp"
#include "scc.hpp"
#include "visitor.hpp"
#include <unordered_map>

namespace exodus::high_ir {
namespace {

auto pure_effects(bool speculatable = true) -> OpEffects {
  OpEffects effects;
  effects.speculatable = speculatable;
  effects.cseable = true;
  return effects;
}

} // namespace

auto get_addr_root(Value *value) -> Value * {
  while (value && value->kind == ValueKind::OpResult) {
    auto *creator = static_cast<Op *>(static_cast<OpResult *>(value)->creator);
    if (
      !creator || creator->code != OpCode::GetPtr || creator->operands.empty()
    ) {
      break;
    }
    value = creator->operands[0];
  }
  return value;
}

auto get_region_effects(const Region &region) -> OpEffects {
  OpEffects effects;
  for (const auto *op : region) {
    effects.merge(get_op_effects(*op));
    if (op->code == OpCode::If) {
      const auto &payload = std::get<IfPayload>(op->payload);
      effects.merge(get_region_effects(*payload.then_region));
      if (payload.else_region)
        effects.merge(get_region_effects(*payload.else_region));
    } else if (op->code == OpCode::While) {
      const auto &payload = std::get<WhilePayload>(op->payload);
      effects.merge(get_region_effects(*payload.cond_region));
      effects.merge(get_region_effects(*payload.loop_region));
    }
  }
  return effects;
}

auto get_op_effects(const Op &op) -> OpEffects {
  switch (op.code) {
  case OpCode::Add:
  case OpCode::Sub:
  case OpCode::Mul:
  case OpCode::FAdd:
  case OpCode::FSub:
  case OpCode::FMul:
  case OpCode::I2F:
  case OpCode::F2I:
  case OpCode::ZExt:
  case OpCode::Eq:
  case OpCode::Ne:
  case OpCode::Lt:
  case OpCode::Gt:
  case OpCode::Le:
  case OpCode::Ge:
  case OpCode::And:
  case OpCode::Or:
  case OpCode::Xor:
  case OpCode::Shl:
  case OpCode::Shr:
    return pure_effects();
  case OpCode::Div:
  case OpCode::Mod:
  case OpCode::FDiv:
    return pure_effects(false);
  case OpCode::Alloca: {
    OpEffects effects;
    effects.has_unique_identity = true;
    return effects;
  }
  case OpCode::Load: {
    OpEffects effects;
    if (!op.operands.empty())
      effects.reads.insert(op.operands[0]);
    return effects;
  }
  case OpCode::Store: {
    OpEffects effects;
    if (op.operands.size() >= 2)
      effects.writes.insert(op.operands[1]);
    return effects;
  }
  case OpCode::GetPtr: {
    auto effects = pure_effects();
    if (
      op.result && !op.operands.empty() && op.operands[0]->type->is_ptr() &&
      op.result->type->is_ptr()
    ) {
      auto plan = ir::analyze_getptr(
        op.operands[0]->type, op.result->type, op.operands.size() - 1
      );
      if (plan.reads_memory) {
        effects.reads.insert(op.operands[0]);
        effects.speculatable = false;
        effects.cseable = false;
      }
    }
    return effects;
  }
  case OpCode::Memset: {
    OpEffects effects;
    if (!op.operands.empty())
      effects.writes.insert(op.operands[0]);
    return effects;
  }
  case OpCode::Call: {
    OpEffects effects;
    effects.has_unknown_effect = true;
    for (auto *operand : op.operands) {
      if (operand && operand->type->is_ptr()) {
        effects.reads.insert(operand);
        effects.writes.insert(operand);
      }
    }
    return effects;
  }
  case OpCode::Ret:
  case OpCode::Break:
  case OpCode::Continue:
  case OpCode::Jump:
  case OpCode::Branch: {
    OpEffects effects;
    effects.has_control_effect = true;
    return effects;
  }
  case OpCode::If:
  case OpCode::While:
  case OpCode::Condition:
    return {};
  }

  OpEffects effects;
  effects.has_unknown_effect = true;
  return effects;
}

namespace {

auto same_effects(const OpEffects &lhs, const OpEffects &rhs) -> bool {
  return lhs.reads == rhs.reads && lhs.writes == rhs.writes &&
         lhs.has_unknown_effect == rhs.has_unknown_effect &&
         lhs.has_control_effect == rhs.has_control_effect &&
         lhs.speculatable == rhs.speculatable && lhs.cseable == rhs.cseable &&
         lhs.has_unique_identity == rhs.has_unique_identity;
}

auto map_effect_location(
  Value *location, const Function &callee, const Op &call
) -> Value * {
  auto *root = get_addr_root(location);
  if (!root || root->kind != ValueKind::Argument)
    return location;
  auto *argument = static_cast<Argument *>(root);
  if (
    argument->idx < 0 ||
    static_cast<size_t>(argument->idx) >= callee.args.size() ||
    static_cast<size_t>(argument->idx) >= call.operands.size()
  )
    return nullptr;
  return call.operands[argument->idx];
}

auto erase_local_locations(std::unordered_set<Value *> &locations) -> void {
  for (auto it = locations.begin(); it != locations.end();) {
    Value *root = get_addr_root(*it);
    bool is_local = false;
    if (root && root->kind == ValueKind::OpResult) {
      auto *creator = static_cast<OpResult *>(root)->creator;
      is_local = creator && static_cast<Op *>(creator)->code == OpCode::Alloca;
    }
    if (is_local)
      it = locations.erase(it);
    else
      ++it;
  }
}

struct FunctionEffectCollector : RecursiveOpVisitor<FunctionEffectCollector> {
  const std::unordered_map<std::string, Function *> &functions; // NOLINT
  const std::unordered_map<Function *, OpEffects> &summaries;   // NOLINT
  OpEffects effects;

  FunctionEffectCollector(
    const std::unordered_map<std::string, Function *> &f,
    const std::unordered_map<Function *, OpEffects> &s
  )
      : functions(f), summaries(s) {}

  using RecursiveOpVisitor<FunctionEffectCollector>::visit;

  auto visit(Op *op) -> void {
    if (op->code != OpCode::Call) {
      if (
        op->code != OpCode::Ret && op->code != OpCode::Break &&
        op->code != OpCode::Continue
      )
        effects.merge(get_op_effects(*op));
      if (op->code == OpCode::If) {
        auto &payload = std::get<IfPayload>(op->payload);
        visit(*payload.then_region);
        if (payload.else_region)
          visit(*payload.else_region);
      } else if (op->code == OpCode::While) {
        auto &payload = std::get<WhilePayload>(op->payload);
        visit(*payload.cond_region);
        visit(*payload.loop_region);
      }
      return;
    }

    auto call_effects = get_op_effects(*op);
    auto &payload = std::get<CallPayload>(op->payload);
    auto it = functions.find(payload.func_name);
    if (it == functions.end() || it->second->is_decl) {
      effects.merge(call_effects);
      return;
    }

    auto summary = summaries.find(it->second);
    if (summary == summaries.end()) {
      effects.merge(call_effects);
    } else {
      effects.merge(get_call_effects(*op, *it->second, summary->second));
    }
  }
};

} // namespace

auto get_function_effects(const Module &module)
  -> std::unordered_map<Function *, OpEffects> {
  CallGraph call_graph(module);
  std::unordered_map<std::string, Function *> functions;
  std::unordered_map<Function *, OpEffects> summaries;
  functions.reserve(module.functions.size());
  summaries.reserve(module.functions.size());

  for (const auto &function : module.functions) {
    functions.emplace(function->name, function.get());
    summaries.emplace(function.get(), OpEffects{});
  }

  // Process SCCs as groups. Calls inside a recursive SCC use the summary from
  // the previous iteration instead of being treated as unknown calls. The
  // summaries form a monotone dataflow problem, so recursive pure functions
  // remain pure while effects introduced by any member of the SCC propagate to
  // all callers after convergence.
  for (size_t iteration = 0; iteration < module.functions.size() + 1;
       ++iteration) {
    bool changed = false;
    for (const auto &scc : call_graph.getSCCs()) {
      for (auto *function : scc) {
        if (function->is_decl)
          continue;

        FunctionEffectCollector collector(functions, summaries);
        collector.visit(function->body);
        erase_local_locations(collector.effects.reads);
        erase_local_locations(collector.effects.writes);
        collector.effects.has_unique_identity = false;
        auto &old = summaries[function];
        if (!same_effects(old, collector.effects)) {
          old = std::move(collector.effects);
          changed = true;
        }
      }
    }
    if (!changed)
      break;
  }

  // Declarations have no body.  Keep their empty entry useful to clients,
  // while calls to them remain conservative in the collector above.
  return summaries;
}

auto get_call_effects(
  const Op &call, const Function &callee, const OpEffects &callee_effects
) -> OpEffects {
  OpEffects effects = callee_effects;
  effects.reads.clear();
  effects.writes.clear();
  for (auto *location : callee_effects.reads) {
    if (auto *mapped = map_effect_location(location, callee, call))
      effects.reads.insert(mapped);
    else
      effects.has_unknown_effect = true;
  }
  for (auto *location : callee_effects.writes) {
    if (auto *mapped = map_effect_location(location, callee, call))
      effects.writes.insert(mapped);
    else
      effects.has_unknown_effect = true;
  }
  return effects;
}

auto get_resolved_region_effects(
  const Region &region,
  const std::unordered_map<std::string, Function *> &functions,
  const std::unordered_map<Function *, OpEffects> &summaries
) -> OpEffects {
  OpEffects effects;
  for (auto *op : region) {
    if (op->code == OpCode::Call) {
      const auto &payload = std::get<CallPayload>(op->payload);
      auto function = functions.find(payload.func_name);
      if (function != functions.end() && !function->second->is_decl) {
        auto summary = summaries.find(function->second);
        if (summary != summaries.end()) {
          effects.merge(
            get_call_effects(*op, *function->second, summary->second)
          );
          continue;
        }
      }
      effects.merge(get_op_effects(*op));
    } else {
      effects.merge(get_op_effects(*op));
    }
    if (op->code == OpCode::If) {
      const auto &payload = std::get<IfPayload>(op->payload);
      effects.merge(
        get_resolved_region_effects(*payload.then_region, functions, summaries)
      );
      if (payload.else_region)
        effects.merge(get_resolved_region_effects(
          *payload.else_region, functions, summaries
        ));
    } else if (op->code == OpCode::While) {
      const auto &payload = std::get<WhilePayload>(op->payload);
      effects.merge(
        get_resolved_region_effects(*payload.cond_region, functions, summaries)
      );
      effects.merge(
        get_resolved_region_effects(*payload.loop_region, functions, summaries)
      );
    }
  }
  return effects;
}

} // namespace exodus::high_ir
