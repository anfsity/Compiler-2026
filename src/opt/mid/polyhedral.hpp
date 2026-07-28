#pragma once

#include "../../mid/memory.hpp"
#include "scalar_evolution.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace exodus::mid_ir {

enum class PolyhedralAccessKind : uint8_t { Read, Write };

struct PolyhedralAccess {
  Op *operation = nullptr;
  Value *root = nullptr;
  PolyhedralAccessKind kind = PolyhedralAccessKind::Read;
  std::vector<SCEVAffineExpr> subscripts;
  SCEVAffineExpr byte_offset;
  std::string relation;
  int64_t outer_stride = 0;
  int64_t inner_stride = 0;
};

struct PolyhedralScop {
  const Loop *outer = nullptr;
  const Loop *inner = nullptr;
  CountedLoopInfo outer_counted;
  CountedLoopInfo inner_counted;
  std::vector<SCEVAffineExpr> lower_bounds;
  std::vector<SCEVAffineExpr> upper_bounds;
  std::vector<Value *> parameters;
  std::vector<PolyhedralAccess> accesses;
  std::string domain;
  uint64_t original_stride_cost = 0;
  uint64_t interchanged_stride_cost = 0;
  bool interchange_legal = false;
};

class PolyhedralInfo {
public:
  auto compute(
    LinearFunction &func,
    LoopInfo &loops,
    AffineLoopInfo &affine,
    ScalarEvolution &scev,
    MidModule *module = nullptr
  ) -> void;

  auto get_scops() const -> const std::vector<PolyhedralScop> & {
    return scops;
  }

private:
  std::vector<PolyhedralScop> scops;
  std::unordered_map<Op *, Block *> op_blocks;
  BasicAliasAnalysis alias;
  LinearFunction *function = nullptr;
  MidModule *module = nullptr;

  auto build_scop(
    const Loop &outer,
    const Loop &inner,
    AffineLoopInfo &affine,
    ScalarEvolution &scev
  ) const -> std::optional<PolyhedralScop>;
  auto collect_accesses(PolyhedralScop &scop, ScalarEvolution &scev) const
    -> bool;
  auto build_polyhedral_model(PolyhedralScop &scop) const -> bool;
  auto has_legal_interchange(const PolyhedralScop &scop) const -> bool;
  auto resolve_invariant_pointer_slot(Value *slot) const -> Value *;
  auto roots_are_proven_distinct(Value *lhs, Value *rhs) const -> bool;
};

struct PolyhedralAnalysis {
  using Result = PolyhedralInfo;

  MidModule *module = nullptr;

  auto run(LinearFunction &func, exodus::opt::LinearFunctionAnalysisManager &am)
    -> Result {
    auto &loops = am.get_result<LoopAnalysis>(func);
    auto &affine = am.get_result<AffineLoopAnalysis>(func);
    auto &scev = am.get_result<ScalarEvolutionAnalysis>(func);
    PolyhedralInfo result;
    result.compute(func, loops, affine, scev, module);
    return result;
  }
};

} // namespace exodus::mid_ir
