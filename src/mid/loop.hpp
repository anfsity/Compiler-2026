#pragma once

#include "dom.hpp"
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace exodus::mid_ir {

class Loop {
public:
  using BackEdge = std::pair<Block *, Block *>;

  auto get_header() const -> Block * { return header; }
  auto get_preheader() const -> Block * { return preheader; }
  auto get_parent() const -> Loop * { return parent; }
  auto get_depth() const -> unsigned { return depth; }

  auto get_back_edges() const -> const std::vector<BackEdge> & {
    return back_edges;
  }
  auto get_blocks() const -> const std::unordered_set<Block *> & {
    return blocks;
  }
  auto get_subloops() const -> const std::vector<Loop *> & { return subloops; }
  auto get_exiting_blocks() const -> const std::vector<Block *> & {
    return exiting_blocks;
  }
  auto get_exit_blocks() const -> const std::vector<Block *> & {
    return exit_blocks;
  }
  auto get_return_blocks() const -> const std::vector<Block *> & {
    return return_blocks;
  }

  auto contains(Block *block) const -> bool { return blocks.count(block) != 0; }
  auto contains(const Loop *other) const -> bool;

private:
  friend class LoopInfo;

  Block *header = nullptr;
  Block *preheader = nullptr;
  Loop *parent = nullptr;
  unsigned depth = 1;
  std::vector<BackEdge> back_edges;
  std::unordered_set<Block *> blocks;
  std::vector<Loop *> subloops;
  std::vector<Block *> exiting_blocks;
  std::vector<Block *> exit_blocks;
  std::vector<Block *> return_blocks;
};

class LoopInfo {
public:
  auto compute(LinearFunction &func, DomTree &dom) -> void;

  auto get_loops() const -> std::vector<Loop *>;
  auto get_loops_innermost_first() const -> std::vector<Loop *>;
  auto get_top_level_loops() const -> std::vector<Loop *>;
  auto get_loop_for(Block *block) const -> Loop *;
  auto is_reachable(Block *block) const -> bool {
    return reachable.count(block) != 0;
  }

private:
  auto collect_reachable(LinearFunction &func) -> void;
  auto build_natural_loops(LinearFunction &func, DomTree &dom) -> void;
  auto compute_loop_metadata(LinearFunction &func) -> void;
  auto compute_loop_nesting() -> void;

  std::vector<std::unique_ptr<Loop>> loops;
  std::unordered_set<Block *> reachable;
  std::unordered_map<Block *, Loop *> block_loops;
};

struct LoopAnalysis {
  using Result = LoopInfo;

  auto run(LinearFunction &func, exodus::opt::LinearFunctionAnalysisManager &am)
    -> Result {
    auto &dom = am.get_result<DominanceAnalysis>(func);
    LoopInfo info;
    info.compute(func, dom);
    return info;
  }
};

} // namespace exodus::mid_ir
