#pragma once

#include "../../high/ir.hpp"
#include "../../high/visitor.hpp"
#include <algorithm>
#include <vector>

namespace exodus::opt {

using namespace exodus::high_ir;

struct CostModel : RecursiveOpVisitor<CostModel> {
  int cost = 0;
  const std::vector<Value *> *call_args = nullptr;
  const Function *func = nullptr;

  CostModel(
    const Function *f = nullptr, const std::vector<Value *> *args = nullptr
  )
      : call_args(args), func(f) {}

  using RecursiveOpVisitor<CostModel>::visit;

  bool will_be_const(Value *v) const {
    if (!v)
      return true;
    if (v->kind == ValueKind::Constant)
      return true;
    if (v->kind == ValueKind::Argument && call_args && func) {
      auto *arg = static_cast<Argument *>(v);
      if (
        arg->idx < (int)call_args->size() &&
        (*call_args)[arg->idx]->kind == ValueKind::Constant
      ) {
        return true;
      }
    }
    return false;
  }

  void check_fold(Op *op, int base_cost) {
    bool all_const = true;
    for (auto *operand : op->operands) {
      if (!will_be_const(operand)) {
        all_const = false;
        break;
      }
    }
    if (all_const && !op->operands.empty())
      cost += 0;
    else
      cost += base_cost;
  }

  void visit(Op *op, OpTag<OpCode::Add>) { check_fold(op, 5); }
  void visit(Op *op, OpTag<OpCode::Sub>) { check_fold(op, 5); }
  void visit(Op *op, OpTag<OpCode::Mul>) { check_fold(op, 15); }
  void visit(Op *op, OpTag<OpCode::Div>) { check_fold(op, 15); }
  void visit(Op *op, OpTag<OpCode::Mod>) { check_fold(op, 15); }
  void visit(Op *op, OpTag<OpCode::FAdd>) { check_fold(op, 5); }
  void visit(Op *op, OpTag<OpCode::FSub>) { check_fold(op, 5); }
  void visit(Op *op, OpTag<OpCode::FMul>) { check_fold(op, 15); }
  void visit(Op *op, OpTag<OpCode::FDiv>) { check_fold(op, 15); }

  void visit(Op *, OpTag<OpCode::Alloca>) { cost += 5; }
  void visit(Op *, OpTag<OpCode::Load>) { cost += 5; }
  void visit(Op *, OpTag<OpCode::Store>) { cost += 5; }
  void visit(Op *, OpTag<OpCode::GetPtr>) { cost += 5; }

  void visit(Op *, OpTag<OpCode::Call>) { cost += 25; }
  void visit(Op *, OpTag<OpCode::Ret>) { cost += 5; }

  void visit(Op *op, OpTag<OpCode::If>) {
    cost += 10;
    RecursiveOpVisitor<CostModel>::visit(op, OpTag<OpCode::If>{});
  }

  void visit(Op *op, OpTag<OpCode::While>) {
    cost += 10;
    RecursiveOpVisitor<CostModel>::visit(op, OpTag<OpCode::While>{});
  }

  void visit(Op *, OpTag<OpCode::ZExt>) { cost += 5; }
  void visit(Op *, OpTag<OpCode::I2F>) { cost += 5; }
  void visit(Op *, OpTag<OpCode::F2I>) { cost += 5; }

  static int
  calculate(Function &f, const std::vector<Value *> *args = nullptr) {
    CostModel cm(&f, args);
    cm.visit(f);

    // SROA bonus: if function has small allocas
    for (auto *op : f.body) {
      if (op->code == OpCode::Alloca)
        cm.cost -= 15;
    }

    return std::max(0, cm.cost);
  }
};

} // namespace exodus::opt
