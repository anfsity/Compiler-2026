#pragma once

#include "../../mid/dom.hpp"
#include "../../mid/getptr.hpp"
#include "../../mid/rewriter.hpp"
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace exodus::mid_ir::opt {

class ImmutablePointerSlotCanonicalize {
  struct Candidate {
    Op *alloca = nullptr;
    Op *store = nullptr;
    Value *stored_pointer = nullptr;
    std::vector<Op *> getptrs;
  };

  struct Context {
    std::unordered_map<Op *, Block *> op_blocks;
    std::unordered_set<Op *> scope;
  };

public:
  explicit ImmutablePointerSlotCanonicalize([[maybe_unused]] MidModule *m) {}

  auto run(LinearFunction &func, exodus::opt::LinearFunctionAnalysisManager &am)
    -> exodus::opt::PreservedAnalysis;

private:
  static auto build_scope(Context &context, LinearFunction &func) -> void;
  static auto
  dominates(Op *definition, Op *use, DomTree &dom, const Context &context)
    -> bool;
  static auto
  collect_candidate(Op *alloca, DomTree &dom, const Context &context)
    -> std::optional<Candidate>;
  static auto preserves_getptr_plan(Op *getptr, Value *replacement) -> bool;
};

} // namespace exodus::mid_ir::opt
