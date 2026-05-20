#include "../include/high/ir.hpp"
#include "../include/high/verifier.hpp"
#include "../include/type.hpp"

#include <iostream>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace exodus;
using namespace exodus::high_ir;

namespace {

auto make_function(Module &m, const std::shared_ptr<Type> &ret_type)
  -> Function * {
  auto func = std::make_unique<Function>();
  func->name = "test";
  func->type = Func::get(ret_type, {});
  func->is_decl = false;
  auto *ptr = func.get();
  m.functions.emplace_back(std::move(func));
  return ptr;
}

auto make_const(
  Module &m, const std::shared_ptr<Type> &type, const Constant::Data &val
) -> Constant * {
  return m.ctx.make_value<Constant>(type, val);
}

auto attach_result(Module &m, Op *op, const std::shared_ptr<Type> &type)
  -> void {
  auto *res = m.ctx.make_value<OpResult>(type, op);
  res->creator = op;
  op->result = res;
}

auto add_operand(Op *op, Value *val) -> void {
  op->operands.push_back(val);
  if (val) {
    val->addUse(op);
  }
}

auto add_operand_no_use(Op *op, Value *val) -> void {
  op->operands.push_back(val);
}

auto add_op(Function *func, Op *op) -> Op * {
  func->body.push_back(op);
  return op;
}

auto make_unary_op(
  Module &m,
  Function *func,
  OpCode code,
  Value *operand,
  const std::shared_ptr<Type> &result_type
) -> Op * {
  auto *op = m.ctx.make_op(code);
  add_operand(op, operand);
  attach_result(m, op, result_type);
  return add_op(func, op);
}

auto make_binary_op(
  Module &m,
  Function *func,
  OpCode code,
  Value *lhs,
  Value *rhs,
  const std::shared_ptr<Type> &result_type
) -> Op * {
  auto *op = m.ctx.make_op(code);
  add_operand(op, lhs);
  add_operand(op, rhs);
  attach_result(m, op, result_type);
  return add_op(func, op);
}

auto verify_function(Function &func, bool expected) -> bool {
  Verifier v;
  return v.verify(func) == expected;
}

auto test_valid_arithmetic() -> bool {
  Module m;
  auto *func = make_function(m, Void::get());

  auto *i1 = make_const(m, I32::get(), 1);
  auto *i2 = make_const(m, I32::get(), 2);
  auto *f1 = make_const(m, Float::get(), 1.0f);
  auto *f2 = make_const(m, Float::get(), 2.0f);

  make_binary_op(m, func, OpCode::Add, i1, i2, I32::get());
  make_binary_op(m, func, OpCode::Sub, i1, i2, I32::get());
  make_binary_op(m, func, OpCode::Mul, i1, i2, I32::get());
  make_binary_op(m, func, OpCode::Div, i1, i2, I32::get());
  make_binary_op(m, func, OpCode::Mod, i1, i2, I32::get());

  make_binary_op(m, func, OpCode::FAdd, f1, f2, Float::get());
  make_binary_op(m, func, OpCode::FSub, f1, f2, Float::get());
  make_binary_op(m, func, OpCode::FMul, f1, f2, Float::get());
  make_binary_op(m, func, OpCode::FDiv, f1, f2, Float::get());

  return verify_function(*func, true);
}

auto test_valid_compare() -> bool {
  Module m;
  auto *func = make_function(m, Void::get());

  auto *i1 = make_const(m, I32::get(), 1);
  auto *i2 = make_const(m, I32::get(), 2);
  auto *f1 = make_const(m, Float::get(), 1.0f);
  auto *f2 = make_const(m, Float::get(), 2.0f);
  auto *b0 = make_const(m, Bool::get(), 0);
  auto *b1 = make_const(m, Bool::get(), 1);

  make_binary_op(m, func, OpCode::Eq, i1, i2, Bool::get());
  make_binary_op(m, func, OpCode::Ne, f1, f2, Bool::get());
  make_binary_op(m, func, OpCode::Lt, i1, i2, Bool::get());
  make_binary_op(m, func, OpCode::Gt, f1, f2, Bool::get());
  make_binary_op(m, func, OpCode::Le, i1, i2, Bool::get());
  make_binary_op(m, func, OpCode::Ge, i1, i2, Bool::get());
  make_binary_op(m, func, OpCode::Eq, b0, b1, Bool::get());

  return verify_function(*func, true);
}

auto test_valid_bitwise() -> bool {
  Module m;
  auto *func = make_function(m, Void::get());

  auto *i1 = make_const(m, I32::get(), 1);
  auto *i2 = make_const(m, I32::get(), 2);

  make_binary_op(m, func, OpCode::And, i1, i2, I32::get());
  make_binary_op(m, func, OpCode::Or, i1, i2, I32::get());
  make_binary_op(m, func, OpCode::Xor, i1, i2, I32::get());
  make_binary_op(m, func, OpCode::Shl, i1, i2, I32::get());
  make_binary_op(m, func, OpCode::Shr, i1, i2, I32::get());

  return verify_function(*func, true);
}

auto test_valid_casts() -> bool {
  Module m;
  auto *func = make_function(m, Void::get());

  auto *i1 = make_const(m, I32::get(), 1);
  auto *f1 = make_const(m, Float::get(), 1.0f);
  auto *b1 = make_const(m, Bool::get(), 1);

  make_unary_op(m, func, OpCode::I2F, i1, Float::get());
  make_unary_op(m, func, OpCode::F2I, f1, I32::get());
  make_unary_op(m, func, OpCode::ZExt, b1, I32::get());

  return verify_function(*func, true);
}

auto test_valid_memory() -> bool {
  Module m;
  auto *func = make_function(m, Void::get());

  auto *alloca_op = m.ctx.make_op(OpCode::Alloca);
  attach_result(m, alloca_op, I32::get()->ptr_to());
  add_op(func, alloca_op);

  auto *ptr = alloca_op->result;
  auto *val = make_const(m, I32::get(), 1);

  make_unary_op(m, func, OpCode::Load, ptr, I32::get());

  auto *store_op = m.ctx.make_op(OpCode::Store);
  add_operand(store_op, val);
  add_operand(store_op, ptr);
  add_op(func, store_op);

  auto *idx = make_const(m, I32::get(), 0);
  auto *getptr_op = m.ctx.make_op(OpCode::GetPtr);
  add_operand(getptr_op, ptr);
  add_operand(getptr_op, idx);
  attach_result(m, getptr_op, I32::get()->ptr_to());
  add_op(func, getptr_op);

  return verify_function(*func, true);
}

auto test_valid_control() -> bool {
  Module m;
  auto *func = make_function(m, Void::get());

  auto *cond = make_const(m, Bool::get(), 1);
  auto *if_op = m.ctx.make_op(OpCode::If);
  add_operand(if_op, cond);
  if_op->payload = IfPayload{std::make_unique<Region>(), std::nullopt};
  add_op(func, if_op);

  auto *while_op = m.ctx.make_op(OpCode::While);
  auto cond_region = std::make_unique<Region>();
  auto loop_region = std::make_unique<Region>();

  auto *cond_op = m.ctx.make_op(OpCode::Condition);
  add_operand(cond_op, cond);
  cond_region->push_back(cond_op);

  loop_region->push_back(m.ctx.make_op(OpCode::Break));
  loop_region->push_back(m.ctx.make_op(OpCode::Continue));

  while_op->payload =
    WhilePayload{std::move(cond_region), std::move(loop_region)};
  add_op(func, while_op);

  return verify_function(*func, true);
}

auto test_valid_return_void() -> bool {
  Module m;
  auto *func = make_function(m, Void::get());

  auto *ret_op = m.ctx.make_op(OpCode::Ret);
  add_op(func, ret_op);

  return verify_function(*func, true);
}

auto test_valid_return_nonvoid() -> bool {
  Module m;
  auto *func = make_function(m, I32::get());

  auto *val = make_const(m, I32::get(), 1);
  auto *ret_op = m.ctx.make_op(OpCode::Ret);
  add_operand(ret_op, val);
  add_op(func, ret_op);

  return verify_function(*func, true);
}

auto test_valid_call() -> bool {
  Module m;
  auto *func = make_function(m, Void::get());

  auto *call_op = m.ctx.make_op(OpCode::Call);
  call_op->payload = CallPayload{"foo"};
  attach_result(m, call_op, I32::get());
  add_op(func, call_op);

  return verify_function(*func, true);
}

auto test_invalid_use_def_chain() -> bool {
  Module m;
  auto *func = make_function(m, Void::get());

  auto *lhs = make_const(m, I32::get(), 1);
  auto *rhs = make_const(m, I32::get(), 2);

  auto *op = m.ctx.make_op(OpCode::Add);
  add_operand_no_use(op, lhs);
  add_operand_no_use(op, rhs);
  attach_result(m, op, I32::get());
  add_op(func, op);

  return verify_function(*func, false);
}

auto test_invalid_creator_mismatch() -> bool {
  Module m;
  auto *func = make_function(m, Void::get());

  auto *op = m.ctx.make_op(OpCode::Alloca);
  auto *res = m.ctx.make_value<OpResult>(I32::get()->ptr_to(), nullptr);
  op->result = res;
  add_op(func, op);

  return verify_function(*func, false);
}

auto test_invalid_null_operand() -> bool {
  Module m;
  auto *func = make_function(m, Void::get());

  auto *lhs = make_const(m, I32::get(), 1);

  auto *op = m.ctx.make_op(OpCode::Add);
  add_operand(op, lhs);
  add_operand(op, nullptr);
  attach_result(m, op, I32::get());
  add_op(func, op);

  return verify_function(*func, false);
}

auto test_invalid_arithmetic() -> bool {
  Module m;
  auto *func = make_function(m, Void::get());

  auto *i1 = make_const(m, I32::get(), 1);
  auto *i2 = make_const(m, I32::get(), 2);
  auto *f1 = make_const(m, Float::get(), 1.0f);

  auto *count_op = m.ctx.make_op(OpCode::Add);
  add_operand(count_op, i1);
  attach_result(m, count_op, I32::get());
  add_op(func, count_op);

  auto *mismatch_op = m.ctx.make_op(OpCode::Add);
  add_operand(mismatch_op, i1);
  add_operand(mismatch_op, f1);
  attach_result(m, mismatch_op, I32::get());
  add_op(func, mismatch_op);

  auto *type_op = m.ctx.make_op(OpCode::Add);
  add_operand(type_op, f1);
  add_operand(type_op, f1);
  attach_result(m, type_op, Float::get());
  add_op(func, type_op);

  auto *result_op = m.ctx.make_op(OpCode::Add);
  add_operand(result_op, i1);
  add_operand(result_op, i2);
  attach_result(m, result_op, Float::get());
  add_op(func, result_op);

  auto *fadd_op = m.ctx.make_op(OpCode::FAdd);
  add_operand(fadd_op, i1);
  add_operand(fadd_op, i2);
  attach_result(m, fadd_op, I32::get());
  add_op(func, fadd_op);

  return verify_function(*func, false);
}

auto test_invalid_compare() -> bool {
  Module m;
  auto *func = make_function(m, Void::get());

  auto *i1 = make_const(m, I32::get(), 1);
  auto *i2 = make_const(m, I32::get(), 2);
  auto *b1 = make_const(m, Bool::get(), 1);

  auto *alloca_op = m.ctx.make_op(OpCode::Alloca);
  attach_result(m, alloca_op, I32::get()->ptr_to());
  add_op(func, alloca_op);
  auto *ptr = alloca_op->result;

  auto *count_op = m.ctx.make_op(OpCode::Eq);
  add_operand(count_op, i1);
  attach_result(m, count_op, Bool::get());
  add_op(func, count_op);

  auto *mismatch_op = m.ctx.make_op(OpCode::Eq);
  add_operand(mismatch_op, i1);
  add_operand(mismatch_op, b1);
  attach_result(m, mismatch_op, Bool::get());
  add_op(func, mismatch_op);

  auto *ptr_op = m.ctx.make_op(OpCode::Eq);
  add_operand(ptr_op, ptr);
  add_operand(ptr_op, ptr);
  attach_result(m, ptr_op, Bool::get());
  add_op(func, ptr_op);

  auto *ord_op = m.ctx.make_op(OpCode::Lt);
  add_operand(ord_op, b1);
  add_operand(ord_op, b1);
  attach_result(m, ord_op, Bool::get());
  add_op(func, ord_op);

  auto *result_op = m.ctx.make_op(OpCode::Eq);
  add_operand(result_op, i1);
  add_operand(result_op, i2);
  attach_result(m, result_op, I32::get());
  add_op(func, result_op);

  return verify_function(*func, false);
}

auto test_invalid_bitwise() -> bool {
  Module m;
  auto *func = make_function(m, Void::get());

  auto *i1 = make_const(m, I32::get(), 1);
  auto *b1 = make_const(m, Bool::get(), 1);

  auto *count_op = m.ctx.make_op(OpCode::And);
  add_operand(count_op, i1);
  attach_result(m, count_op, I32::get());
  add_op(func, count_op);

  auto *type_op = m.ctx.make_op(OpCode::Or);
  add_operand(type_op, b1);
  add_operand(type_op, b1);
  attach_result(m, type_op, I32::get());
  add_op(func, type_op);

  auto *result_op = m.ctx.make_op(OpCode::Xor);
  add_operand(result_op, i1);
  add_operand(result_op, i1);
  attach_result(m, result_op, Float::get());
  add_op(func, result_op);

  return verify_function(*func, false);
}

auto test_invalid_casts() -> bool {
  Module m;
  auto *func = make_function(m, Void::get());

  auto *i1 = make_const(m, I32::get(), 1);
  auto *f1 = make_const(m, Float::get(), 1.0f);
  auto *b1 = make_const(m, Bool::get(), 1);

  auto *count_op = m.ctx.make_op(OpCode::I2F);
  add_operand(count_op, i1);
  add_operand(count_op, i1);
  attach_result(m, count_op, Float::get());
  add_op(func, count_op);

  auto *i2f_op = m.ctx.make_op(OpCode::I2F);
  add_operand(i2f_op, f1);
  attach_result(m, i2f_op, Float::get());
  add_op(func, i2f_op);

  auto *f2i_op = m.ctx.make_op(OpCode::F2I);
  add_operand(f2i_op, i1);
  attach_result(m, f2i_op, I32::get());
  add_op(func, f2i_op);

  auto *zext_op = m.ctx.make_op(OpCode::ZExt);
  add_operand(zext_op, f1);
  attach_result(m, zext_op, I32::get());
  add_op(func, zext_op);

  auto *result_op = m.ctx.make_op(OpCode::ZExt);
  add_operand(result_op, b1);
  attach_result(m, result_op, Bool::get());
  add_op(func, result_op);

  return verify_function(*func, false);
}

auto test_invalid_memory() -> bool {
  Module m;
  auto *func = make_function(m, Void::get());

  auto *val = make_const(m, I32::get(), 1);
  auto *idx = make_const(m, I32::get(), 0);
  auto *fval = make_const(m, Float::get(), 1.0f);

  auto *alloca_op = m.ctx.make_op(OpCode::Alloca);
  attach_result(m, alloca_op, I32::get());
  add_op(func, alloca_op);

  auto *load_op = m.ctx.make_op(OpCode::Load);
  add_operand(load_op, val);
  attach_result(m, load_op, I32::get());
  add_op(func, load_op);

  auto *load_res_op = m.ctx.make_op(OpCode::Load);
  auto *ptr_val = m.ctx.make_value<OpResult>(I32::get()->ptr_to(), load_res_op);
  ptr_val->creator = load_res_op;
  add_operand(load_res_op, ptr_val);
  attach_result(m, load_res_op, Float::get());
  add_op(func, load_res_op);

  auto *store_ptr_op = m.ctx.make_op(OpCode::Store);
  add_operand(store_ptr_op, val);
  add_operand(store_ptr_op, val);
  add_op(func, store_ptr_op);

  auto *store_val_op = m.ctx.make_op(OpCode::Store);
  add_operand(store_val_op, fval);
  add_operand(store_val_op, ptr_val);
  add_op(func, store_val_op);

  auto *getptr_count_op = m.ctx.make_op(OpCode::GetPtr);
  add_operand(getptr_count_op, ptr_val);
  attach_result(m, getptr_count_op, I32::get()->ptr_to());
  add_op(func, getptr_count_op);

  auto *getptr_idx_op = m.ctx.make_op(OpCode::GetPtr);
  add_operand(getptr_idx_op, ptr_val);
  add_operand(getptr_idx_op, fval);
  attach_result(m, getptr_idx_op, I32::get()->ptr_to());
  add_op(func, getptr_idx_op);

  auto *getptr_res_op = m.ctx.make_op(OpCode::GetPtr);
  add_operand(getptr_res_op, ptr_val);
  add_operand(getptr_res_op, idx);
  attach_result(m, getptr_res_op, I32::get());
  add_op(func, getptr_res_op);

  return verify_function(*func, false);
}

auto test_invalid_control() -> bool {
  Module m;
  auto *func = make_function(m, Void::get());

  auto *b1 = make_const(m, Bool::get(), 1);
  auto *i1 = make_const(m, I32::get(), 1);

  auto *if_count = m.ctx.make_op(OpCode::If);
  if_count->payload = IfPayload{std::make_unique<Region>(), std::nullopt};
  add_op(func, if_count);

  auto *if_type = m.ctx.make_op(OpCode::If);
  add_operand(if_type, i1);
  if_type->payload = IfPayload{std::make_unique<Region>(), std::nullopt};
  add_op(func, if_type);

  add_op(func, m.ctx.make_op(OpCode::Break));
  add_op(func, m.ctx.make_op(OpCode::Continue));

  auto *cond_outside = m.ctx.make_op(OpCode::Condition);
  add_operand(cond_outside, b1);
  add_op(func, cond_outside);

  auto *while_op = m.ctx.make_op(OpCode::While);
  auto cond_region = std::make_unique<Region>();
  auto loop_region = std::make_unique<Region>();

  auto *cond_wrong_type = m.ctx.make_op(OpCode::Condition);
  add_operand(cond_wrong_type, i1);
  cond_region->push_back(cond_wrong_type);

  auto *cond_wrong_count = m.ctx.make_op(OpCode::Condition);
  cond_region->push_back(cond_wrong_count);

  while_op->payload =
    WhilePayload{std::move(cond_region), std::move(loop_region)};
  add_op(func, while_op);

  return verify_function(*func, false);
}

auto test_invalid_return() -> bool {
  Module m;
  auto *void_func = make_function(m, Void::get());
  auto *nonvoid_func = make_function(m, I32::get());

  auto *val = make_const(m, I32::get(), 1);

  auto *ret_void = m.ctx.make_op(OpCode::Ret);
  add_operand(ret_void, val);
  add_op(void_func, ret_void);

  auto *ret_count = m.ctx.make_op(OpCode::Ret);
  add_op(nonvoid_func, ret_count);

  auto *ret_type = m.ctx.make_op(OpCode::Ret);
  auto *b1 = make_const(m, Bool::get(), 1);
  add_operand(ret_type, b1);
  add_op(nonvoid_func, ret_type);

  Verifier v;
  return v.verify(*void_func) == false && v.verify(*nonvoid_func) == false;
}

auto test_invalid_call() -> bool {
  Module m;
  auto *func = make_function(m, Void::get());

  auto *call_op = m.ctx.make_op(OpCode::Call);
  call_op->payload = CallPayload{"foo"};
  attach_result(m, call_op, Void::get());
  add_op(func, call_op);

  return verify_function(*func, false);
}

} // namespace

#ifdef EXODUS_UNIT_TEST
int main() {
  auto run = [](const char *name, bool ok) -> int {
    if (!ok) {
      std::cerr << "Verifier test failed: " << name << "\n";
      return 1;
    }
    return 0;
  };

  if (run("test_valid_arithmetic", test_valid_arithmetic()))
    return 1;
  if (run("test_valid_compare", test_valid_compare()))
    return 1;
  if (run("test_valid_bitwise", test_valid_bitwise()))
    return 1;
  if (run("test_valid_casts", test_valid_casts()))
    return 1;
  if (run("test_valid_memory", test_valid_memory()))
    return 1;
  if (run("test_valid_control", test_valid_control()))
    return 1;
  if (run("test_valid_return_void", test_valid_return_void()))
    return 1;
  if (run("test_valid_return_nonvoid", test_valid_return_nonvoid()))
    return 1;
  if (run("test_valid_call", test_valid_call()))
    return 1;

  if (run("test_invalid_use_def_chain", test_invalid_use_def_chain()))
    return 1;
  if (run("test_invalid_creator_mismatch", test_invalid_creator_mismatch()))
    return 1;
  if (run("test_invalid_null_operand", test_invalid_null_operand()))
    return 1;
  if (run("test_invalid_arithmetic", test_invalid_arithmetic()))
    return 1;
  if (run("test_invalid_compare", test_invalid_compare()))
    return 1;
  if (run("test_invalid_bitwise", test_invalid_bitwise()))
    return 1;
  if (run("test_invalid_casts", test_invalid_casts()))
    return 1;
  if (run("test_invalid_memory", test_invalid_memory()))
    return 1;
  if (run("test_invalid_control", test_invalid_control()))
    return 1;
  if (run("test_invalid_return", test_invalid_return()))
    return 1;
  if (run("test_invalid_call", test_invalid_call()))
    return 1;

  return 0;
}
#endif
