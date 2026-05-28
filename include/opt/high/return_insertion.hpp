#pragma once

#include "../../high/ir.hpp"
#include "../../high/visitor.hpp"
#include "../AnalysisManager.hpp"

namespace exodus::opt {

using namespace exodus::high_ir;

struct ReturnInsertion {
  Module *m;
  ReturnInsertion(Module *_m) : m(_m) {}

  auto run(Function &f, FunctionAnalysisManager & /* FAM */)
    -> PreservedAnalysis {
    if (f.is_decl)
      return PreservedAnalysis::all();

    bool has_terminator = false;
    if (!f.body.empty()) {
      auto last_code = f.body.back()->code;
      if (
        last_code == OpCode::Ret || last_code == OpCode::Break ||
        last_code == OpCode::Continue
      ) {
        has_terminator = true;
      }
    }

    if (!has_terminator) {
      auto func_type = std::static_pointer_cast<Func>(f.type);
      IRContext &ctx = m->ctx;
      Op *ret_op = ctx.make_op(OpCode::Ret);

      if (f.name == "main") {
        auto *zero = ctx.make_zero(I32::get());
        ret_op->operands.push_back(zero);
        zero->addUse(ret_op);
      } else if (!func_type->ret_type->is_void()) {
        auto *zero = ctx.make_zero(func_type->ret_type);
        ret_op->operands.push_back(zero);
        zero->addUse(ret_op);
      }

      f.body.push_back(ret_op);
      return PreservedAnalysis::none();
    }

    return PreservedAnalysis::all();
  }
};

} // namespace exodus::opt
