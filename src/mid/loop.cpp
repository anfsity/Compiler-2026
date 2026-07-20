#include "loop.hpp"

#include <algorithm>

namespace exodus::mid_ir {

auto Loop::contains(const Loop *other) const -> bool {
  if (!other)
    return false;
  return std::all_of(
    other->blocks.begin(), other->blocks.end(), [this](Block *block) {
      return contains(block);
    }
  );
}

auto LoopInfo::compute(LinearFunction &func, DomTree &dom) -> void {
  loops.clear();
  reachable.clear();
  block_loops.clear();

  if (func.blocks.empty())
    return;

  collect_reachable(func);
  build_natural_loops(func, dom);
  compute_loop_metadata(func);
  compute_loop_nesting();
}

auto LoopInfo::collect_reachable(LinearFunction &func) -> void {
  std::vector<Block *> worklist{func.blocks.front().get()};
  while (!worklist.empty()) {
    Block *block = worklist.back();
    worklist.pop_back();
    if (!reachable.insert(block).second)
      continue;
    worklist.insert(worklist.end(), block->succs.begin(), block->succs.end());
  }
}

auto LoopInfo::build_natural_loops(LinearFunction &func, DomTree &dom) -> void {
  std::unordered_map<Block *, std::vector<Block *>> header_latches;
  std::vector<Block *> headers;

  for (auto &block_ptr : func.blocks) {
    Block *latch = block_ptr.get();
    if (!is_reachable(latch))
      continue;

    for (auto *header : latch->succs) {
      if (!is_reachable(header) || !dom.dominate(header, latch))
        continue;
      if (!header_latches.count(header))
        headers.push_back(header);
      header_latches[header].push_back(latch);
    }
  }

  for (auto *header : headers) {
    auto loop = std::make_unique<Loop>();
    loop->header = header;
    loop->blocks.insert(header);

    std::vector<Block *> worklist;
    for (auto *latch : header_latches[header]) {
      loop->back_edges.push_back({latch, header});
      if (loop->blocks.insert(latch).second && latch != header)
        worklist.push_back(latch);
    }

    while (!worklist.empty()) {
      Block *block = worklist.back();
      worklist.pop_back();
      for (auto *pred : block->preds) {
        if (!is_reachable(pred))
          continue;
        if (loop->blocks.insert(pred).second && pred != header)
          worklist.push_back(pred);
      }
    }

    loops.push_back(std::move(loop));
  }
}

auto LoopInfo::compute_loop_metadata(LinearFunction &func) -> void {
  for (auto &loop_ptr : loops) {
    Loop &loop = *loop_ptr;
    std::vector<Block *> outside_preds;

    for (auto *pred : loop.header->preds) {
      if (
        !loop.contains(pred) && is_reachable(pred) &&
        std::find(outside_preds.begin(), outside_preds.end(), pred) ==
          outside_preds.end()
      ) {
        outside_preds.push_back(pred);
      }
    }

    if (outside_preds.size() == 1) {
      Block *candidate = outside_preds.front();
      std::vector<Block *> unique_successors;
      for (auto *successor : candidate->succs) {
        if (
          std::find(
            unique_successors.begin(), unique_successors.end(), successor
          ) == unique_successors.end()
        ) {
          unique_successors.push_back(successor);
        }
      }
      if (
        unique_successors.size() == 1 &&
        unique_successors.front() == loop.header
      )
        loop.preheader = candidate;
    }

    for (auto &block_ptr : func.blocks) {
      Block *block = block_ptr.get();
      if (!loop.contains(block))
        continue;

      bool exits_loop = false;
      for (auto *successor : block->succs) {
        if (loop.contains(successor))
          continue;
        exits_loop = true;
        if (
          std::find(
            loop.exit_blocks.begin(), loop.exit_blocks.end(), successor
          ) == loop.exit_blocks.end()
        ) {
          loop.exit_blocks.push_back(successor);
        }
      }
      if (exits_loop)
        loop.exiting_blocks.push_back(block);

      if (!block->insts.empty() && block->insts.back()->code == OpCode::Ret)
        loop.return_blocks.push_back(block);
    }
  }
}

auto LoopInfo::compute_loop_nesting() -> void {
  for (auto &loop_ptr : loops) {
    Loop *loop = loop_ptr.get();
    Loop *parent = nullptr;

    for (auto &candidate_ptr : loops) {
      Loop *candidate = candidate_ptr.get();
      if (
        candidate == loop || candidate->blocks.size() <= loop->blocks.size() ||
        !candidate->contains(loop)
      ) {
        continue;
      }
      if (!parent || candidate->blocks.size() < parent->blocks.size())
        parent = candidate;
    }

    loop->parent = parent;
    if (parent)
      parent->subloops.push_back(loop);
  }

  for (auto &loop_ptr : loops) {
    unsigned depth = 1;
    for (Loop *parent = loop_ptr->parent; parent; parent = parent->parent)
      ++depth;
    loop_ptr->depth = depth;
  }

  for (auto &loop_ptr : loops) {
    Loop *loop = loop_ptr.get();
    for (auto *block : loop->blocks) {
      auto it = block_loops.find(block);
      if (it == block_loops.end() || it->second->depth < loop->depth)
        block_loops[block] = loop;
    }
  }
}

auto LoopInfo::get_loops() const -> std::vector<Loop *> {
  std::vector<Loop *> result;
  result.reserve(loops.size());
  for (const auto &loop : loops)
    result.push_back(loop.get());
  return result;
}

auto LoopInfo::get_loops_innermost_first() const -> std::vector<Loop *> {
  auto result = get_loops();
  std::stable_sort(result.begin(), result.end(), [](Loop *lhs, Loop *rhs) {
    if (lhs->get_depth() != rhs->get_depth())
      return lhs->get_depth() > rhs->get_depth();
    return lhs->get_blocks().size() < rhs->get_blocks().size();
  });
  return result;
}

auto LoopInfo::get_top_level_loops() const -> std::vector<Loop *> {
  std::vector<Loop *> result;
  for (const auto &loop : loops) {
    if (!loop->get_parent())
      result.push_back(loop.get());
  }
  return result;
}

auto LoopInfo::get_loop_for(Block *block) const -> Loop * {
  auto it = block_loops.find(block);
  return it == block_loops.end() ? nullptr : it->second;
}

} // namespace exodus::mid_ir
