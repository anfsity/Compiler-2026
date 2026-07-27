#include "loop_idiom_recognize.hpp"

#include "../../base/getptr.hpp"
#include "../../mid/memory.hpp"
#include "../../mid/rewriter.hpp"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <unordered_set>

namespace exodus::mid_ir::opt {

auto LoopIdiomRecognize::run(
  LinearFunction &func, exodus::opt::LinearFunctionAnalysisManager &am
) -> exodus::opt::PreservedAnalysis {
  if (func.blocks.empty())
    return exodus::opt::PreservedAnalysis::all();

  auto &loop_info = am.get_result<LoopAnalysis>(func);
  affine_loops = &am.get_result<AffineLoopAnalysis>(func);
  build_op_block_map(func);

  bool changed = false;
  for (auto *loop : loop_info.get_loops_innermost_first()) {
    if (replace_single_store_loop(*loop)) {
      changed = true;
      continue;
    }
    changed |= hoist_independent_store(func, *loop);
  }

  op_blocks.clear();
  affine_loops = nullptr;
  return changed ? exodus::opt::PreservedAnalysis::none()
                 : exodus::opt::PreservedAnalysis::all();
}

auto LoopIdiomRecognize::build_op_block_map(LinearFunction &func) -> void {
  op_blocks.clear();
  for (auto &block : func.blocks) {
    for (auto *op : block->insts)
      op_blocks[op] = block.get();
  }
}

auto LoopIdiomRecognize::integer_constant(Value *value) -> std::optional<int> {
  if (!value || value->kind != ValueKind::Constant)
    return std::nullopt;
  auto *constant = static_cast<Constant *>(value);
  if (!std::holds_alternative<int>(constant->val))
    return std::nullopt;
  return std::get<int>(constant->val);
}

auto LoopIdiomRecognize::is_byte_splat_constant(Value *value) -> bool {
  if (!value || value->kind != ValueKind::Constant)
    return false;

  uint32_t bits = 0;
  const auto &constant = static_cast<Constant *>(value)->val;
  if (std::holds_alternative<int>(constant)) {
    bits = static_cast<uint32_t>(std::get<int>(constant));
  } else {
    auto float_value = std::get<float>(constant);
    static_assert(sizeof(bits) == sizeof(float_value));
    std::memcpy(&bits, &float_value, sizeof(bits));
  }
  auto byte = bits & 0xffu;
  return bits == byte * 0x01010101u;
}

auto LoopIdiomRecognize::match_contiguous_pointer(
  Value *pointer, const CountedLoopInfo &loop
) const -> bool {
  if (
    !pointer || pointer->kind != ValueKind::OpResult || !pointer->type->is_ptr()
  ) {
    return false;
  }

  auto *definition =
    static_cast<Op *>(static_cast<OpResult *>(pointer)->creator);
  auto definition_block = op_blocks.find(definition);
  if (!definition || definition_block == op_blocks.end())
    return false;

  if (
    definition->code == OpCode::GetPtr && definition_block->second == loop.latch
  ) {
    if (
      definition->operands.size() < 2 || !definition->result ||
      definition->result != pointer || !definition->operands[0]->type->is_ptr()
    ) {
      return false;
    }
    auto *base = definition->operands[0];
    if (base->kind == ValueKind::OpResult) {
      auto *base_definition =
        static_cast<Op *>(static_cast<OpResult *>(base)->creator);
      auto base_block = op_blocks.find(base_definition);
      if (
        base_block == op_blocks.end() || loop.header == base_block->second ||
        loop.latch == base_block->second
      ) {
        return false;
      }
    }
    auto plan = ir::analyze_getptr(
      definition->operands[0]->type,
      definition->result->type,
      definition->operands.size() - 1
    );
    // An implicit pointer load must remain live across the memset call and can
    // force the array arguments into callee-saved registers.  Keep this first
    // idiom deliberately limited to pure address formation; a later lowering
    // can revisit pointer-slot loads together with call/pressure costs.
    if (plan.reads_memory)
      return false;
    auto element_type = std::static_pointer_cast<Ptr>(pointer->type)->target;
    if (!element_type)
      return false;
    auto element_size = element_type->byte_size();
    size_t induction_indices = 0;
    for (const auto &step : plan.steps) {
      if (step.kind != ir::GetPtrStep::Kind::Index)
        continue;
      auto operand_index = step.index_pos + 1;
      if (operand_index >= definition->operands.size())
        return false;
      if (definition->operands[operand_index] == loop.induction.phi->result) {
        if (step.scale != element_size)
          return false;
        ++induction_indices;
      } else {
        auto *index = definition->operands[operand_index];
        if (index && index->kind == ValueKind::OpResult) {
          auto *index_definition =
            static_cast<Op *>(static_cast<OpResult *>(index)->creator);
          auto index_block = op_blocks.find(index_definition);
          if (
            index_block == op_blocks.end() ||
            loop.header == index_block->second ||
            loop.latch == index_block->second
          ) {
            return false;
          }
        }
      }
    }
    return induction_indices == 1;
  }

  if (
    definition->code != OpCode::Phi ||
    definition_block->second != loop.header || !definition->result ||
    definition->result != pointer
  ) {
    return false;
  }

  Value *initial_pointer = nullptr;
  Value *next_pointer = nullptr;
  for (const auto &[pred, value] :
       std::get<PhiPayload>(definition->payload).incoming) {
    if (pred == loop.preheader)
      initial_pointer = value;
    else if (pred == loop.latch)
      next_pointer = value;
    else
      return false;
  }
  if (
    !initial_pointer || !next_pointer ||
    next_pointer->kind != ValueKind::OpResult
  ) {
    return false;
  }

  auto *update =
    static_cast<Op *>(static_cast<OpResult *>(next_pointer)->creator);
  auto update_block = op_blocks.find(update);
  if (
    !update || update_block == op_blocks.end() ||
    update_block->second != loop.latch || update->code != OpCode::GetPtr ||
    update->operands.size() != 2 || update->operands[0] != pointer ||
    integer_constant(update->operands[1]) != 1 || !update->result ||
    update->result != next_pointer || !update->operands[0]->type->is_ptr() ||
    !update->result->type->is_ptr()
  ) {
    return false;
  }
  auto plan =
    ir::analyze_getptr(update->operands[0]->type, update->result->type, 1);
  auto element_type = std::static_pointer_cast<Ptr>(pointer->type)->target;
  return !plan.reads_memory && plan.steps.size() == 1 && element_type &&
         plan.steps.front().kind == ir::GetPtrStep::Kind::Index &&
         plan.steps.front().scale == element_type->byte_size();
}

auto LoopIdiomRecognize::has_escaping_result(const Loop &loop) const -> bool {
  std::unordered_set<Value *> loop_results;
  for (auto *block : loop.get_blocks()) {
    for (auto *op : block->insts) {
      if (op->result)
        loop_results.insert(op->result);
    }
  }

  for (const auto &[user, block] : op_blocks) {
    if (loop.contains(block))
      continue;
    if (
      std::any_of(
        user->operands.begin(), user->operands.end(), [&](Value *operand) {
          return loop_results.count(operand) != 0;
        }
      )
    ) {
      return true;
    }
    if (user->code != OpCode::Phi)
      continue;
    for (const auto &[pred, value] :
         std::get<PhiPayload>(user->payload).incoming) {
      (void)pred;
      if (loop_results.count(value))
        return true;
    }
  }
  return false;
}

auto LoopIdiomRecognize::reset_operands(Op *op, std::vector<Value *> operands)
  -> void {
  for (auto *operand : op->operands) {
    if (operand)
      operand->rmUse(op);
  }
  op->operands = std::move(operands);
  for (auto *operand : op->operands) {
    if (operand)
      operand->addUse(op);
  }
}

auto LoopIdiomRecognize::replace_single_store_loop(const Loop &loop) -> bool {
  auto counted =
    affine_loops ? affine_loops->match_counted_loop(loop) : std::nullopt;
  if (
    !counted || loop.get_blocks().size() != 2 ||
    counted->continuation != counted->latch || !counted->continue_on_true ||
    !counted->induction_on_lhs || !counted->compare ||
    counted->compare->code != OpCode::Lt ||
    integer_constant(counted->induction.initial) != 0 ||
    counted->induction.step != 1 || !counted->induction.update ||
    counted->induction.update->code != OpCode::Add ||
    counted->induction.predicate != OpCode::Lt ||
    (!counted->exit->insts.empty() &&
     counted->exit->insts.front()->code == OpCode::Phi) ||
    counted->latch->insts.empty() ||
    counted->latch->insts.back()->code != OpCode::Jump ||
    counted->latch->insts.back()->successors.size() != 1 ||
    counted->latch->insts.back()->successors.front() != counted->header
  ) {
    return false;
  }
  if (!counted || has_escaping_result(loop))
    return false;

  Op *store = nullptr;
  for (auto *block : loop.get_blocks()) {
    for (auto *op : block->insts) {
      switch (op->code) {
      case OpCode::Store:
        if (store)
          return false;
        store = op;
        break;
      case OpCode::Load:
      case OpCode::Call:
      case OpCode::Memset:
      case OpCode::Ret:
      case OpCode::Alloca:
        return false;
      default:
        break;
      }
    }
  }

  if (
    !store || op_blocks.at(store) != counted->latch ||
    store->operands.size() != 2 ||
    !is_byte_splat_constant(store->operands[0]) ||
    !match_contiguous_pointer(store->operands[1], *counted)
  ) {
    return false;
  }

  auto *pointer = store->operands[1];
  auto *pointer_definition =
    pointer->kind == ValueKind::OpResult
      ? static_cast<Op *>(static_cast<OpResult *>(pointer)->creator)
      : nullptr;
  for (auto *block : loop.get_blocks()) {
    for (auto *op : block->insts) {
      if (op->code != OpCode::GetPtr || op == pointer_definition)
        continue;
      if (
        !op->result || op->operands.empty() ||
        !op->operands[0]->type->is_ptr() || !op->result->type->is_ptr()
      )
        return false;
      if (
        ir::analyze_getptr(
          op->operands[0]->type, op->result->type, op->operands.size() - 1
        )
          .reads_memory
      ) {
        return false;
      }
    }
  }

  auto *fill = store->operands[0];
  store->code = OpCode::Memset;
  store->payload = EmptyPayload{};
  reset_operands(store, {pointer, counted->induction.bound, fill});

  auto *terminator = counted->latch->insts.back();
  terminator->successors = {counted->exit};
  return true;
}

auto LoopIdiomRecognize::get_initial_pointer(
  Value *pointer, const CountedLoopInfo &loop
) const -> Value * {
  if (
    !pointer || pointer->kind != ValueKind::OpResult || !pointer->type->is_ptr()
  ) {
    return nullptr;
  }
  auto *phi = static_cast<Op *>(static_cast<OpResult *>(pointer)->creator);
  auto phi_block = op_blocks.find(phi);
  if (
    !phi || phi_block == op_blocks.end() || phi_block->second != loop.header ||
    phi->code != OpCode::Phi || phi->result != pointer
  ) {
    return nullptr;
  }

  Value *initial = nullptr;
  size_t backedge_incoming = 0;
  for (const auto &[pred, value] :
       std::get<PhiPayload>(phi->payload).incoming) {
    if (pred == loop.preheader) {
      if (initial)
        return nullptr;
      initial = value;
    } else if (pred == loop.latch) {
      ++backedge_incoming;
    } else {
      return nullptr;
    }
  }
  return initial && backedge_incoming == 1 ? initial : nullptr;
}

auto LoopIdiomRecognize::memory_accesses_are_independent(
  Op *store, const Loop &loop, const CountedLoopInfo &counted
) const -> bool {
  BasicAliasAnalysis alias_analysis;
  auto store_location = alias_analysis.get_location(*store);
  if (!store_location)
    return false;

  for (auto *block : loop.get_blocks()) {
    for (auto *op : block->insts) {
      if (op == store)
        continue;
      if (
        op->code == OpCode::Call || op->code == OpCode::Memset ||
        op->code == OpCode::Alloca || op->code == OpCode::Ret
      )
        return false;
      if (op->code == OpCode::Load || op->code == OpCode::Store) {
        if (block != counted.latch)
          return false;
        auto location = alias_analysis.get_location(*op);
        if (
          !location || alias_analysis.alias(*store_location, *location) !=
                         AliasResult::NoAlias
        ) {
          return false;
        }
        continue;
      }
      if (op->code != OpCode::GetPtr)
        continue;
      if (
        !op->result || op->operands.empty() ||
        !op->operands[0]->type->is_ptr() || !op->result->type->is_ptr() ||
        ir::analyze_getptr(
          op->operands[0]->type, op->result->type, op->operands.size() - 1
        )
          .reads_memory
      ) {
        return false;
      }
    }
  }
  return true;
}

auto LoopIdiomRecognize::hoist_independent_store(
  LinearFunction &func, const Loop &loop
) -> bool {
  auto counted =
    affine_loops ? affine_loops->match_counted_loop(loop) : std::nullopt;
  if (
    !counted || loop.get_blocks().size() != 2 ||
    counted->continuation != counted->latch || !counted->continue_on_true ||
    !counted->induction_on_lhs || !counted->compare ||
    counted->compare->code != OpCode::Lt ||
    integer_constant(counted->induction.initial) != 0 ||
    counted->induction.step != 1 || !counted->induction.update ||
    counted->induction.update->code != OpCode::Add ||
    counted->induction.predicate != OpCode::Lt ||
    (!counted->exit->insts.empty() &&
     counted->exit->insts.front()->code == OpCode::Phi) ||
    counted->latch->insts.empty() ||
    counted->latch->insts.back()->code != OpCode::Jump ||
    counted->latch->insts.back()->successors.size() != 1 ||
    counted->latch->insts.back()->successors.front() != counted->header ||
    !module || !module->ctx || has_escaping_result(loop)
  ) {
    return false;
  }

  Op *candidate = nullptr;
  size_t store_count = 0;
  for (auto *block : loop.get_blocks()) {
    for (auto *op : block->insts) {
      if (op->code != OpCode::Store)
        continue;
      ++store_count;
      if (
        op_blocks.at(op) != counted->latch || op->operands.size() != 2 ||
        !is_byte_splat_constant(op->operands[0]) ||
        !match_contiguous_pointer(op->operands[1], *counted) ||
        !get_initial_pointer(op->operands[1], *counted)
      ) {
        continue;
      }
      if (candidate)
        return false;
      candidate = op;
    }
  }
  if (
    store_count < 2 || !candidate ||
    !memory_accesses_are_independent(candidate, loop, *counted)
  ) {
    return false;
  }

  auto *preheader_terminator = counted->preheader->insts.empty()
                                 ? nullptr
                                 : counted->preheader->insts.back();
  if (
    !preheader_terminator || preheader_terminator->code != OpCode::Jump ||
    preheader_terminator->successors.size() != 1 ||
    preheader_terminator->successors.front() != counted->header
  ) {
    return false;
  }

  std::vector<Op *> header_phis;
  for (auto *op : counted->header->insts) {
    if (op->code != OpCode::Phi)
      break;
    size_t preheader_incoming = 0;
    size_t backedge_incoming = 0;
    for (const auto &[pred, value] :
         std::get<PhiPayload>(op->payload).incoming) {
      (void)value;
      preheader_incoming += pred == counted->preheader;
      backedge_incoming += pred == counted->latch;
    }
    if (
      preheader_incoming != 1 || backedge_incoming != 1 ||
      std::get<PhiPayload>(op->payload).incoming.size() != 2
    )
      return false;
    header_phis.push_back(op);
  }

  auto header_it = std::find_if(
    func.blocks.begin(), func.blocks.end(), [&](const auto &block) {
      return block.get() == counted->header;
    }
  );
  if (header_it == func.blocks.end())
    return false;

  auto guard_ptr = std::make_unique<Block>(
    static_cast<int>(func.blocks.size()),
    counted->header->name + "_memset_guard"
  );
  auto init_ptr = std::make_unique<Block>(
    static_cast<int>(func.blocks.size() + 1),
    counted->header->name + "_memset_init"
  );
  auto *guard = guard_ptr.get();
  auto *init = init_ptr.get();

  auto *condition = module->make_op(OpCode::Lt);
  condition->operands = {
    counted->induction.initial,
    counted->induction.bound,
  };
  condition->result = module->ctx->make_value<OpResult>(Bool::get(), condition);
  for (auto *operand : condition->operands)
    operand->addUse(condition);
  guard->insts.push_back(condition);

  auto *branch = module->make_op(OpCode::Branch);
  branch->operands = {condition->result};
  condition->result->addUse(branch);
  branch->successors = {init, counted->exit};
  guard->insts.push_back(branch);

  auto *initial_pointer = get_initial_pointer(candidate->operands[1], *counted);
  auto *memset = module->make_op(OpCode::Memset);
  memset->operands = {
    initial_pointer,
    counted->induction.bound,
    candidate->operands[0],
  };
  for (auto *operand : memset->operands)
    operand->addUse(memset);
  init->insts.push_back(memset);

  auto *jump = module->make_op(OpCode::Jump);
  jump->successors = {counted->header};
  init->insts.push_back(jump);

  preheader_terminator->successors = {guard};
  for (auto *phi : header_phis) {
    for (auto &[pred, value] : std::get<PhiPayload>(phi->payload).incoming) {
      (void)value;
      if (pred == counted->preheader)
        pred = init;
    }
  }

  func.blocks.insert(header_it, std::move(guard_ptr));
  func.blocks.insert(header_it, std::move(init_ptr));
  renumber_blocks(func);

  MidIRRewriter rewriter;
  rewriter.set_scope(func);
  rewriter.eraseOp(candidate);
  rewriter.finalize(func);
  return true;
}

auto LoopIdiomRecognize::renumber_blocks(LinearFunction &func) -> void {
  int id = 0;
  for (auto &block : func.blocks)
    block->id = id++;
}

} // namespace exodus::mid_ir::opt
