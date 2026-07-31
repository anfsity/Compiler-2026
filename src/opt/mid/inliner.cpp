#include "inliner.hpp"

#include "../../mid/dom.hpp"
#include "../../mid/getptr.hpp"
#include "../../mid/loop.hpp"
#include "../../mid/memory.hpp"
#include "../../mid/rewriter.hpp"
#include <algorithm>
#include <iterator>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace exodus::mid_ir::opt {
namespace {

auto is_terminator(OpCode code) -> bool {
  return code == OpCode::Ret || code == OpCode::Jump || code == OpCode::Branch;
}

auto function_op_count(const LinearFunction &func) -> size_t {
  size_t count = 0;
  for (const auto &block : func.blocks)
    count += block->insts.size();
  return count;
}

} // namespace

auto Inliner::run(
  LinearFunction &func, exodus::opt::LinearFunctionAnalysisManager &am
) -> exodus::opt::PreservedAnalysis {
  if (func.is_decl || func.blocks.empty())
    return exodus::opt::PreservedAnalysis::all();

  rebuild_call_graph();
  auto &loop_info = am.get_result<LoopAnalysis>(func);

  for (auto &block : func.blocks) {
    auto loop = loop_info.get_loop_for(block.get());
    auto loop_depth = loop ? loop->get_depth() : 0;
    for (auto it = block->insts.begin(); it != block->insts.end(); ++it) {
      Op *call = *it;
      if (call->code != OpCode::Call)
        continue;
      const auto &payload = std::get<CallPayload>(call->payload);
      auto callee_it = functions.find(payload.func_name);
      if (callee_it == functions.end())
        continue;
      LinearFunction *callee = callee_it->second;
      if (callee->is_decl || callee->blocks.empty())
        continue;
      auto callee_cost = cost(*callee);
      if (
        !should_inline(func, *callee, *call, callee_cost, loop_depth) ||
        !validate_callee(*callee, *call)
      ) {
        continue;
      }

      inline_call(func, *block, it, *callee);
      return exodus::opt::PreservedAnalysis::none();
    }
  }

  return exodus::opt::PreservedAnalysis::all();
}

auto Inliner::rebuild_call_graph() -> void {
  functions.clear();
  call_counts.clear();
  edges.clear();
  recursive_functions.clear();

  for (auto &function : module->functions) {
    functions[function->name] = function.get();
    edges[function.get()];
  }

  for (auto &function : module->functions) {
    for (const auto &block : function->blocks) {
      for (auto *op : block->insts) {
        if (op->code != OpCode::Call)
          continue;
        const auto &payload = std::get<CallPayload>(op->payload);
        auto callee = functions.find(payload.func_name);
        if (callee == functions.end() || callee->second->is_decl)
          continue;
        edges[function.get()].insert(callee->second);
        ++call_counts[callee->second];
      }
    }
  }

  for (auto &function : module->functions) {
    LinearFunction *root = function.get();
    std::vector<LinearFunction *> worklist(
      edges[root].begin(), edges[root].end()
    );
    std::unordered_set<LinearFunction *> visited;
    while (!worklist.empty()) {
      LinearFunction *current = worklist.back();
      worklist.pop_back();
      if (current == root) {
        recursive_functions.insert(root);
        break;
      }
      if (!visited.insert(current).second)
        continue;
      const auto &successors = edges[current];
      worklist.insert(worklist.end(), successors.begin(), successors.end());
    }
  }
}

auto Inliner::cost(LinearFunction &func) const -> Cost {
  Cost result;
  result.blocks = func.blocks.size();
  for (const auto &block : func.blocks) {
    for (auto *op : block->insts) {
      ++result.ops;
      switch (op->code) {
      case OpCode::Call: {
        ++result.calls;
        const auto &payload = std::get<CallPayload>(op->payload);
        auto callee = functions.find(payload.func_name);
        if (callee == functions.end() || callee->second->is_decl)
          ++result.external_calls;
        break;
      }
      case OpCode::Store:
      case OpCode::Memset:
        ++result.writes;
        break;
      case OpCode::Alloca:
        ++result.allocas;
        break;
      case OpCode::Ret:
        ++result.returns;
        break;
      default:
        break;
      }
    }
  }
  DomTree dom;
  dom.compute(func);
  LoopInfo loop_info;
  loop_info.compute(func, dom);
  auto loops = loop_info.get_loops();
  result.loops = loops.size();
  for (auto *loop : loops)
    result.max_loop_depth = std::max(result.max_loop_depth, loop->get_depth());
  return result;
}

auto Inliner::should_inline(
  const LinearFunction &caller,
  LinearFunction &callee,
  const Op &call,
  const Cost &callee_cost,
  unsigned loop_depth
) const -> bool {
  if (
    &caller == &callee || callee.is_decl || callee.no_inline ||
    callee.blocks.empty() ||
    recursive_functions.count(const_cast<LinearFunction *>(&callee)) != 0 ||
    callee_cost.blocks > 20 || callee_cost.ops > 120 ||
    callee_cost.returns == 0 || callee_cost.returns > 8 ||
    function_op_count(caller) + callee_cost.ops > 700
  ) {
    return false;
  }

  // A pure, single-return loop usually amortizes its call overhead across its
  // iterations. Without a trip-count or argument-specialization proof, cloning
  // it into a nested caller mainly extends live ranges through the combined
  // loop nest. Tail-recursion elimination is different: inlining then removes
  // a structured recursive boundary, so retain the dedicated cost path below.
  if (
    !callee.tail_recursion_eliminated && callee_cost.loops != 0 &&
    callee_cost.returns == 1 && callee_cost.writes == 0 &&
    callee_cost.calls == 0 &&
    static_cast<size_t>(loop_depth) + callee_cost.max_loop_depth > 2
  ) {
    return false;
  }

  size_t score = callee_cost.ops + callee_cost.blocks * 2 +
                 callee_cost.calls * 10 + callee_cost.writes * 2 +
                 callee_cost.allocas * 6 + callee_cost.loops * 28 +
                 callee_cost.max_loop_depth * 8;
  if (callee_cost.calls == 0 && exposes_noalias_specialization(callee, call)) {
    const auto loop_penalty =
      callee_cost.loops * 28 + callee_cost.max_loop_depth * 8;
    score -= std::min(score, loop_penalty);
  }
  size_t threshold = 52;
  if (loop_depth > 0)
    threshold += 64 + std::min<unsigned>(loop_depth - 1, 2) * 12;

  auto calls = call_counts.find(const_cast<LinearFunction *>(&callee));
  auto call_count = calls == call_counts.end() ? 0 : calls->second;
  if (call_count == 1)
    threshold += 24;
  else if (call_count == 2)
    threshold += 12;
  else if (call_count > 4)
    threshold = threshold > 24 ? threshold - 24 : 0;
  if (call_count > 1 && callee_cost.loops > 1)
    threshold = threshold > 24 ? threshold - 24 : 0;

  if (callee.tail_recursion_eliminated)
    threshold += 24;
  if (callee_cost.calls == 0)
    threshold += 12;
  if (callee_cost.writes == 0 && callee_cost.external_calls == 0)
    threshold += 12;
  if (callee_cost.external_calls != 0)
    threshold = threshold > 24 ? threshold - 24 : 0;

  return score <= threshold;
}

auto Inliner::exposes_noalias_specialization(
  LinearFunction &callee, const Op &call
) const -> bool {
  if (call.operands.size() != callee.args.size())
    return false;

  std::unordered_map<Value *, size_t> formal_indices;
  for (size_t index = 0; index < callee.args.size(); ++index) {
    if (
      callee.args[index] && callee.args[index]->type &&
      callee.args[index]->type->is_ptr()
    )
      formal_indices[callee.args[index]] = index;
  }
  if (formal_indices.size() < 2)
    return false;

  DomTree dom;
  dom.compute(callee);
  LoopInfo loops;
  loops.compute(callee, dom);
  BasicAliasAnalysis alias;
  struct Access {
    size_t formal = 0;
    Loop *loop = nullptr;
  };
  std::vector<Access> reads;
  std::vector<Access> writes;

  for (const auto &block : callee.blocks) {
    auto *loop = loops.get_loop_for(block.get());
    for (auto *op : block->insts) {
      if (op->code == OpCode::Call)
        return false;
      if (op->code == OpCode::GetPtr) {
        auto plan = mid_ir::analyze_getptr(*op);
        if (!plan.valid || plan.reads_memory)
          return false;
        continue;
      }
      if (
        op->code != OpCode::Load && op->code != OpCode::Store &&
        op->code != OpCode::Memset
      ) {
        continue;
      }

      auto location = alias.get_location(*op);
      if (!location || !location->root)
        return false;
      auto formal = formal_indices.find(location->root);
      if (formal == formal_indices.end() || !loop)
        continue;
      if (op->code == OpCode::Load)
        reads.push_back({formal->second, loop});
      else
        writes.push_back({formal->second, loop});
    }
  }

  std::vector<std::pair<size_t, size_t>> alias_pairs;
  for (const auto &read : reads) {
    for (const auto &write : writes) {
      if (read.loop == write.loop && read.formal != write.formal)
        alias_pairs.push_back({read.formal, write.formal});
    }
  }
  std::sort(alias_pairs.begin(), alias_pairs.end());
  alias_pairs.erase(
    std::unique(alias_pairs.begin(), alias_pairs.end()), alias_pairs.end()
  );
  if (alias_pairs.empty())
    return false;

  std::vector<MemoryLocation> actual_locations(callee.args.size());
  for (const auto &[formal, index] : formal_indices) {
    (void)formal;
    if (
      !call.operands[index] || !call.operands[index]->type ||
      !call.operands[index]->type->is_ptr()
    )
      return false;
    actual_locations[index] = alias.get_location(call.operands[index]);
    if (!actual_locations[index].root)
      return false;
  }
  return std::all_of(
    alias_pairs.begin(), alias_pairs.end(), [&](const auto &pair) {
      return alias.alias(
               actual_locations[pair.first], actual_locations[pair.second]
             ) == AliasResult::NoAlias;
    }
  );
}

auto Inliner::validate_callee(
  const LinearFunction &callee, const Op &call
) const -> bool {
  if (
    !callee.type || !callee.type->is_func() ||
    call.operands.size() != callee.args.size()
  )
    return false;

  auto function_type = std::static_pointer_cast<Func>(callee.type);
  if (
    !function_type->ret_type ||
    function_type->params.size() != callee.args.size() ||
    (call.result && !call.result->type)
  ) {
    return false;
  }
  for (size_t i = 0; i < callee.args.size(); ++i) {
    if (
      !callee.args[i] || !call.operands[i] || !callee.args[i]->type ||
      !call.operands[i]->type ||
      callee.args[i]->type != function_type->params[i] ||
      call.operands[i]->type != function_type->params[i]
    ) {
      return false;
    }
  }
  bool returns_void = function_type->ret_type->is_void();
  bool call_has_value = call.result && !call.result->type->is_void();
  if (
    (returns_void && call_has_value) ||
    (!returns_void && call.result &&
     call.result->type != function_type->ret_type)
  ) {
    return false;
  }

  std::unordered_set<const Block *> blocks;
  std::unordered_set<const Op *> ops;
  std::unordered_set<const Value *> arguments(
    callee.args.begin(), callee.args.end()
  );
  for (const auto &block : callee.blocks) {
    blocks.insert(block.get());
    if (block->insts.empty() || !is_terminator(block->insts.back()->code))
      return false;
    for (auto *op : block->insts)
      ops.insert(op);
  }

  auto valid_value = [&](const Value *value) {
    if (!value)
      return false;
    if (
      value->kind == ValueKind::Constant || value->kind == ValueKind::GlobalVar
    ) {
      return true;
    }
    if (value->kind == ValueKind::Argument)
      return arguments.count(value) != 0;
    auto *result = static_cast<const OpResult *>(value);
    return result->creator &&
           ops.count(static_cast<const Op *>(result->creator)) != 0;
  };

  for (const auto &block : callee.blocks) {
    size_t index = 0;
    for (auto *op : block->insts) {
      bool last = ++index == block->insts.size();
      if (is_terminator(op->code) != last)
        return false;
      for (auto *operand : op->operands) {
        if (!valid_value(operand))
          return false;
      }
      for (auto *successor : op->successors) {
        if (blocks.count(successor) == 0)
          return false;
      }

      if (
        (op->code == OpCode::Jump && op->successors.size() != 1) ||
        (op->code == OpCode::Branch &&
         (op->successors.size() != 2 || op->operands.size() != 1)) ||
        (op->code == OpCode::Ret &&
         (op->operands.size() != (returns_void ? 0u : 1u)))
      ) {
        return false;
      }

      if (
        op->code == OpCode::Ret && !returns_void &&
        (!op->operands.front() ||
         op->operands.front()->type != function_type->ret_type)
      ) {
        return false;
      }

      if (op->code == OpCode::Phi) {
        const auto &payload = std::get<PhiPayload>(op->payload);
        if (payload.incoming.empty())
          return false;
        for (const auto &[pred, value] : payload.incoming) {
          if (blocks.count(pred) == 0 || !valid_value(value))
            return false;
        }
      }
    }
  }
  return true;
}

auto Inliner::inline_call(
  LinearFunction &caller,
  Block &call_block,
  std::list<Op *>::iterator call_it,
  LinearFunction &callee
) -> void {
  Op *call = *call_it;
  auto continuation = std::make_unique<Block>(
    0,
    caller.name + "_inline_" + callee.name + "_continuation_" +
      std::to_string(inline_serial)
  );
  Block *continuation_ptr = continuation.get();

  auto call_block_it = std::find_if(
    caller.blocks.begin(),
    caller.blocks.end(),
    [&](const std::unique_ptr<Block> &block) {
      return block.get() == &call_block;
    }
  );
  auto continuation_it =
    caller.blocks.insert(std::next(call_block_it), std::move(continuation));

  auto after_call = std::next(call_it);
  continuation_ptr->insts.splice(
    continuation_ptr->insts.end(),
    call_block.insts,
    after_call,
    call_block.insts.end()
  );
  call_block.insts.erase(call_it);

  // The original outgoing edges now leave the continuation block.  Update
  // existing Phi predecessor labels before connecting the call block to the
  // cloned callee entry.
  for (auto &block : caller.blocks) {
    for (auto *op : block->insts) {
      if (op->code != OpCode::Phi)
        break;
      auto &payload = std::get<PhiPayload>(op->payload);
      for (auto &[pred, value] : payload.incoming) {
        (void)value;
        if (pred == &call_block)
          pred = continuation_ptr;
      }
    }
  }

  std::unordered_map<const Block *, Block *> block_map;
  for (const auto &old_block : callee.blocks) {
    auto cloned_block = std::make_unique<Block>(
      0,
      caller.name + "_inline_" + callee.name + "_" +
        std::to_string(inline_serial) + "_" + old_block->name
    );
    block_map[old_block.get()] = cloned_block.get();
    caller.blocks.insert(continuation_it, std::move(cloned_block));
  }

  std::unordered_map<const Value *, Value *> value_map;
  for (size_t i = 0; i < callee.args.size(); ++i)
    value_map[callee.args[i]] = call->operands[i];

  std::unordered_map<const Op *, Op *> op_map;
  for (const auto &old_block : callee.blocks) {
    Block *cloned_block = block_map.at(old_block.get());
    for (auto *old_op : old_block->insts) {
      if (old_op->code == OpCode::Ret)
        continue;
      Op *cloned_op = nullptr;
      if (old_op->code == OpCode::Call) {
        cloned_op =
          module->make_op(OpCode::Call, std::get<CallPayload>(old_op->payload));
      } else if (old_op->code == OpCode::Phi) {
        cloned_op = module->make_op(OpCode::Phi, PhiPayload{});
      } else {
        cloned_op = module->make_op(old_op->code, old_op->payload);
      }
      if (old_op->result) {
        cloned_op->result =
          module->ctx->make_value<OpResult>(old_op->result->type, cloned_op);
        value_map[old_op->result] = cloned_op->result;
      }
      op_map[old_op] = cloned_op;
      cloned_block->insts.push_back(cloned_op);
    }
  }

  auto map_value = [&](Value *old_value) -> Value * {
    auto mapped = value_map.find(old_value);
    return mapped == value_map.end() ? old_value : mapped->second;
  };

  for (const auto &old_block : callee.blocks) {
    for (auto *old_op : old_block->insts) {
      if (old_op->code == OpCode::Ret)
        continue;
      Op *cloned_op = op_map.at(old_op);
      for (auto *operand : old_op->operands) {
        Value *mapped = map_value(operand);
        cloned_op->operands.push_back(mapped);
        mapped->addUse(cloned_op);
      }
      for (auto *successor : old_op->successors)
        cloned_op->successors.push_back(block_map.at(successor));
      if (old_op->code == OpCode::Phi) {
        auto &cloned_payload = std::get<PhiPayload>(cloned_op->payload);
        for (const auto &[pred, value] :
             std::get<PhiPayload>(old_op->payload).incoming) {
          Value *mapped = map_value(value);
          cloned_payload.incoming.push_back({block_map.at(pred), mapped});
          mapped->addUse(cloned_op);
        }
      }
    }
  }

  std::vector<std::pair<Block *, Value *>> returns;
  for (const auto &old_block : callee.blocks) {
    Op *ret = old_block->insts.back();
    if (ret->code != OpCode::Ret)
      continue;
    Block *cloned_block = block_map.at(old_block.get());
    Value *return_value =
      ret->operands.empty() ? nullptr : map_value(ret->operands.front());
    returns.push_back({cloned_block, return_value});
    auto *jump = module->make_op(OpCode::Jump);
    jump->successors.push_back(continuation_ptr);
    cloned_block->insts.push_back(jump);
  }

  auto *entry_jump = module->make_op(OpCode::Jump);
  entry_jump->successors.push_back(block_map.at(callee.blocks.front().get()));
  call_block.insts.push_back(entry_jump);

  if (call->result && !call->result->type->is_void()) {
    Value *replacement = nullptr;
    if (returns.size() == 1) {
      replacement = returns.front().second;
    } else {
      auto *phi = module->make_op(OpCode::Phi, PhiPayload{});
      phi->result = module->ctx->make_value<OpResult>(call->result->type, phi);
      auto &payload = std::get<PhiPayload>(phi->payload);
      for (const auto &[pred, value] : returns) {
        payload.incoming.push_back({pred, value});
        value->addUse(phi);
      }
      continuation_ptr->insts.push_front(phi);
      replacement = phi->result;
    }
    MidIRRewriter rewriter;
    rewriter.set_scope(caller);
    rewriter.replace_all_uses_with(call->result, replacement);
  }

  for (auto *operand : call->operands)
    operand->rmUse(call);

  ++inline_serial;
  renumber_blocks(caller);
  rebuild_cfg(caller);
}

auto Inliner::rebuild_cfg(LinearFunction &func) -> void {
  for (auto &block : func.blocks) {
    block->preds.clear();
    block->succs.clear();
  }
  for (auto &block : func.blocks) {
    if (block->insts.empty())
      continue;
    Op *terminator = block->insts.back();
    if (terminator->code != OpCode::Jump && terminator->code != OpCode::Branch)
      continue;
    for (auto *successor : terminator->successors) {
      block->succs.push_back(successor);
      successor->preds.push_back(block.get());
    }
  }
}

auto Inliner::renumber_blocks(LinearFunction &func) -> void {
  int id = 0;
  for (auto &block : func.blocks)
    block->id = id++;
}

} // namespace exodus::mid_ir::opt
