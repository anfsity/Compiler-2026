#include "reg_alloca.hpp"

#include "ra_printer.hpp"
#include <algorithm>
#include <cstdlib>
#include <list>

namespace exodus::riscv {
namespace {

auto is_virtual_reg(int reg) -> bool { return reg >= 128; }

auto is_use_operand(const low_ir::MachineOperand &operand) -> bool {
  return operand.is_reg() &&
         std::get<low_ir::MachineOperand::RegData>(operand.data).is_use;
}

auto allocable_int_regs(bool crosses_call) -> std::vector<int> {
  if (crosses_call)
    return {S1, S2, S3, S4, S5, S6, S7, S8, S9, S10, S11};

  // Keep t5/t6 and a7 available for spill and parallel-copy temporaries.
  return {T0, T1, T2, T3, T4, A0, A1, A2, A3, A4, A5, A6};
}

auto allocable_float_regs(bool crosses_call) -> std::vector<int> {
  if (crosses_call)
    return {FS0, FS1, FS2, FS3, FS4, FS5, FS6, FS7, FS8, FS9, FS10, FS11};
  return {FT0, FT1, FT2, FT3, FT4, FT5, FT6, FT7, FT8, FT9};
}

auto is_callee_saved_int_reg(int reg) -> bool {
  return reg == static_cast<int>(S1) ||
         (reg >= static_cast<int>(S2) && reg <= static_cast<int>(S8));
}

auto is_float_phys_reg(int reg) -> bool {
  return reg >= static_cast<int>(F0) && reg <= static_cast<int>(F31);
}

auto merge_reg_class(RegClass old_class, RegClass new_class) -> RegClass {
  if (old_class == RegClass::Unknown)
    return new_class;
  if (new_class == RegClass::Unknown)
    return old_class;
  return old_class == RegClass::Float || new_class == RegClass::Float
           ? RegClass::Float
           : RegClass::Int;
}

auto operand_reg_class(const low_ir::MachineInst &inst, size_t index)
  -> RegClass {
  switch (inst.opcode) {
  case FLW:
  case FSW:
    return index == 0 ? RegClass::Float : RegClass::Int;
  case LD:
  case SD:
    return RegClass::Int;
  case FADD_S:
  case FSUB_S:
  case FMUL_S:
  case FDIV_S:
  case FSQRT_S:
  case FSGNJ_S:
  case FSGNJN_S:
  case FSGNJX_S:
    return RegClass::Float;
  case FEQ_S:
  case FLT_S:
  case FLE_S:
  case FCVT_W_S:
  case FCVT_WU_S:
  case FMV_X_W:
    return index == 0 ? RegClass::Int : RegClass::Float;
  case FCVT_S_W:
  case FCVT_S_WU:
  case FMV_W_X:
    return index == 0 ? RegClass::Float : RegClass::Int;
  case COPY:
    for (const auto &operand : inst.operands) {
      if (operand.is_reg() && is_float_phys_reg(operand.get_reg())) {
        return RegClass::Float;
      }
    }
    for (const auto &operand : inst.operands) {
      if (operand.is_reg() && !is_virtual_reg(operand.get_reg())) {
        return RegClass::Int;
      }
    }
    return RegClass::Unknown;
  default:
    return RegClass::Int;
  }
}

auto operand_storage_size(const low_ir::MachineInst &inst, size_t index)
  -> int {
  switch (inst.opcode) {
  case LD:
  case SD:
  case LA:
    return 8;
  case ADDI:
    if (
      index == 0 && inst.operands.size() >= 3 &&
      inst.operands[2].kind == low_ir::MachineOperand::FrameIdx
    ) {
      return 8;
    }
  default:
    return 4;
  }
}

auto interval_start(const LiveInterval &interval) -> int {
  return interval.segments.empty() ? 0 : interval.segments.front().start;
}

auto interval_end(const LiveInterval &interval) -> int {
  return interval.segments.empty() ? 0 : interval.segments.back().end;
}

auto contains_pos(const LiveInterval &interval, int pos) -> bool {
  for (auto segment : interval.segments) {
    if (pos >= segment.start && pos < segment.end)
      return true;
  }
  return false;
}

auto find_piece(std::vector<LiveInterval> &intervals, int vreg, int pos)
  -> LiveInterval * {
  for (auto &interval : intervals) {
    if (interval.vreg == vreg && contains_pos(interval, pos)) {
      return &interval;
    }
  }
  return nullptr;
}

auto should_hint_copy(const low_ir::MachineInst &inst) -> bool {
  return inst.opcode == COPY && inst.operands.size() == 2 &&
         inst.operands[0].is_reg() && inst.operands[0].is_def() &&
         inst.operands[1].is_reg();
}

auto append_segment(LiveInterval &interval, int start, int end) -> void {
  if (start >= end)
    return;

  interval.segments.push_back({start, end});
}

auto normalize_segments(LiveInterval &interval) -> void {
  if (interval.segments.empty())
    return;

  std::sort(
    interval.segments.begin(), interval.segments.end(), [](auto lhs, auto rhs) {
      if (lhs.start != rhs.start)
        return lhs.start < rhs.start;
      return lhs.end < rhs.end;
    }
  );

  std::vector<LiveSegment> merged;
  merged.reserve(interval.segments.size());
  for (auto segment : interval.segments) {
    if (merged.empty() || segment.start > merged.back().end) {
      merged.push_back(segment);
      continue;
    }
    merged.back().end = std::max(merged.back().end, segment.end);
  }
  interval.segments = std::move(merged);
}

auto append_segment(LiveInterval &interval, int pos) -> void {
  append_segment(interval, pos, pos + 1);
}

auto make_spill_load(const LiveInterval &interval, int tmp_reg)
  -> low_ir::MachineInst {
  auto opcode = interval.reg_class == RegClass::Float ? FLW
                : interval.storage_size == 8          ? LD
                                                      : LW;
  return low_ir::MachineInst(opcode)
    .add_reg(tmp_reg, true, false)
    .add_fi(interval.spill_slot)
    .add_imm(0);
}

auto make_spill_store(const LiveInterval &interval, int tmp_reg)
  -> low_ir::MachineInst {
  auto opcode = interval.reg_class == RegClass::Float ? FSW
                : interval.storage_size == 8          ? SD
                                                      : SW;
  return low_ir::MachineInst(opcode)
    .add_reg(tmp_reg)
    .add_fi(interval.spill_slot)
    .add_imm(0);
}

} // namespace

namespace detail {

auto compute_block_layout(low_ir::MachineFunction &function)
  -> std::vector<BlockLayout> {
  std::vector<BlockLayout> layout;
  int pos = 0;

  for (auto &block : function.blocks) {
    auto start = pos;
    pos += static_cast<int>(block->insts.size());
    layout.push_back({block.get(), start, pos});
  }

  return layout;
}

auto compute_liveness(low_ir::MachineFunction &function) -> LivenessInfo {
  LivenessInfo liveness;
  std::unordered_map<low_ir::MachineBasicBlock *, std::unordered_set<int>>
    phi_edge_uses;

  for (auto &block : function.blocks) {
    auto *mbb = block.get();
    auto &use = liveness.use[mbb];
    auto &def = liveness.def[mbb];

    for (auto &inst : mbb->insts) {
      if (inst.opcode == PHI) {
        if (!inst.operands.empty() && inst.operands[0].is_reg()) {
          auto dst = inst.operands[0].get_reg();
          if (is_virtual_reg(dst)) {
            def.insert(dst);
          }
        }
        for (size_t i = 1; i + 1 < inst.operands.size(); i += 2) {
          if (
            !inst.operands[i].is_reg() ||
            inst.operands[i + 1].kind != low_ir::MachineOperand::MBB
          ) {
            continue;
          }
          auto src = inst.operands[i].get_reg();
          if (!is_virtual_reg(src))
            continue;
          auto *pred =
            std::get<low_ir::MachineBasicBlock *>(inst.operands[i + 1].data);
          phi_edge_uses[pred].insert(src);
        }
        continue;
      }

      for (const auto &operand : inst.operands) {
        if (!operand.is_reg())
          continue;

        auto reg = operand.get_reg();
        if (!is_virtual_reg(reg))
          continue;

        if (is_use_operand(operand) && !def.count(reg)) {
          use.insert(reg);
        }
        if (operand.is_def()) {
          def.insert(reg);
        }
      }
    }

    liveness.live_in[mbb];
    liveness.live_out[mbb];
  }

  // simple dce
  bool changed = true;
  while (changed) {
    changed = false;

    for (auto it = function.blocks.rbegin(); it != function.blocks.rend();
         ++it) {
      auto *mbb = it->get();
      std::unordered_set<int> next_out;
      for (auto *succ : mbb->succs) {
        auto &succ_live_in = liveness.live_in[succ];
        next_out.insert(succ_live_in.begin(), succ_live_in.end());
      }
      if (auto it = phi_edge_uses.find(mbb); it != phi_edge_uses.end()) {
        next_out.insert(it->second.begin(), it->second.end());
      }

      std::unordered_set<int> next_in = liveness.use[mbb];
      for (auto reg : next_out) {
        if (!liveness.def[mbb].count(reg)) {
          next_in.insert(reg);
        }
      }

      if (next_out != liveness.live_out[mbb]) {
        liveness.live_out[mbb] = std::move(next_out);
        changed = true;
      }
      if (next_in != liveness.live_in[mbb]) {
        liveness.live_in[mbb] = std::move(next_in);
        changed = true;
      }
    }
  }

  return liveness;
}

auto build_intervals(low_ir::MachineFunction &function)
  -> std::vector<LiveInterval> {
  std::unordered_map<int, LiveInterval> by_reg;
  auto layout = compute_block_layout(function);
  auto liveness = compute_liveness(function);

  // 逆序遍历
  for (const auto &block_layout : layout) {
    auto live = liveness.live_out[block_layout.block];
    auto pos = block_layout.end;
    for (auto it = block_layout.block->insts.rbegin();
         it != block_layout.block->insts.rend();
         ++it) {
      --pos;

      for (auto reg : live) {
        auto &interval = by_reg[reg];
        interval.vreg = reg;
        append_segment(interval, pos);
      }

      std::vector<int> defs;
      std::vector<int> uses;
      for (size_t operand_index = 0; operand_index < it->operands.size();
           ++operand_index) {
        if (it->opcode == PHI && operand_index > 0)
          continue;

        const auto &operand = it->operands[operand_index];
        if (!operand.is_reg())
          continue;

        auto reg = operand.get_reg();
        if (!is_virtual_reg(reg))
          continue;

        auto &interval = by_reg[reg];
        interval.vreg = reg;
        interval.reg_class = merge_reg_class(
          interval.reg_class, operand_reg_class(*it, operand_index)
        );
        interval.storage_size = std::max(
          interval.storage_size, operand_storage_size(*it, operand_index)
        );
        auto operand_pos =
          it->opcode == PHI && operand.is_def() ? block_layout.start : pos;
        if (it->opcode == PHI && operand.is_def()) {
          append_segment(interval, block_layout.start, pos + 1);
        } else {
          append_segment(interval, operand_pos);
        }
        if (operand.is_def()) {
          interval.def_positions.push_back(operand_pos);
          defs.push_back(reg);
        }
        if (is_use_operand(operand)) {
          interval.use_positions.push_back(pos);
          uses.push_back(reg);
        }
      }

      for (auto reg : defs) {
        live.erase(reg);
      }
      for (auto reg : uses) {
        live.insert(reg);
      }
    }
  }

  // 类型，大小传播
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto &block : function.blocks) {
      for (const auto &inst : block->insts) {
        if (
          inst.opcode == PHI && inst.operands.size() >= 3 &&
          inst.operands[0].is_reg()
        ) {
          auto dst = inst.operands[0].get_reg();
          if (!is_virtual_reg(dst))
            continue;
          for (size_t i = 1; i + 1 < inst.operands.size(); i += 2) {
            if (!inst.operands[i].is_reg())
              continue;
            auto src = inst.operands[i].get_reg();
            if (!is_virtual_reg(src))
              continue;

            auto reg_class =
              merge_reg_class(by_reg[dst].reg_class, by_reg[src].reg_class);
            if (by_reg[dst].reg_class != reg_class) {
              by_reg[dst].reg_class = reg_class;
              changed = true;
            }
            if (by_reg[src].reg_class != reg_class) {
              by_reg[src].reg_class = reg_class;
              changed = true;
            }

            auto width =
              std::max(by_reg[dst].storage_size, by_reg[src].storage_size);
            if (by_reg[dst].storage_size != width) {
              by_reg[dst].storage_size = width;
              changed = true;
            }
            if (by_reg[src].storage_size != width) {
              by_reg[src].storage_size = width;
              changed = true;
            }
          }
          continue;
        }

        if (
          inst.opcode == COPY && inst.operands.size() == 2 &&
          inst.operands[0].is_reg() && inst.operands[1].is_reg()
        ) {
          auto dst = inst.operands[0].get_reg();
          auto src = inst.operands[1].get_reg();
          if (!is_virtual_reg(dst) || !is_virtual_reg(src))
            continue;

          auto reg_class =
            merge_reg_class(by_reg[dst].reg_class, by_reg[src].reg_class);
          if (by_reg[dst].reg_class != reg_class) {
            by_reg[dst].reg_class = reg_class;
            changed = true;
          }
          if (by_reg[src].reg_class != reg_class) {
            by_reg[src].reg_class = reg_class;
            changed = true;
          }

          auto width =
            std::max(by_reg[dst].storage_size, by_reg[src].storage_size);
          if (by_reg[dst].storage_size != width) {
            by_reg[dst].storage_size = width;
            changed = true;
          }
          if (by_reg[src].storage_size != width) {
            by_reg[src].storage_size = width;
            changed = true;
          }
          continue;
        }

        if (
          (inst.opcode == ADD || inst.opcode == ADDI) &&
          inst.operands.size() >= 2 && inst.operands[0].is_reg()
        ) {
          auto dst = inst.operands[0].get_reg();
          if (!is_virtual_reg(dst))
            continue;

          auto width = by_reg[dst].storage_size;
          for (size_t i = 1; i < inst.operands.size(); ++i) {
            if (!inst.operands[i].is_reg())
              continue;
            auto src = inst.operands[i].get_reg();
            if (is_virtual_reg(src)) {
              width = std::max(width, by_reg[src].storage_size);
            }
          }

          if (by_reg[dst].storage_size != width) {
            by_reg[dst].storage_size = width;
            changed = true;
          }
          continue;
        }
      }
    }
  }

  // 合并区间
  std::vector<LiveInterval> intervals;
  intervals.reserve(by_reg.size());
  for (auto &[_, interval] : by_reg) {
    normalize_segments(interval);
    std::sort(interval.def_positions.begin(), interval.def_positions.end());
    std::sort(interval.use_positions.begin(), interval.use_positions.end());
    intervals.push_back(std::move(interval));
  }

  std::sort(intervals.begin(), intervals.end(), [](auto &lhs, auto &rhs) {
    auto lhs_start = lhs.segments.empty() ? 0 : lhs.segments.front().start;
    auto rhs_start = rhs.segments.empty() ? 0 : rhs.segments.front().start;
    if (lhs_start != rhs_start)
      return lhs_start < rhs_start;
    return lhs.vreg < rhs.vreg;
  });
  return intervals;
}

auto annotate_call_liveness(
  low_ir::MachineFunction &function, std::vector<LiveInterval> &intervals
) -> void {
  auto layout = compute_block_layout(function);
  std::vector<int> call_positions;
  for (const auto &block_layout : layout) {
    auto pos = block_layout.start;
    for (const auto &inst : block_layout.block->insts) {
      if (inst.opcode == CALL)
        call_positions.push_back(pos);
      ++pos;
    }
  }

  for (auto &interval : intervals) {
    interval.crosses_call =
      std::any_of(call_positions.begin(), call_positions.end(), [&](int pos) {
        return contains_pos(interval, pos);
      });
  }
}

auto collect_hints(low_ir::MachineFunction &function)
  -> std::unordered_map<int, std::vector<RegisterHint>> {
  std::unordered_map<int, std::vector<RegisterHint>> hints;
  int pos = 0;

  for (auto &block : function.blocks) {
    for (auto &inst : block->insts) {
      if (!should_hint_copy(inst)) {
        ++pos;
        continue;
      }

      auto dst = inst.operands[0].get_reg();
      auto src = inst.operands[1].get_reg();
      if (is_virtual_reg(dst) && !is_virtual_reg(src)) {
        hints[dst].push_back({src, 10, pos});
      }
      if (is_virtual_reg(src) && !is_virtual_reg(dst)) {
        hints[src].push_back({dst, 10, pos});
      }
      ++pos;
    }
  }

  return hints;
}

auto collect_fixed_reg_positions(low_ir::MachineFunction &function)
  -> std::unordered_map<int, std::vector<int>> {
  std::unordered_map<int, std::vector<int>> positions;
  int pos = 0;
  for (const auto &block : function.blocks) {
    for (const auto &inst : block->insts) {
      for (const auto &operand : inst.operands) {
        if (operand.is_reg() && !is_virtual_reg(operand.get_reg()))
          positions[operand.get_reg()].push_back(pos);
      }
      ++pos;
    }
  }
  return positions;
}

auto compute_spill_cost(LiveInterval &interval) -> void {
  interval.spill_cost.use_count =
    static_cast<int>(interval.use_positions.size());
  interval.spill_cost.def_count =
    static_cast<int>(interval.def_positions.size());
  interval.spill_cost.span = interval_end(interval) - interval_start(interval);
  interval.spill_cost.score = interval.spill_cost.use_count * 4 +
                              interval.spill_cost.def_count * 2 -
                              interval.spill_cost.span;
}

auto allocate_registers(
  std::vector<LiveInterval> &intervals,
  const std::unordered_map<int, std::vector<int>> &fixed_reg_positions,
  bool function_has_call
) -> void {
  std::vector<LiveInterval *> active;
  std::unordered_set<int> used_callee_saved;
  for (auto &interval : intervals) {
    active.erase(
      std::remove_if(
        active.begin(),
        active.end(),
        [&](auto *active_interval) {
          return interval_end(*active_interval) <= interval_start(interval);
        }
      ),
      active.end()
    );

    auto regs = interval.reg_class == RegClass::Float
                  ? allocable_float_regs(interval.crosses_call)
                  : allocable_int_regs(interval.crosses_call);
    auto conflicts_with_fixed_use = [&](int reg) {
      auto found = fixed_reg_positions.find(reg);
      if (found == fixed_reg_positions.end())
        return false;
      return std::any_of(
        found->second.begin(), found->second.end(), [&](int pos) {
          if (!contains_pos(interval, pos))
            return false;
          return std::none_of(
            interval.hints.begin(),
            interval.hints.end(),
            [&](const auto &hint) {
              return hint.reg == reg && hint.copy_pos == pos;
            }
          );
        }
      );
    };
    regs.erase(
      std::remove_if(regs.begin(), regs.end(), conflicts_with_fixed_use),
      regs.end()
    );
    const auto allowed_regs = regs;
    for (auto *active_interval : active) {
      if (active_interval->reg_class == interval.reg_class) {
        regs.erase(
          std::remove(regs.begin(), regs.end(), active_interval->assigned_reg),
          regs.end()
        );
      }
    }

    for (const auto &hint : interval.hints) {
      if (std::find(regs.begin(), regs.end(), hint.reg) != regs.end()) {
        interval.assigned_reg = hint.reg;
        break;
      }
    }

    if (interval.assigned_reg < 0 && !regs.empty()) {
      interval.assigned_reg = regs.front();
    }

    auto victim = active.end();
    if (interval.assigned_reg < 0) {
      for (auto it = active.begin(); it != active.end(); ++it) {
        if (
          (*it)->reg_class != interval.reg_class ||
          std::find(
            allowed_regs.begin(), allowed_regs.end(), (*it)->assigned_reg
          ) == allowed_regs.end()
        ) {
          continue;
        }
        if (
          victim == active.end() ||
          (*it)->spill_cost.score < (*victim)->spill_cost.score
        ) {
          victim = it;
        }
      }

      auto *would_spill = &interval;
      if (
        victim != active.end() &&
        (*victim)->spill_cost.score < interval.spill_cost.score
      ) {
        would_spill = *victim;
      }

      // A leaf may use s1-s8 only as a profitable alternative to spilling.
      if (interval.reg_class == RegClass::Int && !function_has_call) {
        std::vector<int> callee_regs{S1, S2, S3, S4, S5, S6, S7, S8};
        callee_regs.erase(
          std::remove_if(
            callee_regs.begin(),
            callee_regs.end(),
            [&](int reg) {
              if (conflicts_with_fixed_use(reg))
                return true;
              return std::any_of(
                active.begin(), active.end(), [&](const auto *active_interval) {
                  return active_interval->reg_class == RegClass::Int &&
                         active_interval->assigned_reg == reg;
                }
              );
            }
          ),
          callee_regs.end()
        );

        auto spill_memory_ops =
          would_spill->spill_cost.use_count + would_spill->spill_cost.def_count;
        auto callee_saved_cost = used_callee_saved.empty() ? 6 : 2;
        if (!callee_regs.empty() && spill_memory_ops > callee_saved_cost) {
          interval.assigned_reg = callee_regs.front();
          used_callee_saved.insert(interval.assigned_reg);
        }
      }

      if (
        interval.assigned_reg < 0 && victim != active.end() &&
        (*victim)->spill_cost.score < interval.spill_cost.score
      ) {
        interval.assigned_reg = (*victim)->assigned_reg;
        (*victim)->assigned_reg = -1;
        (*victim)->spilled = true;
        active.erase(victim);
      } else {
        interval.spilled = interval.assigned_reg < 0;
      }
    }
    if (!interval.spilled) {
      active.push_back(&interval);
      if (is_callee_saved_int_reg(interval.assigned_reg))
        used_callee_saved.insert(interval.assigned_reg);
    }
  }
}

auto choose_split_point(const LiveInterval &interval) -> SplitPoint {
  if (interval.segments.size() < 2)
    return {};

  return {
    interval.segments[1].start,
    SplitPoint::Kind::BlockBoundary,
  };
}

auto plan_split(const LiveInterval &interval) -> SplitPlan {
  auto point = choose_split_point(interval);
  if (
    interval.segments.size() < 2 ||
    point.pos <= interval.segments.front().end ||
    point.pos >= interval_end(interval)
  ) {
    return {};
  }

  return {true, point};
}

auto split_interval(const LiveInterval &interval) -> std::vector<LiveInterval> {
  std::vector<LiveInterval> pieces;
  std::vector<LiveInterval> pending{interval};
  while (!pending.empty()) {
    auto current = std::move(pending.back());
    pending.pop_back();

    auto split_plan = plan_split(current);
    if (!split_plan.should_split) {
      current.split_plan = {};
      pieces.push_back(std::move(current));
      continue;
    }

    LiveInterval left = current;
    LiveInterval right = current;
    left.segments.clear();
    left.def_positions.clear();
    left.use_positions.clear();
    right.segments.clear();
    right.def_positions.clear();
    right.use_positions.clear();

    for (auto segment : current.segments) {
      if (segment.end <= split_plan.point.pos) {
        left.segments.push_back(segment);
      } else if (segment.start >= split_plan.point.pos) {
        right.segments.push_back(segment);
      } else {
        left.segments.push_back({segment.start, split_plan.point.pos});
        right.segments.push_back({split_plan.point.pos, segment.end});
      }
    }

    for (auto pos : current.def_positions) {
      (pos < split_plan.point.pos ? left.def_positions : right.def_positions)
        .push_back(pos);
    }
    for (auto pos : current.use_positions) {
      (pos < split_plan.point.pos ? left.use_positions : right.use_positions)
        .push_back(pos);
    }

    if (!right.segments.empty()) {
      pending.push_back(std::move(right));
    }
    if (!left.segments.empty()) {
      pending.push_back(std::move(left));
    }
  }

  return pieces.empty() ? std::vector<LiveInterval>{interval} : pieces;
}

auto ensure_spill_slots(
  low_ir::MachineFunction &function, std::vector<LiveInterval> &intervals
) -> void;

auto insert_spills(
  low_ir::MachineFunction &function, std::vector<LiveInterval> &intervals
) -> void {
  ensure_spill_slots(function, intervals);

  int pos = 0;
  for (auto &block : function.blocks) {
    for (auto it = block->insts.begin(); it != block->insts.end();) {
      auto before = std::vector<low_ir::MachineInst>{};
      auto after = std::vector<low_ir::MachineInst>{};
      auto tmp_by_spill_slot = std::unordered_map<int, int>{};
      auto used_tmp_regs = std::unordered_set<int>{};

      if (it->opcode == PHI) {
        ++it;
        ++pos;
        continue;
      }

      auto assign_tmp = [&](LiveInterval &interval) -> int {
        if (
          auto found = tmp_by_spill_slot.find(interval.spill_slot);
          found != tmp_by_spill_slot.end()
        ) {
          return found->second;
        }

        std::vector<int> candidates;
        if (interval.reg_class == RegClass::Float) {
          candidates = {
            FT11, FT10, FA7, FA6, FA5, FA4, FA3, FA2, FA1, FA0,
            FT9,  FT8,  FT7, FT6, FT5, FT4, FT3, FT2, FT1, FT0,
          };
        } else {
          candidates = {
            T5,
            A7,
            T6,
            A6,
            A5,
            A4,
            A3,
            A2,
            A1,
            A0,
            T4,
            T3,
            T2,
            T1,
            T0,
          };
        }

        auto is_occupied = [&](int candidate) {
          if (used_tmp_regs.count(candidate) != 0)
            return true;
          for (const auto &other : intervals) {
            if (
              other.spilled || other.reg_class != interval.reg_class ||
              other.assigned_reg != candidate
            ) {
              continue;
            }
            if (contains_pos(other, pos))
              return true;
          }
          for (const auto &operand : it->operands) {
            if (operand.is_reg() && operand.get_reg() == candidate)
              return true;
          }
          return false;
        };

        auto candidate =
          std::find_if(candidates.begin(), candidates.end(), [&](int reg) {
            return !is_occupied(reg);
          });
        if (candidate == candidates.end())
          std::abort();
        auto tmp_reg = *candidate;
        used_tmp_regs.insert(tmp_reg);
        tmp_by_spill_slot[interval.spill_slot] = tmp_reg;
        return tmp_reg;
      };

      for (auto &operand : it->operands) {
        if (!operand.is_reg())
          continue;

        auto &reg_data =
          std::get<low_ir::MachineOperand::RegData>(operand.data);
        if (!is_virtual_reg(reg_data.id))
          continue;

        auto *interval = find_piece(intervals, reg_data.id, pos);
        if (!interval || !interval->spilled)
          continue;

        auto tmp_reg = assign_tmp(*interval);
        if (reg_data.is_use) {
          before.push_back(make_spill_load(*interval, tmp_reg));
          reg_data.id = tmp_reg;
        }
        if (reg_data.is_def) {
          reg_data.id = tmp_reg;
          after.push_back(make_spill_store(*interval, tmp_reg));
        }
      }

      for (auto &inst : before) {
        block->insts.insert(it, std::move(inst));
      }

      ++it;
      for (auto &inst : after) {
        it = block->insts.insert(it, std::move(inst));
        ++it;
      }
      ++pos;
    }
  }
}

auto apply_assignments(
  low_ir::MachineFunction &function, std::vector<LiveInterval> &intervals
) -> void {
  int pos = 0;
  for (auto &block : function.blocks) {
    for (auto &inst : block->insts) {
      for (auto &operand : inst.operands) {
        if (!operand.is_reg())
          continue;

        auto &reg_data =
          std::get<low_ir::MachineOperand::RegData>(operand.data);
        if (!is_virtual_reg(reg_data.id))
          continue;

        auto *interval = find_piece(intervals, reg_data.id, pos);
        if (interval && !interval->spilled && interval->assigned_reg >= 0) {
          reg_data.id = interval->assigned_reg;
        }
      }
      ++pos;
    }
  }
}

auto ensure_spill_slots(
  low_ir::MachineFunction &function, std::vector<LiveInterval> &intervals
) -> void {
  std::unordered_map<int, int> slot_by_vreg;
  for (auto &interval : intervals) {
    if (!interval.spilled)
      continue;

    if (
      auto found = slot_by_vreg.find(interval.vreg); found != slot_by_vreg.end()
    ) {
      interval.spill_slot = found->second;
      continue;
    }

    if (interval.spill_slot < 0) {
      auto align = interval.storage_size >= 8 ? 8 : 4;
      interval.spill_slot =
        function.add_spill_slot(interval.storage_size, align);
    }
    slot_by_vreg[interval.vreg] = interval.spill_slot;
  }
}

auto eliminate_spill_stores(low_ir::MachineFunction &function) -> void {
  for (auto &block : function.blocks) {
    std::unordered_map<int, std::list<low_ir::MachineInst>::iterator>
      last_store;
    for (auto it = block->insts.begin(); it != block->insts.end();) {
      auto is_stack_op = [&](int opcode) {
        return opcode == LW || opcode == SW || opcode == LD || opcode == SD ||
               opcode == FLW || opcode == FSW;
      };

      if (
        is_stack_op(it->opcode) && it->operands.size() >= 2 &&
        it->operands[1].kind == low_ir::MachineOperand::FrameIdx
      ) {
        auto slot = std::get<int>(it->operands[1].data);
        if (
          function.stack_slots[slot].kind !=
          low_ir::MachineFunction::FrameSlotKind::Spill
        ) {
          ++it;
          continue;
        }
        if (it->opcode == LW || it->opcode == LD || it->opcode == FLW) {
          last_store.erase(slot);
        } else {
          if (auto prev = last_store.find(slot); prev != last_store.end()) {
            block->insts.erase(prev->second);
            last_store.erase(prev);
          }
          last_store[slot] = it;
        }
      }

      ++it;
    }
  }
}

auto prepare_phi_operands(low_ir::MachineFunction &function) -> void {
  auto find_def_in_block =
    [](low_ir::MachineBasicBlock *block, int reg) -> low_ir::MachineInst * {
    for (auto it = block->insts.rbegin(); it != block->insts.rend(); ++it) {
      if (it->operands.empty() || !it->operands[0].is_reg())
        continue;
      if (it->operands[0].is_def() && it->operands[0].get_reg() == reg) {
        return &*it;
      }
    }
    return nullptr;
  };

  auto find_def_before = [](
                           low_ir::MachineBasicBlock *block,
                           std::list<low_ir::MachineInst>::iterator before,
                           int reg
                         ) -> low_ir::MachineInst * {
    for (auto it = std::make_reverse_iterator(before);
         it != block->insts.rend();
         ++it) {
      if (it->operands.empty() || !it->operands[0].is_reg())
        continue;
      if (it->operands[0].is_def() && it->operands[0].get_reg() == reg) {
        return &*it;
      }
    }
    return nullptr;
  };

  auto clone_materialization = [&](
                                 int dst,
                                 const low_ir::MachineInst &src,
                                 low_ir::MachineBasicBlock *block,
                                 std::list<low_ir::MachineInst>::iterator before
                               ) -> std::vector<low_ir::MachineInst> {
    std::vector<low_ir::MachineInst> insts;
    switch (src.opcode) {
    case LI:
      insts.push_back(
        low_ir::MachineInst(LI)
          .add_reg(dst, true, false)
          .add_imm(std::get<int>(src.operands[1].data))
      );
      break;
    case LA:
      insts.push_back(
        low_ir::MachineInst(LA)
          .add_reg(dst, true, false)
          .add_operand(
            low_ir::MachineOperand::symbol(
              std::get<std::string>(src.operands[1].data)
            )
          )
      );
      break;
    case FMV_W_X:
      if (src.operands.size() >= 2 && src.operands[1].is_reg()) {
        auto bits = src.operands[1].get_reg();
        auto *bits_def = find_def_before(block, before, bits);
        if (bits_def && bits_def->opcode == LI) {
          auto tmp = function.new_vreg();
          insts.push_back(
            low_ir::MachineInst(LI)
              .add_reg(tmp, true, false)
              .add_imm(std::get<int>(bits_def->operands[1].data))
          );
          insts.push_back(
            low_ir::MachineInst(FMV_W_X).add_reg(dst, true, false).add_reg(tmp)
          );
        }
      }
      break;
    default:
      break;
    }
    return insts;
  };

  for (auto &block : function.blocks) {
    for (auto it = block->insts.begin(); it != block->insts.end(); ++it) {
      if (it->opcode != PHI)
        continue;

      for (size_t i = 1; i + 1 < it->operands.size(); i += 2) {
        if (
          !it->operands[i].is_reg() ||
          it->operands[i + 1].kind != low_ir::MachineOperand::MBB
        ) {
          continue;
        }

        auto src = it->operands[i].get_reg();
        auto *pred =
          std::get<low_ir::MachineBasicBlock *>(it->operands[i + 1].data);
        if (find_def_in_block(pred, src))
          continue;

        auto *phi_block_def = find_def_before(block.get(), it, src);
        if (!phi_block_def)
          continue;

        auto tmp = function.new_vreg();
        auto cloned =
          clone_materialization(tmp, *phi_block_def, block.get(), it);
        if (cloned.empty())
          continue;

        auto insert_pos = pred->insts.end();
        if (insert_pos != pred->insts.begin()) {
          auto last = std::prev(insert_pos);
          if (last->opcode == JAL || last->opcode == RET) {
            insert_pos = last;
          }
        }
        for (auto &mi : cloned) {
          insert_pos = pred->insts.insert(insert_pos, std::move(mi));
          ++insert_pos;
        }
        std::get<low_ir::MachineOperand::RegData>(it->operands[i].data).id =
          tmp;
      }
    }
  }

  bool changed = true;
  while (changed) {
    changed = false;
    std::unordered_map<int, int> use_count;
    for (auto &block : function.blocks) {
      for (auto &inst : block->insts) {
        for (const auto &operand : inst.operands) {
          if (!operand.is_reg() || !is_use_operand(operand))
            continue;
          auto reg = operand.get_reg();
          if (is_virtual_reg(reg)) {
            ++use_count[reg];
          }
        }
      }
    }

    for (auto &block : function.blocks) {
      for (auto it = block->insts.begin(); it != block->insts.end();) {
        if (
          (it->opcode == LI || it->opcode == LA || it->opcode == FMV_W_X) &&
          !it->operands.empty() && it->operands[0].is_reg() &&
          it->operands[0].is_def() &&
          is_virtual_reg(it->operands[0].get_reg()) &&
          use_count[it->operands[0].get_reg()] == 0
        ) {
          it = block->insts.erase(it);
          changed = true;
          continue;
        }
        ++it;
      }
    }
  }
}

struct Location {
  enum class Kind : uint8_t {
    Invalid,
    Reg,
    Stack,
  };

  Kind kind = Kind::Invalid;
  int reg = -1;
  int slot = -1;
  RegClass reg_class = RegClass::Int;
  int storage_size = 4;
};

struct LocationMove {
  Location dst;
  Location src;
};

struct EdgeMoves {
  low_ir::MachineBasicBlock *from = nullptr;
  low_ir::MachineBasicBlock *to = nullptr;
  std::vector<LocationMove> moves;
};

auto same_location(const Location &lhs, const Location &rhs) -> bool {
  if (lhs.kind != rhs.kind)
    return false;
  if (lhs.kind == Location::Kind::Reg)
    return lhs.reg == rhs.reg;
  if (lhs.kind == Location::Kind::Stack)
    return lhs.slot == rhs.slot;
  return true;
}

auto stack_move_temp_location(RegClass reg_class, int storage_size)
  -> Location {
  return {
    Location::Kind::Reg,
    reg_class == RegClass::Float ? static_cast<int>(FT11)
                                 : static_cast<int>(T5),
    -1,
    reg_class,
    storage_size,
  };
}

auto cycle_temp_location(RegClass reg_class, int storage_size) -> Location {
  return {
    Location::Kind::Reg,
    reg_class == RegClass::Float ? static_cast<int>(FT10)
                                 : static_cast<int>(A7),
    -1,
    reg_class,
    storage_size,
  };
}

auto location_for(std::vector<LiveInterval> &intervals, int vreg, int pos)
  -> Location {
  auto *interval = find_piece(intervals, vreg, pos);
  if (!interval && pos > 0) {
    interval = find_piece(intervals, vreg, pos - 1);
  }
  if (!interval)
    return {};

  auto reg_class = interval->reg_class == RegClass::Unknown
                     ? RegClass::Int
                     : interval->reg_class;
  if (interval->spilled) {
    return {
      Location::Kind::Stack,
      -1,
      interval->spill_slot,
      reg_class,
      interval->storage_size,
    };
  }
  return {
    Location::Kind::Reg,
    interval->assigned_reg,
    -1,
    reg_class,
    interval->storage_size,
  };
}

auto stack_load_opcode(const Location &loc) -> int {
  if (loc.reg_class == RegClass::Float)
    return FLW;
  return loc.storage_size == 8 ? LD : LW;
}

auto stack_store_opcode(const Location &loc) -> int {
  if (loc.reg_class == RegClass::Float)
    return FSW;
  return loc.storage_size == 8 ? SD : SW;
}

auto emit_location_move(
  std::vector<low_ir::MachineInst> &out,
  const Location &dst,
  const Location &src
) -> void {
  if (same_location(dst, src))
    return;

  if (dst.kind == Location::Kind::Reg && src.kind == Location::Kind::Reg) {
    out.push_back(
      low_ir::MachineInst(COPY).add_reg(dst.reg, true, false).add_reg(src.reg)
    );
    return;
  }

  if (dst.kind == Location::Kind::Reg && src.kind == Location::Kind::Stack) {
    out.push_back(
      low_ir::MachineInst(stack_load_opcode(src))
        .add_reg(dst.reg, true, false)
        .add_fi(src.slot)
        .add_imm(0)
    );
    return;
  }

  if (dst.kind == Location::Kind::Stack && src.kind == Location::Kind::Reg) {
    out.push_back(
      low_ir::MachineInst(stack_store_opcode(dst))
        .add_reg(src.reg)
        .add_fi(dst.slot)
        .add_imm(0)
    );
    return;
  }

  if (dst.kind == Location::Kind::Stack && src.kind == Location::Kind::Stack) {
    auto tmp = stack_move_temp_location(src.reg_class, src.storage_size);
    emit_location_move(out, tmp, src);
    emit_location_move(out, dst, tmp);
  }
}

auto resolve_location_moves(std::vector<LocationMove> moves)
  -> std::vector<low_ir::MachineInst> {
  std::vector<low_ir::MachineInst> resolved;
  while (!moves.empty()) {
    moves.erase(
      std::remove_if(
        moves.begin(),
        moves.end(),
        [](const auto &move) { return same_location(move.dst, move.src); }
      ),
      moves.end()
    );
    if (moves.empty())
      break;

    auto ready =
      std::find_if(moves.begin(), moves.end(), [&](const auto &move) {
        return std::none_of(moves.begin(), moves.end(), [&](const auto &other) {
          return same_location(other.src, move.dst);
        });
      });

    if (ready != moves.end()) {
      emit_location_move(resolved, ready->dst, ready->src);
      moves.erase(ready);
      continue;
    }

    auto src = moves.front().src;
    // A cycle temporary must survive while other ready moves are emitted.
    // Stack-to-stack moves use a different scratch register so they cannot
    // overwrite the value saved here.
    auto tmp = cycle_temp_location(src.reg_class, src.storage_size);
    emit_location_move(resolved, tmp, src);
    for (auto &move : moves) {
      if (same_location(move.src, src)) {
        move.src = tmp;
      }
    }
  }
  return resolved;
}

auto collect_ssa_data_flow_moves(
  low_ir::MachineFunction &function, std::vector<LiveInterval> &intervals
) -> std::vector<EdgeMoves> {
  auto layout = compute_block_layout(function);
  auto liveness = compute_liveness(function);
  std::unordered_map<low_ir::MachineBasicBlock *, BlockLayout> by_block;
  for (auto block_layout : layout) {
    by_block[block_layout.block] = block_layout;
  }

  std::vector<EdgeMoves> edge_moves;
  auto add_edge_move = [&](
                         low_ir::MachineBasicBlock *from,
                         low_ir::MachineBasicBlock *to,
                         LocationMove move
                       ) {
    auto found =
      std::find_if(edge_moves.begin(), edge_moves.end(), [&](const auto &edge) {
        return edge.from == from && edge.to == to;
      });
    if (found == edge_moves.end()) {
      edge_moves.push_back({from, to, {}});
      found = std::prev(edge_moves.end());
    }
    found->moves.push_back(move);
  };

  for (auto &block : function.blocks) {
    auto from_layout = by_block[block.get()];
    auto src_pos = from_layout.end > from_layout.start ? from_layout.end - 1
                                                       : from_layout.start;
    for (auto *succ : block->succs) {
      auto to_layout = by_block[succ];
      std::vector<int> live_across;
      for (auto vreg : liveness.live_out[block.get()]) {
        if (liveness.live_in[succ].count(vreg)) {
          live_across.push_back(vreg);
        }
      }
      std::sort(live_across.begin(), live_across.end());

      for (auto vreg : live_across) {
        auto src = location_for(intervals, vreg, src_pos);
        auto dst = location_for(intervals, vreg, to_layout.start);
        if (
          src.kind != Location::Kind::Invalid &&
          dst.kind != Location::Kind::Invalid
        ) {
          add_edge_move(block.get(), succ, {dst, src});
        }
      }
    }
  }

  for (auto &block : function.blocks) {
    for (auto it = block->insts.begin(); it != block->insts.end(); ++it) {
      if (it->opcode != PHI) {
        continue;
      }

      if (!it->operands.empty() && it->operands[0].is_reg()) {
        auto dst_reg = it->operands[0].get_reg();
        auto dst =
          location_for(intervals, dst_reg, by_block[block.get()].start);
        for (size_t i = 1; i + 1 < it->operands.size(); i += 2) {
          if (
            !it->operands[i].is_reg() ||
            it->operands[i + 1].kind != low_ir::MachineOperand::MBB
          ) {
            continue;
          }

          auto src_reg = it->operands[i].get_reg();
          auto *pred =
            std::get<low_ir::MachineBasicBlock *>(it->operands[i + 1].data);
          auto pred_layout = by_block[pred];
          auto src_pos = pred_layout.end > pred_layout.start
                           ? pred_layout.end - 1
                           : pred_layout.start;
          auto src = location_for(intervals, src_reg, src_pos);
          if (
            dst.kind != Location::Kind::Invalid &&
            src.kind != Location::Kind::Invalid
          ) {
            add_edge_move(pred, block.get(), {dst, src});
          }
        }
      }
    }
  }

  return edge_moves;
}

auto replace_successor_target(
  low_ir::MachineBasicBlock *from, // NOLINT
  low_ir::MachineBasicBlock *old_to,
  low_ir::MachineBasicBlock *new_to
) -> void {
  for (auto &succ : from->succs) {
    if (succ == old_to) {
      succ = new_to;
    }
  }
  for (auto &inst : from->insts) {
    for (auto &operand : inst.operands) {
      if (operand.kind != low_ir::MachineOperand::MBB)
        continue;
      if (std::get<low_ir::MachineBasicBlock *>(operand.data) == old_to) {
        operand.data = new_to;
      }
    }
  }
}

auto replace_predecessor(
  low_ir::MachineBasicBlock *to, // NOLINT
  low_ir::MachineBasicBlock *old_from,
  low_ir::MachineBasicBlock *new_from
) -> void {
  for (auto &pred : to->preds) {
    if (pred == old_from) {
      pred = new_from;
    }
  }
}

auto insert_edge_moves(
  low_ir::MachineFunction &function,
  const EdgeMoves &edge,
  std::vector<low_ir::MachineInst> resolved
) -> void {
  if (resolved.empty())
    return;

  if (edge.from->succs.size() == 1) {
    auto insert_pos = edge.from->insts.end();
    if (insert_pos != edge.from->insts.begin()) {
      auto last = std::prev(insert_pos);
      if (last->opcode == JAL || last->opcode == RET) {
        insert_pos = last;
      }
    }
    for (auto &mi : resolved) {
      insert_pos = edge.from->insts.insert(insert_pos, std::move(mi));
      ++insert_pos;
    }
    return;
  }

  if (edge.to->preds.size() == 1) {
    auto insert_pos = edge.to->insts.begin();
    for (auto &mi : resolved) {
      insert_pos = edge.to->insts.insert(insert_pos, std::move(mi));
      ++insert_pos;
    }
    return;
  }

  auto split_id = static_cast<int>(function.blocks.size());
  auto split = std::make_unique<low_ir::MachineBasicBlock>(
    split_id, edge.to->name + "_resolve_" + std::to_string(edge.from->id)
  );
  auto *split_ptr = split.get();
  split_ptr->preds.push_back(edge.from);
  split_ptr->succs.push_back(edge.to);
  for (auto &mi : resolved) {
    split_ptr->insts.push_back(std::move(mi));
  }
  split_ptr->insts.emplace_back(JAL)
    .add_reg(ZERO, true, false)
    .add_mbb(edge.to);

  replace_successor_target(edge.from, edge.to, split_ptr);
  replace_predecessor(edge.to, edge.from, split_ptr);
  function.blocks.push_back(std::move(split));
}

auto resolve_ssa_data_flow(
  low_ir::MachineFunction &function, std::vector<EdgeMoves> edge_moves
) -> void {
  for (auto &block : function.blocks) {
    for (auto it = block->insts.begin(); it != block->insts.end();) {
      if (it->opcode == PHI) {
        it = block->insts.erase(it);
        continue;
      }
      ++it;
    }
  }

  for (auto &edge : edge_moves) {
    auto resolved = resolve_location_moves(std::move(edge.moves));
    insert_edge_moves(function, edge, std::move(resolved));
  }
}

auto resolve_parallel_copies(low_ir::MachineFunction &function) -> void {
  for (auto &block : function.blocks) {
    for (auto it = block->insts.begin(); it != block->insts.end();) {
      if (
        it->opcode == COPY && it->operands.size() == 2 &&
        it->operands[0].is_reg() && it->operands[1].is_reg() &&
        it->operands[0].get_reg() == it->operands[1].get_reg()
      ) {
        it = block->insts.erase(it);
        continue;
      }
      ++it;
    }
  }
}

using BlockSet = std::unordered_set<low_ir::MachineBasicBlock *>;

auto reachable_from(low_ir::MachineBasicBlock *root) -> BlockSet {
  BlockSet reachable;
  std::vector<low_ir::MachineBasicBlock *> worklist;
  if (root)
    worklist.push_back(root);
  while (!worklist.empty()) {
    auto *block = worklist.back();
    worklist.pop_back();
    if (!reachable.insert(block).second)
      continue;
    worklist.insert(worklist.end(), block->succs.begin(), block->succs.end());
  }
  return reachable;
}

auto plan_frame_region(low_ir::MachineFunction &function) -> void {
  if (function.blocks.empty())
    return;

  std::vector<low_ir::MachineBasicBlock *> call_blocks;
  BlockSet all_blocks;
  for (const auto &block : function.blocks) {
    all_blocks.insert(block.get());
    if (
      std::any_of(block->insts.begin(), block->insts.end(), [](const auto &mi) {
        return mi.opcode == CALL;
      })
    ) {
      call_blocks.push_back(block.get());
    }
  }
  if (call_blocks.empty())
    return;

  auto *entry = function.blocks.front().get();
  std::unordered_map<low_ir::MachineBasicBlock *, BlockSet> dominators;
  for (const auto &block : function.blocks)
    dominators[block.get()] =
      block.get() == entry ? BlockSet{entry} : all_blocks;

  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto &owned_block : function.blocks) {
      auto *block = owned_block.get();
      if (block == entry)
        continue;

      BlockSet next;
      if (!block->preds.empty()) {
        next = dominators[block->preds.front()];
        for (size_t i = 1; i < block->preds.size(); ++i) {
          const auto &pred_doms = dominators[block->preds[i]];
          for (auto it = next.begin(); it != next.end();) {
            if (pred_doms.count(*it) == 0)
              it = next.erase(it);
            else
              ++it;
          }
        }
      }
      next.insert(block);
      if (next != dominators[block]) {
        dominators[block] = std::move(next);
        changed = true;
      }
    }
  }

  auto common = dominators[call_blocks.front()];
  for (size_t i = 1; i < call_blocks.size(); ++i) {
    const auto &call_doms = dominators[call_blocks[i]];
    for (auto it = common.begin(); it != common.end();) {
      if (call_doms.count(*it) == 0)
        it = common.erase(it);
      else
        ++it;
    }
  }

  auto *root = entry;
  for (auto *candidate : common) {
    if (dominators[candidate].size() > dominators[root].size())
      root = candidate;
  }

  auto active = reachable_from(root);
  auto safe = root != entry &&
              std::all_of(active.begin(), active.end(), [&](auto *block) {
                return dominators[block].count(root) != 0;
              });
  safe =
    safe &&
    std::none_of(root->insts.begin(), root->insts.end(), [](const auto &mi) {
      return mi.opcode == PHI;
    });
  safe = safe &&
         std::none_of(root->preds.begin(), root->preds.end(), [&](auto *pred) {
           return active.count(pred) != 0;
         });
  if (!safe) {
    root = entry;
    active = reachable_from(entry);
  }

  if (root != entry) {
    auto liveness = compute_liveness(function);
    std::vector<int> live_in(
      liveness.live_in[root].begin(), liveness.live_in[root].end()
    );
    std::sort(live_in.begin(), live_in.end());

    std::unordered_map<int, int> split_regs;
    for (auto old_reg : live_in) {
      auto storage_size = 4;
      if (
        auto it = function.vreg_storage_sizes.find(old_reg);
        it != function.vreg_storage_sizes.end()
      ) {
        storage_size = it->second;
      }
      split_regs.emplace(old_reg, function.new_vreg(storage_size));
    }

    for (auto *block : active) {
      for (auto &mi : block->insts) {
        for (auto &operand : mi.operands) {
          if (!is_use_operand(operand))
            continue;
          auto &reg = std::get<low_ir::MachineOperand::RegData>(operand.data);
          if (auto it = split_regs.find(reg.id); it != split_regs.end())
            reg.id = it->second;
        }
      }
    }

    for (auto it = live_in.rbegin(); it != live_in.rend(); ++it) {
      root->insts.emplace_front(COPY)
        .add_reg(split_regs.at(*it), true, false)
        .add_reg(*it);
    }
  }
  root->insts.emplace_front(PROLOGUE);
}

auto is_callee_saved_reg(int reg) -> bool {
  return reg == static_cast<int>(S1) ||
         (reg >= static_cast<int>(S2) && reg <= static_cast<int>(S11)) ||
         reg == static_cast<int>(FS0) || reg == static_cast<int>(FS1) ||
         (reg >= static_cast<int>(FS2) && reg <= static_cast<int>(FS11));
}

auto block_requires_frame(const low_ir::MachineBasicBlock &block) -> bool {
  for (const auto &inst : block.insts) {
    if (inst.opcode == CALL)
      return true;

    for (const auto &operand : inst.operands) {
      if (operand.kind == low_ir::MachineOperand::FrameIdx)
        return true;
      if (operand.is_reg() && is_callee_saved_reg(operand.get_reg()))
        return true;
    }
  }
  return false;
}

auto finalize_frame_region(low_ir::MachineFunction &function) -> void {
  if (function.blocks.empty())
    return;

  auto *entry = function.blocks.front().get();
  low_ir::MachineBasicBlock *root = nullptr;
  for (auto &block : function.blocks) {
    for (auto it = block->insts.begin(); it != block->insts.end();) {
      if (it->opcode == PROLOGUE) {
        if (!root)
          root = block.get();
        it = block->insts.erase(it);
      } else {
        if (it->opcode == RET_NOFRAME)
          it->opcode = RET;
        ++it;
      }
    }
  }

  auto active = reachable_from(root);
  auto needs_entry_frame = false;
  for (const auto &block : function.blocks) {
    if (block_requires_frame(*block) && active.count(block.get()) == 0) {
      needs_entry_frame = true;
      break;
    }
  }

  if (
    needs_entry_frame ||
    (!root && std::any_of(
                function.blocks.begin(),
                function.blocks.end(),
                [](const auto &block) { return block_requires_frame(*block); }
              ))
  ) {
    root = entry;
    active = reachable_from(entry);
  }

  if (root)
    root->insts.emplace_front(PROLOGUE);

  for (auto &block : function.blocks) {
    if (active.count(block.get()) != 0)
      continue;
    for (auto &mi : block->insts) {
      if (mi.opcode == RET)
        mi.opcode = RET_NOFRAME;
    }
  }
}

} // namespace detail

auto run_ra(low_ir::MachineFunction &function, bool dump_ra, bool emit_ra)
  -> void {
  RegAllocPrinter printer{dump_ra};
  detail::prepare_phi_operands(function);
  detail::plan_frame_region(function);

  auto intervals = detail::build_intervals(function);
  for (auto &interval : intervals) {
    if (
      auto it = function.vreg_storage_sizes.find(interval.vreg);
      it != function.vreg_storage_sizes.end()
    ) {
      interval.storage_size = std::max(interval.storage_size, it->second);
    }
  }
  auto hints = detail::collect_hints(function);
  auto fixed_reg_positions = detail::collect_fixed_reg_positions(function);

  for (auto &interval : intervals) {
    if (auto it = hints.find(interval.vreg); it != hints.end()) {
      interval.hints = std::move(it->second);
    }
    detail::compute_spill_cost(interval);
    interval.split_plan = detail::plan_split(interval);
  }

  printer.dump_intervals(function, "intervals", intervals);

  std::vector<LiveInterval> split_intervals;
  split_intervals.reserve(intervals.size());
  for (const auto &interval : intervals) {
    // Keep one location for a virtual value across CFG edges.  The current
    // edge-move resolver does not model every split boundary, and a split
    // value can otherwise reach a successor through an unmaterialized
    // physical register.  Register preference still handles short-lived
    // values without sacrificing this invariant.
    split_intervals.push_back(interval);
  }
  std::sort(
    split_intervals.begin(),
    split_intervals.end(),
    [](const auto &lhs, const auto &rhs) {
      if (interval_start(lhs) != interval_start(rhs))
        return interval_start(lhs) < interval_start(rhs);
      if (interval_end(lhs) != interval_end(rhs))
        return interval_end(lhs) < interval_end(rhs);
      return lhs.vreg < rhs.vreg;
    }
  );
  detail::annotate_call_liveness(function, split_intervals);
  for (auto &interval : split_intervals) {
    detail::compute_spill_cost(interval);
    interval.split_plan = detail::plan_split(interval);
  }
  printer.dump_intervals(function, "split", split_intervals);

  auto function_has_call = std::any_of(
    function.blocks.begin(), function.blocks.end(), [](const auto &block) {
      return std::any_of(
        block->insts.begin(), block->insts.end(), [](auto &inst) {
          return inst.opcode == CALL;
        }
      );
    }
  );
  detail::allocate_registers(
    split_intervals, fixed_reg_positions, function_has_call
  );
  printer.dump_intervals(function, "allocated", split_intervals);

  if (!emit_ra) {
    return;
  }

  detail::ensure_spill_slots(function, split_intervals);
  auto ssa_moves =
    detail::collect_ssa_data_flow_moves(function, split_intervals);
  detail::apply_assignments(function, split_intervals);
  detail::insert_spills(function, split_intervals);
  detail::resolve_ssa_data_flow(function, std::move(ssa_moves));
  detail::eliminate_spill_stores(function);
  detail::resolve_parallel_copies(function);
  detail::finalize_frame_region(function);

  printer.dump_function(function);
}
} // namespace exodus::riscv
