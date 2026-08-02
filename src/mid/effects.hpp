#pragma once

#include "getptr.hpp"
#include "ir.hpp"
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace exodus::mid_ir {

struct MidFunctionEffectSummary {
  bool unknown = false;
  bool reads_memory = false;
  bool writes_memory = false;

  auto merge(const MidFunctionEffectSummary &other) -> void {
    unknown |= other.unknown;
    reads_memory |= other.reads_memory;
    writes_memory |= other.writes_memory;
  }

  auto operator==(const MidFunctionEffectSummary &other) const -> bool {
    return unknown == other.unknown && reads_memory == other.reads_memory &&
           writes_memory == other.writes_memory;
  }
};

inline auto call_result_is_scalar(const LinearFunction &function) -> bool {
  if (!function.type || !function.type->is_func())
    return false;
  auto type = std::static_pointer_cast<Func>(function.type);
  return type->ret_type->is_i32() || type->ret_type->is_f32() ||
         type->ret_type->is_bool();
}

inline auto op_effect_summary(
  const Op &op,
  const std::unordered_map<std::string, LinearFunction *> &functions,
  const std::unordered_map<LinearFunction *, MidFunctionEffectSummary>
    &summaries
) -> MidFunctionEffectSummary {
  MidFunctionEffectSummary effects;
  switch (op.code) {
  case OpCode::Load:
    effects.reads_memory = true;
    return effects;
  case OpCode::Store:
  case OpCode::Memset:
    effects.writes_memory = true;
    return effects;
  case OpCode::GetPtr: {
    auto plan = analyze_getptr(op);
    if (!plan.valid || plan.reads_memory)
      effects.reads_memory = true;
    return effects;
  }
  case OpCode::Call: {
    const auto &payload = std::get<CallPayload>(op.payload);
    auto callee = functions.find(payload.func_name);
    if (callee == functions.end() || callee->second->is_decl) {
      effects.unknown = true;
      return effects;
    }
    auto summary = summaries.find(callee->second);
    if (summary == summaries.end()) {
      effects.unknown = true;
      return effects;
    }
    effects.merge(summary->second);
    return effects;
  }
  default:
    return effects;
  }
}

inline auto compute_mid_function_effects(const MidModule &module)
  -> std::unordered_map<LinearFunction *, MidFunctionEffectSummary> {
  std::unordered_map<std::string, LinearFunction *> functions;
  std::unordered_map<LinearFunction *, MidFunctionEffectSummary> summaries;
  functions.reserve(module.functions.size());
  summaries.reserve(module.functions.size());

  for (const auto &function : module.functions) {
    functions.emplace(function->name, function.get());
    summaries.emplace(function.get(), MidFunctionEffectSummary{});
  }

  for (size_t iteration = 0; iteration < module.functions.size() + 1;
       ++iteration) {
    bool changed = false;
    for (const auto &function : module.functions) {
      if (function->is_decl)
        continue;

      MidFunctionEffectSummary next;
      for (const auto &block : function->blocks) {
        for (auto *op : block->insts) {
          next.merge(op_effect_summary(*op, functions, summaries));
        }
      }

      auto &old = summaries[function.get()];
      if (!(old == next)) {
        old = next;
        changed = true;
      }
    }
    if (!changed)
      break;
  }

  return summaries;
}

inline auto compute_readnone_scalar_functions(const MidModule &module)
  -> std::unordered_set<std::string> {
  auto summaries = compute_mid_function_effects(module);
  std::unordered_set<std::string> result;
  for (const auto &function : module.functions) {
    if (function->is_decl || !call_result_is_scalar(*function))
      continue;
    auto summary = summaries.find(function.get());
    if (
      summary != summaries.end() && !summary->second.unknown &&
      !summary->second.reads_memory && !summary->second.writes_memory
    ) {
      result.insert(function->name);
    }
  }
  return result;
}

inline auto is_readnone_scalar_call(
  const Op &op, const std::unordered_set<std::string> &readnone_functions
) -> bool {
  if (
    op.code != OpCode::Call || !op.result ||
    !(op.result->type->is_i32() || op.result->type->is_f32() ||
      op.result->type->is_bool())
  ) {
    return false;
  }
  const auto &payload = std::get<CallPayload>(op.payload);
  return readnone_functions.count(payload.func_name) != 0;
}

} // namespace exodus::mid_ir
