#pragma once

#include <cstdio>
#include <string>
#include <vector>

namespace exodus::low_ir {
struct MachineFunction;
}

namespace exodus::riscv {

struct LiveInterval;

struct RegAllocPrinter {
  explicit RegAllocPrinter(bool enabled, FILE *out = stderr)
      : enabled_(enabled), out_(out) {}

  auto to_string(const low_ir::MachineFunction &function) const -> std::string;
  auto dump_function(const low_ir::MachineFunction &function) const -> void;

  auto dump_intervals(
    const low_ir::MachineFunction &function,
    std::string_view stage,
    const std::vector<LiveInterval> &intervals
  ) const -> void;

private:
  bool enabled_;
  FILE *out_;

  auto intervals_to_string(
    const low_ir::MachineFunction &function,
    std::string_view stage,
    const std::vector<LiveInterval> &intervals
  ) const -> std::string;
};

} // namespace exodus::riscv
