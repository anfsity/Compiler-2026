#include "const_prop.hpp"

namespace exodus::high_ir::opt {

ConstProp::ConstProp(Module *_m) : ctx(&_m->ctx), m(_m) {}

auto ConstProp::run(Function &f, FunctionAnalysisManager & /* FAM */)
  -> PreservedAnalysis {
  env.clear();
  safe_allocas.clear();
  safe_globals.clear();
  rewriter.clear();
  changed = false;
  functions.clear();
  for (auto &function : m->functions)
    functions.emplace(function->name, function.get());
  function_effects = get_function_effects(*m);

  for (auto &g : m->globals) {
    if (g->is_const && (g->type->is_i32() || g->type->is_f32())) {
      safe_globals.insert(g->addr);
      if (g->addr) {
        if (
          std::holds_alternative<int>(g->init.data) ||
          std::holds_alternative<float>(g->init.data)
        ) {
          env[g->addr] = ctx->make_const(g->type, g->init);
        } else if (std::holds_alternative<ZeroInit>(g->init.data)) {
          env[g->addr] = ctx->make_zero(g->type);
        }
      }
    }
  }

  visit(f.body);
  rewriter.finalize(f);

  return changed ? PreservedAnalysis::none() : PreservedAnalysis::all();
}

auto ConstProp::visit(Op *op) -> void {
  RecursiveOpVisitor<ConstProp>::visit(op);

  if (op->result && !op->operands.empty()) {
    try_fold(op);
  }
}

auto ConstProp::visit(Op *op, OpTag<OpCode::Alloca>) -> void {
  auto ptr_type = std::static_pointer_cast<exodus::Ptr>(op->result->type);
  if (
    ptr_type->target->is_i32() || ptr_type->target->is_f32() ||
    ptr_type->target->is_bool()
  ) {
    safe_allocas.insert(op->result);
  }
}

auto ConstProp::visit(Op *op, OpTag<OpCode::Load>) -> void {
  Value *ptr = op->operands[0];
  if (env.count(ptr)) {
    rewriter.replace_op(op, env[ptr]);
    changed = true;
  }
}

auto ConstProp::visit(Op *op, OpTag<OpCode::Store>) -> void {
  Value *val = op->operands[0];
  Value *ptr = op->operands[1];

  if (safe_allocas.count(ptr) || safe_globals.count(ptr)) {
    env[ptr] = val;
  }
}

auto ConstProp::visit(Op *op, OpTag<OpCode::Call>) -> void {
  RecursiveOpVisitor<ConstProp>::visit(op, OpTag<OpCode::Call>{});
  const auto &payload = std::get<CallPayload>(op->payload);
  auto function = functions.find(payload.func_name);
  if (function != functions.end() && !function->second->is_decl) {
    auto summary = function_effects.find(function->second);
    if (summary != function_effects.end()) {
      invalidate_writes(
        get_call_effects(*op, *function->second, summary->second)
      );
      return;
    }
  }
  invalidate_writes(get_op_effects(*op));
}

auto ConstProp::visit(Op *op, OpTag<OpCode::If>) -> void {
  auto &p = std::get<IfPayload>(op->payload);
  auto effects = resolved_effects(*p.then_region);
  if (p.else_region)
    effects.merge(resolved_effects(*p.else_region));

  auto saved_env = env;
  visit(*p.then_region);

  if (p.else_region) {
    env = saved_env;
    visit(*p.else_region);
  }

  env = saved_env;
  invalidate_writes(effects);
}

auto ConstProp::visit(Op *op, OpTag<OpCode::While>) -> void {
  auto &p = std::get<WhilePayload>(op->payload);
  auto effects = resolved_effects(*p.cond_region);
  effects.merge(resolved_effects(*p.loop_region));

  auto saved_allocas = safe_allocas;
  auto saved_globals = safe_globals;

  // Loop entry: invalidate potentially modified variables
  for (auto *v : effects.writes) {
    safe_allocas.erase(v);
    safe_globals.erase(v);
    env.erase(v);
  }

  if (effects.has_unknown_effect) {
    safe_globals.clear();
    clear_global_env();
  }

  visit(*p.cond_region);
  visit(*p.loop_region);

  safe_allocas = saved_allocas;
  safe_globals = saved_globals;
  invalidate_writes(effects);
}

auto ConstProp::invalidate_writes(const OpEffects &effects) -> void {
  for (auto *value : effects.writes)
    env.erase(value);
  if (effects.has_unknown_effect)
    clear_global_env();
}

auto ConstProp::resolved_effects(const Region &region) const -> OpEffects {
  return get_resolved_region_effects(region, functions, function_effects);
}

auto ConstProp::clear_global_env() -> void {
  for (auto it = env.begin(); it != env.end();) {
    Value *addr = it->first;
    if (addr->kind == ValueKind::GlobalVar) {
      it = env.erase(it);
    } else {
      ++it;
    }
  }
}

template <typename T>
auto ConstProp::fold_arith(OpCode code, T l, T r)
  -> std::optional<Constant::Data> {
  switch (code) {
    // clang-format off
  case OpCode::Add:  case OpCode::FAdd: return l + r;
  case OpCode::Sub:  case OpCode::FSub: return l - r;
  case OpCode::Mul:  case OpCode::FMul: return l * r;
  case OpCode::Div:  case OpCode::FDiv: { 
    return r != 0 ? std::optional<Constant::Data>(l / r) : std::nullopt; 
  }
  case OpCode::Mod:  if constexpr (std::is_integral_v<T>) { 
    return r != 0 ? std::optional<Constant::Data>(l % r) : std::nullopt; 
  }
  case OpCode::Eq:   return (int)(l == r);
  case OpCode::Ne:   return (int)(l != r);
  case OpCode::Lt:   return (int)(l < r);
  case OpCode::Gt:   return (int)(l > r);
  case OpCode::Le:   return (int)(l <= r);
  case OpCode::Ge:   return (int)(l >= r);
  default: return std::nullopt;
    // clang-format on
  }
}

auto ConstProp::try_fold(Op *op) -> void {
  for (auto *v : op->operands)
    if (v->kind != ValueKind::Constant)
      return;

  std::optional<Constant::Data> res;
  if (op->operands.size() == 2u) {
    auto *l = static_cast<Constant *>(op->operands[0]);
    auto *r = static_cast<Constant *>(op->operands[1]);

    if (
      std::holds_alternative<int>(l->val) && std::holds_alternative<int>(r->val)
    ) {
      res = fold_arith(op->code, std::get<int>(l->val), std::get<int>(r->val));
    } else if (
      std::holds_alternative<float>(l->val) &&
      std::holds_alternative<float>(r->val)
    ) {
      res =
        fold_arith(op->code, std::get<float>(l->val), std::get<float>(r->val));
    }

  } else if (op->operands.size() == 1u) {
    auto *v = static_cast<Constant *>(op->operands[0]);
    if (op->code == OpCode::I2F)
      res = (float)std::get<int>(v->val);
    else if (op->code == OpCode::F2I)
      res = (int)std::get<float>(v->val);
    else if (op->code == OpCode::ZExt)
      res = std::get<int>(v->val);
  }

  if (res) {
    rewriter.replace_op(op, ctx->make_const(op->result->type, *res));
    changed = true;
  }
}

} // namespace exodus::high_ir::opt
