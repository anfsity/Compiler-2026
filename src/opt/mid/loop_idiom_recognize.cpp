#include "loop_idiom_recognize.hpp"

#include "../../base/getptr.hpp"
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
  build_op_block_map(func);

  bool changed = false;
  for (auto *loop : loop_info.get_loops_innermost_first())
    changed |= replace_single_store_loop(*loop);

  op_blocks.clear();
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

auto LoopIdiomRecognize::match_counted_loop(const Loop &loop) const
  -> std::optional<CountedLoop> {
  if (
    !loop.get_preheader() || loop.get_blocks().size() != 2 ||
    loop.get_back_edges().size() != 1 ||
    loop.get_exiting_blocks().size() != 1 ||
    loop.get_exiting_blocks().front() != loop.get_header() ||
    loop.get_exit_blocks().size() != 1
  ) {
    return std::nullopt;
  }

  auto *preheader = loop.get_preheader();
  auto *header = loop.get_header();
  auto *latch = loop.get_back_edges().front().first;
  auto *exit = loop.get_exit_blocks().front();
  if (
    latch == header || !loop.contains(latch) ||
    (!exit->insts.empty() && exit->insts.front()->code == OpCode::Phi) ||
    header->insts.empty() || latch->insts.empty()
  ) {
    return std::nullopt;
  }

  auto *branch = header->insts.back();
  if (
    branch->code != OpCode::Branch || branch->operands.size() != 1 ||
    branch->successors.size() != 2 || branch->successors[0] != latch ||
    branch->successors[1] != exit
  ) {
    return std::nullopt;
  }

  auto *condition = branch->operands[0];
  if (!condition || condition->kind != ValueKind::OpResult)
    return std::nullopt;
  auto *compare =
    static_cast<Op *>(static_cast<OpResult *>(condition)->creator);
  auto compare_block = op_blocks.find(compare);
  if (
    !compare || compare_block == op_blocks.end() ||
    compare_block->second != header || compare->code != OpCode::Lt ||
    compare->operands.size() != 2
  ) {
    return std::nullopt;
  }

  auto *induction = compare->operands[0];
  auto *bound = compare->operands[1];
  if (
    !induction || induction->kind != ValueKind::OpResult || !bound ||
    !bound->type->is_i32()
  ) {
    return std::nullopt;
  }
  if (bound->kind == ValueKind::OpResult) {
    auto *bound_definition =
      static_cast<Op *>(static_cast<OpResult *>(bound)->creator);
    auto bound_block = op_blocks.find(bound_definition);
    if (bound_block == op_blocks.end() || loop.contains(bound_block->second)) {
      return std::nullopt;
    }
  }
  auto *phi = static_cast<Op *>(static_cast<OpResult *>(induction)->creator);
  auto phi_block = op_blocks.find(phi);
  if (
    !phi || phi_block == op_blocks.end() || phi_block->second != header ||
    phi->code != OpCode::Phi || !phi->result || phi->result != induction ||
    !phi->result->type->is_i32()
  ) {
    return std::nullopt;
  }

  Value *initial = nullptr;
  Value *backedge = nullptr;
  for (const auto &[pred, value] :
       std::get<PhiPayload>(phi->payload).incoming) {
    if (pred == preheader)
      initial = value;
    else if (pred == latch)
      backedge = value;
    else
      return std::nullopt;
  }
  if (
    integer_constant(initial) != 0 || !backedge ||
    backedge->kind != ValueKind::OpResult
  ) {
    return std::nullopt;
  }

  auto *update = static_cast<Op *>(static_cast<OpResult *>(backedge)->creator);
  auto update_block = op_blocks.find(update);
  if (
    !update || update_block == op_blocks.end() ||
    update_block->second != latch || update->code != OpCode::Add ||
    update->operands.size() != 2 ||
    !((update->operands[0] == induction &&
       integer_constant(update->operands[1]) == 1) ||
      (update->operands[1] == induction &&
       integer_constant(update->operands[0]) == 1))
  ) {
    return std::nullopt;
  }

  auto *terminator = latch->insts.back();
  if (
    terminator->code != OpCode::Jump || terminator->successors.size() != 1 ||
    terminator->successors.front() != header
  ) {
    return std::nullopt;
  }

  return CountedLoop{preheader, header, latch, exit, phi, bound};
}

auto LoopIdiomRecognize::match_contiguous_pointer(
  Value *pointer, const CountedLoop &loop
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
      if (definition->operands[operand_index] == loop.induction_phi->result) {
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
  auto counted = match_counted_loop(loop);
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
  reset_operands(store, {pointer, counted->bound, fill});

  auto *terminator = counted->latch->insts.back();
  terminator->successors = {counted->exit};
  return true;
}

} // namespace exodus::mid_ir::opt
