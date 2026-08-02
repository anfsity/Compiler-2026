#!/usr/bin/env python3
"""
Compare Exodus against clang -O3 on SysY performance tests.

The harness keeps the comparison ABI-matched: both compilers produce RV64
assembly, both are linked by the same RISC-V GCC against the same libsysy.a,
and every timing sample is accepted only after stdout and exit status match
the expected .out file.
"""

from __future__ import annotations

import argparse
import csv
import difflib
import json
import math
import os
import platform
import shlex
import shutil
import statistics
import subprocess
import sys
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any, Optional


SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent
sys.path.insert(0, str(SCRIPT_DIR))

import validate_riscv as vr  # noqa: E402


DEFAULT_CLANG_SYSY_COMPAT_FLAGS = [
    "-Wno-incompatible-pointer-types",
    "-fwrapv",
    "-ffp-contract=off",
]


@dataclass
class Artifact:
    compiler: str
    temp_dir: Path
    asm_path: Path
    elf_path: Path
    compile_cmd: list[str]
    link_cmd: list[str]


@dataclass
class RunOutcome:
    valid: bool
    elapsed: Optional[float] = None
    guest_total_us: Optional[int] = None
    timeout: bool = False
    message: str = ""


class CaseError(RuntimeError):
    def __init__(self, status: str, message: str):
        super().__init__(message)
        self.status = status
        self.message = message


def run_command(
    cmd: list[str],
    *,
    cwd: Path,
    timeout: Optional[float],
) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(
        cmd,
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=timeout,
        check=False,
    )


def decode(data: bytes) -> str:
    return data.decode("utf-8", errors="ignore")


def truncate(text: str, limit: int = 6000) -> str:
    if len(text) <= limit:
        return text
    return text[:limit] + "\n... <truncated> ..."


def command_text(cmd: list[str]) -> str:
    return " ".join(shlex.quote(part) for part in cmd)


def clean_case_dir(path: Path) -> None:
    if path.exists():
        shutil.rmtree(path)
    (path / "output").mkdir(parents=True, exist_ok=True)


def ensure_success(
    res: subprocess.CompletedProcess[bytes],
    *,
    status: str,
    cmd: list[str],
) -> None:
    if res.returncode == 0:
        return
    message = (
        f"Command: {command_text(cmd)}\n"
        f"Exit Code: {res.returncode}\n"
        f"Stdout:\n{truncate(decode(res.stdout))}\n"
        f"Stderr:\n{truncate(decode(res.stderr))}"
    )
    raise CaseError(status, message)


def link_asm(temp_dir: Path, args: argparse.Namespace) -> list[str]:
    cmd = [
        args.gcc,
        "-static",
        "output/output.s",
        f"-L{args.libdir.resolve()}",
        "-lsysy",
        "-o",
        "test.elf",
        "-lm",
    ]
    try:
        res = run_command(cmd, cwd=temp_dir, timeout=args.compile_timeout)
    except subprocess.TimeoutExpired as exc:
        raise CaseError("LINK_TIMEOUT", f"Command timed out: {command_text(cmd)}") from exc
    ensure_success(res, status="LINK_FAILED", cmd=cmd)
    elf_path = temp_dir / "test.elf"
    if not elf_path.exists() or elf_path.stat().st_size == 0:
        raise CaseError("LINK_FAILED", "Link succeeded but did not produce test.elf")
    return cmd


def compile_exodus(test_file: Path, temp_dir: Path, args: argparse.Namespace) -> Artifact:
    clean_case_dir(temp_dir)
    cmd = [str(args.compiler.resolve()), str(test_file.resolve())]
    cmd.extend(shlex.split(args.exodus_opt))
    try:
        res = run_command(cmd, cwd=temp_dir, timeout=args.compile_timeout)
    except subprocess.TimeoutExpired as exc:
        raise CaseError("EXODUS_COMPILE_TIMEOUT", f"Command timed out: {command_text(cmd)}") from exc
    ensure_success(res, status="EXODUS_COMPILE_FAILED", cmd=cmd)

    asm_path = temp_dir / "output" / "output.s"
    if not asm_path.exists() or asm_path.stat().st_size == 0:
        raise CaseError(
            "EXODUS_COMPILE_FAILED",
            "Exodus succeeded but did not produce output/output.s",
        )
    link_cmd = link_asm(temp_dir, args)
    return Artifact("exodus", temp_dir, asm_path, temp_dir / "test.elf", cmd, link_cmd)


def compile_clang(test_file: Path, temp_dir: Path, args: argparse.Namespace) -> Artifact:
    clean_case_dir(temp_dir)
    cmd = [
        args.clang,
        f"--target={args.target}",
        f"-march={args.march}",
        f"-mabi={args.mabi}",
    ]
    if args.sysroot:
        cmd.append(f"--sysroot={args.sysroot}")
    cmd.extend(shlex.split(args.clang_opt))
    if args.clang_sysy_compat:
        cmd.extend(args.clang_sysy_compat_flags)
    cmd.extend(
        [
            "-x",
            "c",
            "-std=c99",
            "-include",
            str((args.libdir / "sylib.h").resolve()),
            "-fcommon",
            "-fno-addrsig",
            "-S",
            str(test_file.resolve()),
            "-o",
            "output/output.s",
        ]
    )
    cmd.extend(shlex.split(args.clang_extra))

    try:
        res = run_command(cmd, cwd=temp_dir, timeout=args.compile_timeout)
    except subprocess.TimeoutExpired as exc:
        raise CaseError("CLANG_COMPILE_TIMEOUT", f"Command timed out: {command_text(cmd)}") from exc
    ensure_success(res, status="CLANG_COMPILE_FAILED", cmd=cmd)

    asm_path = temp_dir / "output" / "output.s"
    if not asm_path.exists() or asm_path.stat().st_size == 0:
        raise CaseError(
            "CLANG_COMPILE_FAILED",
            "clang succeeded but did not produce output/output.s",
        )
    link_cmd = link_asm(temp_dir, args)
    return Artifact("clang", temp_dir, asm_path, temp_dir / "test.elf", cmd, link_cmd)


def run_binary(
    artifact: Artifact,
    *,
    qemu: str,
    in_file: Optional[Path],
    expected_content: str,
    timeout: float,
) -> RunOutcome:
    stdin_f = None
    if in_file and in_file.exists():
        stdin_f = in_file.open("rb")
    try:
        start = time.perf_counter()
        res = subprocess.run(
            [qemu, "./test.elf"],
            cwd=artifact.temp_dir,
            stdin=stdin_f,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
            check=False,
        )
        elapsed = time.perf_counter() - start
    except subprocess.TimeoutExpired:
        return RunOutcome(False, timeout=True, message=f"timed out after {timeout}s")
    finally:
        if stdin_f:
            stdin_f.close()

    stdout = decode(res.stdout)
    stderr = decode(res.stderr)
    if res.returncode < 0:
        return RunOutcome(False, elapsed=elapsed, message=f"signal {-res.returncode}: {stderr}")

    matched, actual_lines, expected_lines = vr.normalize_and_compare(
        stdout, res.returncode, expected_content
    )
    if not matched:
        diff = "\n".join(
            difflib.unified_diff(
                expected_lines,
                actual_lines,
                fromfile="Expected",
                tofile="Actual",
                lineterm="",
            )
        )
        return RunOutcome(False, elapsed=elapsed, message=f"output mismatch\n{diff}")

    return RunOutcome(
        True,
        elapsed=elapsed,
        guest_total_us=vr._parse_guest_total_us(stderr),
    )


def summarize_samples(raw: dict[str, Any]) -> dict[str, Any]:
    samples = raw["samples"]
    host_runs = [sample["host_seconds"] for sample in samples]
    guest_runs = [
        sample["guest_total_us"]
        for sample in samples
        if sample["guest_total_us"] is not None
    ]
    summary: dict[str, Any] = {
        "samples": samples,
        "warmup_runs": raw["warmup_runs"],
        "warmup_invalid_runs": raw["warmup_invalid_runs"],
        "valid_runs": len(host_runs),
        "invalid_runs": raw["invalid_runs"],
        "timeout_runs": raw["timeout_runs"],
        "measurement": "host_wall_time_with_libsysy_total"
        if guest_runs
        else "host_wall_time",
        "host_seconds": {
            "avg": statistics.mean(host_runs) if host_runs else None,
            "median": statistics.median(host_runs) if host_runs else None,
            "min": min(host_runs) if host_runs else None,
            "max": max(host_runs) if host_runs else None,
            "stddev": statistics.stdev(host_runs) if len(host_runs) > 1 else 0.0,
        },
        "guest_total_us": {
            "avg": statistics.mean(guest_runs) if guest_runs else None,
            "median": statistics.median(guest_runs) if guest_runs else None,
            "min": min(guest_runs) if guest_runs else None,
            "max": max(guest_runs) if guest_runs else None,
            "stddev": statistics.stdev(guest_runs) if len(guest_runs) > 1 else 0.0,
        },
    }
    return summary


def measure_pair(
    test_name: str,
    artifacts: dict[str, Artifact],
    args: argparse.Namespace,
    in_file: Optional[Path],
    expected_content: str,
) -> dict[str, dict[str, Any]]:
    raw: dict[str, dict[str, Any]] = {
        name: {
            "samples": [],
            "invalid_runs": 0,
            "timeout_runs": 0,
            "warmup_runs": args.warmup,
            "warmup_invalid_runs": 0,
        }
        for name in artifacts
    }

    for _ in range(args.warmup):
        for name in ("exodus", "clang"):
            outcome = run_binary(
                artifacts[name],
                qemu=args.qemu,
                in_file=in_file,
                expected_content=expected_content,
                timeout=args.timeout,
            )
            if not outcome.valid:
                raw[name]["warmup_invalid_runs"] += 1

    for run_index in range(args.runs):
        order = ("exodus", "clang") if run_index % 2 == 0 else ("clang", "exodus")
        for name in order:
            outcome = run_binary(
                artifacts[name],
                qemu=args.qemu,
                in_file=in_file,
                expected_content=expected_content,
                timeout=args.timeout,
            )
            if outcome.timeout:
                raw[name]["timeout_runs"] += 1
                continue
            if not outcome.valid or outcome.elapsed is None:
                raw[name]["invalid_runs"] += 1
                continue
            raw[name]["samples"].append(
                {
                    "test": test_name,
                    "compiler": name,
                    "run_index": run_index + 1,
                    "host_seconds": outcome.elapsed,
                    "guest_total_us": outcome.guest_total_us,
                }
            )

    return {name: summarize_samples(data) for name, data in raw.items()}


def analyze_static(artifact: Artifact, gcc: str) -> dict[str, Any]:
    try:
        parser = vr.RISCVAsmParser(str(artifact.asm_path))
        if not parser.instructions:
            return {"error": "no assembly instructions parsed"}

        perf = vr.PerfMetrics()
        perf.code_density = vr.analyze_code_density(parser, str(artifact.temp_dir), gcc)
        perf.instruction_mix = vr.analyze_instruction_mix(parser)
        perf.register_pressure = vr.analyze_register_pressure(parser)
        perf.branch_analysis = vr.analyze_branches(parser)
        perf.pipeline_estimate = vr.analyze_pipeline(parser, perf.branch_analysis)
        perf.stack_analysis = vr.analyze_stack(parser)
        perf.memory_access = vr.analyze_memory_access(parser)
        perf.cache_estimate = vr.analyze_cache(perf.code_density, perf.stack_analysis)
        perf.hotspots = vr.analyze_hotspots(parser)
        return {
            "code_density": asdict(perf.code_density),
            "instruction_mix": asdict(perf.instruction_mix),
            "register_pressure": asdict(perf.register_pressure),
            "branch_analysis": asdict(perf.branch_analysis),
            "pipeline_estimate": asdict(perf.pipeline_estimate),
            "stack_analysis": asdict(perf.stack_analysis),
            "memory_access": asdict(perf.memory_access),
            "cache_estimate": asdict(perf.cache_estimate),
            "hotspots": asdict(perf.hotspots),
        }
    except Exception as exc:  # keep comparison report alive for other cases
        return {"error": str(exc)}


def safe_ratio(numerator: Optional[float], denominator: Optional[float]) -> Optional[float]:
    if numerator is None or denominator is None or denominator == 0:
        return None
    return numerator / denominator


def compare_case(test_file: Path, args: argparse.Namespace) -> dict[str, Any]:
    test_name = test_file.stem
    in_file = test_file.with_suffix(".in")
    out_file = test_file.with_suffix(".out")
    if not out_file.exists():
        raise CaseError("MISSING_OUT", f"Expected output file not found: {out_file}")
    expected_content = out_file.read_text(encoding="utf-8", errors="ignore")

    case_root = args.temp_root / test_name
    artifacts = {
        "exodus": compile_exodus(test_file, case_root / "exodus", args),
        "clang": compile_clang(test_file, case_root / "clang", args),
    }

    for name, artifact in artifacts.items():
        outcome = run_binary(
            artifact,
            qemu=args.qemu,
            in_file=in_file if in_file.exists() else None,
            expected_content=expected_content,
            timeout=args.timeout,
        )
        if not outcome.valid:
            raise CaseError(f"{name.upper()}_RUN_FAILED", outcome.message)

    runtime = measure_pair(
        test_name,
        artifacts,
        args,
        in_file if in_file.exists() else None,
        expected_content,
    )
    static = {
        name: analyze_static(artifact, args.gcc)
        for name, artifact in artifacts.items()
    }

    exodus_host = runtime["exodus"]["host_seconds"]["median"]
    clang_host = runtime["clang"]["host_seconds"]["median"]
    exodus_guest = runtime["exodus"]["guest_total_us"]["median"]
    clang_guest = runtime["clang"]["guest_total_us"]["median"]
    exodus_insns = (
        static["exodus"].get("code_density", {}).get("total_instructions")
    )
    clang_insns = static["clang"].get("code_density", {}).get("total_instructions")

    status = "PASSED"
    for name in ("exodus", "clang"):
        if runtime[name]["valid_runs"] != args.runs:
            status = "MEASURE_PARTIAL"

    return {
        "status": status,
        "input": str(in_file) if in_file.exists() else None,
        "expected": str(out_file),
        "commands": {
            name: {
                "compile": command_text(artifact.compile_cmd),
                "link": command_text(artifact.link_cmd),
            }
            for name, artifact in artifacts.items()
        },
        "artifacts": {
            name: {
                "temp_dir": str(artifact.temp_dir),
                "assembly": str(artifact.asm_path),
                "elf": str(artifact.elf_path),
            }
            for name, artifact in artifacts.items()
        },
        "runtime": runtime,
        "static": static,
        "comparison": {
            "host_median_exodus_over_clang": safe_ratio(exodus_host, clang_host),
            "guest_total_median_exodus_over_clang": safe_ratio(exodus_guest, clang_guest),
            "instruction_count_exodus_over_clang": safe_ratio(exodus_insns, clang_insns),
        },
    }


def geometric_mean(values: list[float]) -> Optional[float]:
    filtered = [value for value in values if value > 0 and math.isfinite(value)]
    if not filtered:
        return None
    return math.exp(sum(math.log(value) for value in filtered) / len(filtered))


def aggregate_results(results: dict[str, dict[str, Any]], min_runtime_ms: float) -> dict[str, Any]:
    passed = {
        name: result
        for name, result in results.items()
        if result.get("status") in {"PASSED", "MEASURE_PARTIAL"}
    }
    host_ratios = []
    guest_ratios = []
    stable_host_ratios = []
    for result in passed.values():
        comparison = result.get("comparison", {})
        host_ratio = comparison.get("host_median_exodus_over_clang")
        guest_ratio = comparison.get("guest_total_median_exodus_over_clang")
        if host_ratio:
            host_ratios.append(host_ratio)
            exodus_host = result["runtime"]["exodus"]["host_seconds"]["median"]
            clang_host = result["runtime"]["clang"]["host_seconds"]["median"]
            if min(exodus_host or 0, clang_host or 0) * 1000.0 >= min_runtime_ms:
                stable_host_ratios.append(host_ratio)
        if guest_ratio:
            guest_ratios.append(guest_ratio)

    return {
        "total_tests": len(results),
        "passed_or_partial": len(passed),
        "failed": len(results) - len(passed),
        "min_runtime_ms_for_stable_subset": min_runtime_ms,
        "host_median_ratio_geomean_exodus_over_clang": geometric_mean(host_ratios),
        "stable_host_median_ratio_geomean_exodus_over_clang": geometric_mean(
            stable_host_ratios
        ),
        "guest_total_ratio_geomean_exodus_over_clang": geometric_mean(guest_ratios),
    }


def first_line(cmd: list[str]) -> Optional[str]:
    try:
        res = subprocess.run(
            cmd,
            cwd=REPO_ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=5,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    text = decode(res.stdout).strip().splitlines()
    return text[0] if text else None


def detect_sysroot(gcc: str) -> str:
    try:
        res = subprocess.run(
            [gcc, "-print-sysroot"],
            cwd=REPO_ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=5,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return ""
    if res.returncode != 0:
        return ""
    return decode(res.stdout).strip()


def git_value(args: list[str]) -> Optional[str]:
    try:
        res = subprocess.run(
            ["git", *args],
            cwd=REPO_ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            timeout=5,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    if res.returncode != 0:
        return None
    return decode(res.stdout).strip()


def sibling_tool(gcc_path: str, suffix: str) -> Optional[str]:
    basename = os.path.basename(gcc_path)
    candidate = basename.replace("gcc", suffix, 1) if "gcc" in basename else suffix
    if os.path.isabs(gcc_path):
        sibling = os.path.join(os.path.dirname(gcc_path), candidate)
        if os.path.exists(sibling):
            return sibling
    return shutil.which(candidate)


def write_csv_reports(report_dir: Path, results: dict[str, dict[str, Any]]) -> None:
    summary_fields = [
        "test",
        "status",
        "exodus_host_median_s",
        "clang_host_median_s",
        "host_ratio_exodus_over_clang",
        "exodus_guest_median_us",
        "clang_guest_median_us",
        "guest_ratio_exodus_over_clang",
        "exodus_instructions",
        "clang_instructions",
        "instruction_ratio_exodus_over_clang",
        "exodus_load_store",
        "clang_load_store",
        "exodus_stack_spills",
        "clang_stack_spills",
    ]
    with (report_dir / "comparison.csv").open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=summary_fields)
        writer.writeheader()
        for test_name, result in sorted(results.items()):
            row = {"test": test_name, "status": result.get("status")}
            if "runtime" in result:
                row.update(
                    {
                        "exodus_host_median_s": result["runtime"]["exodus"][
                            "host_seconds"
                        ]["median"],
                        "clang_host_median_s": result["runtime"]["clang"][
                            "host_seconds"
                        ]["median"],
                        "host_ratio_exodus_over_clang": result["comparison"].get(
                            "host_median_exodus_over_clang"
                        ),
                        "exodus_guest_median_us": result["runtime"]["exodus"][
                            "guest_total_us"
                        ]["median"],
                        "clang_guest_median_us": result["runtime"]["clang"][
                            "guest_total_us"
                        ]["median"],
                        "guest_ratio_exodus_over_clang": result["comparison"].get(
                            "guest_total_median_exodus_over_clang"
                        ),
                        "exodus_instructions": result["static"]["exodus"]
                        .get("code_density", {})
                        .get("total_instructions"),
                        "clang_instructions": result["static"]["clang"]
                        .get("code_density", {})
                        .get("total_instructions"),
                        "instruction_ratio_exodus_over_clang": result[
                            "comparison"
                        ].get("instruction_count_exodus_over_clang"),
                        "exodus_load_store": result["static"]["exodus"]
                        .get("instruction_mix", {})
                        .get("load_store"),
                        "clang_load_store": result["static"]["clang"]
                        .get("instruction_mix", {})
                        .get("load_store"),
                        "exodus_stack_spills": result["static"]["exodus"]
                        .get("register_pressure", {})
                        .get("stack_spills"),
                        "clang_stack_spills": result["static"]["clang"]
                        .get("register_pressure", {})
                        .get("stack_spills"),
                    }
                )
            writer.writerow(row)

    with (report_dir / "raw_samples.csv").open("w", newline="", encoding="utf-8") as f:
        fields = ["test", "compiler", "run_index", "host_seconds", "guest_total_us"]
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        for result in results.values():
            if "runtime" not in result:
                continue
            for compiler in ("exodus", "clang"):
                for sample in result["runtime"][compiler]["samples"]:
                    writer.writerow({field: sample.get(field) for field in fields})


def write_summary_md(
    report_dir: Path,
    results: dict[str, dict[str, Any]],
    aggregate: dict[str, Any],
) -> None:
    rows = []
    for test_name, result in sorted(results.items()):
        if "comparison" not in result:
            continue
        ratio = result["comparison"].get("host_median_exodus_over_clang")
        exodus = result["runtime"]["exodus"]["host_seconds"]["median"]
        clang = result["runtime"]["clang"]["host_seconds"]["median"]
        rows.append((test_name, ratio, exodus, clang, result["status"]))

    rows_by_gap = sorted(
        [row for row in rows if row[1] is not None],
        key=lambda row: row[1],
        reverse=True,
    )

    lines = [
        "# Exodus vs clang -O3 Performance Comparison",
        "",
        f"- Tests: {aggregate['passed_or_partial']}/{aggregate['total_tests']} comparable",
        "- Ratio convention: Exodus median host time / clang median host time; higher means Exodus is slower.",
        f"- Host geomean ratio: {format_ratio(aggregate.get('host_median_ratio_geomean_exodus_over_clang'))}",
        f"- Stable-subset host geomean ratio: {format_ratio(aggregate.get('stable_host_median_ratio_geomean_exodus_over_clang'))}",
        f"- Guest TOTAL geomean ratio: {format_ratio(aggregate.get('guest_total_ratio_geomean_exodus_over_clang'))}",
        "",
        "## Largest Exodus-over-clang host-time ratios",
        "",
        "| Test | Status | Exodus median s | clang median s | Ratio |",
        "| --- | --- | ---: | ---: | ---: |",
    ]
    for test_name, ratio, exodus, clang, status in rows_by_gap[:10]:
        lines.append(
            f"| {test_name} | {status} | {format_float(exodus)} | {format_float(clang)} | {format_ratio(ratio)} |"
        )

    lines.extend(
        [
            "",
            "## Smallest Exodus-over-clang host-time ratios",
            "",
            "| Test | Status | Exodus median s | clang median s | Ratio |",
            "| --- | --- | ---: | ---: | ---: |",
        ]
    )
    for test_name, ratio, exodus, clang, status in list(reversed(rows_by_gap[-10:])):
        lines.append(
            f"| {test_name} | {status} | {format_float(exodus)} | {format_float(clang)} | {format_ratio(ratio)} |"
        )

    (report_dir / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def format_float(value: Optional[float]) -> str:
    if value is None:
        return "n/a"
    return f"{value:.6f}"


def format_ratio(value: Optional[float]) -> str:
    if value is None:
        return "n/a"
    return f"{value:.3f}x"


def save_disassemblies(
    report_dir: Path,
    results: dict[str, dict[str, Any]],
    args: argparse.Namespace,
) -> None:
    if args.disasm_top <= 0:
        return
    objdump = sibling_tool(args.gcc, "objdump")
    if not objdump:
        return

    candidates = []
    for test_name, result in results.items():
        ratio = result.get("comparison", {}).get("host_median_exodus_over_clang")
        if ratio and ratio > 0 and math.isfinite(ratio):
            candidates.append((abs(math.log(ratio)), test_name, result))
    candidates.sort(reverse=True)

    disasm_dir = report_dir / "disassembly"
    disasm_dir.mkdir(parents=True, exist_ok=True)
    for _, test_name, result in candidates[: args.disasm_top]:
        for compiler in ("exodus", "clang"):
            elf = result["artifacts"][compiler]["elf"]
            out_path = disasm_dir / f"{test_name}.{compiler}.dump"
            with out_path.open("w", encoding="utf-8") as out_file:
                subprocess.run(
                    [objdump, "-M", "no-aliases", "-d", "--section=.text", elf],
                    stdout=out_file,
                    stderr=subprocess.DEVNULL,
                    timeout=20,
                    check=False,
                )


def write_reports(
    report_dir: Path,
    results: dict[str, dict[str, Any]],
    aggregate: dict[str, Any],
    args: argparse.Namespace,
    elapsed: float,
) -> None:
    report_dir.mkdir(parents=True, exist_ok=True)
    metadata = {
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "repo_root": str(REPO_ROOT),
        "git_revision": git_value(["rev-parse", "HEAD"]),
        "git_dirty": bool(git_value(["status", "--porcelain"])),
        "host": {
            "platform": platform.platform(),
            "python": sys.version.split()[0],
        },
        "comparison": {
            "baseline": "Exodus",
            "candidate": "clang -O3",
            "target": args.target,
            "march": args.march,
            "mabi": args.mabi,
            "sysroot": args.sysroot,
            "clang_sysy_compat": args.clang_sysy_compat,
            "clang_sysy_compat_flags": args.clang_sysy_compat_flags
            if args.clang_sysy_compat
            else [],
            "clang_extra": shlex.split(args.clang_extra),
            "runs": args.runs,
            "warmup": args.warmup,
            "timeout_seconds": args.timeout,
            "measurement_order": "alternating; order flips each measured run",
            "ratio_convention": "exodus / clang",
        },
        "tools": {
            "exodus_compiler": str(args.compiler.resolve()),
            "clang": args.clang,
            "clang_version": first_line([args.clang, "--version"]),
            "gcc": args.gcc,
            "gcc_version": first_line([args.gcc, "--version"]),
            "qemu": args.qemu,
            "qemu_version": first_line([args.qemu, "--version"]),
            "libdir": str(args.libdir.resolve()),
        },
        "elapsed_seconds": elapsed,
    }
    with (report_dir / "comparison.json").open("w", encoding="utf-8") as f:
        json.dump(
            {
                "metadata": metadata,
                "aggregate": aggregate,
                "tests": results,
            },
            f,
            indent=2,
        )

    write_csv_reports(report_dir, results)
    write_summary_md(report_dir, results, aggregate)
    save_disassemblies(report_dir, results, args)
    shutil.copy2(Path(__file__).resolve(), report_dir / "harness_compare_clang_o3_perf.py")


def find_tests(args: argparse.Namespace) -> list[Path]:
    if args.case:
        tests = []
        for name in args.case:
            case_name = name if name.endswith(".sy") else f"{name}.sy"
            path = args.dir / case_name
            if not path.exists():
                raise SystemExit(f"Error: test case not found: {path}")
            tests.append(path)
        return tests
    return sorted(args.dir.glob("*.sy"))


def is_relative_to(path: Path, base: Path) -> bool:
    try:
        path.relative_to(base)
    except ValueError:
        return False
    return True


def validate_temp_root(args: argparse.Namespace) -> None:
    temp_root = args.temp_root
    allowed_build_root = REPO_ROOT / "build"
    allowed_tmp_root = Path("/tmp").resolve()
    if temp_root in {Path("/"), REPO_ROOT, args.dir, args.libdir, args.report_dir}:
        raise SystemExit(f"Error: unsafe --temp-root: {temp_root}")
    if is_relative_to(args.report_dir, temp_root) or is_relative_to(
        temp_root, args.report_dir
    ):
        raise SystemExit("Error: --temp-root and --report-dir must not overlap")
    if not (
        is_relative_to(temp_root, allowed_build_root)
        or is_relative_to(temp_root, allowed_tmp_root)
    ):
        raise SystemExit(
            "Error: --temp-root must be under the repo build/ directory or /tmp"
        )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Compare Exodus against clang -O3 on SysY performance tests."
    )
    parser.add_argument(
        "dir",
        nargs="?",
        type=Path,
        default=Path("tests/performance"),
        help="Directory containing .sy tests (default: tests/performance)",
    )
    parser.add_argument(
        "--case",
        action="append",
        help="Run only a named case, without or with .sy; may be repeated.",
    )
    parser.add_argument(
        "-c",
        "--compiler",
        type=Path,
        default=Path("build/compiler"),
        help="Path to Exodus compiler (default: build/compiler)",
    )
    parser.add_argument(
        "--exodus-opt",
        default="-O2",
        help="Options passed to Exodus compiler (default: -O2)",
    )
    parser.add_argument("--clang", default="clang", help="clang executable (default: clang)")
    parser.add_argument(
        "--clang-opt",
        default="-O3",
        help="Optimization/options passed to clang before source args (default: -O3)",
    )
    parser.add_argument(
        "--clang-extra",
        default="",
        help="Extra clang arguments appended after the source/output args.",
    )
    parser.add_argument(
        "--no-clang-sysy-compat",
        action="store_false",
        dest="clang_sysy_compat",
        help=(
            "Disable default SysY compatibility flags. By default the harness "
            "adds -Wno-incompatible-pointer-types, -fwrapv, and -ffp-contract=off "
            "so every performance test can compile and validate under clang."
        ),
    )
    parser.add_argument(
        "--clang-sysy-compat-flags",
        default=" ".join(DEFAULT_CLANG_SYSY_COMPAT_FLAGS),
        help=(
            "Flags used when SysY compatibility is enabled "
            f"(default: {' '.join(DEFAULT_CLANG_SYSY_COMPAT_FLAGS)})"
        ),
    )
    parser.add_argument(
        "--target",
        default="riscv64-linux-gnu",
        help="clang target triple (default: riscv64-linux-gnu)",
    )
    parser.add_argument("--march", default="rv64imafdc", help="RISC-V march")
    parser.add_argument("--mabi", default="lp64d", help="RISC-V ABI")
    parser.add_argument(
        "--sysroot",
        default="auto",
        help="clang sysroot; use 'auto' to read it from GCC (default: auto)",
    )
    parser.add_argument(
        "-g",
        "--gcc",
        default="riscv64-linux-gnu-gcc",
        help="RISC-V GCC/linker driver (default: riscv64-linux-gnu-gcc)",
    )
    parser.add_argument(
        "-q",
        "--qemu",
        default="qemu-riscv64",
        help="QEMU user runner (default: qemu-riscv64)",
    )
    parser.add_argument(
        "-l",
        "--libdir",
        type=Path,
        default=Path("lib"),
        help="Directory containing libsysy.a and sylib.h (default: lib)",
    )
    parser.add_argument(
        "--runs",
        type=int,
        default=5,
        help="Measured runs per compiler per case (default: 5)",
    )
    parser.add_argument(
        "--warmup",
        type=int,
        default=1,
        help="Warm-up runs per compiler before measurement (default: 1)",
    )
    parser.add_argument(
        "-t",
        "--timeout",
        type=float,
        default=10.0,
        help="QEMU timeout per run in seconds (default: 10.0)",
    )
    parser.add_argument(
        "--compile-timeout",
        type=float,
        default=60.0,
        help="Compile/link timeout per command in seconds (default: 60.0)",
    )
    parser.add_argument(
        "--report-dir",
        type=Path,
        default=None,
        help="Report directory (default: docs/performance/<date>/clang-o3-comparison)",
    )
    parser.add_argument(
        "--temp-root",
        type=Path,
        default=Path("build/tmp_compare_clang_o3"),
        help="Temporary build root (default: build/tmp_compare_clang_o3)",
    )
    parser.add_argument(
        "--min-runtime-ms",
        type=float,
        default=1.0,
        help="Minimum median runtime for stable-subset geomean (default: 1.0ms)",
    )
    parser.add_argument(
        "--disasm-top",
        type=int,
        default=5,
        help="Save disassembly for top N absolute host-ratio gaps (default: 5)",
    )
    parser.add_argument(
        "--keep-temps",
        action="store_true",
        help="Keep build/tmp_compare_clang_o3 after reports are written.",
    )
    return parser.parse_args()


def validate_args(args: argparse.Namespace) -> None:
    args.dir = args.dir.resolve()
    args.compiler = args.compiler.resolve()
    args.libdir = args.libdir.resolve()
    args.temp_root = args.temp_root.resolve()
    if args.report_dir is None:
        args.report_dir = (
            REPO_ROOT
            / "docs"
            / "performance"
            / time.strftime("%Y-%m-%d")
            / "clang-o3-comparison"
        )
    else:
        args.report_dir = args.report_dir.resolve()

    if args.sysroot == "auto":
        args.sysroot = detect_sysroot(args.gcc)
    validate_temp_root(args)
    args.clang_sysy_compat_flags = shlex.split(args.clang_sysy_compat_flags)

    if args.runs < 1:
        raise SystemExit("Error: --runs must be positive")
    if args.warmup < 0:
        raise SystemExit("Error: --warmup cannot be negative")
    if args.timeout <= 0:
        raise SystemExit("Error: --timeout must be positive")
    if args.compile_timeout <= 0:
        raise SystemExit("Error: --compile-timeout must be positive")

    checks = [
        (args.dir.exists(), f"Validation directory not found: {args.dir}"),
        (args.compiler.exists(), f"Exodus compiler not found: {args.compiler}"),
        (shutil.which(args.clang) is not None, f"clang not found: {args.clang}"),
        (shutil.which(args.gcc) is not None, f"RISC-V GCC not found: {args.gcc}"),
        (shutil.which(args.qemu) is not None, f"QEMU runner not found: {args.qemu}"),
        ((args.libdir / "libsysy.a").exists(), f"libsysy.a not found in {args.libdir}"),
        ((args.libdir / "sylib.h").exists(), f"sylib.h not found in {args.libdir}"),
    ]
    for ok, message in checks:
        if not ok:
            raise SystemExit(f"Error: {message}")


def main() -> int:
    args = parse_args()
    validate_args(args)
    tests = find_tests(args)
    if not tests:
        print(f"No .sy files found under {args.dir}")
        return 1

    print(
        f"Comparing {len(tests)} test(s): Exodus {args.exodus_opt} vs clang {args.clang_opt}"
    )
    print(f"Target: {args.target} {args.march} {args.mabi} | sysroot: {args.sysroot or 'none'}")
    print(f"Runs: {args.runs} measured + {args.warmup} warmup per compiler")
    print(f"Report dir: {args.report_dir}")

    args.temp_root.mkdir(parents=True, exist_ok=True)
    results: dict[str, dict[str, Any]] = {}
    start = time.time()

    for index, test_file in enumerate(tests, 1):
        test_name = test_file.stem
        print(f"[{index}/{len(tests)}] {test_name}")
        try:
            result = compare_case(test_file, args)
        except CaseError as exc:
            result = {
                "status": exc.status,
                "message": exc.message,
            }
            print(f"  {exc.status}")
        else:
            ratio = result["comparison"].get("host_median_exodus_over_clang")
            print(f"  {result['status']} host_ratio={format_ratio(ratio)}")
        results[test_name] = result

    elapsed = time.time() - start
    aggregate = aggregate_results(results, args.min_runtime_ms)
    write_reports(args.report_dir, results, aggregate, args, elapsed)

    if not args.keep_temps:
        shutil.rmtree(args.temp_root, ignore_errors=True)

    print(f"Reports written under: {args.report_dir}")
    print(
        "Host geomean ratio Exodus/clang: "
        f"{format_ratio(aggregate.get('host_median_ratio_geomean_exodus_over_clang'))}"
    )
    return 0 if aggregate["failed"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
