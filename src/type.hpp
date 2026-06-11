// include/type.hpp
#pragma once

#ifndef FMT_HEADER_ONLY
#define FMT_HEADER_ONLY
#endif
#include "../3rd-party/fmt/format.h"
#include "../3rd-party/fmt/ranges.h"
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace exodus {

// 虽然 function
// 不是一等公民，但是如果把函数作为类型，我们可以统一符号表的逻辑。这样写起来代码会很清爽
enum class Kind : uint8_t { I32, F32, Ptr, Array, Bool, Void, Func };

struct Type : std::enable_shared_from_this<Type> {
  Kind kind;
  Type(Kind kind_) : kind(kind_) {}
  virtual ~Type() = default;

  Type(const Type &) = delete;
  Type &operator=(const Type &) = delete;
  Type(Type &&) = delete;
  Type &operator=(Type &&) = delete;

  virtual auto is_i32() const -> bool { return kind == Kind::I32; };
  virtual auto is_f32() const -> bool { return kind == Kind::F32; };
  virtual auto is_ptr() const -> bool { return kind == Kind::Ptr; };
  virtual auto is_array() const -> bool { return kind == Kind::Array; };
  virtual auto is_bool() const -> bool { return kind == Kind::Bool; };
  virtual auto is_void() const -> bool { return kind == Kind::Void; };
  virtual auto is_func() const -> bool { return kind == Kind::Func; };

  virtual auto to_string() const -> std::string = 0;

  auto ptr_to() -> std::shared_ptr<Type>;
  auto array_of(int n) -> std::shared_ptr<Type>;
};

struct I32 : Type {
  I32() : Type(Kind::I32) {}

  static auto get() -> std::shared_ptr<Type> {
    static auto inst = std::make_shared<I32>();
    return inst;
  }

  auto to_string() const -> std::string override { return "i32"; }
};

struct Float : Type {
  Float() : Type(Kind::F32) {}

  static auto get() -> std::shared_ptr<Type> {
    static auto inst = std::make_shared<Float>();
    return inst;
  }

  auto to_string() const -> std::string override { return "f32"; }
};

struct Void : Type {
  Void() : Type(Kind::Void) {}

  static auto get() -> std::shared_ptr<Type> {
    static auto inst = std::make_shared<Void>();
    return inst;
  }

  auto to_string() const -> std::string override { return "void"; }
};

struct Bool : Type {
  Bool() : Type(Kind::Bool) {}

  static auto get() -> std::shared_ptr<Type> {
    static auto inst = std::make_shared<Bool>();
    return inst;
  }

  auto to_string() const -> std::string override { return "i1"; }
};

struct Func : Type {
  std::shared_ptr<Type> ret_type;
  std::vector<std::shared_ptr<Type>> params;

  Func(std::shared_ptr<Type> ret, std::vector<std::shared_ptr<Type>> _params)
      : Type(Kind::Func), ret_type(std::move(ret)), params(std::move(_params)) {
  }

  static auto get(
    const std::shared_ptr<Type> &ret,
    const std::vector<std::shared_ptr<Type>> &_params
  ) -> std::shared_ptr<Type> {
    static std::map<
      std::pair<Type *, std::vector<std::shared_ptr<Type>>>,
      std::shared_ptr<Type>>
      cache;
    auto key = std::make_pair(ret.get(), _params);
    if (cache.find(key) != cache.end()) {
      return cache[key];
    }
    return cache[key] = std::make_shared<Func>(ret, _params);
  }

  auto to_string() const -> std::string override {
    std::string ptrs;
    for (auto &p : params) {
      ptrs += p->to_string() + (&p == &params.back() ? "" : ", ");
    }
    return fmt::format("({}) -> {}", ptrs, ret_type->to_string());
  }
};

struct Ptr : Type {
  std::shared_ptr<Type> target;

  Ptr(std::shared_ptr<Type> _target)
      : Type(Kind::Ptr), target(std::move(_target)) {}

  static auto get(const std::shared_ptr<Type> &_target)
    -> std::shared_ptr<Type> {
    static std::map<Type *, std::shared_ptr<Type>> cache;
    auto key = _target.get();
    if (cache.find(key) != cache.end()) {
      return cache[key];
    }
    return cache[key] = std::make_shared<Ptr>(_target);
  }

  auto to_string() const -> std::string override {
    return fmt::format("{}*", target->to_string());
  }
};

struct Array : Type {
  std::shared_ptr<Type> base;
  int len;

  Array(std::shared_ptr<Type> _target, int _len)
      : Type(Kind::Array), base(std::move(_target)), len(_len) {}

  auto size() const -> int {
    int res = 1;
    auto cur = static_cast<const Type *>(this);
    while (cur->is_array()) {
      auto arr = static_cast<const Array *>(cur); // NOLINT
      res *= arr->len;
      cur = arr->base.get();
    }
    return res;
  }

  static auto get(const std::shared_ptr<Type> &_base, int _len)
    -> std::shared_ptr<Type> {
    static std::map<std::pair<Type *, int>, std::shared_ptr<Array>> cache;
    auto key = std::make_pair(_base.get(), _len);

    if (cache.find(key) != cache.end()) {
      return cache[key];
    }
    return cache[key] = std::make_shared<Array>(_base, _len);
  }

  auto to_string() const -> std::string override {
    std::vector<int> dims;
    auto cur = static_cast<const Type *>(this);
    while (cur->is_array()) {
      auto arr = static_cast<const Array *>(cur); // NOLINT
      dims.push_back(arr->len);
      cur = arr->base.get();
    }

    std::string res = cur->to_string();
    for (int d : dims) {
      res += fmt::format("[{}]", d);
    }
    return res;
  }
};

inline auto Type::ptr_to() -> std::shared_ptr<Type> {
  return Ptr::get(shared_from_this());
}

inline auto Type::array_of(int n) -> std::shared_ptr<Type> {
  return Array::get(shared_from_this(), n);
}

} // namespace exodus
