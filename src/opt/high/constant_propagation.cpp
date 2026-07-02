#include "constant_propagation.hpp"

namespace exodus::high_ir::opt {

CP::CP(Module *_m) : ctx(&_m->ctx), m(_m) {}

auto CP::run(Function &f, FunctionAnalysisManager & /* FAM */)
  -> PreservedAnalysis {
  env.clear();
  safe_allocas.clear();
  safe_globals.clear();
  rewriter.clear();
  changed = false;

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

auto CP::visit(Op *op) -> void {
  if (op->code == OpCode::If && op->operands[0]->kind == ValueKind::Constant) {
    changed = true;
    auto value = std::get<int>(static_cast<Constant *>(op->operands[0])->val);
    auto &p = std::get<IfPayload>(op->payload);
    if (value) {
      rewriter.replace_op_with_region(op, *p.then_region);
      if (p.else_region)
        rewriter.erase_region(*p.else_region);
      visit(*p.then_region);

    } else {
      if (p.else_region) {
        rewriter.replace_op_with_region(op, *p.else_region);
        visit(*p.else_region);
      }
      rewriter.erase_region(*p.then_region);
    }
    return;
  }

  RecursiveOpVisitor<CP>::visit(op);

  if (op->result && !op->operands.empty()) {
    try_fold(op);
  }
}

auto CP::ModifiedFinder::visit(Op *op, OpTag<OpCode::Store>) -> void {
  modified.insert(op->operands[1]);
}

auto CP::ModifiedFinder::visit(Op *op, OpTag<OpCode::Memset>) -> void {
  modified.insert(op->operands[0]);
}

auto CP::ModifiedFinder::visit(Op * /* op */, OpTag<OpCode::Call>) -> void {
  has_call = true;
}

auto CP::visit(Op *op, OpTag<OpCode::Alloca>) -> void {
  auto ptr_type = std::static_pointer_cast<exodus::Ptr>(op->result->type);
  if (ptr_type->target->is_i32() || ptr_type->target->is_f32()) {
    safe_allocas.insert(op->result);
  }
}

auto CP::visit(Op *op, OpTag<OpCode::Load>) -> void {
  Value *ptr = op->operands[0];
  if (env.count(ptr)) {
    rewriter.replace_op(op, env[ptr]);
    changed = true;
  }
}

auto CP::visit(Op *op, OpTag<OpCode::Store>) -> void {
  Value *val = op->operands[0];
  Value *ptr = op->operands[1];

  if (safe_allocas.count(ptr) || safe_globals.count(ptr)) {
    env[ptr] = val;
  }
}

auto CP::visit(Op *op, OpTag<OpCode::Call>) -> void {
  RecursiveOpVisitor<CP>::visit(op, OpTag<OpCode::Call>{});
  clear_global_env();
}

auto CP::visit(Op *op, OpTag<OpCode::If>) -> void {
  auto &p = std::get<IfPayload>(op->payload);
  ModifiedFinder finder;
  finder.visit(*p.then_region);
  if (p.else_region)
    finder.visit(*p.else_region);

  auto saved_env = env;
  visit(*p.then_region);

  if (p.else_region) {
    env = saved_env;
    visit(*p.else_region);
  }

  env = saved_env;
  for (auto *v : finder.modified)
    env.erase(v);

  if (finder.has_call)
    clear_global_env();
}

auto CP::visit(Op *op, OpTag<OpCode::While>) -> void {
  auto &p = std::get<WhilePayload>(op->payload);
  ModifiedFinder finder;
  finder.visit(*p.cond_region);
  finder.visit(*p.loop_region);

  auto saved_allocas = safe_allocas;
  auto saved_globals = safe_globals;

  // Loop entry: invalidate potentially modified variables
  for (auto *v : finder.modified) {
    safe_allocas.erase(v);
    safe_globals.erase(v);
    env.erase(v);
  }

  if (finder.has_call) {
    safe_globals.clear();
    clear_global_env();
  }

  visit(*p.cond_region);
  visit(*p.loop_region);

  safe_allocas = saved_allocas;
  safe_globals = saved_globals;
  for (auto *v : finder.modified)
    env.erase(v);

  if (finder.has_call)
    clear_global_env();
}

auto CP::clear_global_env() -> void {
  for (auto it = env.begin(); it != env.end();) {
    Value *addr = it->first;
    if (addr->kind == ValueKind::GlobalVar) {
      it = env.erase(it);
    } else {
      ++it;
    }
  }
}

auto CP::clear_all_env() -> void { env.clear(); }

template <typename T>
auto CP::fold_arith(OpCode code, T l, T r) -> std::optional<Constant::Data> {
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

auto CP::try_fold(Op *op) -> void {
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
