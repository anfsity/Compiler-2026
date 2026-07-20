#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace exodus::riscv {

/**
 * RISC-V ABI Register Cheat Sheet
 * Source: https://projectf.io/posts/riscv-cheat-sheet/
 *
 * +----------+----------+---------------------------------+-----------+
 * | ABI Name | Register | Description                     | Preserved |
 * +----------+----------+---------------------------------+-----------+
 * | zero     | x0       | always 0 (zero)                 | n/a       |
 * | ra       | x1       | return address                  | no        |
 * | sp       | x2       | stack pointer                   | yes       |
 * | gp       | x3       | global pointer*                 | n/a       |
 * | tp       | x4       | thread pointer*                 | n/a       |
 * | t0       | x5       | temporary                       | no        |
 * | t1       | x6       | temporary                       | no        |
 * | t2       | x7       | temporary                       | no        |
 * | fp (s0)  | x8       | frame pointer+                  | yes       |
 * | s1       | x9       | saved register                  | yes       |
 * | a0       | x10      | function argument / result#     | no        |
 * | a1       | x11      | function argument / result#     | no        |
 * | a2       | x12      | function argument               | no        |
 * | a3       | x13      | function argument               | no        |
 * | a4       | x14      | function argument               | no        |
 * | a5       | x15      | function argument               | no        |
 * | a6       | x16      | function argument               | no        |
 * | a7       | x17      | function argument               | no        |
 * | s2       | x18      | saved register                  | yes       |
 * | s3       | x19      | saved register                  | yes       |
 * | s4       | x20      | saved register                  | yes       |
 * | s5       | x21      | saved register                  | yes       |
 * | s6       | x22      | saved register                  | yes       |
 * | s7       | x23      | saved register                  | yes       |
 * | s8       | x24      | saved register                  | yes       |
 * | s9       | x25      | saved register                  | yes       |
 * | s10      | x26      | saved register                  | yes       |
 * | s11      | x27      | saved register                  | yes       |
 * | t3       | x28      | temporary                       | no        |
 * | t4       | x29      | temporary                       | no        |
 * | t5       | x30      | temporary                       | no        |
 * | t6       | x31      | temporary                       | no        |
 * +----------+----------+---------------------------------+-----------+
 *
 * * Let the compiler use gp and tp pointers; ignore them in your code.
 * + The frame pointer fp supports local variables or can be a saved register.
 * # Argument registers a0/a1 and fa0/fa1 also handle return values.
 */

// clang-format off
enum PhysReg : uint8_t {
  X0 = 0, X1, X2, X3, X4, X5, X6, X7,
  X8, X9, X10, X11, X12, X13, X14, X15,
  X16, X17, X18, X19, X20, X21, X22, X23,
  X24, X25, X26, X27, X28, X29, X30, X31,

  ZERO = X0, RA = X1, SP = X2, GP = X3, TP = X4,
  T0 = X5, T1 = X6, T2 = X7,      
  S0 = X8, FP = X8, S1 = X9,                        
  A0 = X10, A1 = X11, A2 = X12, A3 = X13, A4 = X14, A5 = X15, A6 = X16, A7 = X17,   
  S2 = X18, S3 = X19, S4 = X20, S5 = X21, S6 = X22, S7 = X23, S8 = X24, S9 = X25,
  S10 = X26, S11 = X27,
  T3 = X28, T4 = X29, T5 = X30, T6 = X31
};


/**
 * +----------+----------+---------------------------------+-----------+
 * | ft0      | f0       | floating temporary              | no        |
 * | ft1      | f1       | floating temporary              | no        |
 * | ft2      | f2       | floating temporary              | no        |
 * | ft3      | f3       | floating temporary              | no        |
 * | ft4      | f4       | floating temporary              | no        |
 * | ft5      | f5       | floating temporary              | no        |
 * | ft6      | f6       | floating temporary              | no        |
 * | ft7      | f7       | floating temporary              | no        |
 * | fs0      | f8       | floating saved register         | yes       |
 * | fs1      | f9       | floating saved register         | yes       |
 * | fa0      | f10      | floating argument / result#     | no        |
 * | fa1      | f11      | floating argument / result#     | no        |
 * | fa2      | f12      | floating argument               | no        |
 * | fa3      | f13      | floating argument               | no        |
 * | fa4      | f14      | floating argument               | no        |
 * | fa5      | f15      | floating argument               | no        |
 * | fa6      | f16      | floating argument               | no        |
 * | fa7      | f17      | floating argument               | no        |
 * | fs2      | f18      | floating saved register         | yes       |
 * | fs3      | f19      | floating saved register         | yes       |
 * | fs4      | f20      | floating saved register         | yes       |
 * | fs5      | f21      | floating saved register         | yes       |
 * | fs6      | f22      | floating saved register         | yes       |
 * | fs7      | f23      | floating saved register         | yes       |
 * | fs8      | f24      | floating saved register         | yes       |
 * | fs9      | f25      | floating saved register         | yes       |
 * | fs10     | f26      | floating saved register         | yes       |
 * | fs11     | f27      | floating saved register         | yes       |
 * | ft8      | f28      | floating temporary              | no        |
 * | ft9      | f29      | floating temporary              | no        |
 * | ft10     | f30      | floating temporary              | no        |
 * | ft11     | f31      | floating temporary              | no        |
 * +----------+----------+---------------------------------+-----------+
*/

enum FloatReg : uint8_t {
  F0 = 32, F1, F2, F3, F4, F5, F6, F7,
  F8, F9, F10, F11, F12, F13, F14, F15,
  F16, F17, F18, F19, F20, F21, F22, F23,
  F24, F25, F26, F27, F28, F29, F30, F31,

  FT0 = F0, FT1 = F1, FT2 = F2, FT3 = F3, FT4 = F4, FT5 = F5, FT6 = F6, FT7 = F7,
  FS0 = F8, FS1 = F9,
  FA0 = F10, FA1 = F11, FA2 = F12, FA3 = F13, FA4 = F14, FA5 = F15, FA6 = F16, FA7 = F17,
  FS2 = F18, FS3 = F19, FS4 = F20, FS5 = F21, FS6 = F22, FS7 = F23, FS8 = F24, FS9 = F25,
  FS10 = F26, FS11 = F27,
  FT8 = F28, FT9 = F29, FT10 = F30, FT11 = F31
};

enum Opcode : uint16_t {
  // [通用伪指令] 0-127
  PHI = 0,
  COPY,
  
  // [RV32I 基础指令] 128-255
  ADD = 128, ADDI, SUB,
  LUI, AUIPC,
  SLL, SLLI, SRL, SRLI, SRA, SRAI,
  AND, ANDI, OR, ORI, XOR, XORI,
  SLT, SLTI, SLTU, SLTIU,
  LW, LH, LB, LHU, LBU, LD,
  SW, SH, SB, SD,
  BEQ, BNE, BLT, BGE, BLTU, BGEU,
  JAL, JALR,

  // [RV64 word / RV32M 乘除法] 256-383
  MUL = 256, MULH, MULHSU, MULHU,
  DIV, DIVU, REM, REMU,
  ADDW, SUBW, MULW, DIVW, REMW,

  // [RV32F 单精度浮点] 384-511
  FLW = 384, FSW,
  FADD_S, FSUB_S, FMUL_S, FDIV_S, FSQRT_S,
  FSGNJ_S, FSGNJN_S, FSGNJX_S,
  FEQ_S, FLT_S, FLE_S,
  FCVT_W_S, FCVT_WU_S, FCVT_S_W, FCVT_S_WU,
  FMV_X_W, FMV_W_X,

  // [伪指令] 512+
  LI = 512,       
  LA,         
  CALL,       
  RET,        
  RET_NOFRAME,
  PROLOGUE,
  ADJSTACKDOWN,
  ADJSTACKUP
};

inline auto get_reg_name(int id) -> std::string {
  static const std::vector<std::string> names = {
    // Integer registers (0-31)
    "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2",
    "s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5",
    "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7",
    "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6",
    // Floating point registers (32-63)
    "ft0", "ft1", "ft2", "ft3", "ft4", "ft5", "ft6", "ft7",
    "fs0", "fs1", "fa0", "fa1", "fa2", "fa3", "fa4", "fa5",
    "fa6", "fa7", "fs2", "fs3", "fs4", "fs5", "fs6", "fs7",
    "fs8", "fs9", "fs10", "fs11", "ft8", "ft9", "ft10", "ft11"
  };
  if (id >= 0 && id < 64) return names[id];
  if (id >= 128) return "v" + std::to_string(id); 
  return "unknown";
}
// clang-format on

} // namespace exodus::riscv
