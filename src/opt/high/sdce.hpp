#pragma once

#include "../../helper/log.hpp"
#include "../../high/effects.hpp"
#include "../../high/ir.hpp"
#include "../../high/rewriter.hpp"
#include "../../high/visitor.hpp"
#include "../AnalysisManager.hpp"
#include <deque>
#include <unordered_map>
#include <unordered_set>

namespace exodus::high_ir::opt {

using namespace exodus::high_ir;
using namespace exodus::opt;

struct SimpleDCE {
  struct GlobalUse {
    bool reads = false;
    bool writes = false;
    bool escapes = false;
    bool unknown = false;
  };

  IRRewriter rewriter;
  std::unordered_map<Op *, Op *> parents;
  std::unordered_set<Op *> liveset;
  std::unordered_set<Value *> escaped_allocas;
  std::unordered_map<Value *, GlobalUse> global_uses;
  std::unordered_set<Value *> dead_global_roots;
  std::deque<Op *> worklist;

  Module *module;

  explicit SimpleDCE(Module *m) : module(m) {}

  auto collect_global_uses() -> void;
  auto scan_global_uses(const Region &region) -> void;
  auto mark_global_read(Value *address) -> void;
  auto mark_global_write(Value *address) -> void;
  auto mark_global_escape(Value *value) -> void;
  auto mark_global_unknown(Value *value) -> void;
  auto global_root(Value *value) const -> Value *;
  auto dead_global_write(Op *op) const -> bool;

  auto is_intrinsically_live(Op *op) const -> bool;

  auto mark_stores_to(Value *ptr) -> void;
  auto collect_escaped_allocas() -> void;
  auto mark_memory_dependencies(Op *op) -> void;

  auto build_parent_map(Region &r, Op *parent = nullptr) -> void;
  auto mark(Op *op) -> void;
  auto initial_mark(Region &r) -> void;
  auto collect_dead(Region &r) -> void;

  auto run(Function &f, FunctionAnalysisManager & /* FAM */)
    -> PreservedAnalysis;
};

} // namespace exodus::high_ir::opt
