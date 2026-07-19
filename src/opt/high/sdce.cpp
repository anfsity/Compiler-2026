#include "sdce.hpp"

namespace exodus::high_ir::opt {

SimpleDCE::SimpleDCE(Module * /* m */) {}

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

auto SimpleDCE::mark_stores_to(Value *ptr) -> void {
  Value *root = get_addr_root(ptr);
  for (auto &[user, _] : parents) {
    if (
      user->code == OpCode::Store && get_addr_root(user->operands[1]) == root
    ) {
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

auto SimpleDCE::is_intrinsically_live(Op *op) -> bool {
  if (op->code == OpCode::If || op->code == OpCode::While)
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
