#pragma once

#include "../helper/log.hpp"
#include "visitor.hpp"
#include <algorithm>

namespace exodus::high_ir {

struct Verifier : RecursiveOpVisitor<Verifier> {

  int depth = 0;
  bool in_cdregion = false;
  Function *cur_func = nullptr;
  Module *cur_module = nullptr;
  bool has_error = false;

  using RecursiveOpVisitor<Verifier>::visit;

  template <typename... Args>
  auto report_error(fmt::format_string<Args...> fmt_str, Args... args) // NOLINT
    -> void;

  auto has_null_operand(const Op *op) const -> bool;
  auto is_numeric_type(const std::shared_ptr<Type> &type) const -> bool;
  auto verify(Module &m) -> bool;
  auto verify(Function &f) -> bool;
  auto visit(Module &m) -> void;
  auto visit(Function &f) -> void;
  auto visit(Op *op) -> void;
  auto visit(Op *op, OpTag<OpCode::While>) -> void;
  auto visit(Op *op, OpTag<OpCode::If>) -> void;
  auto visit(Op * /* op */, OpTag<OpCode::Break>) -> void;
  auto visit(Op * /* op */, OpTag<OpCode::Continue>) -> void;
  auto visit(Op *op, OpTag<OpCode::Condition>) -> void;

  enum class BinaryType : uint8_t { I32, F32 };

  template <OpCode Code>
  auto check_binary(Op *op, const std::string &name, BinaryType expected)
    -> void;

  auto visit(Op *op, OpTag<OpCode::Add>) -> void;
  auto visit(Op *op, OpTag<OpCode::Sub>) -> void;
  auto visit(Op *op, OpTag<OpCode::Mul>) -> void;
  auto visit(Op *op, OpTag<OpCode::Div>) -> void;
  auto visit(Op *op, OpTag<OpCode::Mod>) -> void;
  auto visit(Op *op, OpTag<OpCode::FAdd>) -> void;
  auto visit(Op *op, OpTag<OpCode::FSub>) -> void;
  auto visit(Op *op, OpTag<OpCode::FMul>) -> void;
  auto visit(Op *op, OpTag<OpCode::FDiv>) -> void;

  enum class CompareType : uint8_t { Eq, Ord };

  template <OpCode Code>
  auto check_cmp(Op *op, const std::string &name, CompareType expected) -> void;

  auto visit(Op *op, OpTag<OpCode::Eq>) -> void;
  auto visit(Op *op, OpTag<OpCode::Ne>) -> void;
  auto visit(Op *op, OpTag<OpCode::Lt>) -> void;
  auto visit(Op *op, OpTag<OpCode::Gt>) -> void;
  auto visit(Op *op, OpTag<OpCode::Le>) -> void;
  auto visit(Op *op, OpTag<OpCode::Ge>) -> void;

  template <OpCode Code>
  auto check_bitwise(Op *op, const std::string &name) -> void;

  // clang-format off
  auto visit(Op *op, OpTag<OpCode::And>) -> void;
  auto visit(Op *op, OpTag<OpCode::Or>) -> void;
  auto visit(Op *op, OpTag<OpCode::Xor>) -> void;
  auto visit(Op *op, OpTag<OpCode::Shl>) -> void;
  auto visit(Op *op, OpTag<OpCode::Shr>) -> void;
  // clang-format on

  auto visit(Op *op, OpTag<OpCode::I2F>) -> void;
  auto visit(Op *op, OpTag<OpCode::F2I>) -> void;
  auto visit(Op *op, OpTag<OpCode::ZExt>) -> void;
  auto visit(Op *op, OpTag<OpCode::Memset>) -> void;
  auto visit(Op *op, OpTag<OpCode::Alloca>) -> void;
  auto visit(Op *op, OpTag<OpCode::Load>) -> void;
  auto visit(Op *op, OpTag<OpCode::Store>) -> void;
  auto visit(Op *op, OpTag<OpCode::GetPtr>) -> void;
  auto visit(Op *op, OpTag<OpCode::Call>) -> void;
  auto visit(Op *op, OpTag<OpCode::Ret>) -> void;
};

template <typename... Args>
inline auto
Verifier::report_error(fmt::format_string<Args...> fmt_str, Args... args)
  -> void {
  exodus::Log::log_error(fmt_str, std::forward<Args>(args)...);
  has_error = true;
}

inline auto Verifier::has_null_operand(const Op *op) const -> bool {
  return std::any_of(
    op->operands.begin(), op->operands.end(), [](const auto *operand) {
      return operand == nullptr;
    }
  );
}

inline auto Verifier::is_numeric_type(const std::shared_ptr<Type> &type) const
  -> bool {
  return type->is_i32() || type->is_f32();
}

inline auto Verifier::verify(Module &m) -> bool {
  has_error = false;
  visit(m);
  return !has_error;
}

inline auto Verifier::verify(Function &f) -> bool {
  has_error = false;
  cur_module = nullptr;
  visit(f);
  return !has_error;
}

inline auto Verifier::visit(Module &m) -> void {
  cur_module = &m;
  RecursiveOpVisitor<Verifier>::visit(m);
  cur_module = nullptr;
}

inline auto Verifier::visit(Function &f) -> void {
  depth = 0;
  cur_func = &f;
  in_cdregion = false;
  RecursiveOpVisitor<Verifier>::visit(f);
  cur_func = nullptr;
}

inline auto Verifier::visit(Op *op) -> void {
  for (auto *operand : op->operands) {
    if (!operand) {
      report_error("Null operand found in instruction");
      continue;
    }
    auto it = std::find(operand->users.begin(), operand->users.end(), op);
    if (it == operand->users.end()) {
      report_error("Use-Def chain broken for {}", operand->dump());
    }
  }

  if (op->result && op->result->creator != op) {
    report_error("OpResult creator mismatch for {}", op->result->dump());
  }

  RecursiveOpVisitor<Verifier>::visit(op);
}

inline auto Verifier::visit(Op *op, OpTag<OpCode::While>) -> void {
  auto &p = std::get<WhilePayload>(op->payload);

  bool old_cond = in_cdregion;
  in_cdregion = true;
  visit(*p.cond_region);
  in_cdregion = old_cond;

  depth++;
  visit(*p.loop_region);
  depth--;
}

inline auto Verifier::visit(Op *op, OpTag<OpCode::If>) -> void {
  if (op->operands.size() != 1u) {
    report_error("'if' expects exactly 1 operand (condition)");

  } else if (!has_null_operand(op) && !op->operands[0]->type->is_bool()) {
    report_error("'if' condition must be i1");
  }
  RecursiveOpVisitor<Verifier>::visit(op, OpTag<OpCode::If>{});
}

inline auto Verifier::visit(Op * /* op */, OpTag<OpCode::Break>) -> void {
  if (depth == 0)
    report_error("'break' instruction outside of loop");
}

inline auto Verifier::visit(Op * /* op */, OpTag<OpCode::Continue>) -> void {
  if (depth == 0)
    report_error("'continue' instruction outside of loop");
}

inline auto Verifier::visit(Op *op, OpTag<OpCode::Condition>) -> void {
  if (!in_cdregion)
    report_error("'condition' instruction outside of while cond region");

  if (op->operands.size() != 1u) {
    report_error("'condition' expects exactly 1 operand");

  } else if (!has_null_operand(op) && !op->operands[0]->type->is_bool()) {
    report_error("'condition' operand must be i1");
  }
}

template <OpCode Code>
inline auto
Verifier::check_binary(Op *op, const std::string &name, BinaryType expected)
  -> void {

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

inline auto Verifier::visit(Op *op, OpTag<OpCode::Add>) -> void {
  check_binary<OpCode::Add>(op, "add", BinaryType::I32);
}

inline auto Verifier::visit(Op *op, OpTag<OpCode::Sub>) -> void {
  check_binary<OpCode::Sub>(op, "sub", BinaryType::I32);
}

inline auto Verifier::visit(Op *op, OpTag<OpCode::Mul>) -> void {
  check_binary<OpCode::Mul>(op, "mul", BinaryType::I32);
}

inline auto Verifier::visit(Op *op, OpTag<OpCode::Div>) -> void {
  check_binary<OpCode::Div>(op, "div", BinaryType::I32);
}

inline auto Verifier::visit(Op *op, OpTag<OpCode::Mod>) -> void {
  check_binary<OpCode::Mod>(op, "mod", BinaryType::I32);
}

inline auto Verifier::visit(Op *op, OpTag<OpCode::FAdd>) -> void {
  check_binary<OpCode::FAdd>(op, "fadd", BinaryType::F32);
}

inline auto Verifier::visit(Op *op, OpTag<OpCode::FSub>) -> void {
  check_binary<OpCode::FSub>(op, "fsub", BinaryType::F32);
}

inline auto Verifier::visit(Op *op, OpTag<OpCode::FMul>) -> void {
  check_binary<OpCode::FMul>(op, "fmul", BinaryType::F32);
}

inline auto Verifier::visit(Op *op, OpTag<OpCode::FDiv>) -> void {
  check_binary<OpCode::FDiv>(op, "fdiv", BinaryType::F32);
}

template <OpCode Code>
inline auto
Verifier::check_cmp(Op *op, const std::string &name, CompareType expected)
  -> void {
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

inline auto Verifier::visit(Op *op, OpTag<OpCode::Eq>) -> void {
  check_cmp<OpCode::Eq>(op, "eq", CompareType::Eq);
}

inline auto Verifier::visit(Op *op, OpTag<OpCode::Ne>) -> void {
  check_cmp<OpCode::Ne>(op, "ne", CompareType::Eq);
}

inline auto Verifier::visit(Op *op, OpTag<OpCode::Lt>) -> void {
  check_cmp<OpCode::Lt>(op, "lt", CompareType::Ord);
}

inline auto Verifier::visit(Op *op, OpTag<OpCode::Gt>) -> void {
  check_cmp<OpCode::Gt>(op, "gt", CompareType::Ord);
}

inline auto Verifier::visit(Op *op, OpTag<OpCode::Le>) -> void {
  check_cmp<OpCode::Le>(op, "le", CompareType::Ord);
}

inline auto Verifier::visit(Op *op, OpTag<OpCode::Ge>) -> void {
  check_cmp<OpCode::Ge>(op, "ge", CompareType::Ord);
}

template <OpCode Code>
inline auto Verifier::check_bitwise(Op *op, const std::string &name) -> void {
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
inline auto Verifier::visit(Op *op, OpTag<OpCode::And>) -> void {
  check_bitwise<OpCode::And>(op, "and");
}
inline auto Verifier::visit(Op *op, OpTag<OpCode::Or>) -> void { 
  check_bitwise<OpCode::Or>(op, "or"); 
}
inline auto Verifier::visit(Op *op, OpTag<OpCode::Xor>) -> void {
  check_bitwise<OpCode::Xor>(op, "xor");
}
inline auto Verifier::visit(Op *op, OpTag<OpCode::Shl>) -> void {
  check_bitwise<OpCode::Shl>(op, "shl");
}
inline auto Verifier::visit(Op *op, OpTag<OpCode::Shr>) -> void {
  check_bitwise<OpCode::Shr>(op, "shr");
}
// clang-format on

inline auto Verifier::visit(Op *op, OpTag<OpCode::I2F>) -> void {
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

inline auto Verifier::visit(Op *op, OpTag<OpCode::F2I>) -> void {
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

inline auto Verifier::visit(Op *op, OpTag<OpCode::ZExt>) -> void {
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

inline auto Verifier::visit(Op *op, OpTag<OpCode::Memset>) -> void {
  if (op->operands.size() != 3u) {
    report_error("memset expects 3 operands (ptr, count, value)");
    return;
  }

  if (has_null_operand(op)) {
    return;
  }

  if (!op->operands[0]->type->is_ptr()) {
    report_error("memset first operand must be a pointer");
  }
  if (!op->operands[1]->type->is_i32()) {
    report_error("memset second operand (count) must be i32");
  }
  if (!op->operands[2]->type->is_i32() && !op->operands[2]->type->is_f32()) {
    report_error("memset third operand (value) must be i32 or f32");
  }
}

inline auto Verifier::visit(Op *op, OpTag<OpCode::Alloca>) -> void {
  if (!op->result || !op->result->type->is_ptr())
    report_error("alloca result must be a pointer");
}

inline auto Verifier::visit(Op *op, OpTag<OpCode::Load>) -> void {
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

inline auto Verifier::visit(Op *op, OpTag<OpCode::Store>) -> void {
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

inline auto Verifier::visit(Op *op, OpTag<OpCode::GetPtr>) -> void {
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

inline auto Verifier::visit(Op *op, OpTag<OpCode::Call>) -> void {
  if (!cur_module) {
    return;
  }

  auto &payload = std::get<CallPayload>(op->payload);
  Function *callee = nullptr;
  for (auto &f : cur_module->functions) {
    if (f->name == payload.func_name) {
      callee = f.get();
      break;
    }
  }

  if (!callee) {
    report_error("call to undefined function '{}'", payload.func_name);
    return;
  }

  if (!callee->type || !callee->type->is_func()) {
    report_error("call target '{}' is not a function", payload.func_name);
    return;
  }

  auto func_type = std::static_pointer_cast<exodus::Func>(callee->type);
  if (op->operands.size() != func_type->params.size()) {
    report_error(
      "call '{}' expects {} args, got {}",
      payload.func_name,
      func_type->params.size(),
      op->operands.size()
    );
  }

  if (has_null_operand(op)) {
    return;
  }

  auto arg_count = std::min(op->operands.size(), func_type->params.size());
  for (size_t i = 0; i < arg_count; ++i) {
    auto expected = func_type->params[i];
    auto actual = op->operands[i]->type;
    if (expected != actual) {
      report_error(
        "call '{}' arg {} type mismatch: expected {}, got {}",
        payload.func_name,
        i,
        expected->to_string(),
        actual->to_string()
      );
    }
  }

  if (func_type->ret_type->is_void()) {
    if (op->result && !op->result->type->is_void()) {
      report_error("call '{}' result must be void", payload.func_name);
    }
  } else if (!op->result) {
    report_error(
      "call '{}' must return {}",
      payload.func_name,
      func_type->ret_type->to_string()
    );
  } else if (op->result->type != func_type->ret_type) {
    report_error(
      "call '{}' result type mismatch: expected {}, got {}",
      payload.func_name,
      func_type->ret_type->to_string(),
      op->result->type->to_string()
    );
  }
}

inline auto Verifier::visit(Op *op, OpTag<OpCode::Ret>) -> void {
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

} // namespace exodus::high_ir
