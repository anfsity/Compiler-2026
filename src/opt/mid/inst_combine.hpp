#pragma once

#include "../../mid/ir.hpp"
#include "../../mid/rewriter.hpp"
#include "../AnalysisManager.hpp"
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace exodus::mid_ir::opt {

struct CombineContext {
  MidModule *module = nullptr;
  LinearFunction *function = nullptr;
  Block *block = nullptr;
  Op *op = nullptr;
};

struct CombineResult {
  // The old operation is replaced by this value.  prefix contains operations
  // inserted immediately before the old operation; this is enough for rules
  // such as x % 8 -> x & 7 without exposing list manipulation to each rule.
  Value *replacement = nullptr;
  std::vector<Op *> prefix;

  auto changed() const -> bool {
    return replacement != nullptr || !prefix.empty();
  }
};

using CombineMatcher = std::function<bool(const CombineContext &)>;
using CombineRewrite =
  std::function<std::optional<CombineResult>(const CombineContext &)>;

struct CombineRule {
  std::string name;
  CombineMatcher match;
  CombineRewrite rewrite;
};

class CombineRuleSet {
  std::vector<CombineRule> rules;

public:
  auto add(std::string name, CombineMatcher match, CombineRewrite rewrite)
    -> CombineRuleSet &;

  auto apply(const CombineContext &ctx) const -> std::optional<CombineResult>;
};

class InstCombine {
  MidModule *module;
  MidIRRewriter rewriter;
  CombineRuleSet rules;

public:
  explicit InstCombine(MidModule *m);

  auto run(LinearFunction &func, exodus::opt::LinearFunctionAnalysisManager &am)
    -> exodus::opt::PreservedAnalysis;

private:
  auto combine_block(LinearFunction &func, Block &block) -> bool;
  auto
  apply(Block &block, std::list<Op *>::iterator it, const CombineResult &result)
    -> void;

  auto register_rules() -> void;

  static auto fold_constants(const CombineContext &ctx)
    -> std::optional<CombineResult>;
  static auto simplify_mod(const CombineContext &ctx)
    -> std::optional<CombineResult>;
  static auto simplify_arithmetic(const CombineContext &ctx)
    -> std::optional<CombineResult>;
};

} // namespace exodus::mid_ir::opt
