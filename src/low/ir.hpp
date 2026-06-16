#pragma once

#include <cstdint>
#include <list>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace exodus::low_ir {

struct MachineBasicBlock;

struct MachineOperand {
  enum Kind : uint8_t { Reg, Imm, FImm, FrameIdx, MBB, Global, Symbol } kind;

  struct RegData {
    int id;
    bool is_def : 1;
    bool is_use : 1;
    bool is_implicit : 1;
  };

  std::variant<RegData, float, int, MachineBasicBlock *, std::string> data;

  static auto
  reg(int id, bool is_def = false, bool is_use = true, bool is_implicit = false)
    -> MachineOperand {
    return {Reg, RegData{id, is_def, is_use, is_implicit}};
  }
  static auto imm(int32_t val) -> MachineOperand { return {Imm, val}; }
  static auto fimm(float val) -> MachineOperand { return {FImm, val}; }
  static auto fi(int idx) -> MachineOperand { return {FrameIdx, idx}; }
  static auto mbb(MachineBasicBlock *b) -> MachineOperand { return {MBB, b}; }
  static auto global(std::string s) -> MachineOperand {
    return {Global, std::move(s)};
  }
  static auto symbol(std::string s) -> MachineOperand { return {Symbol, s}; }

  auto is_reg() const -> bool { return kind == Reg; }
  auto get_reg() const -> int { return std::get<RegData>(data).id; }
  auto is_def() const -> bool {
    return is_reg() && std::get<RegData>(data).is_def;
  }
};

struct MachineInst {
  int opcode;
  std::vector<MachineOperand> operands;
  MachineInst(int op) : opcode(op) {}

  auto add_operand(MachineOperand op) -> MachineInst & {
    operands.push_back(std::move(op));
    return *this;
  }

  auto add_reg(
    int id, bool is_def = false, bool is_use = true, bool is_implicit = false
  ) -> MachineInst & {
    return add_operand(MachineOperand::reg(id, is_def, is_use, is_implicit));
  }
  auto add_imm(int32_t val) -> MachineInst & {
    return add_operand(MachineOperand::imm(val));
  }
  auto add_fimm(float val) -> MachineInst & {
    return add_operand(MachineOperand::fimm(val));
  }
  auto add_fi(int idx) -> MachineInst & {
    return add_operand(MachineOperand::fi(idx));
  }
  auto add_mbb(MachineBasicBlock *b) -> MachineInst & {
    return add_operand(MachineOperand::mbb(b));
  }
};

struct MachineBasicBlock {
  int id;
  std::string name;
  std::list<MachineInst> insts;

  std::vector<MachineBasicBlock *> preds;
  std::vector<MachineBasicBlock *> succs;

  MachineBasicBlock(int i, std::string n) : id(i), name(std::move(n)) {}
};

struct MachineFunction {
  std::string name;
  std::list<std::unique_ptr<MachineBasicBlock>> blocks;

  enum class FrameSlotKind : uint8_t {
    Local,
    Spill,
    OutgoingArg,
    IncomingArg,
  };

  struct FrameSlot {
    int id;
    int size;
    int align;
    int offset;
    FrameSlotKind kind;
    int arg_index;
  };

  std::vector<FrameSlot> stack_slots;
  int vreg_cnt = 128;

  auto new_vreg() -> int { return vreg_cnt++; }
  auto add_stack_slot(
    int size,
    int align = 4,
    FrameSlotKind kind = FrameSlotKind::Local,
    int arg_index = -1
  ) -> int {
    int id = static_cast<int>(stack_slots.size());
    stack_slots.push_back({id, size, align, 0, kind, arg_index});
    return id;
  }
  auto add_spill_slot(int size, int align = 4) -> int {
    return add_stack_slot(size, align, FrameSlotKind::Spill);
  }
  auto add_outgoing_arg_slot(int arg_index, int size, int align = 4) -> int {
    return add_stack_slot(size, align, FrameSlotKind::OutgoingArg, arg_index);
  }
  auto add_incoming_arg_slot(int arg_index, int size, int align = 4) -> int {
    return add_stack_slot(size, align, FrameSlotKind::IncomingArg, arg_index);
  }
};

} // namespace exodus::low_ir
