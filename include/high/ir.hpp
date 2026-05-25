#pragma once

#ifndef FMT_HEADER_ONLY
#define FMT_HEADER_ONLY
#endif
#include "../../3rd-party/fmt/format.h"
#include "../type.hpp"
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace exodus::mid_ir {
struct Block;
}

namespace exodus::high_ir {

struct Op;

enum class ValueKind : uint8_t { Constant, Argument, OpResult, GlobalVar };

struct Value {
  ValueKind kind;
  std::shared_ptr<Type> type;
  std::list<Op *> users;

  Value(ValueKind k, std::shared_ptr<Type> t) : kind(k), type(std::move(t)) {}
  virtual ~Value() = default;

  Value(const Value &) = delete;
  Value &operator=(const Value &) = delete;
  Value(Value &&) = delete;
  Value &operator=(Value &&) = delete;

  virtual auto dump() const -> std::string = 0;
  void addUse(Op *user) { users.push_back(user); }
  void rmUse(Op *user) { users.remove(user); }
};

struct Constant : Value {
  using Data = std::variant<int, float>;
  Data val;

  Constant(std::shared_ptr<Type> t, Data v)
      : Value(ValueKind::Constant, std::move(t)), val(v) {}

  auto dump() const -> std::string override {
    if (std::holds_alternative<int>(val)) {
      return fmt::format("{}", std::get<int>(val));
    } else {
      return fmt::format("{}f", std::get<float>(val));
    }
  }
};

struct Argument : Value {
  int idx;
  Argument(std::shared_ptr<Type> t, int i)
      : Value(ValueKind::Argument, std::move(t)), idx(i) {}

  auto dump() const -> std::string override {
    return fmt::format("%arg{}", idx);
  }
};

struct OpResult : Value {
  Op *creator;
  mutable int id = -1;

  OpResult(std::shared_ptr<Type> t, Op *c = nullptr)
      : Value(ValueKind::OpResult, std::move(t)), creator(c) {}

  auto dump() const -> std::string override { return fmt::format("%{}", id); }
};

struct GlobalAddr : Value {
  std::string name;
  GlobalAddr(std::shared_ptr<Type> t, std::string n)
      : Value(ValueKind::GlobalVar, std::move(t)), name(std::move(n)) {}

  auto dump() const -> std::string override { return fmt::format("@{}", name); }
};

struct ZeroInit {};
struct InitVal;
struct InitList {
  std::vector<InitVal> values;
};
struct InitVal {
  std::variant<int, float, ZeroInit, InitList> data;
};
using Region = std::list<Op *>;

// clang-format off
enum class OpCode : uint8_t {
    Add, Sub, Mul, Div, Mod, FAdd, FSub, FMul, FDiv, // arithmetic
    I2F, F2I, ZExt,                                  // transform
    Eq, Ne, Lt, Gt, Le, Ge,                          // compare
    And, Or, Xor, Shl, Shr,                          // logic / bitwise
    Alloca, Load, Store, GetPtr,                     // memory
    Call, Ret,                                       // function
    If, While, Break, Continue, Condition,           // control
    Jump, Branch,                                     // for mid ir
    Memset                                           // for efficient zero init
    }; // clang-format on

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

struct Op {
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

struct GlobalVar {
  std::string name;
  std::shared_ptr<Type> type;
  InitVal init;
  bool is_const = false;
  GlobalAddr *addr = nullptr;
};

struct Function {
  std::string name;
  std::shared_ptr<Type> type;
  std::vector<Argument *> args;
  Region body;
  bool is_decl = false;
};

struct IRContext {
  std::vector<std::unique_ptr<Value>> values;
  std::vector<std::unique_ptr<Op>> ops;

  template <typename T, typename... Args>
  auto make_value(Args &&...args) -> T * {
    auto obj = std::make_unique<T>(std::forward<Args>(args)...);
    auto *ptr = obj.get();
    values.emplace_back(std::move(obj));
    return ptr;
  }

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
