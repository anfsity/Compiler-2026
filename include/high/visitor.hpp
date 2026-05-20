#pragma once

#include "ir.hpp"

namespace exodus::high_ir {

template <OpCode Code>
struct OpTag {};

template <typename Derived>
struct RecursiveOpVisitor {

  void visit(Module &m) {
    for (auto &g : m.globals) {
      static_cast<Derived *>(this)->visit(*g);
    }
    for (auto &f : m.functions) {
      if (!f->is_decl) {
        static_cast<Derived *>(this)->visit(*f);
      }
    }
  }

  void visit(GlobalVar &) {}

  void visit(Function &f) { static_cast<Derived *>(this)->visit(f.body); }

  void visit(Region &r) {
    for (auto *op : r) {
      static_cast<Derived *>(this)->visit(op);
    }
  }

  void visit(Op *op) {
    switch (op->code) {
      // clang-format off
      case OpCode::Add:       static_cast<Derived *>(this)->visit(op, OpTag<OpCode::Add>{}); break;
      case OpCode::Sub:       static_cast<Derived *>(this)->visit(op, OpTag<OpCode::Sub>{}); break;
      case OpCode::Mul:       static_cast<Derived *>(this)->visit(op, OpTag<OpCode::Mul>{}); break;
      case OpCode::Div:       static_cast<Derived *>(this)->visit(op, OpTag<OpCode::Div>{}); break;
      case OpCode::Mod:       static_cast<Derived *>(this)->visit(op, OpTag<OpCode::Mod>{}); break;
      case OpCode::FAdd:      static_cast<Derived *>(this)->visit(op, OpTag<OpCode::FAdd>{}); break;
      case OpCode::FSub:      static_cast<Derived *>(this)->visit(op, OpTag<OpCode::FSub>{}); break;
      case OpCode::FMul:      static_cast<Derived *>(this)->visit(op, OpTag<OpCode::FMul>{}); break;
      case OpCode::FDiv:      static_cast<Derived *>(this)->visit(op, OpTag<OpCode::FDiv>{}); break;
      case OpCode::I2F:       static_cast<Derived *>(this)->visit(op, OpTag<OpCode::I2F>{}); break;
      case OpCode::F2I:       static_cast<Derived *>(this)->visit(op, OpTag<OpCode::F2I>{}); break;
      case OpCode::ZExt:      static_cast<Derived *>(this)->visit(op, OpTag<OpCode::ZExt>{}); break;
      case OpCode::Eq:        static_cast<Derived *>(this)->visit(op, OpTag<OpCode::Eq>{}); break;
      case OpCode::Ne:        static_cast<Derived *>(this)->visit(op, OpTag<OpCode::Ne>{}); break;
      case OpCode::Lt:        static_cast<Derived *>(this)->visit(op, OpTag<OpCode::Lt>{}); break;
      case OpCode::Gt:        static_cast<Derived *>(this)->visit(op, OpTag<OpCode::Gt>{}); break;
      case OpCode::Le:        static_cast<Derived *>(this)->visit(op, OpTag<OpCode::Le>{}); break;
      case OpCode::Ge:        static_cast<Derived *>(this)->visit(op, OpTag<OpCode::Ge>{}); break;
      case OpCode::And:       static_cast<Derived *>(this)->visit(op, OpTag<OpCode::And>{}); break;
      case OpCode::Or:        static_cast<Derived *>(this)->visit(op, OpTag<OpCode::Or>{}); break;
      case OpCode::Xor:       static_cast<Derived *>(this)->visit(op, OpTag<OpCode::Xor>{}); break;
      case OpCode::Shl:       static_cast<Derived *>(this)->visit(op, OpTag<OpCode::Shl>{}); break;
      case OpCode::Shr:       static_cast<Derived *>(this)->visit(op, OpTag<OpCode::Shr>{}); break;
      case OpCode::Alloca:    static_cast<Derived *>(this)->visit(op, OpTag<OpCode::Alloca>{}); break;
      case OpCode::Load:      static_cast<Derived *>(this)->visit(op, OpTag<OpCode::Load>{}); break;
      case OpCode::Store:     static_cast<Derived *>(this)->visit(op, OpTag<OpCode::Store>{}); break;
      case OpCode::GetPtr:    static_cast<Derived *>(this)->visit(op, OpTag<OpCode::GetPtr>{}); break;
      case OpCode::Call:      static_cast<Derived *>(this)->visit(op, OpTag<OpCode::Call>{}); break;
      case OpCode::Ret:       static_cast<Derived *>(this)->visit(op, OpTag<OpCode::Ret>{}); break;
      case OpCode::If:        static_cast<Derived *>(this)->visit(op, OpTag<OpCode::If>{}); break;
      case OpCode::While:     static_cast<Derived *>(this)->visit(op, OpTag<OpCode::While>{}); break;
      case OpCode::Break:     static_cast<Derived *>(this)->visit(op, OpTag<OpCode::Break>{}); break;
      case OpCode::Continue:  static_cast<Derived *>(this)->visit(op, OpTag<OpCode::Continue>{}); break;
      case OpCode::Condition: static_cast<Derived *>(this)->visit(op, OpTag<OpCode::Condition>{}); break;
      case OpCode::Jump:      static_cast<Derived *>(this)->visit(op, OpTag<OpCode::Jump>{}); break;
      case OpCode::Branch:    static_cast<Derived *>(this)->visit(op, OpTag<OpCode::Branch>{}); break;
      // clang-format on
    }
  }

  template <OpCode Code>
  void visit(Op * /* op */, OpTag<Code>) {}

  void visit(Op *op, OpTag<OpCode::If>) {
    auto &p = std::get<IfPayload>(op->payload);
    static_cast<Derived *>(this)->visit(*p.then_region);
    if (p.else_region) {
      static_cast<Derived *>(this)->visit(*p.else_region);
    }
  }

  void visit(Op *op, OpTag<OpCode::While>) {
    auto &p = std::get<WhilePayload>(op->payload);
    static_cast<Derived *>(this)->visit(*p.cond_region);
    static_cast<Derived *>(this)->visit(*p.loop_region);
  }
};

} // namespace exodus::high_ir
