#pragma once

#include "../opt/AnalysisManager.hpp"
#include "ir.hpp"
#include <numeric>
#include <unordered_map>
#include <vector>

namespace exodus::mid_ir {

// https://zhuanlan.zhihu.com/p/2047152342236148687
// 我做的笔记⬆
struct DomDsu {
  std::vector<int> father;
  std::vector<int> label;
  const std::vector<int> &dfn;  // NOLINT
  const std::vector<int> &sdom; // NOLINT

  auto compress(int x) -> void {
    if (x == father[x])
      return;
    compress(father[x]);
    if (dfn[sdom[label[father[x]]]] < dfn[sdom[label[x]]])
      label[x] = label[father[x]];
    father[x] = father[father[x]];
  }

  DomDsu(
    int n, const std::vector<int> &_dfn, const std::vector<int> &_sdom // NOLINT
  )
      : father(n + 1, 0), label(n + 1, 0), dfn(_dfn), sdom(_sdom) {
    std::iota(father.begin(), father.end(), 0);
    std::iota(label.begin(), label.end(), 0);
  }

  auto eval(int x) -> int {
    if (x == father[x])
      return label[x];
    compress(x);
    return label[x];
  }

  auto link(int u, int v) -> void { father[v] = u; }
};

struct DomTree {
  struct Node {
    Block *block = nullptr;
    Block *idom = nullptr;
    std::vector<Block *> children;
    std::vector<Block *> df;
    int dfn = -1;
    int in = 0;
    int out = 0;
  };

  auto compute(LinearFunction &func) -> void;
  auto get_idom(Block *b) -> Block * {
    auto it = nodes.find(b);
    return it == nodes.end() ? nullptr : it->second.idom;
  }

  auto get_children(Block *b) -> const std::vector<Block *> & {
    return nodes.at(b).children;
  }
  auto get_df(Block *b) -> const std::vector<Block *> & {
    return nodes.at(b).df;
  }
  auto dominate(Block *a, Block *b) -> bool;

  auto dump_dot() -> std::string {
    std::string res = "digraph DomTree {\n";
    for (auto &[b, node] : nodes) {
      if (node.idom) {
        res += fmt::format("  \"{}\" -> \"{}\";\n", node.idom->name, b->name);
      }
    }
    res += "}\n";
    return res;
  }

private:
  auto reset(size_t n) -> void;
  auto dfs(Block *u, Block *p) -> void;
  auto compute_df() -> void;
  auto dfs_dom_tree(Block *u, int &time) -> void;

  std::unordered_map<Block *, Node> nodes;
  std::vector<int> sdom;
  std::vector<int> idom;
  std::vector<int> parent;
  std::vector<Block *> vertex;
  std::vector<std::vector<int>> bucket;
  int timer = 0;
};

struct DominanceAnalysis {
  using Result = DomTree;
  auto run(
    LinearFunction &func,
    [[maybe_unused]] exodus::opt::LinearFunctionAnalysisManager &am
  ) -> Result {
    DomTree dt;
    dt.compute(func);
    return dt;
  }
};

} // namespace exodus::mid_ir