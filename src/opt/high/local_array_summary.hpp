#pragma once

#include "../../high/ir.hpp"
#include "../AnalysisManager.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace exodus::high_ir::opt {

struct LocalArrayValueSegment {
  size_t begin = 0;
  size_t end = 0;
  int32_t value = 0;
};

struct LocalArraySummary {
  Value *root = nullptr;
  Op *initialization_boundary = nullptr;
  std::vector<int32_t> values;

  auto segments() const -> std::vector<LocalArrayValueSegment>;
};

struct LocalArrayIndexTerm {
  Value *value = nullptr;
  int64_t coefficient = 0;
};

struct LocalArrayLoadAccess {
  const LocalArraySummary *summary = nullptr;
  int64_t constant_offset = 0;
  std::vector<LocalArrayIndexTerm> terms;
};

class LocalArraySummaryInfo {
public:
  auto compute(Function &function) -> void;

  auto find(Value *root_or_address) const -> const LocalArraySummary *;
  auto constant_at(Value *root, int64_t element_index) const
    -> std::optional<int32_t>;
  auto analyze_load(const Op &load) const
    -> std::optional<LocalArrayLoadAccess>;
  auto constant_for_load(const Op &load) const -> std::optional<int32_t>;

  auto summaries() const
    -> const std::unordered_map<Value *, LocalArraySummary> & {
    return result;
  }

private:
  std::unordered_map<Value *, LocalArraySummary> result;
};

struct LocalArraySummaryAnalysis {
  using Result = LocalArraySummaryInfo;

  auto run(Function &function, exodus::opt::FunctionAnalysisManager &) const
    -> Result {
    LocalArraySummaryInfo info;
    info.compute(function);
    return info;
  }
};

} // namespace exodus::high_ir::opt
