#include "polyhedral_opt.hpp"

#include "../../base/getptr.hpp"
#include "../../mid/rewriter.hpp"
#include <algorithm>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace exodus::mid_ir::opt {
namespace {

inline constexpr size_t max_scratch_bytes = 4096;

auto is_jump_to(Block *block, Block *successor) -> bool {
  if (!block || block->insts.empty())
    return false;
  auto *jump = block->insts.back();
  return jump->code == OpCode::Jump && jump->successors.size() == 1 &&
         jump->successors.front() == successor;
}

auto set_branch_successors(
  const CountedLoopInfo &counted, Block *continuation, Block *exit
) -> bool {
  if (!counted.header || counted.header->insts.empty())
    return false;
  auto *branch = counted.header->insts.back();
  if (branch->code != OpCode::Branch || branch->successors.size() != 2)
    return false;
  branch->successors[counted.continue_on_true ? 0 : 1] = continuation;
  branch->successors[counted.continue_on_true ? 1 : 0] = exit;
  return true;
}

auto set_jump_successor(Block *block, Block *successor) -> bool {
  if (!block || block->insts.empty())
    return false;
  auto *jump = block->insts.back();
  if (jump->code != OpCode::Jump || jump->successors.size() != 1)
    return false;
  jump->successors.front() = successor;
  return true;
}

auto retarget_phi(
  Op *phi,
  Block *old_preheader,
  Block *new_preheader,
  Block *old_latch,
  Block *new_latch
) -> bool {
  if (!phi || phi->code != OpCode::Phi)
    return false;
  bool found_preheader = false;
  bool found_latch = false;
  for (auto &[block, value] : std::get<PhiPayload>(phi->payload).incoming) {
    (void)value;
    if (block == old_preheader) {
      block = new_preheader;
      found_preheader = true;
    } else if (block == old_latch) {
      block = new_latch;
      found_latch = true;
    }
  }
  return found_preheader && found_latch;
}

auto replace_value(Value *value, Value *from, Value *to) -> Value * {
  return value == from ? to : value;
}

auto append_operands(Op *op, std::vector<Value *> operands) -> void {
  op->operands = std::move(operands);
  for (auto *operand : op->operands)
    operand->addUse(op);
}

auto stride_magnitude(int64_t value) -> uint64_t {
  return value < 0 ? static_cast<uint64_t>(-static_cast<__int128>(value))
                   : static_cast<uint64_t>(value);
}

auto checked_stride(int64_t current, int64_t coefficient, int scale)
  -> std::optional<int64_t> {
  auto next =
    static_cast<__int128>(current) + static_cast<__int128>(coefficient) * scale;
  if (
    next < std::numeric_limits<int64_t>::min() ||
    next > std::numeric_limits<int64_t>::max()
  ) {
    return std::nullopt;
  }
  return static_cast<int64_t>(next);
}

auto add_cost(uint64_t &cost, uint64_t amount, uint64_t count = 1) -> bool {
  auto increment = static_cast<__int128>(amount) * count;
  if (increment > std::numeric_limits<uint64_t>::max() - cost)
    return false;
  cost += static_cast<uint64_t>(increment);
  return true;
}

auto getptr_strides(
  Op *getptr,
  ScalarEvolution &scev,
  const std::vector<CountedLoopInfo> &iteration_space,
  Value *outer_iterator,
  Value *inner_iterator
) -> std::optional<std::pair<int64_t, int64_t>> {
  if (
    !getptr || getptr->code != OpCode::GetPtr || !getptr->result ||
    getptr->operands.size() < 2 || !getptr->operands[0]->type->is_ptr() ||
    !getptr->result->type->is_ptr()
  ) {
    return std::nullopt;
  }
  auto plan = ir::analyze_getptr(
    getptr->operands[0]->type, getptr->result->type, getptr->operands.size() - 1
  );
  if (plan.reads_memory)
    return std::nullopt;

  std::vector<SCEVAffineExpr> subscripts;
  for (size_t index = 1; index < getptr->operands.size(); ++index) {
    auto expr = scev.get_affine(getptr->operands[index], iteration_space);
    if (!expr || !expr->no_wrap)
      return std::nullopt;
    subscripts.push_back(std::move(*expr));
  }

  int64_t outer_stride = 0;
  int64_t inner_stride = 0;
  for (const auto &step : plan.steps) {
    if (
      step.kind != ir::GetPtrStep::Kind::Index ||
      step.index_pos >= subscripts.size()
    ) {
      return std::nullopt;
    }
    auto next_outer = checked_stride(
      outer_stride,
      subscripts[step.index_pos].coefficient(outer_iterator),
      step.scale
    );
    auto next_inner = checked_stride(
      inner_stride,
      subscripts[step.index_pos].coefficient(inner_iterator),
      step.scale
    );
    if (!next_outer || !next_inner)
      return std::nullopt;
    outer_stride = *next_outer;
    inner_stride = *next_inner;
  }
  return std::pair{outer_stride, inner_stride};
}

auto compatible_getptr_plans(
  const ir::GetPtrPlan &lhs, const ir::GetPtrPlan &rhs
) -> bool {
  if (
    lhs.reads_memory || rhs.reads_memory || lhs.steps.size() != rhs.steps.size()
  )
    return false;
  for (size_t index = 0; index < lhs.steps.size(); ++index) {
    const auto &left = lhs.steps[index];
    const auto &right = rhs.steps[index];
    if (
      left.kind != right.kind || left.index_pos != right.index_pos ||
      left.scale != right.scale || left.from_type != right.from_type ||
      left.to_type != right.to_type
    ) {
      return false;
    }
  }
  return true;
}

auto row_delayed_access_capacity(
  Op *output,
  Op *input,
  Value *row_iterator,
  Value *column_iterator,
  Value *reduction_iterator
) -> std::optional<size_t> {
  if (
    !output || !input || output->code != OpCode::GetPtr ||
    input->code != OpCode::GetPtr || !output->result || !input->result ||
    output->operands.size() < 3 ||
    output->operands.size() != input->operands.size() ||
    output->operands[0] != input->operands[0] ||
    !output->operands[0]->type->is_ptr() || !output->result->type->is_ptr() ||
    !input->result->type->is_ptr()
  ) {
    return std::nullopt;
  }

  auto output_plan = ir::analyze_getptr(
    output->operands[0]->type, output->result->type, output->operands.size() - 1
  );
  auto input_plan = ir::analyze_getptr(
    input->operands[0]->type, input->result->type, input->operands.size() - 1
  );
  if (!compatible_getptr_plans(output_plan, input_plan))
    return std::nullopt;

  std::optional<size_t> column_capacity;
  size_t row_positions = 0;
  size_t column_positions = 0;
  for (size_t index = 1; index < output->operands.size(); ++index) {
    auto *output_index = output->operands[index];
    auto *input_index = input->operands[index];
    if (output_index == row_iterator && input_index == reduction_iterator) {
      ++row_positions;
      continue;
    }
    if (output_index == column_iterator && input_index == column_iterator) {
      ++column_positions;
      const auto step = std::find_if(
        output_plan.steps.begin(),
        output_plan.steps.end(),
        [index](const ir::GetPtrStep &candidate) {
          return candidate.kind == ir::GetPtrStep::Kind::Index &&
                 candidate.index_pos == index - 1;
        }
      );
      if (
        step == output_plan.steps.end() || !step->from_type ||
        !step->from_type->is_array() || !step->to_type ||
        !step->to_type->is_i32() || step->scale != I32::get()->byte_size()
      ) {
        return std::nullopt;
      }
      const auto length = std::static_pointer_cast<Array>(step->from_type)->len;
      if (length <= 0)
        return std::nullopt;
      column_capacity = static_cast<size_t>(length);
      continue;
    }
    if (output_index != input_index)
      return std::nullopt;
  }

  if (row_positions != 1 || column_positions != 1 || !column_capacity)
    return std::nullopt;
  return column_capacity;
}

auto loop_nest_has_no_live_outs(const LinearFunction &func, const Loop &loop)
  -> bool {
  std::unordered_set<Value *> definitions;
  for (auto *block : loop.get_blocks()) {
    for (auto *op : block->insts) {
      if (op->result)
        definitions.insert(op->result);
      for (auto *successor : op->successors) {
        if (
          !loop.contains(successor) &&
          successor != loop.get_exit_blocks().front()
        )
          return false;
      }
    }
  }

  for (const auto &block : func.blocks) {
    if (loop.contains(block.get()))
      continue;
    for (auto *op : block->insts) {
      for (auto *operand : op->operands) {
        if (definitions.count(operand))
          return false;
      }
      if (op->code != OpCode::Phi)
        continue;
      for (const auto &[predecessor, value] :
           std::get<PhiPayload>(op->payload).incoming) {
        (void)predecessor;
        if (definitions.count(value))
          return false;
      }
    }
  }
  return true;
}

auto clone_loop_nest(
  LinearFunction &func,
  const Loop &loop,
  Block *old_preheader,
  Block *new_preheader,
  MidModule *module
) -> std::optional<Block *> {
  if (
    !module || !old_preheader || !new_preheader ||
    loop.get_exit_blocks().size() != 1 ||
    !loop_nest_has_no_live_outs(func, loop)
  ) {
    return std::nullopt;
  }

  std::vector<Block *> source_blocks;
  for (const auto &block : func.blocks) {
    if (loop.contains(block.get()))
      source_blocks.push_back(block.get());
  }
  if (source_blocks.empty())
    return std::nullopt;

  auto insertion_point = std::find_if(
    func.blocks.begin(),
    func.blocks.end(),
    [header = loop.get_header()](const std::unique_ptr<Block> &block) {
      return block.get() == header;
    }
  );
  if (insertion_point == func.blocks.end())
    return std::nullopt;

  std::unordered_map<Block *, Block *> block_map;
  int next_id = static_cast<int>(func.blocks.size());
  for (auto *source : source_blocks) {
    auto clone =
      std::make_unique<Block>(next_id++, source->name + "_poly_fallback");
    block_map[source] = clone.get();
    func.blocks.insert(insertion_point, std::move(clone));
  }

  std::unordered_map<Value *, Value *> value_map;
  std::unordered_map<Op *, Op *> op_map;
  for (auto *source_block : source_blocks) {
    auto *destination = block_map.at(source_block);
    for (auto *source : source_block->insts) {
      auto *clone = module->make_op(source->code, source->payload);
      if (source->result) {
        clone->result =
          module->ctx->make_value<OpResult>(source->result->type, clone);
        value_map[source->result] = clone->result;
      }
      op_map[source] = clone;
      destination->insts.push_back(clone);
    }
  }

  auto map_value = [&value_map](Value *value) {
    auto mapped = value_map.find(value);
    return mapped == value_map.end() ? value : mapped->second;
  };
  for (auto *source_block : source_blocks) {
    for (auto *source : source_block->insts) {
      auto *clone = op_map.at(source);
      for (auto *operand : source->operands) {
        auto *mapped = map_value(operand);
        clone->operands.push_back(mapped);
        mapped->addUse(clone);
      }
      for (auto *successor : source->successors) {
        auto mapped = block_map.find(successor);
        clone->successors.push_back(
          mapped == block_map.end() ? successor : mapped->second
        );
      }
      if (source->code != OpCode::Phi)
        continue;
      auto &incoming = std::get<PhiPayload>(clone->payload).incoming;
      incoming.clear();
      for (const auto &[predecessor, value] :
           std::get<PhiPayload>(source->payload).incoming) {
        Block *mapped_predecessor = predecessor;
        if (predecessor == old_preheader) {
          mapped_predecessor = new_preheader;
        } else if (
          auto mapped = block_map.find(predecessor); mapped != block_map.end()
        ) {
          mapped_predecessor = mapped->second;
        }
        auto *mapped_value = map_value(value);
        incoming.push_back({mapped_predecessor, mapped_value});
        mapped_value->addUse(clone);
      }
    }
  }

  return block_map.at(loop.get_header());
}

} // namespace

auto PolyhedralOpt::run(
  LinearFunction &func, exodus::opt::LinearFunctionAnalysisManager &am
) -> exodus::opt::PreservedAnalysis {
  if (prepare_reduction(func, am)) {
    DomTree dom;
    dom.compute(func);
    LoopInfo loops;
    loops.compute(func, dom);
    AffineLoopInfo affine;
    affine.compute(func, loops, dom);
    ScalarEvolution scev;
    scev.compute(func, loops, affine, dom);
    PolyhedralInfo polyhedral;
    polyhedral.compute(func, loops, affine, scev, module);
    for (const auto &scop : polyhedral.get_scops()) {
      if (
        scop.interchange_legal &&
        scop.interchanged_stride_cost < scop.original_stride_cost
      ) {
        if (interchange(func, scop))
          break;
      }
    }
    return exodus::opt::PreservedAnalysis::none();
  }

  auto &polyhedral = am.get_result<PolyhedralAnalysis>(func);
  for (const auto &scop : polyhedral.get_scops()) {
    if (
      !scop.interchange_legal ||
      scop.interchanged_stride_cost >= scop.original_stride_cost
    ) {
      continue;
    }
    if (interchange(func, scop))
      return exodus::opt::PreservedAnalysis::none();
  }
  return exodus::opt::PreservedAnalysis::all();
}

auto PolyhedralOpt::prepare_reduction(
  LinearFunction &func, exodus::opt::LinearFunctionAnalysisManager &am
) -> bool {
  if (!module)
    return false;
  auto &loops = am.get_result<LoopAnalysis>(func);
  auto &affine = am.get_result<AffineLoopAnalysis>(func);
  auto &scev = am.get_result<ScalarEvolutionAnalysis>(func);
  BasicAliasAnalysis alias;
  std::unordered_map<Op *, Block *> op_blocks;
  for (const auto &block : func.blocks) {
    for (auto *op : block->insts)
      op_blocks[op] = block.get();
  }

  for (auto *inner_loop : loops.get_loops_innermost_first()) {
    auto *outer_loop = inner_loop->get_parent();
    if (
      !outer_loop || outer_loop->get_subloops().size() != 1 ||
      outer_loop->get_subloops().front() != inner_loop ||
      !inner_loop->get_subloops().empty() ||
      excluded_fallback_headers.count(outer_loop->get_header())
    ) {
      continue;
    }
    auto outer = affine.match_counted_loop(*outer_loop);
    auto inner = affine.match_counted_loop(*inner_loop);
    if (
      !outer || !inner || outer->induction.step != 1 ||
      inner->induction.step != 1 || outer->induction.predicate != OpCode::Lt ||
      inner->induction.predicate != OpCode::Lt ||
      outer->continuation != inner->preheader || inner->exit != outer->latch ||
      !outer->preheader || outer->preheader->insts.empty() ||
      !outer->induction.phi || !inner->induction.phi ||
      !is_jump_to(outer->preheader, outer->header)
    ) {
      continue;
    }

    Op *reduction_phi = nullptr;
    size_t header_phis = 0;
    bool clean_inner_header = true;
    for (auto *op : inner->header->insts) {
      if (op->code == OpCode::Phi) {
        ++header_phis;
        if (op != inner->induction.phi) {
          if (reduction_phi) {
            clean_inner_header = false;
            break;
          }
          reduction_phi = op;
        }
      } else if (op != inner->compare && op != inner->header->insts.back()) {
        clean_inner_header = false;
        break;
      }
    }
    if (
      !clean_inner_header || header_phis != 2 || !reduction_phi ||
      !reduction_phi->result || !reduction_phi->result->type->is_i32()
    ) {
      continue;
    }

    Value *initial = nullptr;
    Value *backedge = nullptr;
    for (const auto &[block, value] :
         std::get<PhiPayload>(reduction_phi->payload).incoming) {
      if (block == inner->preheader)
        initial = value;
      else if (block == inner->latch)
        backedge = value;
      else
        initial = nullptr;
    }
    if (
      !initial || !backedge ||
      !affine.is_available_at_preheader(initial, outer->preheader)
    ) {
      continue;
    }

    Op *output_store = nullptr;
    Op *output_getptr = nullptr;
    bool clean_outer_latch = true;
    for (auto *op : outer->latch->insts) {
      if (op == outer->induction.update || op == outer->latch->insts.back())
        continue;
      if (
        op->code == OpCode::Store && op->operands.size() == 2 &&
        op->operands[0] == reduction_phi->result && !output_store
      ) {
        output_store = op;
        continue;
      }
      if (op->code == OpCode::GetPtr && !output_getptr) {
        output_getptr = op;
        continue;
      }
      clean_outer_latch = false;
      break;
    }
    if (
      !clean_outer_latch || !output_store || !output_getptr ||
      output_store->operands[1] != output_getptr->result
    ) {
      continue;
    }
    bool uses_are_local = true;
    for (auto *user_base : reduction_phi->result->users) {
      auto *user = static_cast<Op *>(user_base);
      auto block = op_blocks.find(user);
      if (
        block != op_blocks.end() && user != output_store &&
        !inner_loop->contains(block->second)
      ) {
        uses_are_local = false;
        break;
      }
    }
    for (auto *user_base : output_getptr->result->users) {
      auto *user = static_cast<Op *>(user_base);
      if (op_blocks.count(user) && user != output_store) {
        uses_are_local = false;
        break;
      }
    }
    if (!uses_are_local)
      continue;
    bool output_available = true;
    for (size_t index = 1; index < output_getptr->operands.size(); ++index) {
      auto *operand = output_getptr->operands[index];
      if (
        operand != outer->induction.phi->result &&
        !affine.is_available_at_preheader(operand, outer->preheader)
      ) {
        output_available = false;
        break;
      }
    }
    if (!output_available)
      continue;

    const std::vector<CountedLoopInfo> iteration_space{*outer, *inner};
    auto output_strides = getptr_strides(
      output_getptr,
      scev,
      iteration_space,
      outer->induction.phi->result,
      inner->induction.phi->result
    );
    if (!output_strides || output_strides->second != 0)
      continue;

    auto output_location = alias.get_location(output_getptr->result);
    if (!output_location.root)
      continue;
    auto *row_loop = outer_loop->get_parent();
    auto row = row_loop ? affine.match_counted_loop(*row_loop) : std::nullopt;
    std::optional<size_t> scratch_capacity;
    uint64_t original_cost = 0;
    uint64_t interchanged_cost = 0;
    if (
      !add_cost(original_cost, stride_magnitude(output_strides->second), 2) ||
      !add_cost(interchanged_cost, stride_magnitude(output_strides->first), 2)
    ) {
      continue;
    }
    bool memory_safe = true;
    for (auto *block : inner_loop->get_blocks()) {
      for (auto *op : block->insts) {
        if (
          op->code == OpCode::Call || op->code == OpCode::Memset ||
          op->code == OpCode::Alloca || op->code == OpCode::Ret
        ) {
          memory_safe = false;
          break;
        }
        if (op->code != OpCode::Load && op->code != OpCode::Store)
          continue;
        Value *pointer =
          op->code == OpCode::Load ? op->operands[0] : op->operands[1];
        if (!pointer || pointer->kind != ValueKind::OpResult) {
          memory_safe = false;
          break;
        }
        auto *getptr =
          static_cast<Op *>(static_cast<OpResult *>(pointer)->creator);
        auto strides = getptr_strides(
          getptr,
          scev,
          iteration_space,
          outer->induction.phi->result,
          inner->induction.phi->result
        );
        auto location = alias.get_location(pointer);
        if (!strides || !location.root) {
          memory_safe = false;
          break;
        }
        if (alias.may_alias(output_location, location)) {
          if (
            op->code != OpCode::Load || location.root != output_location.root ||
            !row || row->induction.step != 1 ||
            row->induction.predicate != OpCode::Lt ||
            row->continuation != outer->preheader || outer->exit != row->latch
          ) {
            memory_safe = false;
            break;
          }
          auto capacity = row_delayed_access_capacity(
            output_getptr,
            getptr,
            row->induction.phi->result,
            outer->induction.phi->result,
            inner->induction.phi->result
          );
          if (!capacity) {
            memory_safe = false;
            break;
          }
          auto effective_capacity = std::min(
            *capacity,
            max_scratch_bytes / static_cast<size_t>(I32::get()->byte_size())
          );
          if (scratch_capacity && *scratch_capacity != effective_capacity) {
            memory_safe = false;
            break;
          }
          scratch_capacity = effective_capacity;
        }
        if (
          !add_cost(original_cost, stride_magnitude(strides->second)) ||
          !add_cost(interchanged_cost, stride_magnitude(strides->first))
        ) {
          memory_safe = false;
          break;
        }
      }
      if (!memory_safe)
        break;
    }
    if (!memory_safe || interchanged_cost >= original_cost)
      continue;

    auto header_position = std::find_if(
      func.blocks.begin(),
      func.blocks.end(),
      [header = outer->header](const std::unique_ptr<Block> &block) {
        return block.get() == header;
      }
    );
    auto exit_position = func.blocks.end();
    if (scratch_capacity) {
      exit_position = std::find_if(
        func.blocks.begin(),
        func.blocks.end(),
        [exit = outer->exit](const std::unique_ptr<Block> &block) {
          return block.get() == exit;
        }
      );
    }
    if (
      header_position == func.blocks.end() ||
      (scratch_capacity && (!row->preheader || row->preheader->insts.empty() ||
                            exit_position == func.blocks.end()))
    ) {
      continue;
    }

    bool needs_runtime_guard = false;
    Block *fallback_header = nullptr;
    if (scratch_capacity) {
      auto initial_index =
        AffineLoopInfo::integer_constant(outer->induction.initial);
      if (
        !row || !initial_index || *initial_index < 0 ||
        *scratch_capacity >
          static_cast<size_t>(std::numeric_limits<int32_t>::max()) ||
        !affine.is_available_at_preheader(
          outer->induction.bound, outer->preheader
        )
      ) {
        continue;
      }
      auto constant_bound =
        AffineLoopInfo::integer_constant(outer->induction.bound);
      if (
        constant_bound &&
        *constant_bound > static_cast<int32_t>(*scratch_capacity)
      )
        continue;
      needs_runtime_guard = !constant_bound;
      if (needs_runtime_guard) {
        auto fallback = clone_loop_nest(
          func, *outer_loop, outer->preheader, outer->preheader, module
        );
        if (!fallback)
          continue;
        fallback_header = *fallback;
        excluded_fallback_headers.insert(fallback_header);
      }
    }

    Op *scratch_alloca = nullptr;
    if (scratch_capacity) {
      scratch_alloca = module->make_op(OpCode::Alloca);
      scratch_alloca->result = module->ctx->make_value<OpResult>(
        I32::get()->array_of(static_cast<int>(*scratch_capacity))->ptr_to(),
        scratch_alloca
      );
      row->preheader->insts.insert(
        std::prev(row->preheader->insts.end()), scratch_alloca
      );
    }

    auto init_header_ptr = std::make_unique<Block>(
      static_cast<int>(func.blocks.size()), outer->header->name + "_init"
    );
    auto init_body_ptr = std::make_unique<Block>(
      static_cast<int>(func.blocks.size() + 1),
      outer->continuation->name + "_init"
    );
    auto schedule_ptr = std::make_unique<Block>(
      static_cast<int>(func.blocks.size() + 2),
      outer->header->name + "_schedule"
    );
    auto *init_header = init_header_ptr.get();
    auto *init_body = init_body_ptr.get();
    auto *schedule = schedule_ptr.get();
    func.blocks.insert(header_position, std::move(init_header_ptr));
    func.blocks.insert(header_position, std::move(init_body_ptr));
    func.blocks.insert(header_position, std::move(schedule_ptr));

    auto *init_phi = module->make_op(OpCode::Phi, PhiPayload{});
    init_phi->result = module->ctx->make_value<OpResult>(
      outer->induction.phi->result->type, init_phi
    );
    init_header->insts.push_back(init_phi);

    auto *init_compare = module->make_op(outer->compare->code);
    append_operands(
      init_compare,
      {replace_value(
         outer->compare->operands[0],
         outer->induction.phi->result,
         init_phi->result
       ),
       replace_value(
         outer->compare->operands[1],
         outer->induction.phi->result,
         init_phi->result
       )}
    );
    init_compare->result = module->ctx->make_value<OpResult>(
      outer->compare->result->type, init_compare
    );
    init_header->insts.push_back(init_compare);

    auto *init_branch = module->make_op(OpCode::Branch);
    append_operands(init_branch, {init_compare->result});
    init_branch->successors = outer->continue_on_true
                                ? std::vector<Block *>{init_body, schedule}
                                : std::vector<Block *>{schedule, init_body};
    init_header->insts.push_back(init_branch);

    auto *init_pointer = module->make_op(OpCode::GetPtr);
    std::vector<Value *> init_pointer_operands;
    if (scratch_alloca) {
      init_pointer_operands = {scratch_alloca->result, init_phi->result};
    } else {
      for (auto *operand : output_getptr->operands) {
        init_pointer_operands.push_back(
          replace_value(operand, outer->induction.phi->result, init_phi->result)
        );
      }
    }
    append_operands(init_pointer, std::move(init_pointer_operands));
    init_pointer->result = module->ctx->make_value<OpResult>(
      output_getptr->result->type, init_pointer
    );
    init_body->insts.push_back(init_pointer);

    auto *init_store = module->make_op(OpCode::Store);
    append_operands(init_store, {initial, init_pointer->result});
    init_body->insts.push_back(init_store);

    auto *init_update = module->make_op(outer->induction.update->code);
    std::vector<Value *> init_update_operands;
    for (auto *operand : outer->induction.update->operands) {
      init_update_operands.push_back(
        replace_value(operand, outer->induction.phi->result, init_phi->result)
      );
    }
    append_operands(init_update, std::move(init_update_operands));
    init_update->result = module->ctx->make_value<OpResult>(
      outer->induction.update->result->type, init_update
    );
    init_body->insts.push_back(init_update);
    auto *init_jump = module->make_op(OpCode::Jump);
    init_jump->successors = {init_header};
    init_body->insts.push_back(init_jump);

    auto &init_incoming = std::get<PhiPayload>(init_phi->payload).incoming;
    init_incoming = {
      {outer->preheader, outer->induction.initial},
      {init_body, init_update->result},
    };
    outer->induction.initial->addUse(init_phi);
    init_update->result->addUse(init_phi);

    auto *schedule_jump = module->make_op(OpCode::Jump);
    schedule_jump->successors = {outer->header};
    schedule->insts.push_back(schedule_jump);

    set_jump_successor(outer->preheader, init_header);
    for (auto &[block, value] :
         std::get<PhiPayload>(outer->induction.phi->payload).incoming) {
      (void)value;
      if (block == outer->preheader)
        block = schedule;
    }

    Op *accumulation_pointer = output_getptr;
    if (scratch_alloca) {
      accumulation_pointer = module->make_op(OpCode::GetPtr);
      append_operands(
        accumulation_pointer,
        {scratch_alloca->result, outer->induction.phi->result}
      );
      accumulation_pointer->result = module->ctx->make_value<OpResult>(
        output_getptr->result->type, accumulation_pointer
      );
      inner->continuation->insts.push_front(accumulation_pointer);
    } else {
      auto pointer_position = std::find(
        outer->latch->insts.begin(), outer->latch->insts.end(), output_getptr
      );
      inner->continuation->insts.splice(
        inner->continuation->insts.begin(),
        outer->latch->insts,
        pointer_position
      );
    }
    auto pointer_in_body = std::find(
      inner->continuation->insts.begin(),
      inner->continuation->insts.end(),
      accumulation_pointer
    );
    auto load_position = std::next(pointer_in_body);
    auto *load = module->make_op(OpCode::Load);
    append_operands(load, {accumulation_pointer->result});
    load->result =
      module->ctx->make_value<OpResult>(reduction_phi->result->type, load);
    inner->continuation->insts.insert(load_position, load);

    auto *store = module->make_op(OpCode::Store);
    append_operands(store, {backedge, accumulation_pointer->result});
    auto update_position = std::find(
      inner->latch->insts.begin(),
      inner->latch->insts.end(),
      inner->induction.update
    );
    inner->latch->insts.insert(update_position, store);

    if (scratch_alloca) {
      auto writeback_preheader_ptr = std::make_unique<Block>(
        static_cast<int>(func.blocks.size()),
        outer->header->name + "_writeback_preheader"
      );
      auto writeback_header_ptr = std::make_unique<Block>(
        static_cast<int>(func.blocks.size() + 1),
        outer->header->name + "_writeback"
      );
      auto writeback_body_ptr = std::make_unique<Block>(
        static_cast<int>(func.blocks.size() + 2),
        outer->continuation->name + "_writeback"
      );
      auto *writeback_preheader = writeback_preheader_ptr.get();
      auto *writeback_header = writeback_header_ptr.get();
      auto *writeback_body = writeback_body_ptr.get();
      func.blocks.insert(exit_position, std::move(writeback_preheader_ptr));
      func.blocks.insert(exit_position, std::move(writeback_header_ptr));
      func.blocks.insert(exit_position, std::move(writeback_body_ptr));

      auto *preheader_jump = module->make_op(OpCode::Jump);
      preheader_jump->successors = {writeback_header};
      writeback_preheader->insts.push_back(preheader_jump);

      auto *writeback_phi = module->make_op(OpCode::Phi, PhiPayload{});
      writeback_phi->result = module->ctx->make_value<OpResult>(
        outer->induction.phi->result->type, writeback_phi
      );
      writeback_header->insts.push_back(writeback_phi);

      auto *writeback_compare = module->make_op(outer->compare->code);
      append_operands(
        writeback_compare,
        {replace_value(
           outer->compare->operands[0],
           outer->induction.phi->result,
           writeback_phi->result
         ),
         replace_value(
           outer->compare->operands[1],
           outer->induction.phi->result,
           writeback_phi->result
         )}
      );
      writeback_compare->result = module->ctx->make_value<OpResult>(
        outer->compare->result->type, writeback_compare
      );
      writeback_header->insts.push_back(writeback_compare);

      auto *writeback_branch = module->make_op(OpCode::Branch);
      append_operands(writeback_branch, {writeback_compare->result});
      writeback_branch->successors =
        outer->continue_on_true
          ? std::vector<Block *>{writeback_body, outer->exit}
          : std::vector<Block *>{outer->exit, writeback_body};
      writeback_header->insts.push_back(writeback_branch);

      auto *scratch_pointer = module->make_op(OpCode::GetPtr);
      append_operands(
        scratch_pointer, {scratch_alloca->result, writeback_phi->result}
      );
      scratch_pointer->result = module->ctx->make_value<OpResult>(
        output_getptr->result->type, scratch_pointer
      );
      writeback_body->insts.push_back(scratch_pointer);

      auto *scratch_load = module->make_op(OpCode::Load);
      append_operands(scratch_load, {scratch_pointer->result});
      scratch_load->result = module->ctx->make_value<OpResult>(
        reduction_phi->result->type, scratch_load
      );
      writeback_body->insts.push_back(scratch_load);

      auto *writeback_pointer = module->make_op(OpCode::GetPtr);
      std::vector<Value *> writeback_pointer_operands;
      for (auto *operand : output_getptr->operands) {
        writeback_pointer_operands.push_back(replace_value(
          operand, outer->induction.phi->result, writeback_phi->result
        ));
      }
      append_operands(writeback_pointer, std::move(writeback_pointer_operands));
      writeback_pointer->result = module->ctx->make_value<OpResult>(
        output_getptr->result->type, writeback_pointer
      );
      writeback_body->insts.push_back(writeback_pointer);

      auto *writeback_store = module->make_op(OpCode::Store);
      append_operands(
        writeback_store, {scratch_load->result, writeback_pointer->result}
      );
      writeback_body->insts.push_back(writeback_store);

      auto *writeback_update = module->make_op(outer->induction.update->code);
      std::vector<Value *> writeback_update_operands;
      for (auto *operand : outer->induction.update->operands) {
        writeback_update_operands.push_back(replace_value(
          operand, outer->induction.phi->result, writeback_phi->result
        ));
      }
      append_operands(writeback_update, std::move(writeback_update_operands));
      writeback_update->result = module->ctx->make_value<OpResult>(
        outer->induction.update->result->type, writeback_update
      );
      writeback_body->insts.push_back(writeback_update);
      auto *writeback_jump = module->make_op(OpCode::Jump);
      writeback_jump->successors = {writeback_header};
      writeback_body->insts.push_back(writeback_jump);

      auto &writeback_incoming =
        std::get<PhiPayload>(writeback_phi->payload).incoming;
      writeback_incoming = {
        {writeback_preheader, outer->induction.initial},
        {writeback_body, writeback_update->result},
      };
      outer->induction.initial->addUse(writeback_phi);
      writeback_update->result->addUse(writeback_phi);

      set_branch_successors(*outer, outer->continuation, writeback_preheader);
    }

    if (needs_runtime_guard) {
      auto *guard_terminator = outer->preheader->insts.back();
      auto *transformed_entry = guard_terminator->successors.front();
      auto *capacity = module->ctx->make_const(
        I32::get(), static_cast<int>(*scratch_capacity)
      );
      auto *guard_compare = module->make_op(OpCode::Le);
      append_operands(guard_compare, {outer->induction.bound, capacity});
      guard_compare->result =
        module->ctx->make_value<OpResult>(Bool::get(), guard_compare);
      outer->preheader->insts.insert(
        std::prev(outer->preheader->insts.end()), guard_compare
      );
      guard_terminator->code = OpCode::Branch;
      append_operands(guard_terminator, {guard_compare->result});
      guard_terminator->successors = {transformed_entry, fallback_header};
    }

    MidIRRewriter rewriter;
    rewriter.set_scope(func);
    rewriter.eraseOp(output_store);
    if (scratch_alloca)
      rewriter.eraseOp(output_getptr);
    rewriter.replace_all_uses_with(reduction_phi->result, load->result);
    rewriter.eraseOp(reduction_phi);
    rewriter.finalize(func);
    rebuild_cfg(func);
    return true;
  }
  return false;
}

auto PolyhedralOpt::interchange(
  LinearFunction &func, const PolyhedralScop &scop
) -> bool {
  const auto &outer = scop.outer_counted;
  const auto &inner = scop.inner_counted;
  if (
    !outer.preheader || !outer.header || !outer.latch || !outer.exit ||
    !inner.preheader || !inner.header || !inner.latch ||
    inner.preheader != outer.continuation || inner.exit != outer.latch ||
    outer.header->insts.empty() || inner.header->insts.empty()
  ) {
    return false;
  }

  if (
    !retarget_phi(
      outer.induction.phi,
      outer.preheader,
      inner.preheader,
      outer.latch,
      inner.latch
    ) ||
    !retarget_phi(
      inner.induction.phi,
      inner.preheader,
      outer.preheader,
      inner.latch,
      outer.latch
    )
  ) {
    return false;
  }

  auto outer_update = std::find(
    outer.latch->insts.begin(), outer.latch->insts.end(), outer.induction.update
  );
  auto inner_update = std::find(
    inner.latch->insts.begin(), inner.latch->insts.end(), inner.induction.update
  );
  if (
    outer_update == outer.latch->insts.end() ||
    inner_update == inner.latch->insts.end()
  ) {
    return false;
  }

  auto outer_insert = std::prev(outer.latch->insts.end());
  auto inner_insert = std::prev(inner.latch->insts.end());
  inner.latch->insts.splice(inner_insert, outer.latch->insts, outer_update);
  outer.latch->insts.splice(outer_insert, inner.latch->insts, inner_update);

  // LICM may have placed pure address calculations in the old inner
  // preheader.  They depend on the old outer induction, which becomes the new
  // inner induction, so execute them at the start of the new inner body.
  auto preheader_terminator = std::prev(inner.preheader->insts.end());
  inner.continuation->insts.splice(
    inner.continuation->insts.begin(),
    inner.preheader->insts,
    inner.preheader->insts.begin(),
    preheader_terminator
  );

  if (
    !set_jump_successor(outer.preheader, inner.header) ||
    !set_branch_successors(inner, inner.preheader, outer.exit) ||
    !set_jump_successor(inner.preheader, outer.header) ||
    !set_branch_successors(outer, inner.continuation, outer.latch) ||
    !set_jump_successor(inner.latch, outer.header) ||
    !set_jump_successor(outer.latch, inner.header)
  ) {
    return false;
  }

  rebuild_cfg(func);
  return true;
}

auto PolyhedralOpt::rebuild_cfg(LinearFunction &func) -> void {
  for (auto &block : func.blocks) {
    block->preds.clear();
    block->succs.clear();
  }
  for (auto &block : func.blocks) {
    if (block->insts.empty())
      continue;
    auto *terminator = block->insts.back();
    if (
      terminator->code != OpCode::Jump && terminator->code != OpCode::Branch
    ) {
      continue;
    }
    block->succs = terminator->successors;
    for (auto *successor : block->succs)
      successor->preds.push_back(block.get());
  }
}

} // namespace exodus::mid_ir::opt
