#pragma once

#include "dom.hpp"
#include "flatten.hpp"
#include "ir.hpp"
#include "rewriter.hpp"
#include <map>
#include <set>
#include <stack>
#include <unordered_map>
#include <vector>

namespace exodus::mid_ir {

struct Mem2Reg {
  Mem2Reg(MidModule *m) : module(m) {}

  auto run(LinearFunction &func, exodus::opt::LinearFunctionAnalysisManager &am)
    -> exodus::opt::PreservedAnalysis;

private:
  MidModule *module;
  std::unordered_map<Block *, int> block2idx;
  std::unordered_map<Op *, Op *> phi2alloca;
  std::unordered_map<Op *, std::vector<Op *>> alloca2phis;

  auto initialize_block_indices(LinearFunction &func) -> void;

  auto collect_promotable_allocas(LinearFunction &func) -> std::vector<Op *>;

  auto insert_phi(
    [[maybe_unused]] LinearFunction &func,
    DomTree &dom,
    Op *alloca,
    const std::vector<Block *> &stores
  ) -> void;

  auto rename(
    Block *b,
    DomTree &dom,
    std::unordered_map<Op *, std::stack<Value *>> &stacks,
    MidIRRewriter &rewriter
  ) -> void;

  auto cleanup(
    LinearFunction &func,
    const std::vector<Op *> &allocas,
    MidIRRewriter &rewriter
  ) -> void;
};

} // namespace exodus::mid_ir
