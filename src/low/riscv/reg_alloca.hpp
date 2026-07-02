#pragma once

#include "../ir.hpp"
#include "instr.hpp"
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace exodus::riscv {

enum class RegClass : uint8_t {
  Unknown,
  Int,
  Float,
};

struct LiveSegment {
  int start = 0;
  int end = 0;
};

struct RegisterHint {
  int reg = -1;
  int score = 0;
};

struct SpillCost {
  int use_count = 0;
  int def_count = 0;
  int span = 0;
  int score = 0;
};

struct BlockLayout {
  low_ir::MachineBasicBlock *block = nullptr;
  int start = 0;
  int end = 0;
};

struct LivenessInfo {
  std::unordered_map<low_ir::MachineBasicBlock *, std::unordered_set<int>> use;
  std::unordered_map<low_ir::MachineBasicBlock *, std::unordered_set<int>> def;
  std::unordered_map<low_ir::MachineBasicBlock *, std::unordered_set<int>>
    live_in;
  std::unordered_map<low_ir::MachineBasicBlock *, std::unordered_set<int>>
    live_out;
};

struct SplitPoint {
  enum class Kind : uint8_t {
    Conflict,
    BlockBoundary,
    LoopBoundary,
  };

  int pos = 0;
  Kind kind = Kind::Conflict;
};

struct SplitPlan {
  bool should_split = false;
  SplitPoint point;
};

struct LiveInterval {
  int vreg = -1;
  RegClass reg_class = RegClass::Unknown;
  std::vector<LiveSegment> segments;
  std::vector<int> def_positions;
  std::vector<int> use_positions;
  std::vector<RegisterHint> hints;
  int assigned_reg = -1;
  int spill_slot = -1;
  int storage_size = 4;
  SpillCost spill_cost;
  SplitPlan split_plan;
  bool spilled = false;
};

auto run_ra(low_ir::MachineFunction &function, bool dump_ra, bool emit_ra)
  -> void;

} // namespace exodus::riscv
