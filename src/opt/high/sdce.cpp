#include "sdce.hpp"

namespace exodus::high_ir::opt {

SimpleDCE::SimpleDCE(Module * /* m */) {}

auto SimpleDCE::isIntrinsicallyLive(Op *op) -> bool {
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

auto SimpleDCE::buildParentMap(Region &r, Op *parent) -> void {
  for (auto *op : r) {
    parents[op] = parent;
    if (op->code == OpCode::If) {
      auto &p = std::get<IfPayload>(op->payload);
      if (p.then_region)
        buildParentMap(*p.then_region, op);
      if (p.else_region)
        buildParentMap(*p.else_region, op);
    } else if (op->code == OpCode::While) {
      auto &p = std::get<WhilePayload>(op->payload);
      if (p.cond_region)
        buildParentMap(*p.cond_region, op);
      if (p.loop_region)
        buildParentMap(*p.loop_region, op);
    }
  }
}

auto SimpleDCE::mark(Op *op) -> void {
  if (op && liveset.insert(op).second) {
    worklist.push_back(op);
  }
}

auto SimpleDCE::initialMark(Region &r) -> void {
  for (auto *op : r) {
    if (isIntrinsicallyLive(op)) {
      mark(op);
    }
    if (op->code == OpCode::If) {
      auto &p = std::get<IfPayload>(op->payload);
      if (p.then_region)
        initialMark(*p.then_region);
      if (p.else_region)
        initialMark(*p.else_region);
    } else if (op->code == OpCode::While) {
      auto &p = std::get<WhilePayload>(op->payload);
      if (p.cond_region)
        initialMark(*p.cond_region);
      if (p.loop_region)
        initialMark(*p.loop_region);
    }
  }
}

auto SimpleDCE::collectDead(Region &r) -> void {
  for (auto *op : r) {
    if (!liveset.count(op)) {
      rewriter.eraseOp(op);
    }
    if (op->code == OpCode::If) {
      auto &p = std::get<IfPayload>(op->payload);
      if (p.then_region)
        collectDead(*p.then_region);
      if (p.else_region)
        collectDead(*p.else_region);
    } else if (op->code == OpCode::While) {
      auto &p = std::get<WhilePayload>(op->payload);
      if (p.cond_region)
        collectDead(*p.cond_region);
      if (p.loop_region)
        collectDead(*p.loop_region);
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

  buildParentMap(f.body);
  initialMark(f.body);

  while (!worklist.empty()) {
    Op *op = worklist.front();
    worklist.pop_front();

    for (auto *v : op->operands) {
      if (v && v->kind == ValueKind::OpResult) {
        mark(static_cast<Op *>(static_cast<OpResult *>(v)->creator));
      }
    }

    if (op->code == OpCode::Load) {
      Value *ptr = op->operands[0];
      for (auto *user_base : ptr->users) {
        auto *user = static_cast<Op *>(user_base);
        if (user->code == OpCode::Store && user->operands[1] == ptr) {
          mark(user);
        }
      }
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

  collectDead(f.body);

  if (rewriter.empty()) {
    return PreservedAnalysis::all();
  }

  Log::log_info("DCE removed {} ops in function {}", rewriter.size(), f.name);

  rewriter.finalize(f);
  return PreservedAnalysis::none();
}

} // namespace exodus::high_ir::opt