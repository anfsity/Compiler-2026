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

auto allocable_int_regs() -> std::vector<int> {
  return {S1, S2, S3, S4, S5, S6, S7, S8, S9, S10, S11};
}

auto allocable_float_regs() -> std::vector<int> {
  return {FS0, FS1, FS2, FS3, FS4, FS5, FS6, FS7, FS8, FS9, FS10, FS11};
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
    return 4;
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

auto spill_tmp_regs_for(RegClass reg_class) -> const std::vector<int> & {
  static const std::vector<int> int_regs = {T6, T5, T4, T3, T2, T1, T0};
  static const std::vector<int> float_regs = {
    FT11, FT10, FT9, FT8, FT7, FT6, FT5, FT4, FT3, FT2, FT1, FT0
  };
  return reg_class == RegClass::Float ? float_regs : int_regs;
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
        append_segment(interval, operand_pos);
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

auto collect_hints(low_ir::MachineFunction &function)
  -> std::unordered_map<int, std::vector<RegisterHint>> {
  std::unordered_map<int, std::vector<RegisterHint>> hints;

  for (auto &block : function.blocks) {
    for (auto &inst : block->insts) {
      if (!should_hint_copy(inst))
        continue;

      auto dst = inst.operands[0].get_reg();
      auto src = inst.operands[1].get_reg();
      if (is_virtual_reg(dst) && !is_virtual_reg(src)) {
        hints[dst].push_back({src, 10});
      }
      if (is_virtual_reg(src) && !is_virtual_reg(dst)) {
        hints[src].push_back({dst, 10});
      }
    }
  }

  return hints;
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

auto allocate_registers(std::vector<LiveInterval> &intervals) -> void {
  std::vector<LiveInterval *> active;
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

    auto regs = interval.reg_class == RegClass::Float ? allocable_float_regs()
                                                      : allocable_int_regs();
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

    if (interval.assigned_reg < 0) {
      auto victim = std::min_element(
        active.begin(), active.end(), [&](auto *lhs, auto *rhs) {
          if (lhs->reg_class != interval.reg_class)
            return false;
          if (rhs->reg_class != interval.reg_class)
            return true;
          return lhs->spill_cost.score < rhs->spill_cost.score;
        }
      );
      if (
        victim != active.end() && (*victim)->reg_class != interval.reg_class
      ) {
        victim = active.end();
      }

      if (
        victim != active.end() &&
        (*victim)->spill_cost.score < interval.spill_cost.score
      ) {
        interval.assigned_reg = (*victim)->assigned_reg;
        (*victim)->assigned_reg = -1;
        (*victim)->spilled = true;
        active.erase(victim);
        active.push_back(&interval);
      } else {
        interval.spilled = true;
      }
    } else {
      active.push_back(&interval);
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
  (void)interval;
  return {};
}

auto split_interval(const LiveInterval &interval) -> std::vector<LiveInterval> {
  auto split_plan = plan_split(interval);
  if (!split_plan.should_split)
    return {interval};

  LiveInterval left = interval;
  LiveInterval right = interval;
  left.segments.clear();
  left.def_positions.clear();
  left.use_positions.clear();
  right.segments.clear();
  right.def_positions.clear();
  right.use_positions.clear();

  for (auto segment : interval.segments) {
    if (segment.end <= split_plan.point.pos) {
      left.segments.push_back(segment);
    } else if (segment.start >= split_plan.point.pos) {
      right.segments.push_back(segment);
    } else {
      left.segments.push_back({segment.start, split_plan.point.pos});
      right.segments.push_back({split_plan.point.pos, segment.end});
    }
  }

  for (auto pos : interval.def_positions) {
    if (pos < split_plan.point.pos) {
      left.def_positions.push_back(pos);
    } else {
      right.def_positions.push_back(pos);
    }
  }
  for (auto pos : interval.use_positions) {
    if (pos < split_plan.point.pos) {
      left.use_positions.push_back(pos);
    } else {
      right.use_positions.push_back(pos);
    }
  }

  std::vector<LiveInterval> pieces;
  if (!left.segments.empty()) {
    pieces.push_back(std::move(left));
  }
  if (!right.segments.empty()) {
    pieces.push_back(std::move(right));
  }
  return pieces.empty() ? std::vector<LiveInterval>{interval}
                        : std::move(pieces);
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
      auto next_int_tmp = size_t{0};
      auto next_float_tmp = size_t{0};

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

        const auto &tmp_regs = spill_tmp_regs_for(interval.reg_class);
        auto &next_tmp =
          interval.reg_class == RegClass::Float ? next_float_tmp : next_int_tmp;
        auto tmp_reg =
          tmp_regs[std::min(next_tmp, tmp_regs.size() - size_t{1})];
        if (next_tmp + 1 < tmp_regs.size()) {
          ++next_tmp;
        }
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
  for (auto &interval : intervals) {
    if (interval.spilled && interval.spill_slot < 0) {
      auto align = interval.storage_size >= 8 ? 8 : 4;
      interval.spill_slot =
        function.add_spill_slot(interval.storage_size, align);
    }
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

auto temp_location(RegClass reg_class, int storage_size) -> Location {
  return {
    Location::Kind::Reg,
    reg_class == RegClass::Float ? static_cast<int>(FT11)
                                 : static_cast<int>(T6),
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
    auto tmp = temp_location(src.reg_class, src.storage_size);
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
    auto tmp = temp_location(src.reg_class, src.storage_size);
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

} // namespace detail

auto run_ra(low_ir::MachineFunction &function, bool dump_ra, bool emit_ra)
  -> void {
  RegAllocPrinter printer{dump_ra};
  detail::prepare_phi_operands(function);

  auto intervals = detail::build_intervals(function);
  auto hints = detail::collect_hints(function);

  for (auto &interval : intervals) {
    if (auto it = hints.find(interval.vreg); it != hints.end()) {
      interval.hints = std::move(it->second);
    }
    detail::compute_spill_cost(interval);
    interval.split_plan = detail::plan_split(interval);
  }

  printer.dump_intervals(function, "intervals", intervals);

  std::vector<LiveInterval> split_intervals;
  for (const auto &interval : intervals) {
    auto pieces = detail::split_interval(interval);
    split_intervals.insert(split_intervals.end(), pieces.begin(), pieces.end());
  }
  for (auto &interval : split_intervals) {
    detail::compute_spill_cost(interval);
    interval.split_plan = detail::plan_split(interval);
  }
  printer.dump_intervals(function, "split", split_intervals);

  detail::allocate_registers(split_intervals);
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

  printer.dump_function(function);
}
} // namespace exodus::riscv
