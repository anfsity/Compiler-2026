#pragma once

#include "../base/ir.hpp"
#include <list>
#include <string>

namespace exodus::mid_ir {

using namespace exodus::ir;

struct Block;

struct EmptyPayload {};

struct CallPayload {
  std::string func_name;
};

struct PhiPayload {
  std::vector<std::pair<Block *, Value *>> incoming;
};

struct GetPtrPayload {
  // Type cursor used immediately before the first index.  It is independent
  // from the physical base so an immutable T** slot can become its stored T*
  // without changing any byte stride.
  std::shared_ptr<Type> layout_type;
};

enum class OpCode : uint8_t {
  // clang-format off
  Add, Sub, Mul, Div, Mod, FAdd, FSub, FMul, FDiv, // arithmetic
  I2F, F2I, ZExt,                                  // transform
  Eq, Ne, Lt, Gt, Le, Ge,                          // compare
  And, Or, Xor, Shl, Shr,                          // logic / bitwise
  Alloca, Load, Store, GetPtr,                     // memory
  Call, Ret,                                       // function
  Jump, Branch,                                    // control flow
  Phi,                                             // SSA
  Memset                                           // misc
  // clang-format on
};

struct Op : OpBase {
  OpCode code;

  std::vector<Value *> operands;
  std::vector<Block *> successors;
  OpResult *result = nullptr;

  using Payload =
    std::variant<EmptyPayload, CallPayload, PhiPayload, GetPtrPayload>;

  Payload payload;

  Op(OpCode c, Payload p = EmptyPayload{}) : code(c), payload(std::move(p)) {}
};

struct Block {
  int id;
  std::string name;
  std::list<Op *> insts;

  std::vector<Block *> preds;
  std::vector<Block *> succs;

  Block(int _id, std::string n) : id(_id), name(std::move(n)) {}
};

struct LinearFunction {
  std::string name;
  std::shared_ptr<Type> type;
  std::vector<Argument *> args;

  std::list<std::unique_ptr<Block>> blocks;
  bool is_decl = false;
  bool tail_recursion_eliminated = false;
  bool no_inline = false;
};

struct MidModule {
  exodus::ir::IRContext *ctx = nullptr;
  std::vector<std::unique_ptr<Op>> ops;
  std::vector<exodus::ir::GlobalVar *> globals;
  std::vector<std::unique_ptr<LinearFunction>> functions;

  template <typename... Args>
  auto make_op(Args &&...args) -> Op * {
    auto obj = std::make_unique<Op>(std::forward<Args>(args)...);
    auto *ptr = obj.get();
    ops.emplace_back(std::move(obj));
    return ptr;
  }
};

} // namespace exodus::mid_ir
