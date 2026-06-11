#pragma once

#include "../type.hpp"
#include "ir.hpp"
#include <cassert>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace exodus::high_ir {

struct Symbol {
  std::shared_ptr<Type> type;
  Value *val;
  Constant *const_val;
  Function *func;
  bool is_const;
};

struct SymTab {
  SymTab() {
    enter_scope();
    register_sysy();
  }

  auto enter_scope() -> void { scopes.emplace_back(); }

  auto exit_scope() -> void {
    if (scopes.size() > 1u) {
      scopes.pop_back();
    }
  }

  auto push(const std::string &name, Symbol sym) -> bool {
    auto &cur = scopes.back();
    if (cur.find(name) != cur.end()) {
      return false;
    }
    cur[name] = std::move(sym);
    return true;
  }

  auto lookup(const std::string &name) -> std::optional<Symbol> {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
      auto res = it->find(name);
      if (res != it->end()) {
        return res->second;
      }
    }
    return std::nullopt;
  }

  auto register_sysy() -> void;

  auto is_global() const -> bool { return scopes.size() == 1u; }

  std::vector<std::map<std::string, Symbol>> scopes;
};

inline auto SymTab::register_sysy() -> void {
  auto add_builtin = [&](
                       const std::string &name,
                       const std::shared_ptr<Type> &ret_type,
                       const std::vector<std::shared_ptr<Type>> &params
                     ) {
    [[maybe_unused]] const bool ok = push(
      name, {Func::get(ret_type, params), nullptr, nullptr, nullptr, false}
    );
    assert(ok && "duplicated SysY builtin symbol");
  };

  auto i32 = I32::get();
  auto f32 = Float::get();
  auto v0 = Void::get();
  auto pi32 = i32->ptr_to();
  auto pf32 = f32->ptr_to();

  add_builtin("getint", i32, {});
  add_builtin("getch", i32, {});
  add_builtin("getarray", i32, {pi32});
  add_builtin("getfloat", f32, {});
  add_builtin("getfarray", i32, {pf32});

  add_builtin("putint", v0, {i32});
  add_builtin("putch", v0, {i32});
  add_builtin("putarray", v0, {i32, pi32});
  add_builtin("putfloat", v0, {f32});
  add_builtin("putfarray", v0, {i32, pf32});

  // keep both macro-style names in tests and runtime symbols in sylib.c
  add_builtin("starttime", v0, {});
  add_builtin("stoptime", v0, {});
  add_builtin("_sysy_starttime", v0, {i32});
  add_builtin("_sysy_stoptime", v0, {i32});
}

} // namespace exodus::high_ir
