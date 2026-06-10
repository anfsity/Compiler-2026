#pragma once

#ifndef FMT_HEADER_ONLY
#define FMT_HEADER_ONLY
#endif
#include "../../3rd-party/fmt/format.h"
#include "../type.hpp"
#include <list>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace exodus::ir {

struct OpBase;

enum class ValueKind : uint8_t { Constant, Argument, OpResult, GlobalVar };

struct Value {
  ValueKind kind;
  std::shared_ptr<Type> type;
  std::list<OpBase *> users;

  Value(ValueKind k, std::shared_ptr<Type> t) : kind(k), type(std::move(t)) {}
  virtual ~Value() = default;

  Value(const Value &) = delete;
  Value &operator=(const Value &) = delete;
  Value(Value &&) = delete;
  Value &operator=(Value &&) = delete;

  virtual auto dump() const -> std::string = 0;
  auto addUse(OpBase *user) -> void { users.push_back(user); }
  auto rmUse(OpBase *user) -> void { users.remove(user); }
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
  OpBase *creator;
  mutable int id = -1;

  OpResult(std::shared_ptr<Type> t, OpBase *c = nullptr)
      : Value(ValueKind::OpResult, std::move(t)), creator(c) {}

  auto dump() const -> std::string override { return fmt::format("%{}", id); }
};

struct GlobalAddr : Value {
  std::string name;
  GlobalAddr(std::shared_ptr<Type> t, std::string n)
      : Value(ValueKind::GlobalVar, std::move(t)), name(std::move(n)) {}

  auto dump() const -> std::string override { return fmt::format("@{}", name); }
};

struct OpBase { // NOLINT
  virtual ~OpBase() = default;
};

struct ZeroInit {};
struct InitVal;
struct InitList {
  std::vector<InitVal> values;
};
struct InitVal {
  std::variant<int, float, ZeroInit, InitList> data;
};

struct GlobalVar {
  std::string name;
  std::shared_ptr<Type> type;
  InitVal init;
  bool is_const = false;
  GlobalAddr *addr = nullptr;
};

struct IRContext {
  std::vector<std::unique_ptr<Value>> values;

  template <typename T, typename... Args>
  auto make_value(Args &&...args) -> T * {
    auto obj = std::make_unique<T>(std::forward<Args>(args)...);
    auto *ptr = obj.get();
    values.emplace_back(std::move(obj));
    return ptr;
  }

  auto make_const(const std::shared_ptr<Type> &t, Constant::Data v)
    -> Constant * {
    if (t->is_f32()) {
      if (std::holds_alternative<int>(v)) {
        return make_value<Constant>(t, static_cast<float>(std::get<int>(v)));
      }
    } else {
      if (std::holds_alternative<float>(v)) {
        return make_value<Constant>(t, static_cast<int>(std::get<float>(v)));
      }
    }
    return make_value<Constant>(t, v);
  }

  auto make_const(const std::shared_ptr<Type> &t, const InitVal &iv)
    -> Constant * {
    if (std::holds_alternative<int>(iv.data)) {
      return make_const(t, std::get<int>(iv.data));
    } else if (std::holds_alternative<float>(iv.data)) {
      return make_const(t, std::get<float>(iv.data));
    } else if (std::holds_alternative<ZeroInit>(iv.data)) {
      return make_zero(t);
    }
    return nullptr;
  }

  auto make_zero(const std::shared_ptr<Type> &t) -> Constant * {
    if (t->is_f32()) {
      return make_value<Constant>(t, 0.0f);
    }
    return make_value<Constant>(t, 0);
  }
};

} // namespace exodus::ir
