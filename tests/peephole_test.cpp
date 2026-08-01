#ifdef EXODUS_UNIT_TEST
#include "../src/low/ir.hpp"
#include "../src/low/riscv/instr.hpp"
#include "../src/low/riscv/peephole.hpp"
#include <cassert>

using namespace exodus::low_ir;
using namespace exodus::riscv;

auto main() -> int {
  MachineFunction function;
  function.name = "peephole";
  auto *entry = function.blocks
                  .emplace_back(std::make_unique<MachineBasicBlock>(0, "entry"))
                  .get();

  entry->insts.emplace_back(LI).add_reg(128, true, false).add_imm(7);
  entry->insts.emplace_back(ADDW)
    .add_reg(129, true, false)
    .add_reg(130)
    .add_reg(128);
  entry->insts.emplace_back(LI).add_reg(131, true, false).add_imm(0);
  entry->insts.emplace_back(OR)
    .add_reg(132, true, false)
    .add_reg(130)
    .add_reg(131);

  run_peephole(function);

  assert(entry->insts.size() == 2);
  auto it = entry->insts.begin();
  assert(it->opcode == ADDIW);
  assert(it->operands.size() == 3);
  assert(it->operands[2].kind == MachineOperand::Imm);
  assert(std::get<int>(it->operands[2].data) == 7);
  ++it;
  assert(it->opcode == COPY);
  assert(it->operands[1].get_reg() == 130);
  return 0;
}
#endif
