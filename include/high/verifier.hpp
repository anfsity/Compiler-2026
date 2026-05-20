#pragma once

#include "../helper/log.hpp"
#include "visitor.hpp"
#include <algorithm>

namespace exodus::high_ir {

struct Verifier : RecursiveOpVisitor<Verifier> {

  int depth = 0;
  bool in_cdregion = false;
  Function *cur_func = nullptr;
  bool has_error = false;

  using RecursiveOpVisitor<Verifier>::visit;

  template <typename... Args>
  auto report_error(fmt::format_string<Args...> fmt_str, Args... args) -> void {
    exodus::Log::log_error(fmt_str, std::forward<Args>(args)...);
    has_error = true;
  }

  auto has_null_operand(const Op *op) const -> bool {
    return std::any_of(
      op->operands.begin(), op->operands.end(), [](const auto *operand) {
        return operand == nullptr;
      }
    );
  }

  auto is_numeric_type(const std::shared_ptr<Type> &type) const -> bool {
    return type->is_i32() || type->is_f32();
  }

  auto verify(Module &m) -> bool {
    has_error = false;
    RecursiveOpVisitor<Verifier>::visit(m);
    return !has_error;
  }

  auto verify(Function &f) -> bool {
    has_error = false;
    visit(f);
    return !has_error;
  }

  auto visit(Function &f) -> void {
    depth = 0;
    cur_func = &f;
    in_cdregion = false;
    RecursiveOpVisitor<Verifier>::visit(f);
    cur_func = nullptr;
  }

  void visit(Op *op) {
    for (auto *operand : op->operands) {
      if (!operand) {
        report_error("Null operand found in instruction");
        continue;
      }

      auto it = std::find(operand->users.begin(), operand->users.end(), op);
      if (it == operand->users.end()) {
        report_error(
          "Use-Def chain broken: user not found in value's user list for {}",
          // FIXME: 也许这个打印不够清晰
          operand->dump()
        );
      }
    }

    if (op->result && op->result->creator != op) {
      report_error("OpResult creator mismatch for {}", op->result->dump());
    }

    RecursiveOpVisitor<Verifier>::visit(op);
  }

  auto visit(Op *op, OpTag<OpCode::While>) -> void {
    auto &p = std::get<WhilePayload>(op->payload);

    bool old_cond = in_cdregion;
    in_cdregion = true;
    RecursiveOpVisitor<Verifier>::visit(*p.cond_region);
    in_cdregion = old_cond;

    depth++;
    RecursiveOpVisitor<Verifier>::visit(*p.loop_region);
    depth--;
  }

  auto visit(Op *op, OpTag<OpCode::If>) -> void {
    if (op->operands.size() != 1u) {
      report_error("'if' expects exactly 1 operand (condition)");

    } else if (!has_null_operand(op) && !op->operands[0]->type->is_bool()) {
      report_error("'if' condition must be i1");
    }
    RecursiveOpVisitor<Verifier>::visit(op, OpTag<OpCode::If>{});
  }

  void visit(Op * /* op */, OpTag<OpCode::Break>) {
    if (depth == 0)
      report_error("'break' instruction outside of loop");
  }

  void visit(Op * /* op */, OpTag<OpCode::Continue>) {
    if (depth == 0)
      report_error("'continue' instruction outside of loop");
  }

  void visit(Op *op, OpTag<OpCode::Condition>) {
    if (!in_cdregion)
      report_error("'condition' instruction outside of while cond region");

    if (op->operands.size() != 1u) {
      report_error("'condition' expects exactly 1 operand");

    } else if (!has_null_operand(op) && !op->operands[0]->type->is_bool()) {
      report_error("'condition' operand must be i1");
    }
  }

  enum class BinaryType : uint8_t { I32, F32 };

  template <OpCode Code>
  void check_binary(Op *op, const std::string &name, BinaryType expected) {

    if (op->operands.size() != 2u) {
      report_error("{} expects 2 operands", name);
      return;
    }

    if (has_null_operand(op)) {
      return;
    }

    auto t0 = op->operands[0]->type;
    auto t1 = op->operands[1]->type;

    if (t0 != t1) {
      report_error(
        "{} operands type mismatch: {} and {}",
        name,
        t0->to_string(),
        t1->to_string()
      );
    }

    if (expected == BinaryType::I32 && !t0->is_i32()) {
      report_error("{} operands must be i32", name);
    }

    if (expected == BinaryType::F32 && !t0->is_f32()) {
      report_error("{} operands must be f32", name);
    }

    if (!op->result || op->result->type != t0) {
      report_error("{} result type mismatch", name);
    }
  }

  void visit(Op *op, OpTag<OpCode::Add>) {
    check_binary<OpCode::Add>(op, "add", BinaryType::I32);
  }
  void visit(Op *op, OpTag<OpCode::Sub>) {
    check_binary<OpCode::Sub>(op, "sub", BinaryType::I32);
  }
  void visit(Op *op, OpTag<OpCode::Mul>) {
    check_binary<OpCode::Mul>(op, "mul", BinaryType::I32);
  }
  void visit(Op *op, OpTag<OpCode::Div>) {
    check_binary<OpCode::Div>(op, "div", BinaryType::I32);
  }
  void visit(Op *op, OpTag<OpCode::Mod>) {
    check_binary<OpCode::Mod>(op, "mod", BinaryType::I32);
  }
  void visit(Op *op, OpTag<OpCode::FAdd>) {
    check_binary<OpCode::FAdd>(op, "fadd", BinaryType::F32);
  }
  void visit(Op *op, OpTag<OpCode::FSub>) {
    check_binary<OpCode::FSub>(op, "fsub", BinaryType::F32);
  }
  void visit(Op *op, OpTag<OpCode::FMul>) {
    check_binary<OpCode::FMul>(op, "fmul", BinaryType::F32);
  }
  void visit(Op *op, OpTag<OpCode::FDiv>) {
    check_binary<OpCode::FDiv>(op, "fdiv", BinaryType::F32);
  }

  enum class CompareType : uint8_t { Eq, Ord };

  template <OpCode Code>
  void check_cmp(Op *op, const std::string &name, CompareType expected) {
    if (op->operands.size() != 2u) {
      report_error("{} expects 2 operands", name);
      return;
    }

    if (has_null_operand(op)) {
      return;
    }

    if (op->operands[0]->type != op->operands[1]->type) {
      report_error("{} operands type mismatch", name);
    }

    auto operand_type = op->operands[0]->type;
    if (expected == CompareType::Ord) {
      if (!is_numeric_type(operand_type)) {
        report_error("{} operands must be i32 or f32", name);
      }
    } else if (!is_numeric_type(operand_type) && !operand_type->is_bool()) {
      report_error("{} operands must be i32, f32, or i1", name);
    }

    if (!op->result || !op->result->type->is_bool()) {
      report_error("{} result type must be i1 (bool)", name);
    }
  }

  void visit(Op *op, OpTag<OpCode::Eq>) {
    check_cmp<OpCode::Eq>(op, "eq", CompareType::Eq);
  }
  void visit(Op *op, OpTag<OpCode::Ne>) {
    check_cmp<OpCode::Ne>(op, "ne", CompareType::Eq);
  }
  void visit(Op *op, OpTag<OpCode::Lt>) {
    check_cmp<OpCode::Lt>(op, "lt", CompareType::Ord);
  }
  void visit(Op *op, OpTag<OpCode::Gt>) {
    check_cmp<OpCode::Gt>(op, "gt", CompareType::Ord);
  }
  void visit(Op *op, OpTag<OpCode::Le>) {
    check_cmp<OpCode::Le>(op, "le", CompareType::Ord);
  }
  void visit(Op *op, OpTag<OpCode::Ge>) {
    check_cmp<OpCode::Ge>(op, "ge", CompareType::Ord);
  }

  template <OpCode Code>
  void check_bitwise(Op *op, const std::string &name) {
    if (op->operands.size() != 2u) {
      report_error("{} expects 2 operands", name);
      return;
    }

    if (has_null_operand(op)) {
      return;
    }

    if (!op->operands[0]->type->is_i32() || !op->operands[1]->type->is_i32()) {
      report_error("{} operands must be i32", name);
    }

    if (!op->result || !op->result->type->is_i32()) {
      report_error("{} result must be i32", name);
    }
  }

  // clang-format off
  void visit(Op *op, OpTag<OpCode::And>) {
    check_bitwise<OpCode::And>(op, "and");
  }
  void visit(Op *op, OpTag<OpCode::Or>) { 
    check_bitwise<OpCode::Or>(op, "or"); 
  }
  void visit(Op *op, OpTag<OpCode::Xor>) {
    check_bitwise<OpCode::Xor>(op, "xor");
  }
  void visit(Op *op, OpTag<OpCode::Shl>) {
    check_bitwise<OpCode::Shl>(op, "shl");
  }
  void visit(Op *op, OpTag<OpCode::Shr>) {
    check_bitwise<OpCode::Shr>(op, "shr");
  }
  // clang-format on

  void visit(Op *op, OpTag<OpCode::I2F>) {
    if (op->operands.size() != 1u) {
      report_error("i2f expects 1 i32 operand");
      return;
    }

    if (has_null_operand(op)) {
      return;
    }

    if (!op->operands[0]->type->is_i32())
      report_error("i2f expects 1 i32 operand");

    if (!op->result || !op->result->type->is_f32())
      report_error("i2f result must be f32");
  }

  void visit(Op *op, OpTag<OpCode::F2I>) {
    if (op->operands.size() != 1u) {
      report_error("f2i expects 1 f32 operand");
      return;
    }

    if (has_null_operand(op)) {
      return;
    }

    if (!op->operands[0]->type->is_f32())
      report_error("f2i expects 1 f32 operand");

    if (!op->result || !op->result->type->is_i32())
      report_error("f2i result must be i32");
  }

  void visit(Op *op, OpTag<OpCode::ZExt>) {
    if (op->operands.size() != 1u) {
      report_error("zext expects 1 i1 operand");
      return;
    }

    if (has_null_operand(op)) {
      return;
    }

    if (!op->operands[0]->type->is_bool())
      report_error("zext expects 1 i1 operand");

    if (!op->result || !op->result->type->is_i32())
      report_error("zext result must be i32");
  }

  void visit(Op *op, OpTag<OpCode::Alloca>) {
    if (!op->result || !op->result->type->is_ptr())
      report_error("alloca result must be a pointer");
  }

  void visit(Op *op, OpTag<OpCode::Load>) {
    if (op->operands.size() != 1u) {
      report_error("load expects 1 operand");
      return;
    }

    if (has_null_operand(op)) {
      return;
    }

    auto ptr_type = op->operands[0]->type;
    if (!ptr_type->is_ptr()) {
      report_error("load operand must be a pointer");
      return;
    }

    auto base_type = std::static_pointer_cast<exodus::Ptr>(ptr_type)->target;
    if (!op->result || op->result->type != base_type) {
      report_error("load result type mismatch");
    }
  }

  void visit(Op *op, OpTag<OpCode::Store>) {
    if (op->operands.size() != 2u) {
      report_error("store expects 2 operands");
      return;
    }

    if (has_null_operand(op)) {
      return;
    }

    auto val_type = op->operands[0]->type;
    auto ptr_type = op->operands[1]->type;
    if (!ptr_type->is_ptr()) {
      report_error("store target must be a pointer");
      return;
    }

    auto base_type = std::static_pointer_cast<exodus::Ptr>(ptr_type)->target;
    if (val_type != base_type) {
      report_error("store value type does not match pointer base type");
    }
  }

  void visit(Op *op, OpTag<OpCode::GetPtr>) {
    if (op->operands.size() < 2u) {
      report_error("getptr expects at least 2 operands");
      return;
    }

    if (has_null_operand(op)) {
      return;
    }

    auto ptr_type = op->operands[0]->type;
    if (!ptr_type->is_ptr()) {
      report_error("getptr first operand must be a pointer");
      return;
    }

    for (size_t i = 1; i < op->operands.size(); ++i) {
      if (!op->operands[i]->type->is_i32())
        report_error("getptr indices must be i32");
    }

    if (!op->result || !op->result->type->is_ptr())
      report_error("getptr result must be a pointer");
  }

  void visit(Op *op, OpTag<OpCode::Ret>) {
    if (!cur_func)
      return;

    auto func_type = std::static_pointer_cast<exodus::Func>(cur_func->type);
    auto ret_type = func_type->ret_type;

    if (ret_type->is_void()) {
      if (!op->operands.empty())
        report_error("void function should not return a value");

    } else {
      if (op->operands.size() != 1u) {
        report_error("non-void function must return exactly 1 value");

      } else if (!has_null_operand(op) && op->operands[0]->type != ret_type) {
        report_error(
          "return type mismatch: expected {}, got {}",
          ret_type->to_string(),
          op->operands[0]->type->to_string()
        );
      }
    }
  }

  void visit(Op *op, OpTag<OpCode::Call>) {
    if (op->result && op->result->type->is_void()) {
      report_error("call result cannot be void");
    }
  }
};

} // namespace exodus::high_ir
