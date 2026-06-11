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

  auto will_be_const(Value *v) const -> bool {
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

  auto check_fold(Op *op, int base_cost) -> void {
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

  auto visit(Op *op, OpTag<OpCode::Add>) -> void { check_fold(op, 5); }
  auto visit(Op *op, OpTag<OpCode::Sub>) -> void { check_fold(op, 5); }
  auto visit(Op *op, OpTag<OpCode::Mul>) -> void { check_fold(op, 15); }
  auto visit(Op *op, OpTag<OpCode::Div>) -> void { check_fold(op, 15); }
  auto visit(Op *op, OpTag<OpCode::Mod>) -> void { check_fold(op, 15); }
  auto visit(Op *op, OpTag<OpCode::FAdd>) -> void { check_fold(op, 5); }
  auto visit(Op *op, OpTag<OpCode::FSub>) -> void { check_fold(op, 5); }
  auto visit(Op *op, OpTag<OpCode::FMul>) -> void { check_fold(op, 15); }
  auto visit(Op *op, OpTag<OpCode::FDiv>) -> void { check_fold(op, 15); }

  auto visit(Op *, OpTag<OpCode::Alloca>) -> void { cost += 5; }
  auto visit(Op *, OpTag<OpCode::Load>) -> void { cost += 5; }
  auto visit(Op *, OpTag<OpCode::Store>) -> void { cost += 5; }
  auto visit(Op *, OpTag<OpCode::GetPtr>) -> void { cost += 5; }

  auto visit(Op *, OpTag<OpCode::Call>) -> void { cost += 25; }
  auto visit(Op *, OpTag<OpCode::Ret>) -> void { cost += 5; }

  auto visit(Op *op, OpTag<OpCode::If>) -> void {
    cost += 10;
    RecursiveOpVisitor<CostModel>::visit(op, OpTag<OpCode::If>{});
  }

  auto visit(Op *op, OpTag<OpCode::While>) -> void {
    cost += 10;
    RecursiveOpVisitor<CostModel>::visit(op, OpTag<OpCode::While>{});
  }

  auto visit(Op *, OpTag<OpCode::ZExt>) -> void { cost += 5; }
  auto visit(Op *, OpTag<OpCode::I2F>) -> void { cost += 5; }
  auto visit(Op *, OpTag<OpCode::F2I>) -> void { cost += 5; }

  static auto calculate(Function &f, const std::vector<Value *> *args = nullptr)
    -> int {
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
