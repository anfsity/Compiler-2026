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

inline auto DomTree::compute(LinearFunction &func) -> void {
  if (func.blocks.empty())
    return;

  reset(func.blocks.size());
  Block *entry = func.blocks.front().get();
  dfs(entry, entry);

  std::vector<int> dfn_v(timer + 1);
  std::iota(dfn_v.begin(), dfn_v.end(), 0);
  std::iota(sdom.begin(), sdom.end(), 0);
  DomDsu dsu(timer, dfn_v, sdom);

  for (int i = timer; i >= 2; --i) {
    Block *w = vertex[i];

    for (auto *v_ptr : w->preds) {
      if (nodes.find(v_ptr) == nodes.end())
        continue;
      int v = nodes[v_ptr].dfn;
      int u = dsu.eval(v);
      if (sdom[u] < sdom[i])
        sdom[i] = sdom[u];
    }

    bucket[sdom[i]].push_back(i);
    int p = parent[i];
    dsu.link(p, i);

    for (int v : bucket[p]) {
      int u = dsu.eval(v);
      idom[v] = (sdom[u] < sdom[v]) ? u : p;
    }

    bucket[p].clear();
  }

  for (int i = 2; i <= timer; ++i) {
    if (idom[i] != sdom[i])
      idom[i] = idom[idom[i]];
    nodes[vertex[i]].idom = vertex[idom[i]];
    nodes[vertex[idom[i]]].children.push_back(vertex[i]);
  }

  compute_df();

  int time = 0;
  dfs_dom_tree(entry, time);
}

inline auto DomTree::compute_df() -> void {
  for (auto &[b, node] : nodes) {

    if (b->preds.size() >= 2) {
      for (auto *p : b->preds) {

        if (nodes.find(p) == nodes.end())
          continue;
        Block *runner = p;

        while (runner != node.idom) {
          nodes[runner].df.push_back(b);
          runner = nodes[runner].idom;
        }
      }
    }
  }
}

inline auto DomTree::dfs_dom_tree(Block *u, int &time) -> void {
  nodes[u].in = ++time;
  for (auto *v : nodes[u].children) {
    dfs_dom_tree(v, time);
  }
  nodes[u].out = time;
}

inline auto DomTree::reset(size_t n) -> void {
  nodes.clear();
  sdom.assign(n + 1, 0);
  idom.assign(n + 1, 0);
  parent.assign(n + 1, 0);
  vertex.assign(n + 1, nullptr);
  bucket.assign(n + 1, {});
  timer = 0;
}

inline auto DomTree::dfs(Block *u, Block *p) -> void {
  nodes[u].dfn = ++timer;
  nodes[u].block = u;
  vertex[timer] = u;
  if (p)
    parent[timer] = nodes[p].dfn;

  for (auto *v : u->succs) {
    if (nodes.find(v) == nodes.end())
      dfs(v, u);
  }
}

inline auto DomTree::dominate(Block *a, Block *b) -> bool {
  if (a == b)
    return true;
  auto it_a = nodes.find(a);
  auto it_b = nodes.find(b);
  if (it_a == nodes.end() || it_b == nodes.end())
    return false;
  return it_a->second.in <= it_b->second.in &&
         it_a->second.out >= it_b->second.out;
}

} // namespace exodus::mid_ir