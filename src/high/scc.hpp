#pragma once

#include "ir.hpp"
#include "visitor.hpp"
#include <algorithm>
#include <stack>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace exodus::high_ir {

struct CallGraphNode {
  Function *func = nullptr;
  std::unordered_set<Function *> callees;
  int dfn = -1;
  int low = -1;
  bool in_stack = false;
  bool is_recursive = false;
};

class CallGraph {
  std::unordered_map<Function *, CallGraphNode> nodes;
  std::vector<std::vector<Function *>> sccs;
  std::vector<Function *> order;
  std::stack<Function *> tarjan_stack;
  int timer = 0;

public:
  CallGraph();
  explicit CallGraph(const Module &m);

  auto build(const Module &m) -> void;

  auto getSCCs() const -> const std::vector<std::vector<Function *>> &;

  auto getNodes() const
    -> const std::unordered_map<Function *, CallGraphNode> &;

  auto isRecursive(Function *f) const -> bool;

private:
  auto tarjan(Function *u) -> void;
  auto computeSCCs() -> void;
};

} // namespace exodus::high_ir
