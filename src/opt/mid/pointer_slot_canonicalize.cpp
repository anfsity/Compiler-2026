#include "pointer_slot_canonicalize.hpp"

#include <algorithm>

namespace exodus::mid_ir::opt {

auto PointerSlotCanonicalize::run(
  LinearFunction &func, exodus::opt::LinearFunctionAnalysisManager &am
) -> exodus::opt::PreservedAnalysis {
  if (func.blocks.empty())
    return exodus::opt::PreservedAnalysis::all();

  auto &dom = am.get_result<DominanceAnalysis>(func);
  Context context;
  build_scope(context, func);

  std::vector<Candidate> candidates;
  for (auto &block : func.blocks) {
    for (auto *op : block->insts) {
      if (op->code != OpCode::Alloca)
        continue;
      if (auto candidate = collect_candidate(op, dom, context))
        candidates.push_back(std::move(*candidate));
    }
  }

  if (candidates.empty()) {
    return exodus::opt::PreservedAnalysis::all();
  }

  MidIRRewriter rewriter;
  rewriter.set_scope(func);
  for (const auto &candidate : candidates) {
    for (auto *getptr : candidate.getptrs) {
      auto *old_base = getptr->operands[0];
      getptr->operands[0] = candidate.stored_pointer;
      candidate.stored_pointer->addUse(getptr);
      old_base->rmUse(getptr);
    }
    rewriter.eraseOp(candidate.store);
    rewriter.eraseOp(candidate.alloca);
  }
  rewriter.finalize(func);

  return exodus::opt::PreservedAnalysis::none();
}

auto PointerSlotCanonicalize::build_scope(
  Context &context, LinearFunction &func
) -> void {
  context.op_blocks.clear();
  context.scope.clear();
  for (auto &block : func.blocks) {
    for (auto *op : block->insts) {
      context.op_blocks[op] = block.get();
      context.scope.insert(op);
    }
  }
}

auto PointerSlotCanonicalize::dominates(
  Op *definition, Op *use, DomTree &dom, const Context &context
) -> bool {
  auto definition_block = context.op_blocks.find(definition);
  auto use_block = context.op_blocks.find(use);
  if (
    !definition || !use || definition_block == context.op_blocks.end() ||
    use_block == context.op_blocks.end()
  ) {
    return false;
  }
  if (definition_block->second != use_block->second)
    return dom.dominate(definition_block->second, use_block->second);

  for (auto *op : definition_block->second->insts) {
    if (op == definition)
      return true;
    if (op == use)
      return false;
  }
  return false;
}

auto PointerSlotCanonicalize::collect_candidate(
  Op *alloca, DomTree &dom, const Context &context
) -> std::optional<Candidate> {
  if (
    !alloca || !alloca->result || !alloca->result->type ||
    !alloca->result->type->is_ptr()
  ) {
    return std::nullopt;
  }

  auto stored_type =
    std::static_pointer_cast<Ptr>(alloca->result->type)->target;
  if (!stored_type || !stored_type->is_ptr())
    return std::nullopt;

  Candidate candidate;
  candidate.alloca = alloca;
  std::unordered_set<Op *> seen_users;
  for (auto *user_base : alloca->result->users) {
    auto *user = static_cast<Op *>(user_base);
    // High and Mid IR share Value objects during flattening, so the use list
    // also contains upstream High IR operations.  Only Mid operations in this
    // function are part of the escape proof.
    if (!user || !context.scope.count(user))
      continue;
    if (!seen_users.insert(user).second)
      continue;

    const auto occurrences = static_cast<size_t>(
      std::count(user->operands.begin(), user->operands.end(), alloca->result)
    );
    if (
      user->code == OpCode::Store && user->operands.size() == 2 &&
      user->operands[1] == alloca->result && occurrences == 1
    ) {
      if (
        candidate.store || !user->operands[0] ||
        user->operands[0]->type != stored_type
      ) {
        return std::nullopt;
      }
      candidate.store = user;
      candidate.stored_pointer = user->operands[0];
      continue;
    }

    if (
      user->code != OpCode::GetPtr || user->operands.size() < 2 ||
      user->operands[0] != alloca->result || occurrences != 1
    ) {
      return std::nullopt;
    }
    candidate.getptrs.push_back(user);
  }

  if (
    !candidate.store || !candidate.stored_pointer ||
    candidate.getptrs.empty() ||
    !dominates(alloca, candidate.store, dom, context)
  ) {
    return std::nullopt;
  }
  if (candidate.stored_pointer->kind == ValueKind::OpResult) {
    auto *definition = static_cast<Op *>(
      static_cast<OpResult *>(candidate.stored_pointer)->creator
    );
    if (
      !definition || !context.scope.count(definition) ||
      !dominates(definition, candidate.store, dom, context)
    ) {
      return std::nullopt;
    }
  }

  for (auto *getptr : candidate.getptrs) {
    if (
      !dominates(alloca, getptr, dom, context) ||
      !dominates(candidate.store, getptr, dom, context) ||
      !preserves_getptr_plan(getptr, candidate.stored_pointer)
    ) {
      return std::nullopt;
    }
  }
  return candidate;
}

auto PointerSlotCanonicalize::preserves_getptr_plan(
  Op *getptr, Value *replacement
) -> bool {
  if (
    !getptr || !getptr->result || !getptr->result->type || !replacement ||
    !replacement->type
  )
    return false;
  const auto *payload = std::get_if<GetPtrPayload>(&getptr->payload);
  if (
    !payload || !payload->layout_type ||
    replacement->type != payload->layout_type
  ) {
    return false;
  }

  auto before = mid_ir::analyze_getptr(*getptr);
  const auto implicit_loads = std::count_if(
    before.steps.begin(), before.steps.end(), [](const ir::GetPtrStep &step) {
      return step.kind == ir::GetPtrStep::Kind::ImplicitLoad;
    }
  );
  if (
    !before.valid || !before.reads_memory || implicit_loads != 1 ||
    before.steps.empty() ||
    before.steps.front().kind != ir::GetPtrStep::Kind::ImplicitLoad
  ) {
    return false;
  }

  auto after = ir::analyze_getptr_with_layout(
    replacement->type,
    payload->layout_type,
    getptr->result->type,
    getptr->operands.size() - 1
  );
  if (
    !after.valid || after.reads_memory ||
    before.steps.size() != after.steps.size() + 1 ||
    !ir::same_getptr_byte_offset_plan(before, after)
  ) {
    return false;
  }
  return std::equal(
    std::next(before.steps.begin()),
    before.steps.end(),
    after.steps.begin(),
    after.steps.end(),
    ir::same_getptr_step
  );
}

} // namespace exodus::mid_ir::opt
