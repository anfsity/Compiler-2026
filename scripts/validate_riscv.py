#!/usr/bin/env python3
"""
RISC-V Test Validator & Performance Analyzer for Exodus Compiler.

This script validates the correctness of compiled RISC-V binaries and, when
--perf is enabled, collects comprehensive performance metrics through static
assembly analysis, ELF inspection, and repeated QEMU execution.

Performance Metrics Collected:
  1. Runtime: single/avg/weighted-avg/median/stddev across multiple runs
  2. Code Density: instruction count, code size, bytes-per-instruction
  3. Instruction Distribution: arithmetic/load-store/branch/jump/float/other
  4. Register Pressure: unique register usage, callee-saved usage, stack spills
  5. Branch Prediction: forward/backward ratio, static prediction estimate
  6. Pipeline Cycle Simulation: CPI/IPC estimate with a simple 5-stage model
  7. Peak Stack Depth: function frame sizes and estimated call-chain depth
  8. Memory Access Locality: load/store ratio, stack-vs-heap, spatial locality
  9. Cache Hit Estimation: I-cache/D-cache pressure from ELF section sizes
 10. Hotspot Identification: loop detection, largest basic blocks, dense functions
"""

import os
import sys
import re
import json
import math
import argparse
import subprocess
import shutil
import time
import statistics
from collections import defaultdict, Counter
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass, field, asdict
from typing import Optional
import difflib


# ============================================================================
# Terminal Colors
# ============================================================================


class Colors:
    GREEN = "\033[92m"
    RED = "\033[91m"
    YELLOW = "\033[93m"
    BLUE = "\033[94m"
    CYAN = "\033[96m"
    MAGENTA = "\033[95m"
    BOLD = "\033[1m"
    DIM = "\033[2m"
    RESET = "\033[0m"

    @classmethod
    def disable(cls):
        cls.GREEN = ""
        cls.RED = ""
        cls.YELLOW = ""
        cls.BLUE = ""
        cls.CYAN = ""
        cls.MAGENTA = ""
        cls.BOLD = ""
        cls.DIM = ""
        cls.RESET = ""


if not sys.stdout.isatty():
    Colors.disable()


# ============================================================================
# RISC-V Instruction Classification
# ============================================================================

# Arithmetic / Logic / Shift instructions
_ARITH_INSNS = {
    "add",
    "addi",
    "addw",
    "addiw",
    "sub",
    "subw",
    "mul",
    "mulw",
    "mulh",
    "mulhsu",
    "mulhu",
    "div",
    "divu",
    "divw",
    "divuw",
    "rem",
    "remu",
    "remw",
    "remuw",
    "sll",
    "slli",
    "slliw",
    "sllw",
    "srl",
    "srli",
    "srliw",
    "srlw",
    "sra",
    "srai",
    "sraiw",
    "sraw",
    "and",
    "andi",
    "or",
    "ori",
    "xor",
    "xori",
    "slt",
    "slti",
    "sltu",
    "sltiu",
    "lui",
    "auipc",
    # Pseudo-instructions that expand to arithmetic
    "neg",
    "negw",
    "not",
    "seqz",
    "snez",
    "sext.w",
    "zext.b",
    "zext.h",
    "zext.w",
    "sext.b",
    "sext.h",
    # Bit manipulation (Zba/Zbb) if present
    "sh1add",
    "sh2add",
    "sh3add",
    "andn",
    "orn",
    "xnor",
    "clz",
    "clzw",
    "ctz",
    "ctzw",
    "cpop",
    "cpopw",
    "max",
    "maxu",
    "min",
    "minu",
    "rol",
    "rolw",
    "ror",
    "rori",
    "roriw",
    "rorw",
    "bclr",
    "bclri",
    "bext",
    "bexti",
    "binv",
    "binvi",
    "bset",
    "bseti",
    "rev8",
    "orc.b",
}

# Load / Store
_LOAD_INSNS = {
    "lb",
    "lh",
    "lw",
    "ld",
    "lbu",
    "lhu",
    "lwu",
    "flw",
    "fld",
    "flq",
}
_STORE_INSNS = {
    "sb",
    "sh",
    "sw",
    "sd",
    "fsw",
    "fsd",
    "fsq",
}
_LOADSTORE_INSNS = _LOAD_INSNS | _STORE_INSNS

# Branch
_BRANCH_INSNS = {
    "beq",
    "bne",
    "blt",
    "bge",
    "bltu",
    "bgeu",
    # Pseudo-instructions
    "beqz",
    "bnez",
    "blez",
    "bgez",
    "bltz",
    "bgtz",
    "bgt",
    "ble",
    "bgtu",
    "bleu",
}

# Jump / Call
_JUMP_INSNS = {
    "jal",
    "jalr",
    "j",
    "jr",
    "call",
    "tail",
    "ret",
}

# Floating-point
_FLOAT_INSNS = set()
for _op in (
    "add",
    "sub",
    "mul",
    "div",
    "sqrt",
    "madd",
    "msub",
    "nmadd",
    "nmsub",
    "min",
    "max",
    "eq",
    "lt",
    "le",
    "class",
    "cvt",
    "mv",
    "sgnjn",
    "sgnj",
    "sgnjx",
):
    for _suf in (".s", ".d", ".q"):
        _FLOAT_INSNS.add(f"f{_op}{_suf}")
# fcvt variants
for _from in ("w", "wu", "l", "lu", "s", "d"):
    for _to in ("w", "wu", "l", "lu", "s", "d"):
        _FLOAT_INSNS.add(f"fcvt.{_to}.{_from}")
_FLOAT_INSNS.update(
    {
        "fmv.x.w",
        "fmv.w.x",
        "fmv.x.d",
        "fmv.d.x",
        "fmv.s",
        "fmv.d",
        "fabs.s",
        "fabs.d",
        "fneg.s",
        "fneg.d",
        "flt.s",
        "fle.s",
        "feq.s",
        "flt.d",
        "fle.d",
        "feq.d",
        "fclass.s",
        "fclass.d",
        "fmadd.s",
        "fmsub.s",
        "fnmadd.s",
        "fnmsub.s",
        "fmadd.d",
        "fmsub.d",
        "fnmadd.d",
        "fnmsub.d",
        "fsgnj.s",
        "fsgnjn.s",
        "fsgnjx.s",
        "fsgnj.d",
        "fsgnjn.d",
        "fsgnjx.d",
        "fmin.s",
        "fmax.s",
        "fmin.d",
        "fmax.d",
        "fsqrt.s",
        "fsqrt.d",
        "fadd.s",
        "fsub.s",
        "fmul.s",
        "fdiv.s",
        "fadd.d",
        "fsub.d",
        "fmul.d",
        "fdiv.d",
    }
)

# Other / Pseudo
_OTHER_INSNS = {
    "nop",
    "mv",
    "li",
    "la",
    "lla",
    "ecall",
    "ebreak",
    "fence",
    "fence.i",
    "fence.tso",
    "csrr",
    "csrw",
    "csrs",
    "csrc",
    "csrrs",
    "csrrc",
    "csrrw",
    "csrrsi",
    "csrrci",
    "csrrwi",
    "wfi",
    "mret",
    "sret",
    "uret",
    "sfence.vma",
}

# Register name mappings
_INT_REG_ABI = {
    "zero": "x0",
    "ra": "x1",
    "sp": "x2",
    "gp": "x3",
    "tp": "x4",
    "t0": "x5",
    "t1": "x6",
    "t2": "x7",
    "s0": "x8",
    "fp": "x8",
    "s1": "x9",
    "a0": "x10",
    "a1": "x11",
    "a2": "x12",
    "a3": "x13",
    "a4": "x14",
    "a5": "x15",
    "a6": "x16",
    "a7": "x17",
    "s2": "x18",
    "s3": "x19",
    "s4": "x20",
    "s5": "x21",
    "s6": "x22",
    "s7": "x23",
    "s8": "x24",
    "s9": "x25",
    "s10": "x26",
    "s11": "x27",
    "t3": "x28",
    "t4": "x29",
    "t5": "x30",
    "t6": "x31",
}
for _i in range(32):
    _INT_REG_ABI[f"x{_i}"] = f"x{_i}"

_FLOAT_REG_ABI = {
    "ft0": "f0",
    "ft1": "f1",
    "ft2": "f2",
    "ft3": "f3",
    "ft4": "f4",
    "ft5": "f5",
    "ft6": "f6",
    "ft7": "f7",
    "fs0": "f8",
    "fs1": "f9",
    "fa0": "f10",
    "fa1": "f11",
    "fa2": "f12",
    "fa3": "f13",
    "fa4": "f14",
    "fa5": "f15",
    "fa6": "f16",
    "fa7": "f17",
    "fs2": "f18",
    "fs3": "f19",
    "fs4": "f20",
    "fs5": "f21",
    "fs6": "f22",
    "fs7": "f23",
    "fs8": "f24",
    "fs9": "f25",
    "fs10": "f26",
    "fs11": "f27",
    "ft8": "f28",
    "ft9": "f29",
    "ft10": "f30",
    "ft11": "f31",
}
for _i in range(32):
    _FLOAT_REG_ABI[f"f{_i}"] = f"f{_i}"

_ALL_REGS = {**_INT_REG_ABI, **_FLOAT_REG_ABI}

_CALLEE_SAVED_INT = {
    "s0",
    "s1",
    "s2",
    "s3",
    "s4",
    "s5",
    "s6",
    "s7",
    "s8",
    "s9",
    "s10",
    "s11",
    "x8",
    "x9",
    "x18",
    "x19",
    "x20",
    "x21",
    "x22",
    "x23",
    "x24",
    "x25",
    "x26",
    "x27",
}
_CALLEE_SAVED_FLOAT = {
    "fs0",
    "fs1",
    "fs2",
    "fs3",
    "fs4",
    "fs5",
    "fs6",
    "fs7",
    "fs8",
    "fs9",
    "fs10",
    "fs11",
    "f8",
    "f9",
    "f18",
    "f19",
    "f20",
    "f21",
    "f22",
    "f23",
    "f24",
    "f25",
    "f26",
    "f27",
}
_CALLEE_SAVED = _CALLEE_SAVED_INT | _CALLEE_SAVED_FLOAT

# Pipeline cycle cost model (simple 5-stage in-order)
_CYCLE_COST = {
    "arith": 1,
    "load": 2,  # Assume L1 hit
    "store": 1,
    "branch_taken": 2,
    "branch_not_taken": 1,
    "jump": 2,
    "mul": 3,
    "div": 15,
    "float_simple": 3,  # fadd, fsub, fmv, fcvt
    "float_mul": 5,
    "float_div": 15,
    "float_sqrt": 20,
    "other": 1,
}


# ============================================================================
# Data Classes for Performance Metrics
# ============================================================================


@dataclass
class RuntimeMetrics:
    runs: list = field(default_factory=list)
    avg: float = 0.0
    weighted_avg: float = 0.0
    median: float = 0.0
    min_time: float = 0.0
    max_time: float = 0.0
    stddev: float = 0.0


@dataclass
class CodeDensityMetrics:
    total_instructions: int = 0
    text_size_bytes: int = 0
    data_size_bytes: int = 0
    bss_size_bytes: int = 0
    total_elf_size: int = 0
    bytes_per_instruction: float = 0.0
    functions_count: int = 0
    instructions_per_function: float = 0.0


@dataclass
class InstructionMixMetrics:
    total: int = 0
    arith: int = 0
    load_store: int = 0
    load: int = 0
    store: int = 0
    branch: int = 0
    jump: int = 0
    float_ops: int = 0
    other: int = 0
    arith_pct: float = 0.0
    load_store_pct: float = 0.0
    branch_pct: float = 0.0
    jump_pct: float = 0.0
    float_pct: float = 0.0
    other_pct: float = 0.0
    top_instructions: dict = field(default_factory=dict)


@dataclass
class RegisterPressureMetrics:
    int_regs_used: int = 0
    int_regs_total: int = 31  # x1-x31 (x0 is hardwired zero)
    float_regs_used: int = 0
    float_regs_total: int = 32
    int_pressure_pct: float = 0.0
    float_pressure_pct: float = 0.0
    callee_saved_used: int = 0
    stack_spills: int = 0
    reg_usage_top: dict = field(default_factory=dict)


@dataclass
class BranchAnalysisMetrics:
    total_branches: int = 0
    forward_branches: int = 0
    backward_branches: int = 0
    unknown_branches: int = 0
    predicted_hit_rate: float = 0.0
    branch_density: float = 0.0  # per 100 instructions


@dataclass
class PipelineEstimateMetrics:
    estimated_cycles: int = 0
    cpi: float = 0.0
    ipc: float = 0.0
    cycle_breakdown: dict = field(default_factory=dict)


@dataclass
class StackAnalysisMetrics:
    functions: int = 0
    frame_sizes: dict = field(default_factory=dict)  # func_name -> size
    max_frame_size: int = 0
    total_frame_size: int = 0
    estimated_peak_depth: int = 0
    call_graph_depth: int = 0


@dataclass
class MemoryAccessMetrics:
    total_loads: int = 0
    total_stores: int = 0
    load_store_ratio: float = 0.0
    stack_accesses: int = 0
    non_stack_accesses: int = 0
    stack_access_pct: float = 0.0
    spatial_locality_score: str = "unknown"  # good / moderate / poor
    avg_offset_magnitude: float = 0.0


@dataclass
class CacheEstimateMetrics:
    icache_size: int = 32768  # 32KB L1 I-cache
    dcache_size: int = 32768  # 32KB L1 D-cache
    code_fits_icache: bool = True
    data_fits_dcache: bool = True
    icache_usage_pct: float = 0.0
    dcache_usage_pct: float = 0.0
    icache_verdict: str = "fits"
    dcache_verdict: str = "fits"


@dataclass
class HotspotMetrics:
    loop_count: int = 0
    loop_locations: list = field(default_factory=list)
    largest_basic_blocks: list = field(default_factory=list)
    densest_functions: list = field(default_factory=list)


@dataclass
class PerfMetrics:
    """Container for all performance metrics of a single test."""

    runtime: RuntimeMetrics = field(default_factory=RuntimeMetrics)
    code_density: CodeDensityMetrics = field(default_factory=CodeDensityMetrics)
    instruction_mix: InstructionMixMetrics = field(
        default_factory=InstructionMixMetrics
    )
    register_pressure: RegisterPressureMetrics = field(
        default_factory=RegisterPressureMetrics
    )
    branch_analysis: BranchAnalysisMetrics = field(
        default_factory=BranchAnalysisMetrics
    )
    pipeline_estimate: PipelineEstimateMetrics = field(
        default_factory=PipelineEstimateMetrics
    )
    stack_analysis: StackAnalysisMetrics = field(default_factory=StackAnalysisMetrics)
    memory_access: MemoryAccessMetrics = field(default_factory=MemoryAccessMetrics)
    cache_estimate: CacheEstimateMetrics = field(default_factory=CacheEstimateMetrics)
    hotspots: HotspotMetrics = field(default_factory=HotspotMetrics)


# ============================================================================
# Assembly Parser
# ============================================================================


class RISCVAsmParser:
    """Parses a RISC-V assembly (.s) file and extracts structured information."""

    # Regex to match a label definition: `name:` at the start of a line
    _LABEL_RE = re.compile(r"^\.?([a-zA-Z_][a-zA-Z0-9_.]*):\s*$")
    # Regex to match a function label (GAS .type directive)
    _FUNC_TYPE_RE = re.compile(r"^\s*\.type\s+(\S+),\s*@function")
    _FUNC_SIZE_RE = re.compile(r"^\s*\.size\s+(\S+),\s*")
    # Regex for directives
    _DIRECTIVE_RE = re.compile(r"^\s*\.")
    # Regex to match an instruction line: optional label, then mnemonic + operands
    _INSN_RE = re.compile(
        r"^\s*(?:\.?[a-zA-Z_][a-zA-Z0-9_.]*:\s*)?"  # optional label
        r"([a-z][a-z0-9.]*)"  # mnemonic
        r"(?:\s+(.*))?$"  # operands
    )
    # Stack allocation pattern: addi sp, sp, -N
    _STACK_ALLOC_RE = re.compile(r"^\s*addi\s+sp\s*,\s*sp\s*,\s*(-\d+)", re.IGNORECASE)
    # Memory offset pattern: offset(reg)
    _MEM_OFFSET_RE = re.compile(r"(-?\d+)\((\w+)\)")
    # Branch target pattern
    _BRANCH_TARGET_RE = re.compile(r"\.?([a-zA-Z_][a-zA-Z0-9_.]*)\s*$")

    def __init__(self, asm_path: str):
        self.asm_path = asm_path
        self.lines: list[str] = []
        self.instructions: list[dict] = []  # {line_no, mnemonic, operands, raw}
        self.functions: dict[str, list[dict]] = {}  # func_name -> [instructions]
        self.labels: dict[str, int] = {}  # label_name -> line_no
        self.function_names: set[str] = set()

        self._parse()

    def _parse(self):
        """Parse the assembly file into structured data."""
        with open(self.asm_path, "r", encoding="utf-8", errors="ignore") as f:
            self.lines = f.readlines()

        current_func = None
        declared_functions = set()
        globl_symbols = set()

        for line in self.lines:
            m_type = self._FUNC_TYPE_RE.match(line)
            if m_type:
                declared_functions.add(m_type.group(1))
            m_globl = re.match(r"^\s*\.globl\s+(\S+)", line)
            if m_globl:
                globl_symbols.add(m_globl.group(1))

        for line_no, raw_line in enumerate(self.lines, 1):
            stripped = raw_line.strip()

            if not stripped or stripped.startswith("#") or stripped.startswith("//"):
                continue

            label_m = self._LABEL_RE.match(stripped)
            if label_m:
                label_name = label_m.group(1)
                self.labels[label_name] = line_no

                is_func = (
                    label_name in declared_functions
                    or label_name in globl_symbols
                    or (
                        not label_name.startswith(".")
                        and not label_name.startswith("L")
                        and not label_name.startswith("_L")
                        and label_name != "zero"
                    )
                )

                if is_func:
                    current_func = label_name
                    self.function_names.add(label_name)
                    if current_func not in self.functions:
                        self.functions[current_func] = []
                continue

            if self._DIRECTIVE_RE.match(stripped):
                continue

            insn_m = self._INSN_RE.match(stripped)
            if insn_m:
                mnemonic = insn_m.group(1).lower()
                operands = insn_m.group(2) or ""
                insn = {
                    "line_no": line_no,
                    "mnemonic": mnemonic,
                    "operands": operands.strip(),
                    "raw": stripped,
                }
                self.instructions.append(insn)
                if current_func is not None:
                    self.functions[current_func].append(insn)

    def classify_instruction(self, mnemonic: str) -> str:
        """Classify a RISC-V instruction into a category."""
        if mnemonic in _ARITH_INSNS:
            return "arith"
        elif mnemonic in _LOADSTORE_INSNS:
            return "load_store"
        elif mnemonic in _BRANCH_INSNS:
            return "branch"
        elif mnemonic in _JUMP_INSNS:
            return "jump"
        elif mnemonic in _FLOAT_INSNS:
            return "float"
        elif mnemonic in _OTHER_INSNS:
            return "other"
        else:
            # Try partial matching for float variants
            if mnemonic.startswith("f") and "." in mnemonic:
                return "float"
            # Check if it looks like a known pseudo-instruction
            if mnemonic in ("mv", "li", "la", "nop", "lla"):
                return "other"
            return "other"

    def extract_registers(self, operands: str) -> list[str]:
        """Extract register names from an operand string."""
        regs = []
        # Remove memory offset syntax for parsing
        cleaned = re.sub(r"-?\d+\((\w+)\)", r"\1", operands)
        tokens = re.split(r"[,\s]+", cleaned)
        for tok in tokens:
            tok = tok.strip()
            if tok in _ALL_REGS:
                regs.append(tok)
        return regs

    def get_memory_accesses(self) -> list[dict]:
        """Extract all memory access instructions with offset/base info."""
        accesses = []
        for insn in self.instructions:
            if insn["mnemonic"] in _LOADSTORE_INSNS:
                m = self._MEM_OFFSET_RE.search(insn["operands"])
                if m:
                    offset = int(m.group(1))
                    base_reg = m.group(2)
                    accesses.append(
                        {
                            "type": "load"
                            if insn["mnemonic"] in _LOAD_INSNS
                            else "store",
                            "mnemonic": insn["mnemonic"],
                            "offset": offset,
                            "base_reg": base_reg,
                            "line_no": insn["line_no"],
                        }
                    )
                else:
                    # Some load/store forms might not use offset(reg) syntax
                    accesses.append(
                        {
                            "type": "load"
                            if insn["mnemonic"] in _LOAD_INSNS
                            else "store",
                            "mnemonic": insn["mnemonic"],
                            "offset": 0,
                            "base_reg": "unknown",
                            "line_no": insn["line_no"],
                        }
                    )
        return accesses

    def get_branch_info(self) -> list[dict]:
        """Analyze branches and jumps: determine forward vs backward."""
        branches = []
        for insn in self.instructions:
            if insn["mnemonic"] in _BRANCH_INSNS or insn["mnemonic"] in _JUMP_INSNS:
                target_m = self._BRANCH_TARGET_RE.search(insn["operands"])
                direction = "unknown"
                if target_m:
                    target_label = target_m.group(1)
                    target_line = self.labels.get(target_label)
                    if target_line is not None:
                        if target_line > insn["line_no"]:
                            direction = "forward"
                        elif target_line < insn["line_no"]:
                            direction = "backward"
                        else:
                            direction = "self"
                branches.append(
                    {
                        "mnemonic": insn["mnemonic"],
                        "line_no": insn["line_no"],
                        "direction": direction,
                        "operands": insn["operands"],
                    }
                )
        return branches

    def get_stack_frames(self) -> dict[str, int]:
        """Extract stack frame sizes for each function."""
        frames = {}
        for func_name, insns in self.functions.items():
            for insn in insns[:10]:  # Check first 10 instructions (prologue)
                m = self._STACK_ALLOC_RE.match(insn["raw"])
                if m:
                    frames[func_name] = abs(int(m.group(1)))
                    break
            if func_name not in frames:
                frames[func_name] = 0
        return frames

    def get_basic_blocks(self) -> list[dict]:
        """Identify basic blocks (sequences of instructions between branches/labels)."""
        blocks = []
        current_block = []
        current_start = 0

        for insn in self.instructions:
            if not current_block:
                current_start = insn["line_no"]
            current_block.append(insn)

            if insn["mnemonic"] in _BRANCH_INSNS or insn["mnemonic"] in _JUMP_INSNS:
                blocks.append(
                    {
                        "start_line": current_start,
                        "size": len(current_block),
                        "instructions": current_block,
                    }
                )
                current_block = []

        # Last block
        if current_block:
            blocks.append(
                {
                    "start_line": current_start,
                    "size": len(current_block),
                    "instructions": current_block,
                }
            )

        return blocks


# ============================================================================
# Performance Analysis Functions
# ============================================================================


def analyze_runtime(qemu_cmd: str, temp_dir: str, in_file: Optional[str],
                    timeout: float, num_runs: int) -> RuntimeMetrics:
    """Run the binary multiple times and collect timing statistics."""
    metrics = RuntimeMetrics()
    runs = []

    for i in range(num_runs):
        stdin_f = None
        if in_file and os.path.exists(in_file):
            stdin_f = open(in_file, "rb")
        try:
            start = time.perf_counter()
            res = subprocess.run(
                qemu_cmd.split() if isinstance(qemu_cmd, str) else qemu_cmd,
                cwd=temp_dir,
                stdin=stdin_f,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=timeout,
            )
            elapsed = time.perf_counter() - start
            
            if res.returncode == 0:
                runs.append(elapsed)
        except subprocess.TimeoutExpired:
            runs.append(timeout)
        finally:
            if stdin_f:
                stdin_f.close()

    if not runs:
        return metrics

    metrics.runs = runs
    metrics.avg = statistics.mean(runs)
    metrics.median = statistics.median(runs)
    metrics.min_time = min(runs)
    metrics.max_time = max(runs)
    metrics.stddev = statistics.stdev(runs) if len(runs) > 1 else 0.0

    weights = []
    for i in range(len(runs)):
        if i == 0:
            weights.append(0.5)
        elif i == 1:
            weights.append(0.75)
        else:
            weights.append(1.0)
    
    actual_weights = weights[:len(runs)]
    weighted_sum = sum(r * w for r, w in zip(runs, actual_weights))
    weight_total = sum(actual_weights)
    metrics.weighted_avg = weighted_sum / weight_total if weight_total > 0 else 0.0

    return metrics


def analyze_code_density(parser: RISCVAsmParser, temp_dir: str,
                         gcc_path: str) -> CodeDensityMetrics:
    """Analyze code density from assembly and ELF."""
    metrics = CodeDensityMetrics()
    metrics.total_instructions = len(parser.instructions)
    metrics.functions_count = len(parser.functions)

    if metrics.functions_count > 0:
        metrics.instructions_per_function = (
            metrics.total_instructions / metrics.functions_count
        )

    elf_path = os.path.join(temp_dir, "test.elf")
    if os.path.exists(elf_path):
        size_cmd = gcc_path.replace("gcc", "size")
        try:
            res = subprocess.run(
                [size_cmd, elf_path],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                timeout=5
            )
            if res.returncode == 0:
                output = res.stdout.decode('utf-8', errors='ignore')
                lines = output.strip().split('\n')
                if len(lines) >= 2:
                    parts = lines[1].split()
                    if len(parts) >= 4:
                        metrics.text_size_bytes = int(parts[0])
                        metrics.data_size_bytes = int(parts[1])
                        metrics.bss_size_bytes = int(parts[2])
                        metrics.total_elf_size = int(parts[3])
        except (subprocess.TimeoutExpired, FileNotFoundError, ValueError):
            pass

    metrics.bytes_per_instruction = 4.0

    return metrics


def analyze_instruction_mix(parser: RISCVAsmParser) -> InstructionMixMetrics:
    """Analyze instruction type distribution."""
    metrics = InstructionMixMetrics()
    counter = Counter()

    for insn in parser.instructions:
        cat = parser.classify_instruction(insn["mnemonic"])
        counter[insn["mnemonic"]] += 1

        if cat == "arith":
            metrics.arith += 1
        elif cat == "load_store":
            metrics.load_store += 1
            if insn["mnemonic"] in _LOAD_INSNS:
                metrics.load += 1
            else:
                metrics.store += 1
        elif cat == "branch":
            metrics.branch += 1
        elif cat == "jump":
            metrics.jump += 1
        elif cat == "float":
            metrics.float_ops += 1
        else:
            metrics.other += 1

    metrics.total = len(parser.instructions)
    if metrics.total > 0:
        metrics.arith_pct = (metrics.arith / metrics.total) * 100
        metrics.load_store_pct = (metrics.load_store / metrics.total) * 100
        metrics.branch_pct = (metrics.branch / metrics.total) * 100
        metrics.jump_pct = (metrics.jump / metrics.total) * 100
        metrics.float_pct = (metrics.float_ops / metrics.total) * 100
        metrics.other_pct = (metrics.other / metrics.total) * 100

    # Top 10 most used instructions
    metrics.top_instructions = dict(counter.most_common(10))
    return metrics


def analyze_register_pressure(parser: RISCVAsmParser) -> RegisterPressureMetrics:
    """Analyze register usage patterns."""
    metrics = RegisterPressureMetrics()
    int_regs = set()
    float_regs = set()
    reg_counter = Counter()
    spill_count = 0
    callee_saved_used = set()

    for insn in parser.instructions:
        regs = parser.extract_registers(insn["operands"])
        for reg in regs:
            canonical = _ALL_REGS.get(reg, reg)
            reg_counter[reg] += 1

            if canonical.startswith("x") and canonical != "x0":
                int_regs.add(canonical)
            elif canonical.startswith("f"):
                float_regs.add(canonical)

            if reg in _CALLEE_SAVED:
                callee_saved_used.add(reg)

        # Count stack spills: sd/sw reg, offset(sp) or ld/lw reg, offset(sp)
        if insn["mnemonic"] in ("sd", "sw", "ld", "lw", "fsd", "fsw", "fld", "flw"):
            if "sp" in insn["operands"] or "x2" in insn["operands"]:
                spill_count += 1

    metrics.int_regs_used = len(int_regs)
    metrics.float_regs_used = len(float_regs)
    metrics.int_pressure_pct = (metrics.int_regs_used / metrics.int_regs_total) * 100
    metrics.float_pressure_pct = (
        (metrics.float_regs_used / metrics.float_regs_total) * 100
        if metrics.float_regs_total > 0
        else 0.0
    )
    metrics.callee_saved_used = len(callee_saved_used)
    metrics.stack_spills = spill_count
    metrics.reg_usage_top = dict(reg_counter.most_common(10))

    return metrics


def analyze_branches(parser: RISCVAsmParser) -> BranchAnalysisMetrics:
    """Analyze branch patterns for prediction estimation."""
    metrics = BranchAnalysisMetrics()
    branch_info = parser.get_branch_info()

    metrics.total_branches = len(branch_info)

    for b in branch_info:
        if b["direction"] == "forward":
            metrics.forward_branches += 1
        elif b["direction"] == "backward":
            metrics.backward_branches += 1
        else:
            metrics.unknown_branches += 1

    # Static prediction heuristic:
    # - Backward branches (loop back-edges) are predicted TAKEN → usually correct
    # - Forward branches are predicted NOT TAKEN → typically correct for error paths
    # Estimated hit rate based on typical program behavior:
    # - ~90% of backward branches are correctly predicted as taken
    # - ~60% of forward branches are correctly predicted as not-taken
    if metrics.total_branches > 0:
        backward_hits = metrics.backward_branches * 0.90
        forward_hits = metrics.forward_branches * 0.60
        unknown_hits = metrics.unknown_branches * 0.50
        total_hits = backward_hits + forward_hits + unknown_hits
        metrics.predicted_hit_rate = (total_hits / metrics.total_branches) * 100

    # Branch density
    total_insns = len(parser.instructions)
    if total_insns > 0:
        metrics.branch_density = (metrics.total_branches / total_insns) * 100

    return metrics


def analyze_pipeline(
    parser: RISCVAsmParser, branch_metrics: BranchAnalysisMetrics
) -> PipelineEstimateMetrics:
    """Simulate a simple 5-stage in-order pipeline to estimate cycles."""
    metrics = PipelineEstimateMetrics()
    total_cycles = 0
    cycle_breakdown = defaultdict(int)

    for insn in parser.instructions:
        mnemonic = insn["mnemonic"]
        cat = parser.classify_instruction(mnemonic)
        cost = 1

        if cat == "arith":
            if mnemonic in ("mul", "mulw", "mulh", "mulhsu", "mulhu"):
                cost = _CYCLE_COST["mul"]
                cycle_breakdown["mul"] += cost
            elif mnemonic in (
                "div",
                "divu",
                "divw",
                "divuw",
                "rem",
                "remu",
                "remw",
                "remuw",
            ):
                cost = _CYCLE_COST["div"]
                cycle_breakdown["div"] += cost
            else:
                cost = _CYCLE_COST["arith"]
                cycle_breakdown["arith"] += cost
        elif cat == "load_store":
            if mnemonic in _LOAD_INSNS:
                cost = _CYCLE_COST["load"]
                cycle_breakdown["load"] += cost
            else:
                cost = _CYCLE_COST["store"]
                cycle_breakdown["store"] += cost
        elif cat == "branch":
            # Estimate: ~predicted_hit_rate% are correctly predicted
            hit_rate = (
                branch_metrics.predicted_hit_rate / 100
                if branch_metrics.total_branches > 0
                else 0.5
            )
            # Mix of taken/not-taken based on backward/forward ratio
            if branch_metrics.total_branches > 0:
                taken_ratio = (
                    branch_metrics.backward_branches / branch_metrics.total_branches
                )
            else:
                taken_ratio = 0.5
            avg_cost = (
                taken_ratio * _CYCLE_COST["branch_taken"]
                + (1 - taken_ratio) * _CYCLE_COST["branch_not_taken"]
            )
            # Add misprediction penalty (~3 cycles)
            avg_cost += (1 - hit_rate) * 3
            cost = avg_cost
            cycle_breakdown["branch"] += cost
        elif cat == "jump":
            cost = _CYCLE_COST["jump"]
            cycle_breakdown["jump"] += cost
        elif cat == "float":
            if "div" in mnemonic:
                cost = _CYCLE_COST["float_div"]
                cycle_breakdown["float_div"] += cost
            elif "sqrt" in mnemonic:
                cost = _CYCLE_COST["float_sqrt"]
                cycle_breakdown["float_sqrt"] += cost
            elif "mul" in mnemonic or "madd" in mnemonic or "msub" in mnemonic:
                cost = _CYCLE_COST["float_mul"]
                cycle_breakdown["float_mul"] += cost
            else:
                cost = _CYCLE_COST["float_simple"]
                cycle_breakdown["float_simple"] += cost
        else:
            cost = _CYCLE_COST["other"]
            cycle_breakdown["other"] += cost

        total_cycles += cost

    metrics.estimated_cycles = int(total_cycles)
    total_insns = len(parser.instructions)
    if total_insns > 0:
        metrics.cpi = total_cycles / total_insns
        metrics.ipc = total_insns / total_cycles if total_cycles > 0 else 0.0
    metrics.cycle_breakdown = dict(cycle_breakdown)

    return metrics


def analyze_stack(parser: RISCVAsmParser) -> StackAnalysisMetrics:
    """Analyze stack usage patterns."""
    metrics = StackAnalysisMetrics()
    frames = parser.get_stack_frames()

    metrics.functions = len(parser.functions)
    metrics.frame_sizes = frames

    if frames:
        metrics.max_frame_size = max(frames.values()) if frames.values() else 0
        metrics.total_frame_size = sum(frames.values())

    # Build a simple call graph to estimate peak depth
    call_graph = defaultdict(set)
    for func_name, insns in parser.functions.items():
        for insn in insns:
            if insn["mnemonic"] in ("call", "jal"):
                # Extract target from operands
                target = insn["operands"].strip().split(",")[-1].strip()
                # Remove any register prefix for jal (e.g., "ra, func")
                if "," in insn["operands"]:
                    target = insn["operands"].split(",")[-1].strip()
                if target in parser.function_names:
                    call_graph[func_name].add(target)

    # DFS to find deepest call chain
    def _dfs_depth(node, visited):
        if node in visited:
            return 0  # Avoid cycles
        visited.add(node)
        max_child = 0
        for child in call_graph.get(node, set()):
            d = _dfs_depth(child, visited)
            max_child = max(max_child, d)
        visited.discard(node)
        frame = frames.get(node, 0)
        return frame + max_child

    max_depth = 0
    for func in parser.function_names:
        depth = _dfs_depth(func, set())
        max_depth = max(max_depth, depth)

    metrics.estimated_peak_depth = max_depth
    metrics.call_graph_depth = 0

    # Compute call chain depth (number of functions, not bytes)
    def _call_depth(node, visited):
        if node in visited:
            return 0
        visited.add(node)
        max_child = 0
        for child in call_graph.get(node, set()):
            d = _call_depth(child, visited)
            max_child = max(max_child, d)
        visited.discard(node)
        return 1 + max_child

    for func in parser.function_names:
        d = _call_depth(func, set())
        metrics.call_graph_depth = max(metrics.call_graph_depth, d)

    return metrics


def analyze_memory_access(parser: RISCVAsmParser) -> MemoryAccessMetrics:
    """Analyze memory access patterns for locality estimation."""
    metrics = MemoryAccessMetrics()
    accesses = parser.get_memory_accesses()

    for acc in accesses:
        if acc["type"] == "load":
            metrics.total_loads += 1
        else:
            metrics.total_stores += 1

        if acc["base_reg"] in ("sp", "x2", "s0", "fp", "x8"):
            metrics.stack_accesses += 1
        else:
            metrics.non_stack_accesses += 1

    total = metrics.total_loads + metrics.total_stores
    if total > 0:
        metrics.load_store_ratio = (
            metrics.total_loads / metrics.total_stores
            if metrics.total_stores > 0
            else float("inf")
        )
        metrics.stack_access_pct = (metrics.stack_accesses / total) * 100

    # Spatial locality: analyze offset distribution
    offsets = [abs(acc["offset"]) for acc in accesses if acc["offset"] != 0]
    if offsets:
        metrics.avg_offset_magnitude = statistics.mean(offsets)
        # Good locality: most offsets are small (< 256 bytes)
        small_offsets = sum(1 for o in offsets if o < 256)
        ratio = small_offsets / len(offsets)
        if ratio > 0.8:
            metrics.spatial_locality_score = "good"
        elif ratio > 0.5:
            metrics.spatial_locality_score = "moderate"
        else:
            metrics.spatial_locality_score = "poor"
    elif total > 0:
        metrics.spatial_locality_score = "good"  # All zero-offset = good

    return metrics


def analyze_cache(code_density: CodeDensityMetrics,
                  stack: StackAnalysisMetrics) -> CacheEstimateMetrics:
    """Estimate cache pressure from code and data sizes."""
    metrics = CacheEstimateMetrics()

    code_size = code_density.total_instructions * 4
    metrics.icache_usage_pct = (code_size / metrics.icache_size) * 100
    if code_size <= metrics.icache_size:
        metrics.code_fits_icache = True
        metrics.icache_verdict = "fits"
    elif code_size <= metrics.icache_size * 2:
        metrics.code_fits_icache = False
        metrics.icache_verdict = "moderate_pressure"
    else:
        metrics.code_fits_icache = False
        metrics.icache_verdict = "high_pressure"

    data_working_set = stack.total_frame_size
    metrics.dcache_usage_pct = (data_working_set / metrics.dcache_size) * 100
    if data_working_set <= metrics.dcache_size:
        metrics.data_fits_dcache = True
        metrics.dcache_verdict = "fits"
    elif data_working_set <= metrics.dcache_size * 2:
        metrics.data_fits_dcache = False
        metrics.dcache_verdict = "moderate_pressure"
    else:
        metrics.data_fits_dcache = False
        metrics.dcache_verdict = "high_pressure"

    return metrics




def analyze_hotspots(parser: RISCVAsmParser) -> HotspotMetrics:
    """Identify potential performance hotspots."""
    metrics = HotspotMetrics()

    # Loop detection: backward branches indicate loops
    branch_info = parser.get_branch_info()
    for b in branch_info:
        if b["direction"] == "backward":
            metrics.loop_count += 1
            metrics.loop_locations.append(
                {
                    "line": b["line_no"],
                    "instruction": f"{b['mnemonic']} {b['operands']}",
                }
            )

    # Largest basic blocks
    blocks = parser.get_basic_blocks()
    blocks_sorted = sorted(blocks, key=lambda b: b["size"], reverse=True)
    for blk in blocks_sorted[:5]:
        metrics.largest_basic_blocks.append(
            {
                "start_line": blk["start_line"],
                "size": blk["size"],
            }
        )

    # Densest functions (most instructions)
    func_sizes = {name: len(insns) for name, insns in parser.functions.items()}
    sorted_funcs = sorted(func_sizes.items(), key=lambda x: x[1], reverse=True)
    for name, size in sorted_funcs[:5]:
        metrics.densest_functions.append(
            {
                "name": name,
                "instruction_count": size,
            }
        )

    return metrics


# ============================================================================
# Performance Report Rendering
# ============================================================================


def _box_line(text: str, width: int = 62) -> str:
    """Create a line for the box display."""
    return f"│ {text:<{width}} │"


def _format_time(t: float) -> str:
    """Format time value with appropriate unit."""
    if t < 0.001:
        return f"{t * 1e6:.0f}µs"
    elif t < 1.0:
        return f"{t * 1000:.1f}ms"
    else:
        return f"{t:.3f}s"


def _pct_bar(pct: float, width: int = 10) -> str:
    """Create a small percentage bar."""
    filled = int(pct / 100 * width)
    filled = max(0, min(width, filled))
    return "█" * filled + "░" * (width - filled)


def render_perf_card(test_name: str, perf: PerfMetrics):
    """Render a performance summary card for a single test."""
    w = 62
    C = Colors

    print(
        f"┌─ {C.CYAN}Performance: {test_name}{C.RESET} {'─' * max(1, w - 16 - len(test_name))}┐"
    )

    # Runtime
    rt = perf.runtime
    if rt.runs:
        print(_box_line(f"{C.BOLD}Runtime{C.RESET}", w))
        runs_str = ", ".join(_format_time(r) for r in rt.runs)
        print(_box_line(f"  Runs: {runs_str}", w))
        print(
            _box_line(
                f"  Avg: {_format_time(rt.avg)}  "
                f"Weighted: {_format_time(rt.weighted_avg)}  "
                f"σ: {_format_time(rt.stddev)}",
                w,
            )
        )
        print(
            _box_line(
                f"  Min: {_format_time(rt.min_time)}  "
                f"Max: {_format_time(rt.max_time)}  "
                f"Median: {_format_time(rt.median)}",
                w,
            )
        )
        print(_box_line("", w))

    # Code Density
    cd = perf.code_density
    print(_box_line(f"{C.BOLD}Code Density{C.RESET}", w))
    print(
        _box_line(
            f"  Instructions: {cd.total_instructions}  "
            f"Code: {cd.text_size_bytes}B  "
            f"Density: {cd.bytes_per_instruction:.2f} B/I",
            w,
        )
    )
    print(
        _box_line(
            f"  Functions: {cd.functions_count}  "
            f"Avg insns/func: {cd.instructions_per_function:.1f}",
            w,
        )
    )
    print(_box_line("", w))

    # Instruction Mix
    im = perf.instruction_mix
    print(_box_line(f"{C.BOLD}Instruction Mix{C.RESET}", w))
    print(
        _box_line(
            f"  Arith: {im.arith_pct:5.1f}%  "
            f"L/S: {im.load_store_pct:5.1f}%  "
            f"Branch: {im.branch_pct:5.1f}%",
            w,
        )
    )
    print(
        _box_line(
            f"  Jump:  {im.jump_pct:5.1f}%  "
            f"Float: {im.float_pct:5.1f}%  "
            f"Other: {im.other_pct:5.1f}%",
            w,
        )
    )
    if im.top_instructions:
        top3 = list(im.top_instructions.items())[:5]
        top_str = "  Top: " + ", ".join(f"{k}({v})" for k, v in top3)
        print(_box_line(top_str, w))
    print(_box_line("", w))

    # Register Pressure
    rp = perf.register_pressure
    print(_box_line(f"{C.BOLD}Register Pressure{C.RESET}", w))
    int_bar = _pct_bar(rp.int_pressure_pct, 8)
    float_bar = _pct_bar(rp.float_pressure_pct, 8)
    print(
        _box_line(
            f"  Int: {rp.int_regs_used}/{rp.int_regs_total} "
            f"({rp.int_pressure_pct:4.1f}%) {int_bar}  "
            f"Float: {rp.float_regs_used}/{rp.float_regs_total} "
            f"({rp.float_pressure_pct:4.1f}%) {float_bar}",
            w,
        )
    )
    print(
        _box_line(
            f"  Callee-saved: {rp.callee_saved_used}  Stack spills: {rp.stack_spills}",
            w,
        )
    )
    print(_box_line("", w))

    # Branch Analysis
    ba = perf.branch_analysis
    print(_box_line(f"{C.BOLD}Branch Analysis{C.RESET}", w))
    print(
        _box_line(
            f"  Total: {ba.total_branches}  "
            f"Fwd: {ba.forward_branches}  "
            f"Bwd: {ba.backward_branches}  "
            f"Unk: {ba.unknown_branches}",
            w,
        )
    )
    pred_color = (
        C.GREEN
        if ba.predicted_hit_rate >= 75
        else (C.YELLOW if ba.predicted_hit_rate >= 50 else C.RED)
    )
    print(
        _box_line(
            f"  Predicted hit rate: {pred_color}{ba.predicted_hit_rate:5.1f}%{C.RESET}  "
            f"Density: {ba.branch_density:.1f}/100insns",
            w,
        )
    )
    print(_box_line("", w))

    # Pipeline Estimate
    pe = perf.pipeline_estimate
    print(_box_line(f"{C.BOLD}Pipeline Estimate (static){C.RESET}", w))
    print(
        _box_line(
            f"  Est. cycles: {pe.estimated_cycles}  "
            f"CPI: {pe.cpi:.2f}  "
            f"IPC: {pe.ipc:.2f}",
            w,
        )
    )
    # Cycle breakdown summary
    if pe.cycle_breakdown:
        top_costs = sorted(
            pe.cycle_breakdown.items(), key=lambda x: x[1], reverse=True
        )[:4]
        breakdown_str = "  Cost: " + ", ".join(f"{k}={int(v)}" for k, v in top_costs)
        print(_box_line(breakdown_str, w))
    print(_box_line("", w))

    # Stack Analysis
    sa = perf.stack_analysis
    print(_box_line(f"{C.BOLD}Stack Analysis{C.RESET}", w))
    print(
        _box_line(
            f"  Functions: {sa.functions}  "
            f"Max frame: {sa.max_frame_size}B  "
            f"Peak depth: {sa.estimated_peak_depth}B",
            w,
        )
    )
    print(
        _box_line(
            f"  Call depth: {sa.call_graph_depth}  "
            f"Total frames: {sa.total_frame_size}B",
            w,
        )
    )
    print(_box_line("", w))

    # Memory Access
    ma = perf.memory_access
    print(_box_line(f"{C.BOLD}Memory Access{C.RESET}", w))
    ls_ratio_str = (
        f"{ma.load_store_ratio:.2f}" if ma.load_store_ratio != float("inf") else "∞"
    )
    locality_color = (
        C.GREEN
        if ma.spatial_locality_score == "good"
        else C.YELLOW
        if ma.spatial_locality_score == "moderate"
        else C.RED
    )
    print(
        _box_line(
            f"  Loads: {ma.total_loads}  "
            f"Stores: {ma.total_stores}  "
            f"L/S ratio: {ls_ratio_str}",
            w,
        )
    )
    print(
        _box_line(
            f"  Stack access: {ma.stack_access_pct:5.1f}%  "
            f"Locality: {locality_color}{ma.spatial_locality_score}{C.RESET}  "
            f"Avg offset: {ma.avg_offset_magnitude:.0f}B",
            w,
        )
    )
    print(_box_line("", w))

    # Cache Estimate
    ce = perf.cache_estimate
    print(_box_line(f"{C.BOLD}Cache Estimate{C.RESET}", w))
    ic_icon = "✓" if ce.code_fits_icache else "✗"
    dc_icon = "✓" if ce.data_fits_dcache else "✗"
    ic_color = C.GREEN if ce.code_fits_icache else C.RED
    dc_color = C.GREEN if ce.data_fits_dcache else C.RED
    print(
        _box_line(
            f"  I-cache: {ic_color}{ic_icon} {ce.icache_verdict}{C.RESET} "
            f"({ce.icache_usage_pct:.1f}% of {ce.icache_size // 1024}KB)",
            w,
        )
    )
    print(
        _box_line(
            f"  D-cache: {dc_color}{dc_icon} {ce.dcache_verdict}{C.RESET} "
            f"({ce.dcache_usage_pct:.1f}% of {ce.dcache_size // 1024}KB)",
            w,
        )
    )
    print(_box_line("", w))

    # Hotspots
    hs = perf.hotspots
    print(_box_line(f"{C.BOLD}Hotspots{C.RESET}", w))
    print(_box_line(f"  Loops detected: {hs.loop_count}", w))
    if hs.densest_functions:
        top_func = hs.densest_functions[0]
        print(
            _box_line(
                f"  Largest func: {top_func['name']} "
                f"({top_func['instruction_count']} insns)",
                w,
            )
        )
    if hs.largest_basic_blocks:
        top_bb = hs.largest_basic_blocks[0]
        print(
            _box_line(
                f"  Largest BB: {top_bb['size']} insns (line {top_bb['start_line']})", w
            )
        )

    print(f"└{'─' * (w + 2)}┘")


def render_aggregate_summary(all_perfs: dict[str, PerfMetrics]):
    """Render aggregate performance summary across all tests."""
    C = Colors
    n = len(all_perfs)
    if n == 0:
        return

    print(f"\n{C.BOLD}{'═' * 80}{C.RESET}")
    print(f"{C.BOLD}{C.CYAN} Performance Summary ({n} tests){C.RESET}")
    print(f"{C.BOLD}{'═' * 80}{C.RESET}")

    # Collect metric arrays
    def _collect(extractor):
        vals = []
        for p in all_perfs.values():
            try:
                v = extractor(p)
                if v is not None and not (
                    isinstance(v, float) and (math.isinf(v) or math.isnan(v))
                ):
                    vals.append(v)
            except (AttributeError, ZeroDivisionError):
                pass
        return vals

    def _stats_row(name, vals, fmt=".3f"):
        if not vals:
            return f"│ {name:<22} │ {'N/A':^10} │ {'N/A':^10} │ {'N/A':^10} │ {'N/A':^10} │"
        mn = min(vals)
        avg = statistics.mean(vals)
        mx = max(vals)
        med = statistics.median(vals)
        return (
            f"│ {name:<22} │ {mn:>{fmt.replace('.', ':').split(':')[0]}10{fmt[fmt.find('.') :]}} │"
            f" {avg:>10{fmt[fmt.find('.') :]}} │"
            f" {mx:>10{fmt[fmt.find('.') :]}} │"
            f" {med:>10{fmt[fmt.find('.') :]}} │"
        )

    def _stats_row_simple(name, vals, fmt_str="{:.3f}"):
        if not vals:
            return f"│ {name:<22} │ {'N/A':^10} │ {'N/A':^10} │ {'N/A':^10} │ {'N/A':^10} │"
        mn = min(vals)
        avg = statistics.mean(vals)
        mx = max(vals)
        med = statistics.median(vals)
        return (
            f"│ {name:<22} │ {fmt_str.format(mn):>10} │"
            f" {fmt_str.format(avg):>10} │"
            f" {fmt_str.format(mx):>10} │"
            f" {fmt_str.format(med):>10} │"
        )

    header = (
        f"│ {'Metric':<22} │ {'Min':^10} │ {'Avg':^10} │ {'Max':^10} │ {'Median':^10} │"
    )
    sep = f"├{'─' * 24}┼{'─' * 12}┼{'─' * 12}┼{'─' * 12}┼{'─' * 12}┤"
    top = f"┌{'─' * 24}┬{'─' * 12}┬{'─' * 12}┬{'─' * 12}┬{'─' * 12}┐"
    bot = f"└{'─' * 24}┴{'─' * 12}┴{'─' * 12}┴{'─' * 12}┴{'─' * 12}┘"

    print(top)
    print(header)
    print(sep)

    metrics_defs = [
        ("Avg Runtime (s)", lambda p: p.runtime.avg, "{:.4f}"),
        ("Weighted Runtime (s)", lambda p: p.runtime.weighted_avg, "{:.4f}"),
        ("Code Size (B)", lambda p: float(p.code_density.text_size_bytes), "{:.0f}"),
        (
            "Instruction Count",
            lambda p: float(p.code_density.total_instructions),
            "{:.0f}",
        ),
        ("Bytes/Instruction", lambda p: p.code_density.bytes_per_instruction, "{:.2f}"),
        ("CPI Estimate", lambda p: p.pipeline_estimate.cpi, "{:.2f}"),
        ("IPC Estimate", lambda p: p.pipeline_estimate.ipc, "{:.2f}"),
        (
            "Int Reg Pressure %",
            lambda p: p.register_pressure.int_pressure_pct,
            "{:.1f}",
        ),
        (
            "Float Reg Pressure %",
            lambda p: p.register_pressure.float_pressure_pct,
            "{:.1f}",
        ),
        ("Branch Predict %", lambda p: p.branch_analysis.predicted_hit_rate, "{:.1f}"),
        ("Branch Density", lambda p: p.branch_analysis.branch_density, "{:.1f}"),
        ("Stack Spills", lambda p: float(p.register_pressure.stack_spills), "{:.0f}"),
        ("L/S Ratio", lambda p: p.memory_access.load_store_ratio, "{:.2f}"),
        ("Stack Access %", lambda p: p.memory_access.stack_access_pct, "{:.1f}"),
        ("Loop Count", lambda p: float(p.hotspots.loop_count), "{:.0f}"),
        (
            "Peak Stack Depth (B)",
            lambda p: float(p.stack_analysis.estimated_peak_depth),
            "{:.0f}",
        ),
    ]

    for name, extractor, fmt_str in metrics_defs:
        vals = _collect(extractor)
        print(_stats_row_simple(name, vals, fmt_str))

    print(bot)

    # Instruction mix aggregate
    print(f"\n{C.BOLD}  Instruction Mix Aggregate:{C.RESET}")
    total_arith = sum(p.instruction_mix.arith for p in all_perfs.values())
    total_ls = sum(p.instruction_mix.load_store for p in all_perfs.values())
    total_br = sum(p.instruction_mix.branch for p in all_perfs.values())
    total_jmp = sum(p.instruction_mix.jump for p in all_perfs.values())
    total_flt = sum(p.instruction_mix.float_ops for p in all_perfs.values())
    total_oth = sum(p.instruction_mix.other for p in all_perfs.values())
    grand_total = total_arith + total_ls + total_br + total_jmp + total_flt + total_oth

    if grand_total > 0:
        print(
            f"    Arithmetic:  {total_arith:>8}  ({total_arith / grand_total * 100:5.1f}%)  "
            f"{_pct_bar(total_arith / grand_total * 100, 20)}"
        )
        print(
            f"    Load/Store:  {total_ls:>8}  ({total_ls / grand_total * 100:5.1f}%)  "
            f"{_pct_bar(total_ls / grand_total * 100, 20)}"
        )
        print(
            f"    Branch:      {total_br:>8}  ({total_br / grand_total * 100:5.1f}%)  "
            f"{_pct_bar(total_br / grand_total * 100, 20)}"
        )
        print(
            f"    Jump:        {total_jmp:>8}  ({total_jmp / grand_total * 100:5.1f}%)  "
            f"{_pct_bar(total_jmp / grand_total * 100, 20)}"
        )
        print(
            f"    Float:       {total_flt:>8}  ({total_flt / grand_total * 100:5.1f}%)  "
            f"{_pct_bar(total_flt / grand_total * 100, 20)}"
        )
        print(
            f"    Other:       {total_oth:>8}  ({total_oth / grand_total * 100:5.1f}%)  "
            f"{_pct_bar(total_oth / grand_total * 100, 20)}"
        )

    # Cache pressure summary
    icache_miss_count = sum(
        1 for p in all_perfs.values() if not p.cache_estimate.code_fits_icache
    )
    dcache_miss_count = sum(
        1 for p in all_perfs.values() if not p.cache_estimate.data_fits_dcache
    )
    print(f"\n{C.BOLD}  Cache Pressure:{C.RESET}")
    print(f"    I-cache pressure: {icache_miss_count}/{n} tests exceed 32KB")
    print(f"    D-cache pressure: {dcache_miss_count}/{n} tests exceed 32KB")

    # Locality summary
    locality_counts = Counter(
        p.memory_access.spatial_locality_score for p in all_perfs.values()
    )
    print(f"\n{C.BOLD}  Spatial Locality:{C.RESET}")
    for loc in ("good", "moderate", "poor", "unknown"):
        cnt = locality_counts.get(loc, 0)
        if cnt > 0:
            color = (
                C.GREEN
                if loc == "good"
                else C.YELLOW
                if loc == "moderate"
                else C.RED
                if loc == "poor"
                else C.DIM
            )
            print(f"    {color}{loc:>10}{C.RESET}: {cnt}/{n} tests")


def write_json_report(
    filepath: str,
    all_results: dict,
    all_perfs: dict[str, PerfMetrics],
    compiler_opts: str,
    elapsed: float,
):
    """Write detailed JSON performance report."""
    report = {
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "compiler_opts": compiler_opts,
        "total_elapsed_seconds": elapsed,
        "tests": {},
        "aggregate": {},
    }

    for test_file, (status, msg) in all_results.items():
        test_name = os.path.splitext(os.path.basename(test_file))[0]
        entry = {"status": status}

        if test_name in all_perfs:
            perf = all_perfs[test_name]
            entry["runtime"] = asdict(perf.runtime)
            entry["code_density"] = asdict(perf.code_density)
            entry["instruction_mix"] = asdict(perf.instruction_mix)
            entry["register_pressure"] = asdict(perf.register_pressure)
            entry["branch_analysis"] = asdict(perf.branch_analysis)
            entry["pipeline_estimate"] = asdict(perf.pipeline_estimate)
            entry["stack_analysis"] = asdict(perf.stack_analysis)
            entry["memory_access"] = asdict(perf.memory_access)
            entry["cache_estimate"] = asdict(perf.cache_estimate)
            entry["hotspots"] = asdict(perf.hotspots)

        report["tests"][test_name] = entry

    # Compute aggregate statistics
    if all_perfs:
        agg = {}
        metric_extractors = {
            "avg_runtime": lambda p: p.runtime.avg,
            "weighted_runtime": lambda p: p.runtime.weighted_avg,
            "code_size": lambda p: p.code_density.text_size_bytes,
            "instruction_count": lambda p: p.code_density.total_instructions,
            "cpi": lambda p: p.pipeline_estimate.cpi,
            "ipc": lambda p: p.pipeline_estimate.ipc,
            "int_reg_pressure_pct": lambda p: p.register_pressure.int_pressure_pct,
            "branch_predict_pct": lambda p: p.branch_analysis.predicted_hit_rate,
            "stack_spills": lambda p: p.register_pressure.stack_spills,
            "load_store_ratio": lambda p: p.memory_access.load_store_ratio,
            "loop_count": lambda p: p.hotspots.loop_count,
            "peak_stack_depth": lambda p: p.stack_analysis.estimated_peak_depth,
        }

        for metric_name, extractor in metric_extractors.items():
            vals = []
            for p in all_perfs.values():
                try:
                    v = extractor(p)
                    if v is not None and not (
                        isinstance(v, float) and (math.isinf(v) or math.isnan(v))
                    ):
                        vals.append(float(v))
                except (AttributeError, ZeroDivisionError):
                    pass
            if vals:
                agg[metric_name] = {
                    "min": min(vals),
                    "avg": statistics.mean(vals),
                    "max": max(vals),
                    "median": statistics.median(vals),
                    "stddev": statistics.stdev(vals) if len(vals) > 1 else 0.0,
                }

        # Instruction mix aggregate
        total_by_cat = {
            "arith": sum(p.instruction_mix.arith for p in all_perfs.values()),
            "load_store": sum(p.instruction_mix.load_store for p in all_perfs.values()),
            "branch": sum(p.instruction_mix.branch for p in all_perfs.values()),
            "jump": sum(p.instruction_mix.jump for p in all_perfs.values()),
            "float": sum(p.instruction_mix.float_ops for p in all_perfs.values()),
            "other": sum(p.instruction_mix.other for p in all_perfs.values()),
        }
        grand_total = sum(total_by_cat.values())
        if grand_total > 0:
            agg["instruction_mix_pct"] = {
                k: v / grand_total * 100 for k, v in total_by_cat.items()
            }

        report["aggregate"] = agg

    # Handle float('inf') serialization
    def _json_default(obj):
        if isinstance(obj, float):
            if math.isinf(obj):
                return "Infinity"
            if math.isnan(obj):
                return "NaN"
        raise TypeError(f"Object of type {type(obj)} is not JSON serializable")

    with open(filepath, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2, default=_json_default)


# ============================================================================
# Core Test Runner (preserves original functionality)
# ============================================================================


def normalize_and_compare(actual_stdout, exit_code, expected_content):
    """
    Normalizes and compares the output of a test case.
    SysY output validation format:
    1. Output printed to stdout by the program.
    2. If the program output doesn't end with a newline (and is not empty), a newline is appended.
    3. The return code (exit_code & 0xFF) followed by a newline.
    """
    actual_formatted = actual_stdout
    if actual_formatted and not actual_formatted.endswith("\n"):
        actual_formatted += "\n"
    actual_formatted += f"{exit_code & 0xFF}\n"

    # Split lines and strip trailing whitespaces of each line
    actual_lines = [line.rstrip() for line in actual_formatted.splitlines()]
    expected_lines = [line.rstrip() for line in expected_content.splitlines()]

    # Filter out trailing empty lines
    while actual_lines and actual_lines[-1] == "":
        actual_lines.pop()
    while expected_lines and expected_lines[-1] == "":
        expected_lines.pop()

    return actual_lines == expected_lines, actual_lines, expected_lines


def run_test(test_file, args):
    """Run a single test case: compile, link, execute, compare output."""
    test_name = os.path.splitext(os.path.basename(test_file))[0]
    temp_dir = os.path.abspath(f"build/tmp_validate/{test_name}")
    output_dir = os.path.join(temp_dir, "output")
    in_file = os.path.splitext(test_file)[0] + ".in"
    out_file = os.path.splitext(test_file)[0] + ".out"

    # Create temporary isolated directory for the test case
    os.makedirs(output_dir, exist_ok=True)

    # 1. Compile .sy to RISC-V assembly using Exodus compiler
    # Split compiler options if provided
    opt_list = args.opt.split() if args.opt else []
    compiler_cmd = [
        os.path.abspath(args.compiler),
        os.path.abspath(test_file),
    ] + opt_list

    comp_res = subprocess.run(
        compiler_cmd, cwd=temp_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    if comp_res.returncode != 0:
        return "COMPILE_FAILED", (
            f"Compiler command: {' '.join(compiler_cmd)}\n"
            f"Exit Code: {comp_res.returncode}\n"
            f"Compiler Output:\n{comp_res.stdout.decode('utf-8', errors='ignore')}\n"
            f"Compiler Errors:\n{comp_res.stderr.decode('utf-8', errors='ignore')}"
        )

    # Verify generated assembly file output/output.s exists and is not empty
    asm_path = os.path.join(temp_dir, "output", "output.s")
    if not os.path.exists(asm_path) or os.path.getsize(asm_path) == 0:
        return (
            "COMPILE_FAILED",
            "Compiler succeeded but did not produce assembly output at output/output.s",
        )

    # 2. Compile RISC-V assembly using riscv64-linux-gnu-gcc
    gcc_cmd = [
        args.gcc,
        "-static",
        "output/output.s",
        f"-L{os.path.abspath(args.libdir)}",
        "-lsysy",
        "-o",
        "test.elf",
        "-lm",
    ]
    gcc_res = subprocess.run(
        gcc_cmd, cwd=temp_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    if gcc_res.returncode != 0:
        return "LINK_FAILED", (
            f"GCC command: {' '.join(gcc_cmd)}\n"
            f"Exit Code: {gcc_res.returncode}\n"
            f"GCC Output:\n{gcc_res.stdout.decode('utf-8', errors='ignore')}\n"
            f"GCC Errors:\n{gcc_res.stderr.decode('utf-8', errors='ignore')}"
        )

    # 3. Run RISC-V executable using QEMU
    qemu_cmd = [args.qemu, "./test.elf"]

    stdin_f = None
    if os.path.exists(in_file):
        stdin_f = open(in_file, "rb")

    try:
        run_res = subprocess.run(
            qemu_cmd,
            cwd=temp_dir,
            stdin=stdin_f,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=args.timeout,
        )
        actual_stdout = run_res.stdout.decode("utf-8", errors="ignore")
        actual_stderr = run_res.stderr.decode("utf-8", errors="ignore")
        exit_code = run_res.returncode
    except subprocess.TimeoutExpired:
        return "TIMEOUT", f"Execution timed out after {args.timeout} seconds."
    finally:
        if stdin_f:
            stdin_f.close()

    # Detect program crashes
    if exit_code < 0:
        return "CRASHED", (
            f"Program crashed with signal {-exit_code}.\nStderr:\n{actual_stderr}"
        )

    # 4. Compare output against expected .out file
    if not os.path.exists(out_file):
        return "MISSING_OUT", f"Expected output file not found: {out_file}"

    with open(out_file, "r", encoding="utf-8", errors="ignore") as f:
        expected_content = f.read()

    matched, actual_lines, expected_lines = normalize_and_compare(
        actual_stdout, exit_code, expected_content
    )
    if matched:
        # Clean up temporary test directory if passed and not requested to keep
        if not args.keep_temps and not getattr(args, "perf", False):
            shutil.rmtree(temp_dir)
        return "PASSED", ""
    else:
        # Build unified diff on mismatch
        diff = list(
            difflib.unified_diff(
                expected_lines,
                actual_lines,
                fromfile="Expected Output (.out)",
                tofile="Actual Output (stdout + exit_code)",
                lineterm="",
            )
        )
        diff_str = "\n".join(diff)
        return "FAILED", f"Output mismatch!\n{diff_str}"


def run_perf_analysis(test_file, args) -> Optional[PerfMetrics]:
    """Run comprehensive performance analysis on a passed test case."""
    test_name = os.path.splitext(os.path.basename(test_file))[0]
    temp_dir = os.path.abspath(f"build/tmp_validate/{test_name}")
    asm_path = os.path.join(temp_dir, "output", "output.s")
    in_file = os.path.splitext(test_file)[0] + ".in"

    if not os.path.exists(asm_path):
        return None

    perf = PerfMetrics()

    try:
        # 1. Parse assembly
        parser = RISCVAsmParser(asm_path)
        if not parser.instructions:
            return None

        # 2. Runtime metrics (multiple runs)
        qemu_cmd = [args.qemu, "./test.elf"]
        perf.runtime = analyze_runtime(
            qemu_cmd,
            temp_dir,
            in_file if os.path.exists(in_file) else None,
            args.timeout,
            args.perf_runs,
        )

        # 3. Code density
        perf.code_density = analyze_code_density(parser, temp_dir, args.gcc)

        # 4. Instruction mix
        perf.instruction_mix = analyze_instruction_mix(parser)

        # 5. Register pressure
        perf.register_pressure = analyze_register_pressure(parser)

        # 6. Branch analysis
        perf.branch_analysis = analyze_branches(parser)

        # 7. Pipeline estimation
        perf.pipeline_estimate = analyze_pipeline(parser, perf.branch_analysis)

        # 8. Stack analysis
        perf.stack_analysis = analyze_stack(parser)

        # 9. Memory access patterns
        perf.memory_access = analyze_memory_access(parser)

        # 10. Cache estimation
        perf.cache_estimate = analyze_cache(perf.code_density, perf.stack_analysis)

        # 11. Hotspot identification
        perf.hotspots = analyze_hotspots(parser)

    except Exception as e:
        print(
            f"  {Colors.YELLOW}Warning: Performance analysis error for {test_name}: {e}{Colors.RESET}"
        )
        return None

    # Clean up if not keeping temps
    if not args.keep_temps:
        shutil.rmtree(temp_dir, ignore_errors=True)

    return perf


# ============================================================================
# Main Entry Point
# ============================================================================


def main():
    parser = argparse.ArgumentParser(
        description="RISC-V Test Validator & Performance Analyzer for Exodus Compiler"
    )
    parser.add_argument(
        "dir",
        nargs="?",
        default="tests/functional",
        help="Directory containing tests (default: tests/functional)",
    )
    parser.add_argument(
        "-c",
        "--compiler",
        default="build/compiler",
        help="Path to compiler executable (default: build/compiler)",
    )
    parser.add_argument(
        "-g",
        "--gcc",
        default="riscv64-linux-gnu-gcc",
        help="Path to RISC-V GCC (default: riscv64-linux-gnu-gcc)",
    )
    parser.add_argument(
        "-q",
        "--qemu",
        default="qemu-riscv64",
        help="Path to QEMU runner (default: qemu-riscv64)",
    )
    parser.add_argument(
        "-l",
        "--libdir",
        default="lib",
        help="Directory containing libsysy.a (default: lib)",
    )
    parser.add_argument(
        "-o", "--opt", default="", help="Compiler optimization options (e.g. '-O2')"
    )
    parser.add_argument(
        "-t",
        "--timeout",
        type=float,
        default=10.0,
        help="Timeout in seconds for QEMU execution (default: 10.0)",
    )
    parser.add_argument(
        "-j", "--jobs", type=int, default=4, help="Number of parallel jobs (default: 4)"
    )
    parser.add_argument("-v", "--verbose", action="store_true", help="Verbose output")
    parser.add_argument(
        "-s", "--stop-on-fail", action="store_true", help="Stop on first failure"
    )
    parser.add_argument(
        "--keep-temps",
        action="store_true",
        help="Keep all temporary files for passed tests as well",
    )

    # Performance analysis options
    parser.add_argument(
        "--perf",
        action="store_true",
        help="Enable comprehensive performance analysis for passed tests",
    )
    parser.add_argument(
        "--perf-runs",
        type=int,
        default=3,
        help="Number of repeated runs for runtime measurement (default: 3)",
    )
    parser.add_argument(
        "--perf-report",
        type=str,
        default=None,
        metavar="FILE",
        help="Write JSON performance report to FILE",
    )

    args = parser.parse_args()

    # Pre-checks
    compiler_abs = os.path.abspath(args.compiler)
    if not os.path.exists(compiler_abs):
        print(
            f"{Colors.RED}Error: Compiler binary not found at {args.compiler}{Colors.RESET}"
        )
        print("Please build the project first (e.g. run 'make').")
        return 1

    if not shutil.which(args.gcc):
        print(
            f"{Colors.RED}Error: RISC-V GCC compiler '{args.gcc}' not found in PATH.{Colors.RESET}"
        )
        print("Please make sure your RISC-V toolchain is installed and added to PATH.")
        return 1

    if not shutil.which(args.qemu):
        print(
            f"{Colors.RED}Error: QEMU runner '{args.qemu}' not found in PATH.{Colors.RESET}"
        )
        print("Please install QEMU user space emulator (e.g. pacman -S qemu-user).")
        return 1

    if not os.path.exists(args.dir):
        print(
            f"{Colors.RED}Error: Validation directory '{args.dir}' not found.{Colors.RESET}"
        )
        return 1

    # Find all .sy files in specified directory
    test_files = sorted(
        [os.path.join(args.dir, f) for f in os.listdir(args.dir) if f.endswith(".sy")]
    )

    if not test_files:
        print(
            f"{Colors.YELLOW}No .sy files found in directory {args.dir}{Colors.RESET}"
        )
        return 1

    mode_str = (
        f" + {Colors.CYAN}Performance Analysis{Colors.RESET}" if args.perf else ""
    )
    print(
        f"{Colors.BOLD}Starting validation{mode_str} on {len(test_files)} tests "
        f"under '{args.dir}'...{Colors.RESET}"
    )
    print(f"Jobs: {args.jobs} | Timeout: {args.timeout}s | Compiler Opts: '{args.opt}'")
    if args.perf:
        print(f"Perf runs: {args.perf_runs} | Report: {args.perf_report or 'none'}")
    print("=" * 60)

    passed_count = 0
    results = {}
    passed_files = []  # Track passed tests for perf analysis

    start_time = time.time()

    # Phase 1: Run correctness tests in parallel
    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        future_to_file = {executor.submit(run_test, f, args): f for f in test_files}

        try:
            for future in as_completed(future_to_file):
                test_file = future_to_file[future]
                test_name = os.path.basename(test_file)

                try:
                    status, msg = future.result()
                except Exception as e:
                    status, msg = (
                        "ERROR",
                        f"Python runner encountered an exception: {e}",
                    )

                results[test_file] = (status, msg)

                if status == "PASSED":
                    passed_count += 1
                    passed_files.append(test_file)
                    print(f"[{Colors.GREEN} PASS {Colors.RESET}] {test_name}")
                else:
                    print(f"[{Colors.RED} {status} {Colors.RESET}] {test_name}")
                    if status != "PASSED":
                        print(f"  {Colors.YELLOW}Details:{Colors.RESET}")
                        for line in msg.splitlines():
                            print(f"    {line}")
                    if args.stop_on_fail:
                        print(
                            f"\n{Colors.RED}Stop-on-fail triggered. Cancelling pending tests...{Colors.RESET}"
                        )
                        # Cancel remaining execution
                        for fut in future_to_file:
                            fut.cancel()
                        break
        except KeyboardInterrupt:
            print(f"\n{Colors.RED}Validation interrupted by user.{Colors.RESET}")
            executor.shutdown(wait=False, cancel_futures=True)
            return 1

    elapsed = time.time() - start_time
    total = len(test_files)
    failed_count = total - passed_count

    print("=" * 60)
    print(f"{Colors.BOLD}Validation Summary:{Colors.RESET}")
    print(f"  Total tests: {total}")
    print(f"  Passed:      {Colors.GREEN}{passed_count}/{total}{Colors.RESET}")
    if failed_count > 0:
        print(f"  Failed:      {Colors.RED}{failed_count}/{total}{Colors.RESET}")
        print("\nFailed tests:")
        for f, (status, msg) in results.items():
            if status != "PASSED":
                name = os.path.basename(f)
                name_no_ext = os.path.splitext(name)[0]
                print(f"  - {Colors.RED}{name}{Colors.RESET} ({status})")
                print(
                    f"    Temp files kept at: {Colors.BLUE}build/tmp_validate/{name_no_ext}/{Colors.RESET}"
                )
    else:
        print(f"  {Colors.GREEN}All tests passed successfully!{Colors.RESET}")
    print(f"Time elapsed: {elapsed:.2f} seconds")

    # Phase 2: Performance analysis (sequential, for passed tests only)
    all_perfs = {}
    if args.perf and passed_files:
        print(f"\n{'=' * 60}")
        print(
            f"{Colors.BOLD}{Colors.CYAN}Starting Performance Analysis "
            f"({len(passed_files)} passed tests)...{Colors.RESET}"
        )
        print(f"{'=' * 60}\n")

        perf_start = time.time()
        for i, test_file in enumerate(sorted(passed_files), 1):
            test_name = os.path.splitext(os.path.basename(test_file))[0]
            print(
                f"{Colors.DIM}[{i}/{len(passed_files)}] Analyzing {test_name}...{Colors.RESET}"
            )

            perf_metrics = run_perf_analysis(test_file, args)
            if perf_metrics:
                all_perfs[test_name] = perf_metrics
                if args.verbose:
                    render_perf_card(test_name, perf_metrics)
                    print()

        perf_elapsed = time.time() - perf_start

        # Print per-test perf cards (non-verbose: print all at end)
        if not args.verbose and all_perfs:
            print()
            for test_name, perf_metrics in sorted(all_perfs.items()):
                render_perf_card(test_name, perf_metrics)
                print()

        # Print aggregate summary
        if all_perfs:
            render_aggregate_summary(all_perfs)

        print(
            f"\n{Colors.DIM}Performance analysis took {perf_elapsed:.2f} seconds{Colors.RESET}"
        )

        # Write JSON report if requested
        if args.perf_report:
            write_json_report(
                args.perf_report, results, all_perfs, args.opt, elapsed + perf_elapsed
            )
            print(
                f"{Colors.GREEN}Performance report written to: "
                f"{Colors.BOLD}{args.perf_report}{Colors.RESET}"
            )

    return 1 if failed_count > 0 else 0


if __name__ == "__main__":
    sys.exit(main())
