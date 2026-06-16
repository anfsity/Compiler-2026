#include "sdce.hpp"

namespace exodus::high_ir::opt {

SimpleDCE::SimpleDCE(Module * /* m */) {}

auto SimpleDCE::get_addr_root(Value *v) -> Value * {
  while (v && v->kind == ValueKind::OpResult) {
    auto *creator = static_cast<Op *>(static_cast<OpResult *>(v)->creator);
    if (
      !creator || creator->code != OpCode::GetPtr || creator->operands.empty()
    ) {
      break;
    }
    v = creator->operands[0];
  }
  return v;
}

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

auto SimpleDCE::mark_implicit_get_ptr_stores(Op *op) -> void {
  if (op->code != OpCode::GetPtr || !op->result || op->operands.empty()) {
    return;
  }

  auto plan = ir::analyze_getptr(
    op->operands[0]->type, op->result->type, op->operands.size() - 1
  );
  if (plan.reads_memory) {
    mark_stores_to(op->operands[0]);
  }
}

auto SimpleDCE::is_intrinsically_live(Op *op) -> bool {
  switch (op->code) {
  case OpCode::Ret:
  case OpCode::Call:
  case OpCode::Break:
  case OpCode::Continue:
  case OpCode::Memset:
  case OpCode::Jump:
  case OpCode::Branch:
    return true;
  case OpCode::Store: {
    Value *ptr = op->operands[1];
    if (ptr->kind == ValueKind::GlobalVar)
      return true;
    if (ptr->kind == ValueKind::OpResult) {
      auto *creator = static_cast<Op *>(static_cast<OpResult *>(ptr)->creator);
      if (creator && creator->code != OpCode::Alloca)
        return true;
      return false;
    }
    return true;
  }
  // case OpCode::Condition:
  // Condition operations in While loops are tricky.
  // If the While loop container itself is marked live (due to something
  // inside its body), then its Condition must also be marked live.
  // So we handle this in the worklist loop.
  default:
    return false;
  }
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
  worklist.clear();

  build_parent_map(f.body);
  initial_mark(f.body);

  while (!worklist.empty()) {
    Op *op = worklist.front();
    worklist.pop_front();

    for (auto *v : op->operands) {
      if (v && v->kind == ValueKind::OpResult) {
        mark(static_cast<Op *>(static_cast<OpResult *>(v)->creator));
      }
    }

    if (op->code == OpCode::Load) {
      mark_stores_to(op->operands[0]);
    }

    if (op->code == OpCode::GetPtr) {
      mark_implicit_get_ptr_stores(op);
    }

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
