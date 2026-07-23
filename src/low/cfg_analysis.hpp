#pragma once

#include "ir.hpp"
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace exodus::low_ir {

class DominatorTree {
public:
  auto compute(const MachineFunction &function) -> void;

  auto is_reachable(MachineBasicBlock *block) const -> bool;
  auto idom(MachineBasicBlock *block) const -> MachineBasicBlock *;
  auto dominates(MachineBasicBlock *dominator, MachineBasicBlock *block) const
    -> bool;

private:
  auto dfs(MachineBasicBlock *block, int parent_index) -> void;
  auto compress(int node) -> void;
  auto eval(int node) -> int;
  auto link(int from, int to) -> void;

  std::unordered_map<MachineBasicBlock *, int> dfn;
  std::vector<MachineBasicBlock *> vertex;
  std::vector<int> parent;
  std::vector<int> sdom;
  std::vector<int> idom_by_dfn;
  std::vector<int> ancestor;
  std::vector<int> label;
  std::vector<std::vector<int>> bucket;
  MachineBasicBlock *entry = nullptr;
  int timer = 0;
};

struct NaturalLoop {
  MachineBasicBlock *header = nullptr;
  std::unordered_set<MachineBasicBlock *> blocks;

  auto contains(MachineBasicBlock *block) const -> bool {
    return blocks.count(block) != 0;
  }
};

class LoopInfo {
public:
  auto
  compute(const MachineFunction &function, const DominatorTree &dominator_tree)
    -> void;

  auto get_loops() const -> const std::vector<NaturalLoop> & { return loops; }

private:
  std::vector<NaturalLoop> loops;
};

} // namespace exodus::low_ir
