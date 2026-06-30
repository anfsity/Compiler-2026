#pragma once

#include "../base/ir.hpp"
#include <optional>

namespace exodus::mid_ir {
struct Block;
}

namespace exodus::high_ir {

using namespace exodus::ir;

struct Op;

using Region = std::list<Op *>;

enum class OpCode : uint8_t {
  // clang-format off
  Add, Sub, Mul, Div, Mod, FAdd, FSub, FMul, FDiv, // arithmetic
  I2F, F2I, ZExt,                                  // transform
  Eq, Ne, Lt, Gt, Le, Ge,                          // compare
  And, Or, Xor, Shl, Shr,                          // logic / bitwise
  Alloca, Load, Store, GetPtr,                     // memory
  Call, Ret,                                       // function
  If, While, Break, Continue, Condition,           // control
  Jump, Branch,                                    // for mid ir
  Memset                                           // for efficient zero init
  // clang-format on
};

// empty, call, if ,while
struct EmptyPayload {};

struct CallPayload {
  std::string func_name;
};

struct IfPayload {
  std::unique_ptr<Region> then_region;
  std::optional<Region> else_region;
};

struct WhilePayload {
  std::unique_ptr<Region> cond_region;
  std::unique_ptr<Region> loop_region;
};

struct Op : OpBase {
  OpCode code;

  std::vector<Value *> operands;
  // 经过深思熟虑，最终决定这种方式来代表 jump 和 branch
  // 对于普通指令来说，他们的 successors 是空的，对于 mid ir 的
  // jump 和 branch 来说，jump 的 operands 是空的，仅有一个 successors
  // branch 的 operands 大小为 1, 有两个 successors，then 和 else
  std::vector<mid_ir::Block *> successors;
  OpResult *result = nullptr;

  using Payload =
    std::variant<EmptyPayload, CallPayload, IfPayload, WhilePayload>;

  Payload payload;

  Op(OpCode c, Payload p = EmptyPayload{}) : code(c), payload(std::move(p)) {}
};

struct Function {
  std::string name;
  std::shared_ptr<Type> type;
  std::vector<Argument *> args;
  Region body;
  bool is_decl = false;
};

struct IRContext : exodus::ir::IRContext {
  std::vector<std::unique_ptr<Op>> ops;

  template <typename... Args>
  auto make_op(Args &&...args) -> Op * {
    auto obj = std::make_unique<Op>(std::forward<Args>(args)...);
    auto *ptr = obj.get();
    ops.emplace_back(std::move(obj));
    return ptr;
  }
};

struct Module {
  IRContext ctx;
  std::vector<std::unique_ptr<GlobalVar>> globals;
  std::vector<std::unique_ptr<Function>> functions;
};

} // namespace exodus::high_ir
