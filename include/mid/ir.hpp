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

  using Payload = std::variant<EmptyPayload, CallPayload>;

  Payload payload;

  Op(OpCode c, Payload p = EmptyPayload{}) : code(c), payload(std::move(p)) {}
};

struct Block {
  std::string name;
  std::list<Op *> insts;

  std::vector<Block *> preds;
  std::vector<Block *> succs;

  Block(std::string n) : name(std::move(n)) {}
};

struct LinearFunction {
  std::string name;
  std::shared_ptr<Type> type;
  std::vector<Argument *> args;

  std::list<std::unique_ptr<Block>> blocks;
  bool is_decl = false;
};

} // namespace exodus::mid_ir
