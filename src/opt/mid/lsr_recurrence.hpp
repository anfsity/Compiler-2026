#pragma once

#include "../../mid/rewriter.hpp"
#include "lsr_analysis.hpp"
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace exodus::mid_ir::opt {

struct GetPtrReductionPlan {
  Value *initial_expression = nullptr;
  size_t varying_index = 0;
  int pointer_step = 0;
  std::optional<int> seed_pointer_offset;
};

struct FormedGetPtrRecurrence {
  std::vector<Value *> operands;
  std::shared_ptr<Type> result_type;
  std::shared_ptr<Type> layout_type;
  Value *value = nullptr;
};

class PointerRecurrenceBuilder {
public:
  explicit PointerRecurrenceBuilder(LSRContext &context) : context(context) {}

  auto materialize(
    Op *getptr,
    const Loop &loop,
    const CountedLoopInfo &counted,
    const GetPtrReductionPlan &plan,
    MidIRRewriter &rewriter,
    std::unordered_map<Value *, Value *> &cache,
    Value **replacement
  ) -> bool;

  static auto make_getptr(
    LSRContext &context,
    std::vector<Value *> operands,
    Op::Payload payload,
    const std::shared_ptr<Type> &result_type
  ) -> Op *;

private:
  LSRContext &context;

  auto clone_initial_expression(
    Value *value,
    Value *induction,
    Value *initial,
    const Loop &loop,
    Block *preheader,
    std::unordered_map<Value *, Value *> &cache
  ) -> Value *;
};

class GetPtrRebaser {
public:
  explicit GetPtrRebaser(
    LSRContext &context, const AffineExpressionMatcher &expressions
  )
      : context(context), expressions(expressions) {}

  auto rebase(
    const Loop &loop,
    const std::vector<Op *> &candidates,
    const std::vector<FormedGetPtrRecurrence> &formed,
    const std::unordered_set<Op *> &reduced,
    MidIRRewriter &rewriter
  ) -> bool;

private:
  LSRContext &context;
  const AffineExpressionMatcher &expressions;

  auto find_best_anchor(
    Op *getptr,
    const Loop &loop,
    const std::vector<FormedGetPtrRecurrence> &formed
  ) const -> std::optional<std::pair<Value *, int64_t>>;
};

class GetPtrRecurrencePlanner {
public:
  GetPtrRecurrencePlanner(LSRContext &context, LinearFunction &function)
      : context(context), function(function), expressions(context),
        immutable_slots(context), builder(context),
        rebaser(context, expressions) {}

  auto reduce(const Loop &loop) -> bool;

private:
  LSRContext &context;
  LinearFunction &function;
  AffineExpressionMatcher expressions;
  ImmutablePointerSlotAnalysis immutable_slots;
  PointerRecurrenceBuilder builder;
  GetPtrRebaser rebaser;

  auto
  analyze(Op *getptr, const Loop &loop, const CountedLoopInfo &counted) const
    -> std::optional<GetPtrReductionPlan>;
};

} // namespace exodus::mid_ir::opt
