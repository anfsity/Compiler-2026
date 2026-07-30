#include "sdce.hpp"

namespace exodus::high_ir::opt {

namespace {

auto get_alloca_root(Value *value) -> Value * {
  auto *root = get_addr_root(value);
  if (!root || root->kind != ValueKind::OpResult)
    return nullptr;

  auto *creator = static_cast<OpResult *>(root)->creator;
  if (!creator || static_cast<Op *>(creator)->code != OpCode::Alloca)
    return nullptr;
  return root;
}

} // namespace

auto SimpleDCE::global_root(Value *value) const -> Value * {
  auto *root = get_addr_root(value);
  return root && root->kind == ValueKind::GlobalVar ? root : nullptr;
}

auto SimpleDCE::mark_global_read(Value *address) -> void {
  if (auto *root = global_root(address))
    global_uses[root].reads = true;
}

auto SimpleDCE::mark_global_write(Value *address) -> void {
  if (auto *root = global_root(address))
    global_uses[root].writes = true;
}

auto SimpleDCE::mark_global_escape(Value *value) -> void {
  if (auto *root = global_root(value))
    global_uses[root].escapes = true;
}

auto SimpleDCE::mark_global_unknown(Value *value) -> void {
  if (auto *root = global_root(value))
    global_uses[root].unknown = true;
}

auto SimpleDCE::scan_global_uses(const Region &region) -> void {
  for (auto *op : region) {
    switch (op->code) {
    case OpCode::Load:
      if (!op->operands.empty())
        mark_global_read(op->operands[0]);
      break;
    case OpCode::Store:
      if (op->operands.size() >= 2) {
        mark_global_write(op->operands[1]);
        if (op->operands[0] && op->operands[0]->type->is_ptr())
          mark_global_escape(op->operands[0]);
      }
      break;
    case OpCode::GetPtr: {
      auto effects = get_op_effects(*op);
      for (auto *address : effects.reads)
        mark_global_read(address);
      break;
    }
    case OpCode::Memset:
      if (!op->operands.empty())
        mark_global_write(op->operands[0]);
      break;
    case OpCode::Call:
      for (auto *operand : op->operands) {
        if (operand && operand->type->is_ptr())
          mark_global_escape(operand);
      }
      break;
    case OpCode::Ret:
      for (auto *operand : op->operands) {
        if (operand && operand->type->is_ptr())
          mark_global_escape(operand);
      }
      break;
    default:
      for (auto *operand : op->operands) {
        if (operand && operand->type->is_ptr())
          mark_global_unknown(operand);
      }
      break;
    }

    if (op->code == OpCode::If) {
      const auto &payload = std::get<IfPayload>(op->payload);
      scan_global_uses(*payload.then_region);
      if (payload.else_region)
        scan_global_uses(*payload.else_region);
    } else if (op->code == OpCode::While) {
      const auto &payload = std::get<WhilePayload>(op->payload);
      scan_global_uses(*payload.cond_region);
      scan_global_uses(*payload.loop_region);
    }
  }
}

auto SimpleDCE::collect_global_uses() -> void {
  global_uses.clear();
  dead_global_roots.clear();
  if (!module)
    return;

  for (const auto &function : module->functions) {
    if (!function->is_decl)
      scan_global_uses(function->body);
  }

  for (const auto &[root, use] : global_uses) {
    if (use.writes && !use.reads && !use.escapes && !use.unknown)
      dead_global_roots.insert(root);
  }
}

auto SimpleDCE::dead_global_write(Op *op) const -> bool {
  Value *address = nullptr;
  if (op->code == OpCode::Store && op->operands.size() >= 2)
    address = op->operands[1];
  else if (op->code == OpCode::Memset && !op->operands.empty())
    address = op->operands[0];
  auto *root = global_root(address);
  return root && dead_global_roots.count(root) != 0;
}

auto SimpleDCE::mark_stores_to(Value *ptr) -> void {
  Value *root = get_addr_root(ptr);
  for (auto &[user, _] : parents) {
    Value *address = nullptr;
    if (user->code == OpCode::Store && user->operands.size() >= 2)
      address = user->operands[1];
    else if (user->code == OpCode::Memset && !user->operands.empty())
      address = user->operands[0];
    if (address && get_addr_root(address) == root) {
      mark(user);
    }
  }
}

auto SimpleDCE::collect_escaped_allocas() -> void {
  for (auto &[op, _] : parents) {
    auto record_pointer = [&](Value *value) {
      if (!value || !value->type->is_ptr())
        return;
      if (auto *root = get_alloca_root(value))
        escaped_allocas.insert(root);
    };

    if (op->code == OpCode::Call) {
      for (auto *operand : op->operands)
        record_pointer(operand);
    } else if (op->code == OpCode::Store && !op->operands.empty()) {
      record_pointer(op->operands[0]);
    }
  }
}

auto SimpleDCE::mark_memory_dependencies(Op *op) -> void {
  auto effects = get_op_effects(*op);
  for (auto *location : effects.reads)
    mark_stores_to(location);
  if (effects.has_unknown_effect) {
    for (auto *root : escaped_allocas)
      mark_stores_to(root);
  }
}

auto SimpleDCE::is_intrinsically_live(Op *op) const -> bool {
  if (op->code == OpCode::If || op->code == OpCode::While)
    return false;

  if (dead_global_write(op))
    return false;

  auto effects = get_op_effects(*op);
  if (effects.has_control_effect || effects.has_unknown_effect)
    return true;

  if (!effects.writes_memory())
    return false;

  if (op->code != OpCode::Store || effects.writes.size() != 1)
    return true;

  auto *address = *effects.writes.begin();
  if (address && address->kind == ValueKind::OpResult) {
    auto *creator = static_cast<OpResult *>(address)->creator;
    return !creator || static_cast<Op *>(creator)->code != OpCode::Alloca;
  }
  return true;
}

auto SimpleDCE::build_parent_map(Region &r, Op *parent) -> void {
  for (auto *op : r) {
    parents[op] = parent;
    if (op->code == OpCode::If) {
      auto &p = std::get<IfPayload>(op->payload);
      if (p.then_region)
        build_parent_map(*p.then_region, op);
      if (p.else_region)
        build_parent_map(*p.else_region, op);
    } else if (op->code == OpCode::While) {
      auto &p = std::get<WhilePayload>(op->payload);
      if (p.cond_region)
        build_parent_map(*p.cond_region, op);
      if (p.loop_region)
        build_parent_map(*p.loop_region, op);
    }
  }
}

auto SimpleDCE::mark(Op *op) -> void {
  if (op && liveset.insert(op).second) {
    worklist.push_back(op);
  }
}

auto SimpleDCE::initial_mark(Region &r) -> void {
  for (auto *op : r) {
    if (is_intrinsically_live(op)) {
      mark(op);
    }
    if (op->code == OpCode::If) {
      auto &p = std::get<IfPayload>(op->payload);
      if (p.then_region)
        initial_mark(*p.then_region);
      if (p.else_region)
        initial_mark(*p.else_region);
    } else if (op->code == OpCode::While) {
      auto &p = std::get<WhilePayload>(op->payload);
      if (p.cond_region)
        initial_mark(*p.cond_region);
      if (p.loop_region)
        initial_mark(*p.loop_region);
    }
  }
}

auto SimpleDCE::collect_dead(Region &r) -> void {
  for (auto *op : r) {
    if (!liveset.count(op)) {
      rewriter.eraseOp(op);
    }
    if (op->code == OpCode::If) {
      auto &p = std::get<IfPayload>(op->payload);
      if (p.then_region)
        collect_dead(*p.then_region);
      if (p.else_region)
        collect_dead(*p.else_region);
    } else if (op->code == OpCode::While) {
      auto &p = std::get<WhilePayload>(op->payload);
      if (p.cond_region)
        collect_dead(*p.cond_region);
      if (p.loop_region)
        collect_dead(*p.loop_region);
    }
  }
}

auto SimpleDCE::run(Function &f, FunctionAnalysisManager & /* FAM */)
  -> PreservedAnalysis {
  if (f.is_decl)
    return PreservedAnalysis::all();

  rewriter.clear();
  parents.clear();
  liveset.clear();
  escaped_allocas.clear();
  worklist.clear();

  collect_global_uses();
  build_parent_map(f.body);
  collect_escaped_allocas();
  initial_mark(f.body);

  while (!worklist.empty()) {
    Op *op = worklist.front();
    worklist.pop_front();

    for (auto *v : op->operands) {
      if (v && v->kind == ValueKind::OpResult) {
        mark(static_cast<Op *>(static_cast<OpResult *>(v)->creator));
      }
    }

    mark_memory_dependencies(op);

    if (auto it = parents.find(op); it != parents.end()) {
      Op *parent = it->second;
      mark(parent);
    }

    if (op->code == OpCode::While) {
      auto &p = std::get<WhilePayload>(op->payload);
      if (p.cond_region) {
        for (auto *cond_op : *p.cond_region) {
          if (cond_op->code == OpCode::Condition) {
            mark(cond_op);
          }
        }
      }
    }
  }

  collect_dead(f.body);

  if (rewriter.empty()) {
    return PreservedAnalysis::all();
  }

  Log::log_info("DCE removed {} ops in function {}", rewriter.size(), f.name);

  rewriter.finalize(f);
  return PreservedAnalysis::none();
}

} // namespace exodus::high_ir::opt
