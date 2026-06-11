#include "dom.hpp"

namespace exodus::mid_ir {

auto DomTree::compute(LinearFunction &func) -> void {
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

auto DomTree::compute_df() -> void {
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

auto DomTree::dfs_dom_tree(Block *u, int &time) -> void {
  nodes[u].in = ++time;
  for (auto *v : nodes[u].children) {
    dfs_dom_tree(v, time);
  }
  nodes[u].out = time;
}

auto DomTree::reset(size_t n) -> void {
  nodes.clear();
  sdom.assign(n + 1, 0);
  idom.assign(n + 1, 0);
  parent.assign(n + 1, 0);
  vertex.assign(n + 1, nullptr);
  bucket.assign(n + 1, {});
  timer = 0;
}

auto DomTree::dfs(Block *u, Block *p) -> void {
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

auto DomTree::dominate(Block *a, Block *b) -> bool {
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