#include "cfg_analysis.hpp"

namespace exodus::low_ir {

auto DominatorTree::compute(const MachineFunction &function) -> void {
  dfn.clear();
  timer = 0;
  entry = function.blocks.empty() ? nullptr : function.blocks.front().get();
  auto size = function.blocks.size() + 1;
  vertex.assign(size, nullptr);
  parent.assign(size, 0);
  sdom.assign(size, 0);
  idom_by_dfn.assign(size, 0);
  ancestor.assign(size, 0);
  label.assign(size, 0);
  bucket.assign(size, {});
  if (!entry)
    return;

  dfs(entry, 0);
  if (timer == 0)
    return;

  parent[1] = 1;
  for (int i = 1; i <= timer; ++i) {
    sdom[i] = i;
    idom_by_dfn[i] = i;
    ancestor[i] = i;
    label[i] = i;
  }

  for (int i = timer; i >= 2; --i) {
    auto *block = vertex[i];
    for (auto *pred : block->preds) {
      auto found = dfn.find(pred);
      if (found == dfn.end())
        continue;
      auto u = eval(found->second);
      if (sdom[u] < sdom[i])
        sdom[i] = sdom[u];
    }

    bucket[sdom[i]].push_back(i);
    link(parent[i], i);
    for (auto v : bucket[parent[i]]) {
      auto u = eval(v);
      idom_by_dfn[v] = sdom[u] < sdom[v] ? u : parent[i];
    }
    bucket[parent[i]].clear();
  }

  for (int i = 2; i <= timer; ++i) {
    if (idom_by_dfn[i] != sdom[i])
      idom_by_dfn[i] = idom_by_dfn[idom_by_dfn[i]];
  }
}

auto DominatorTree::is_reachable(MachineBasicBlock *block) const -> bool {
  return dfn.find(block) != dfn.end();
}

auto DominatorTree::idom(MachineBasicBlock *block) const
  -> MachineBasicBlock * {
  auto found = dfn.find(block);
  if (found == dfn.end() || found->second == 1)
    return nullptr;
  return vertex[idom_by_dfn[found->second]];
}

auto DominatorTree::dominates(
  MachineBasicBlock *dominator, MachineBasicBlock *block
) const -> bool {
  if (dominator == block)
    return is_reachable(block);

  auto found = dfn.find(block);
  if (found == dfn.end() || !is_reachable(dominator))
    return false;

  for (auto current = found->second; current > 0;) {
    if (vertex[current] == dominator)
      return true;
    if (current == 1)
      break;
    current = idom_by_dfn[current];
  }
  return false;
}

auto DominatorTree::dfs(MachineBasicBlock *block, int parent_index) -> void {
  if (dfn.count(block) != 0)
    return;
  auto index = ++timer;
  dfn[block] = index;
  vertex[index] = block;
  parent[index] = parent_index;
  for (auto *succ : block->succs)
    dfs(succ, index);
}

auto DominatorTree::compress(int node) -> void {
  if (ancestor[ancestor[node]] == ancestor[node])
    return;
  compress(ancestor[node]);
  if (sdom[label[ancestor[node]]] < sdom[label[node]])
    label[node] = label[ancestor[node]];
  ancestor[node] = ancestor[ancestor[node]];
}

auto DominatorTree::eval(int node) -> int {
  if (ancestor[node] == node)
    return label[node];
  compress(node);
  return label[node];
}

auto DominatorTree::link(int from, int to) -> void { ancestor[to] = from; }

auto LoopInfo::compute(
  const MachineFunction &function, const DominatorTree &dominator_tree
) -> void {
  loops.clear();

  std::unordered_map<MachineBasicBlock *, std::vector<MachineBasicBlock *>>
    latches;
  std::vector<MachineBasicBlock *> headers;
  for (const auto &owned_block : function.blocks) {
    auto *latch = owned_block.get();
    if (!dominator_tree.is_reachable(latch))
      continue;

    for (auto *header : latch->succs) {
      if (
        !dominator_tree.is_reachable(header) ||
        !dominator_tree.dominates(header, latch)
      ) {
        continue;
      }
      if (latches.count(header) == 0)
        headers.push_back(header);
      latches[header].push_back(latch);
    }
  }

  for (auto *header : headers) {
    NaturalLoop loop;
    loop.header = header;
    loop.blocks.insert(header);
    std::vector<MachineBasicBlock *> worklist;
    for (auto *latch : latches[header]) {
      if (loop.blocks.insert(latch).second && latch != header)
        worklist.push_back(latch);
    }

    while (!worklist.empty()) {
      auto *block = worklist.back();
      worklist.pop_back();
      for (auto *pred : block->preds) {
        if (
          dominator_tree.is_reachable(pred) &&
          loop.blocks.insert(pred).second && pred != header
        ) {
          worklist.push_back(pred);
        }
      }
    }
    loops.push_back(std::move(loop));
  }
}

} // namespace exodus::low_ir
