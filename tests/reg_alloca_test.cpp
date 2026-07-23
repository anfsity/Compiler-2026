#ifdef EXODUS_UNIT_TEST
#include "../src/low/riscv/reg_alloca.hpp"
#include <cstdlib>

using namespace exodus::low_ir;
using namespace exodus::riscv;

auto require(bool condition) -> void {
  if (!condition) {
    std::abort();
  }
}

auto test_live_range_covers_gap_between_def_and_use() -> void {
  MachineFunction mf;
  mf.name = "live_gap";
  auto *entry =
    mf.blocks.emplace_back(std::make_unique<MachineBasicBlock>(0, "entry"))
      .get();

  entry->insts.emplace_back(ADDI)
    .add_reg(128, true, false)
    .add_reg(SP)
    .add_imm(0);
  entry->insts.emplace_back(LI).add_reg(129, true, false).add_imm(1);
  entry->insts.emplace_back(SW).add_reg(129).add_reg(128).add_imm(0);
  entry->insts.emplace_back(RET);

  run_ra(mf, false, true);

  auto it = entry->insts.begin();
  auto &addr = std::get<MachineOperand::RegData>((it++)->operands[0].data);
  auto &value = std::get<MachineOperand::RegData>((it++)->operands[0].data);
  const auto &store = *(it++);

  require(store.opcode == SW);
  require(store.operands[0].get_reg() == value.id);
  require(store.operands[1].get_reg() == addr.id);
  require(store.operands[0].get_reg() != store.operands[1].get_reg());
}

auto test_phi_clone_uses_predecessor_definition() -> void {
  MachineFunction mf;
  mf.name = "phi_pred";
  auto *entry =
    mf.blocks.emplace_back(std::make_unique<MachineBasicBlock>(0, "entry"))
      .get();
  auto *cond =
    mf.blocks.emplace_back(std::make_unique<MachineBasicBlock>(1, "cond"))
      .get();
  entry->succs.push_back(cond);
  cond->preds.push_back(entry);

  entry->insts.emplace_back(LI).add_reg(128, true, false).add_imm(5);
  entry->insts.emplace_back(JAL).add_reg(ZERO, true, false).add_mbb(cond);
  cond->insts.emplace_back(PHI)
    .add_reg(129, true, false)
    .add_reg(128)
    .add_mbb(entry);
  cond->insts.emplace_back(LI).add_reg(128, true, false).add_imm(100);
  cond->insts.emplace_back(RET);

  run_ra(mf, false, true);

  for (const auto &inst : entry->insts) {
    require(
      inst.opcode != LI || inst.operands.size() < 2 ||
      std::get<int>(inst.operands[1].data) != 100
    );
  }
}

auto test_phi_constant_is_materialized_in_predecessor() -> void {
  MachineFunction mf;
  mf.name = "phi_const";
  auto *entry =
    mf.blocks.emplace_back(std::make_unique<MachineBasicBlock>(0, "entry"))
      .get();
  auto *merge =
    mf.blocks.emplace_back(std::make_unique<MachineBasicBlock>(1, "merge"))
      .get();
  entry->succs.push_back(merge);
  merge->preds.push_back(entry);

  entry->insts.emplace_back(JAL).add_reg(ZERO, true, false).add_mbb(merge);
  merge->insts.emplace_back(LI).add_reg(128, true, false).add_imm(25);
  merge->insts.emplace_back(PHI)
    .add_reg(129, true, false)
    .add_reg(128)
    .add_mbb(entry);
  merge->insts.emplace_back(RET);

  run_ra(mf, false, true);

  bool found_materialized_constant = false;
  for (const auto &inst : entry->insts) {
    found_materialized_constant |= inst.opcode == LI &&
                                   inst.operands.size() == 2 &&
                                   std::get<int>(inst.operands[1].data) == 25;
  }
  require(found_materialized_constant);
}

auto test_multiple_phis_keep_old_predecessor_value() -> void {
  MachineFunction mf;
  mf.name = "multi_phi";
  auto *pred =
    mf.blocks.emplace_back(std::make_unique<MachineBasicBlock>(0, "pred"))
      .get();
  auto *merge =
    mf.blocks.emplace_back(std::make_unique<MachineBasicBlock>(1, "merge"))
      .get();
  pred->succs.push_back(merge);
  merge->preds.push_back(pred);

  pred->insts.emplace_back(LI).add_reg(126, true, false).add_imm(0);
  pred->insts.emplace_back(LI).add_reg(127, true, false).add_imm(1);
  pred->insts.emplace_back(ADD)
    .add_reg(128, true, false)
    .add_reg(126)
    .add_reg(127);
  pred->insts.emplace_back(ADD)
    .add_reg(129, true, false)
    .add_reg(128)
    .add_reg(127);
  pred->insts.emplace_back(JAL).add_reg(ZERO, true, false).add_mbb(merge);
  merge->insts.emplace_back(PHI)
    .add_reg(130, true, false)
    .add_reg(129)
    .add_mbb(pred);
  merge->insts.emplace_back(PHI)
    .add_reg(131, true, false)
    .add_reg(128)
    .add_mbb(pred);
  merge->insts.emplace_back(RET);

  run_ra(mf, false, true);

  for (const auto &inst : merge->insts) {
    require(inst.opcode != PHI);
  }
}

auto test_phi_copy_keeps_original_virtual_value() -> void {
  MachineFunction mf;
  mf.name = "phi_copy_source";
  auto *entry =
    mf.blocks.emplace_back(std::make_unique<MachineBasicBlock>(0, "entry"))
      .get();
  auto *merge =
    mf.blocks.emplace_back(std::make_unique<MachineBasicBlock>(1, "merge"))
      .get();
  entry->succs.push_back(merge);
  merge->preds.push_back(entry);

  entry->insts.emplace_back(COPY).add_reg(128, true, false).add_reg(A0);
  entry->insts.emplace_back(COPY).add_reg(129, true, false).add_reg(A0);
  entry->insts.emplace_back(JAL).add_reg(ZERO, true, false).add_mbb(merge);
  merge->insts.emplace_back(PHI)
    .add_reg(130, true, false)
    .add_reg(128)
    .add_mbb(entry);
  merge->insts.emplace_back(RET);

  run_ra(mf, false, false);

  bool found_phi = false;
  for (const auto &inst : merge->insts) {
    if (
      inst.opcode == PHI && inst.operands.size() >= 3 &&
      inst.operands[0].is_reg() && inst.operands[0].get_reg() == 130 &&
      inst.operands[1].is_reg() && inst.operands[1].get_reg() == 128
    ) {
      found_phi = true;
    }
  }
  require(found_phi);
}

auto test_spilled_operands_use_distinct_temporaries() -> void {
  MachineFunction mf;
  mf.name = "multi_spill_operands";
  auto *entry =
    mf.blocks.emplace_back(std::make_unique<MachineBasicBlock>(0, "entry"))
      .get();

  for (int reg = 128; reg <= 140; ++reg) {
    entry->insts.emplace_back(LI).add_reg(reg, true, false).add_imm(reg);
  }

  entry->insts.emplace_back(ADD)
    .add_reg(141, true, false)
    .add_reg(128)
    .add_reg(129);
  for (int reg = 130; reg <= 140; ++reg) {
    entry->insts.emplace_back(ADD)
      .add_reg(141, true, false)
      .add_reg(141)
      .add_reg(reg);
  }
  entry->insts.emplace_back(RET);

  run_ra(mf, false, true);

  bool found_distinct_spill_add = false;
  for (auto it = entry->insts.begin(); it != entry->insts.end(); ++it) {
    if (
      it->opcode != ADD || it->operands.size() != 3 ||
      !it->operands[1].is_reg() || !it->operands[2].is_reg()
    )
      continue;

    auto rhs = it->operands[2].get_reg();
    auto lhs = it->operands[1].get_reg();
    if (lhs == rhs || it == entry->insts.begin())
      continue;

    auto second_reload = std::prev(it);
    if (second_reload == entry->insts.begin())
      continue;
    auto first_reload = std::prev(second_reload);
    if (
      first_reload->opcode == LW && second_reload->opcode == LW &&
      first_reload->operands[0].get_reg() == lhs &&
      second_reload->operands[0].get_reg() == rhs &&
      first_reload->operands[1].kind == MachineOperand::FrameIdx &&
      second_reload->operands[1].kind == MachineOperand::FrameIdx &&
      std::get<int>(first_reload->operands[1].data) !=
        std::get<int>(second_reload->operands[1].data)
    ) {
      found_distinct_spill_add = true;
      break;
    }
  }
  require(found_distinct_spill_add);
}

int main() {
  test_live_range_covers_gap_between_def_and_use();
  test_phi_clone_uses_predecessor_definition();
  test_phi_constant_is_materialized_in_predecessor();
  test_multiple_phis_keep_old_predecessor_value();
  test_phi_copy_keeps_original_virtual_value();
  test_spilled_operands_use_distinct_temporaries();
  return 0;
}

#endif
