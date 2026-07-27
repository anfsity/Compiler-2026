#include "inst_combine.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>
#include <variant>

namespace exodus::mid_ir::opt {
namespace {

auto int_constant(Value *value) -> std::optional<int> {
  if (!value || value->kind != ValueKind::Constant)
    return std::nullopt;

  auto *constant = static_cast<Constant *>(value);
  if (!std::holds_alternative<int>(constant->val))
    return std::nullopt;
  return std::get<int>(constant->val);
}

auto make_i32(MidModule *module, int value) -> Value * {
  return module->ctx->make_const(I32::get(), value);
}

auto make_binary(
  MidModule *module,
  OpCode code,
  Value *lhs,
  Value *rhs,
  const std::shared_ptr<Type> &type = I32::get()
) -> Op * {
  auto *op = module->make_op(code);
  op->operands = {lhs, rhs};
  lhs->addUse(op);
  rhs->addUse(op);
  op->result = module->ctx->make_value<OpResult>(type, op);
  return op;
}

auto is_zero(Value *value) -> bool {
  auto constant = int_constant(value);
  return constant && *constant == 0;
}

auto is_one(Value *value) -> bool {
  auto constant = int_constant(value);
  return constant && *constant == 1;
}

auto is_power_of_two(int value) -> bool {
  return value > 0 && (value & (value - 1)) == 0;
}

struct Range {
  int64_t min;
  int64_t max;
};

auto is_proven_non_negative(Value *value) -> bool {
  if (auto constant = int_constant(value))
    return *constant >= 0;

  if (!value || value->kind != ValueKind::OpResult)
    return false;

  auto *creator = static_cast<Op *>(static_cast<OpResult *>(value)->creator);
  if (!creator)
    return false;

  switch (creator->code) {
  case OpCode::Eq:
  case OpCode::Ne:
  case OpCode::Lt:
  case OpCode::Gt:
  case OpCode::Le:
  case OpCode::Ge:
  case OpCode::ZExt:
    return true;
  case OpCode::And:
    if (creator->operands.size() == 2) {
      auto lhs = int_constant(creator->operands[0]);
      auto rhs = int_constant(creator->operands[1]);
      return (lhs && *lhs >= 0) || (rhs && *rhs >= 0);
    }
    return false;
  case OpCode::Mod: {
    if (creator->operands.size() != 2)
      return false;
    auto divisor = int_constant(creator->operands[1]);
    return divisor && *divisor != 0 &&
           is_proven_non_negative(creator->operands[0]);
  }
  default:
    return false;
  }
}

auto range_of(Value *value) -> std::optional<Range> {
  if (!value)
    return std::nullopt;

  if (auto constant = int_constant(value))
    return Range{*constant, *constant};

  if (value->kind != ValueKind::OpResult)
    return std::nullopt;

  auto *creator = static_cast<Op *>(static_cast<OpResult *>(value)->creator);
  if (!creator)
    return std::nullopt;

  switch (creator->code) {
  case OpCode::Eq:
  case OpCode::Ne:
  case OpCode::Lt:
  case OpCode::Gt:
  case OpCode::Le:
  case OpCode::Ge:
  case OpCode::ZExt:
    return Range{0, 1};
  case OpCode::And: {
    if (creator->operands.size() != 2)
      return std::nullopt;
    auto lhs = int_constant(creator->operands[0]);
    auto rhs = int_constant(creator->operands[1]);
    if (lhs && *lhs >= 0)
      return Range{0, *lhs};
    if (rhs && *rhs >= 0)
      return Range{0, *rhs};
    return std::nullopt;
  }
  case OpCode::Mod: {
    if (creator->operands.size() != 2)
      return std::nullopt;
    auto divisor = int_constant(creator->operands[1]);
    if (!divisor || *divisor == 0)
      return std::nullopt;

    auto magnitude = std::llabs(static_cast<int64_t>(*divisor));
    if (is_proven_non_negative(creator->operands[0]))
      return Range{0, magnitude - 1};
    return Range{-(magnitude - 1), magnitude - 1};
  }
  default:
    return std::nullopt;
  }
}

auto abs_less_than(const Range &range, int64_t bound) -> bool {
  return range.min > -bound && range.max < bound;
}

auto is_binary(OpCode code) -> CombineMatcher {
  return [code](const CombineContext &ctx) {
    return ctx.op && ctx.op->code == code && ctx.op->operands.size() == 2;
  };
}

auto is_zero_compare(Value *value, OpCode code, Value *&compared) -> bool {
  if (!value || value->kind != ValueKind::OpResult)
    return false;

  auto *creator = static_cast<Op *>(static_cast<OpResult *>(value)->creator);
  if (!creator || creator->code != code || creator->operands.size() != 2)
    return false;

  if (is_zero(creator->operands[0])) {
    compared = creator->operands[1];
    return true;
  }
  if (is_zero(creator->operands[1])) {
    compared = creator->operands[0];
    return true;
  }
  return false;
}

} // namespace

auto CombineRuleSet::add(
  std::string name, CombineMatcher match, CombineRewrite rewrite
) -> CombineRuleSet & {
  rules.push_back(
    CombineRule{std::move(name), std::move(match), std::move(rewrite)}
  );
  return *this;
}

auto CombineRuleSet::apply(const CombineContext &ctx) const
  -> std::optional<CombineResult> {
  for (const auto &rule : rules) {
    if (!rule.match(ctx))
      continue;
    if (auto result = rule.rewrite(ctx); result && result->changed())
      return result;
  }
  return std::nullopt;
}

InstCombine::InstCombine(MidModule *m) : module(m) { register_rules(); }

auto InstCombine::register_rules() -> void {
  // The rule table is intentionally ordered from specific folds to general
  // algebraic identities. New rules can be added without changing the driver.
  rules
    .add(
      "cancel-boolean-zero-compare",
      [](const CombineContext &ctx) {
        if (
          !ctx.op ||
          (ctx.op->code != OpCode::Eq && ctx.op->code != OpCode::Ne) ||
          ctx.op->operands.size() != 2 || !is_zero(ctx.op->operands[1])
        )
          return false;
        Value *compared = nullptr;
        return is_zero_compare(ctx.op->operands[0], OpCode::Eq, compared) ||
               is_zero_compare(ctx.op->operands[0], OpCode::Ne, compared);
      },
      [](const CombineContext &ctx) -> std::optional<CombineResult> {
        Value *compared = nullptr;
        OpCode inner_code = OpCode::Eq;
        if (is_zero_compare(ctx.op->operands[0], OpCode::Eq, compared)) {
          inner_code = OpCode::Eq;
        } else if (is_zero_compare(ctx.op->operands[0], OpCode::Ne, compared)) {
          inner_code = OpCode::Ne;
        } else {
          return std::nullopt;
        }

        auto replacement_code =
          ctx.op->code == OpCode::Eq
            ? (inner_code == OpCode::Eq ? OpCode::Ne : OpCode::Eq)
            : inner_code;

        auto *inner = static_cast<Op *>(
          static_cast<OpResult *>(ctx.op->operands[0])->creator
        );
        auto *inner_zero = inner->operands[0] == compared ? inner->operands[1]
                                                          : inner->operands[0];
        auto *replacement = make_binary(
          ctx.module,
          replacement_code,
          compared,
          inner_zero,
          ctx.op->result->type
        );
        return CombineResult{replacement->result, {replacement}};
      }
    )
    .add(
      "remove-bool-zext-before-branch",
      [](const CombineContext &ctx) {
        if (
          !ctx.op ||
          (ctx.op->code != OpCode::Eq && ctx.op->code != OpCode::Ne) ||
          ctx.op->operands.size() != 2 || !is_zero(ctx.op->operands[1])
        )
          return false;
        auto *zext = ctx.op->operands[0];
        if (zext->kind != ValueKind::OpResult)
          return false;
        auto *zext_op =
          static_cast<Op *>(static_cast<OpResult *>(zext)->creator);
        return zext_op && zext_op->code == OpCode::ZExt &&
               zext_op->operands.size() == 1 &&
               zext_op->operands[0]->type->is_bool();
      },
      [](const CombineContext &ctx) -> std::optional<CombineResult> {
        auto *zext_op = static_cast<Op *>(
          static_cast<OpResult *>(ctx.op->operands[0])->creator
        );
        auto *source = zext_op->operands[0];
        if (ctx.op->code == OpCode::Ne)
          return CombineResult{source, {}};

        auto *zero = ctx.module->ctx->make_const(Bool::get(), 0);
        auto *replacement = make_binary(
          ctx.module, OpCode::Eq, source, zero, ctx.op->result->type
        );
        return CombineResult{replacement->result, {replacement}};
      }
    )
    .add(
      "fold-constants",
      [](const CombineContext &ctx) {
        return ctx.op && ctx.op->operands.size() == 2;
      },
      [](const CombineContext &ctx) { return fold_constants(ctx); }
    )
    .add(
      "mod-simplify",
      is_binary(OpCode::Mod),
      [](const CombineContext &ctx) { return simplify_mod(ctx); }
    )
    .add(
      "arithmetic-identities",
      [](const CombineContext &ctx) {
        return ctx.op && ctx.op->operands.size() == 2;
      },
      [](const CombineContext &ctx) { return simplify_arithmetic(ctx); }
    );
}

auto InstCombine::run(
  LinearFunction &func, exodus::opt::LinearFunctionAnalysisManager &am
) -> exodus::opt::PreservedAnalysis {
  collect_affine_mod_proofs(func, am);
  bool ever_changed = false;

  // Keep this fixed point local to the pass. This also makes an explicit
  // -Oinstcombine invocation behave like the default pipeline.
  for (;;) {
    rewriter.set_scope(func);
    bool changed = false;

    for (auto &block : func.blocks)
      changed |= combine_block(func, *block);

    rewriter.finalize(func);
    if (!changed)
      break;

    ever_changed = true;
  }

  affine_non_negative_mods.clear();
  return ever_changed ? exodus::opt::PreservedAnalysis::none()
                      : exodus::opt::PreservedAnalysis::all();
}

auto InstCombine::collect_affine_mod_proofs(
  LinearFunction &func, exodus::opt::LinearFunctionAnalysisManager &am
) -> void {
  affine_non_negative_mods.clear();
  auto &loops = am.get_result<LoopAnalysis>(func);
  auto &affine = am.get_result<AffineLoopAnalysis>(func);

  for (auto &block : func.blocks) {
    auto *innermost = loops.get_loop_for(block.get());
    if (!innermost)
      continue;
    for (auto *op : block->insts) {
      if (op->code != OpCode::Mod || op->operands.size() != 2)
        continue;
      auto divisor = int_constant(op->operands[1]);
      if (!divisor || !is_power_of_two(*divisor))
        continue;

      for (auto *loop = innermost; loop; loop = loop->get_parent()) {
        auto counted = affine.match_counted_loop(*loop);
        if (
          counted && affine.is_non_negative(op->operands[0], *counted, *loop)
        ) {
          affine_non_negative_mods.insert(op);
          break;
        }
      }
    }
  }
}

auto InstCombine::combine_block(LinearFunction &func, Block &block) -> bool {
  bool changed = false;
  for (auto it = block.insts.begin(); it != block.insts.end(); ++it) {
    CombineContext ctx{
      module,
      &func,
      &block,
      *it,
      affine_non_negative_mods.count(*it) != 0,
    };
    auto result = rules.apply(ctx);
    if (!result)
      continue;

    apply(block, it, *result);
    changed = true;
  }
  return changed;
}

auto InstCombine::apply(
  Block &block, std::list<Op *>::iterator it, const CombineResult &result
) -> void {
  auto *old_op = *it;

  for (auto *new_op : result.prefix)
    block.insts.insert(it, new_op);

  if (result.replacement && old_op->result)
    rewriter.replace_all_uses_with(old_op->result, result.replacement);

  rewriter.eraseOp(old_op);
}

auto InstCombine::fold_constants(const CombineContext &ctx)
  -> std::optional<CombineResult> {
  auto *op = ctx.op;
  auto lhs = int_constant(op->operands[0]);
  auto rhs = int_constant(op->operands[1]);
  if (!lhs || !rhs)
    return std::nullopt;

  int64_t result = 0;
  switch (op->code) {
  case OpCode::Add:
    result = static_cast<int64_t>(*lhs) + *rhs;
    break;
  case OpCode::Sub:
    result = static_cast<int64_t>(*lhs) - *rhs;
    break;
  case OpCode::Mul:
    result = static_cast<int64_t>(*lhs) * *rhs;
    break;
  case OpCode::Div:
    if (*rhs == 0 || (*lhs == std::numeric_limits<int>::min() && *rhs == -1))
      return std::nullopt;
    result = *lhs / *rhs;
    break;
  case OpCode::Mod:
    if (*rhs == 0 || (*lhs == std::numeric_limits<int>::min() && *rhs == -1))
      return std::nullopt;
    result = *lhs % *rhs;
    break;
  default:
    return std::nullopt;
  }

  if (
    result < std::numeric_limits<int>::min() ||
    result > std::numeric_limits<int>::max()
  )
    return std::nullopt;

  return CombineResult{make_i32(ctx.module, static_cast<int>(result)), {}};
}

auto InstCombine::simplify_mod(const CombineContext &ctx)
  -> std::optional<CombineResult> {
  auto *op = ctx.op;
  auto *lhs = op->operands[0];
  auto rhs = int_constant(op->operands[1]);
  if (!rhs || *rhs == 0)
    return std::nullopt;

  if (is_zero(lhs))
    return CombineResult{make_i32(ctx.module, 0), {}};

  if (*rhs == 1 || *rhs == -1)
    return CombineResult{make_i32(ctx.module, 0), {}};

  if (auto range = range_of(lhs)) {
    auto divisor = std::llabs(static_cast<int64_t>(*rhs));
    if (abs_less_than(*range, divisor))
      return CombineResult{lhs, {}};
  }

  // Boundedness and signedness are independent facts.  A non-negative value
  // does not need a finite range here: x % 2^k is exactly x & (2^k - 1).
  if (
    *rhs > 0 && is_power_of_two(*rhs) &&
    (is_proven_non_negative(lhs) || ctx.affine_non_negative)
  ) {
    auto *mask = make_i32(ctx.module, *rhs - 1);
    auto *and_op = make_binary(ctx.module, OpCode::And, lhs, mask);
    return CombineResult{and_op->result, {and_op}};
  }

  return std::nullopt;
}

auto InstCombine::simplify_arithmetic(const CombineContext &ctx)
  -> std::optional<CombineResult> {
  auto *op = ctx.op;
  auto *lhs = op->operands[0];
  auto *rhs = op->operands[1];

  switch (op->code) {
  case OpCode::Add:
    if (is_zero(rhs))
      return CombineResult{lhs, {}};
    if (is_zero(lhs))
      return CombineResult{rhs, {}};
    break;
  case OpCode::Sub:
    if (is_zero(rhs))
      return CombineResult{lhs, {}};
    break;
  case OpCode::Mul:
    if (is_zero(lhs) || is_zero(rhs))
      return CombineResult{make_i32(ctx.module, 0), {}};
    if (is_one(lhs))
      return CombineResult{rhs, {}};
    if (is_one(rhs))
      return CombineResult{lhs, {}};
    break;
  case OpCode::Div:
    if (is_one(rhs))
      return CombineResult{lhs, {}};
    break;
  default:
    break;
  }

  return std::nullopt;
}

} // namespace exodus::mid_ir::opt
