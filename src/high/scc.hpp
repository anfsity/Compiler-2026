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
  explicit CallGraph(Module &m);

  auto build(Module &m) -> void;

  auto getSCCs() const -> const std::vector<std::vector<Function *>> &;

  auto getNodes() const
    -> const std::unordered_map<Function *, CallGraphNode> &;

  auto isRecursive(Function *f) const -> bool;

private:
  auto tarjan(Function *u) -> void;
  auto computeSCCs() -> void;
};

inline CallGraph::CallGraph() = default;

inline CallGraph::CallGraph(Module &m) { build(m); }

inline auto CallGraph::build(Module &m) -> void {
  nodes.clear();
  sccs.clear();
  order.clear();
  timer = 0;
  while (!tarjan_stack.empty())
    tarjan_stack.pop();

  std::unordered_map<std::string, Function *> func_map;
  func_map.reserve(m.functions.size());
  for (auto &f : m.functions) {
    Function *func = f.get();
    func_map[func->name] = func;
    CallGraphNode node;
    node.func = func;
    nodes.emplace(func, std::move(node));
    order.push_back(func);
  }

  struct CallFinder : RecursiveOpVisitor<CallFinder> {
    CallGraphNode &node; // NOLINT make clang-tidy happy :)
    std::unordered_map<std::string, Function *> &f_map; // NOLINT
    CallFinder(
      CallGraphNode &n, std::unordered_map<std::string, Function *> &fm
    )
        : node(n), f_map(fm) {}

    using RecursiveOpVisitor<CallFinder>::visit;
    void visit(Op *op, OpTag<OpCode::Call>) {
      auto &p = std::get<CallPayload>(op->payload);
      if (f_map.count(p.func_name)) {
        Function *callee = f_map[p.func_name];
        node.callees.insert(callee);
        if (callee == node.func)
          node.is_recursive = true;
      }
    }
  };

  for (auto &f : m.functions) {
    if (f->is_decl)
      continue;
    CallFinder finder(nodes[f.get()], func_map);
    finder.visit(*f);
  }

  computeSCCs();
}

inline auto CallGraph::getSCCs() const
  -> const std::vector<std::vector<Function *>> & {
  return sccs;
}

inline auto CallGraph::getNodes() const
  -> const std::unordered_map<Function *, CallGraphNode> & {
  return nodes;
}

inline auto CallGraph::isRecursive(Function *f) const -> bool {
  auto it = nodes.find(f);
  return it != nodes.end() && it->second.is_recursive;
}

inline auto CallGraph::tarjan(Function *u) -> void {
  auto &node = nodes[u];
  node.dfn = node.low = ++timer;
  tarjan_stack.push(u);
  node.in_stack = true;

  for (auto *v : node.callees) {
    if (nodes[v].dfn == -1) {
      tarjan(v);
      node.low = std::min(node.low, nodes[v].low);
    } else if (nodes[v].in_stack) {
      node.low = std::min(node.low, nodes[v].dfn);
    }
  }

  if (node.low == node.dfn) {
    std::vector<Function *> scc;
    while (true) {
      Function *v = tarjan_stack.top();
      tarjan_stack.pop();
      nodes[v].in_stack = false;
      scc.push_back(v);
      if (u == v)
        break;
    }
    if (scc.size() > 1) {
      for (auto *f : scc)
        nodes[f].is_recursive = true;
    }
    sccs.push_back(std::move(scc));
  }
}

inline auto CallGraph::computeSCCs() -> void {
  for (auto &entry : nodes) {
    entry.second.dfn = -1;
    entry.second.low = -1;
    entry.second.in_stack = false;
  }
  for (auto *f : order) {
    if (nodes[f].dfn == -1)
      tarjan(f);
  }
}

} // namespace exodus::high_ir
