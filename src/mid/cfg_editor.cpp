#include "cfg_editor.hpp"

#include <algorithm>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace exodus::mid_ir {
namespace {

auto contains_user(const Value *value, const Op *user) -> bool {
  return value && std::find(value->users.begin(), value->users.end(), user) !=
                    value->users.end();
}

auto count_block(const std::vector<Block *> &blocks, const Block *block)
  -> size_t {
  return static_cast<size_t>(std::count(blocks.begin(), blocks.end(), block));
}

auto append_reason(std::string *reason, const std::string &message) -> bool {
  if (reason)
    *reason = message;
  return false;
}

} // namespace

CFGEditor::CFGEditor(MidModule &module, LinearFunction &function)
    : mid_module(module), func(function) {
  BlockId next = func.next_block_id;
  std::unordered_set<BlockId> ids;
  for (const auto &block : func.blocks) {
    if (!ids.insert(block->id).second)
      continue;
    if (block->id == std::numeric_limits<BlockId>::max())
      next = block->id;
    else
      next = std::max(next, block->id + 1);
  }
  func.next_block_id = next;
}

auto CFGEditor::find_block(Block *block) const
  -> std::list<std::unique_ptr<Block>>::iterator {
  return std::find_if(
    func.blocks.begin(),
    func.blocks.end(),
    [block](const std::unique_ptr<Block> &candidate) {
      return candidate.get() == block;
    }
  );
}

auto CFGEditor::owns(Block *block) const -> bool {
  return block != nullptr && find_block(block) != func.blocks.end();
}

auto CFGEditor::owns_op(Op *op) const -> bool {
  if (!op)
    return false;
  return std::any_of(
    mid_module.ops.begin(),
    mid_module.ops.end(),
    [op](const std::unique_ptr<Op> &candidate) { return candidate.get() == op; }
  );
}

auto CFGEditor::op_in_function(Op *op, Block **owner) const -> bool {
  if (!op)
    return false;
  for (const auto &block : func.blocks) {
    if (
      std::find(block->insts.begin(), block->insts.end(), op) ==
      block->insts.end()
    ) {
      continue;
    }
    if (owner)
      *owner = block.get();
    return true;
  }
  return false;
}

auto CFGEditor::check_block_id(BlockId id) const -> bool {
  for (const auto &block : func.blocks) {
    if (block->id == id)
      return true;
  }
  return false;
}

auto CFGEditor::allocate_block_id() -> BlockId {
  BlockId id = func.next_block_id;
  while (check_block_id(id)) {
    if (id == std::numeric_limits<BlockId>::max())
      throw std::overflow_error("mid CFG block id space exhausted");
    ++id;
  }
  if (id == std::numeric_limits<BlockId>::max())
    throw std::overflow_error("mid CFG block id space exhausted");
  func.next_block_id = id + 1;
  return id;
}

auto CFGEditor::create_block(std::string name, Block *before) -> Block * {
  if (before && !owns(before))
    return nullptr;

  auto block = std::make_unique<Block>(allocate_block_id(), std::move(name));
  auto *result = block.get();
  if (!before) {
    func.blocks.push_back(std::move(block));
    return result;
  }

  func.blocks.insert(find_block(before), std::move(block));
  return result;
}

auto CFGEditor::create_block_after(std::string name, Block *after) -> Block * {
  if (!owns(after))
    return nullptr;
  auto *result = create_block(std::move(name));
  if (!result)
    return nullptr;
  auto after_it = find_block(after);
  auto result_it = find_block(result);
  func.blocks.splice(std::next(after_it), func.blocks, result_it);
  return result;
}

auto CFGEditor::is_terminator(const Op *op) const -> bool {
  if (!op)
    return false;
  return op->code == OpCode::Jump || op->code == OpCode::Branch ||
         op->code == OpCode::Ret;
}

auto CFGEditor::detach_op_uses(Op *op) const -> void {
  if (!op)
    return;
  for (auto *operand : op->operands) {
    if (operand)
      operand->rmUse(op);
  }
  if (op->code == OpCode::Phi) {
    auto &payload = std::get<PhiPayload>(op->payload);
    for (const auto &[_, value] : payload.incoming) {
      if (value)
        value->rmUse(op);
    }
  }
  op->operands.clear();
  op->successors.clear();
}

auto CFGEditor::attach_op_uses(Op *op) const -> void {
  if (!op)
    return;
  for (auto *operand : op->operands) {
    if (operand && !contains_user(operand, op))
      operand->addUse(op);
  }
  if (op->code == OpCode::Phi) {
    auto &payload = std::get<PhiPayload>(op->payload);
    for (const auto &[_, value] : payload.incoming) {
      if (value && !contains_user(value, op))
        value->addUse(op);
    }
  }
}

auto CFGEditor::synchronize() -> void {
  for (auto &block : func.blocks) {
    block->preds.clear();
    block->succs.clear();
  }

  for (auto &block : func.blocks) {
    if (block->insts.empty())
      continue;
    auto *terminator = block->insts.back();
    if (!terminator || !is_terminator(terminator))
      continue;
    if (terminator->code != OpCode::Jump && terminator->code != OpCode::Branch)
      continue;
    for (auto *successor : terminator->successors) {
      if (!owns(successor))
        continue;
      block->succs.push_back(successor);
      successor->preds.push_back(block.get());
    }
  }
}

auto CFGEditor::remove_phi_incoming(Op *phi, Block *pred) -> bool {
  if (!phi || phi->code != OpCode::Phi || !pred)
    return false;
  auto &payload = std::get<PhiPayload>(phi->payload);
  bool changed = false;
  for (auto it = payload.incoming.begin(); it != payload.incoming.end();) {
    if (it->first != pred) {
      ++it;
      continue;
    }
    if (it->second)
      it->second->rmUse(phi);
    it = payload.incoming.erase(it);
    changed = true;
  }
  return changed;
}

auto CFGEditor::add_phi_incoming(Op *phi, Block *pred, Value *value) -> bool {
  if (
    !phi || phi->code != OpCode::Phi || !op_in_function(phi) || !owns(pred) ||
    !value
  )
    return false;
  auto &payload = std::get<PhiPayload>(phi->payload);
  auto it = std::find_if(
    payload.incoming.begin(),
    payload.incoming.end(),
    [pred](const auto &incoming) { return incoming.first == pred; }
  );
  if (it != payload.incoming.end())
    return false;
  payload.incoming.push_back({pred, value});
  value->addUse(phi);
  return true;
}

auto CFGEditor::set_phi_incoming(
  Op *phi, std::vector<std::pair<Block *, Value *>> incoming
) -> bool {
  if (!phi || phi->code != OpCode::Phi || !op_in_function(phi))
    return false;
  std::unordered_set<Block *> seen;
  for (const auto &[pred, value] : incoming) {
    if (!owns(pred) || !value || !seen.insert(pred).second)
      return false;
  }

  auto &payload = std::get<PhiPayload>(phi->payload);
  for (const auto &[_, value] : payload.incoming) {
    if (value)
      value->rmUse(phi);
  }
  payload.incoming = std::move(incoming);
  for (const auto &[_, value] : payload.incoming) {
    value->addUse(phi);
  }
  return true;
}

auto CFGEditor::remove_edge(Block *from, Block *to, size_t occurrence) -> bool {
  if (!owns(from) || !owns(to) || from->insts.empty())
    return false;
  auto *terminator = from->insts.back();
  if (
    !terminator ||
    (terminator->code != OpCode::Jump && terminator->code != OpCode::Branch)
  )
    return false;

  size_t seen = 0;
  bool removed = false;
  for (auto it = terminator->successors.begin();
       it != terminator->successors.end();) {
    if (*it != to || (occurrence != all_edges && seen++ != occurrence)) {
      ++it;
      continue;
    }
    it = terminator->successors.erase(it);
    removed = true;
    if (occurrence != all_edges)
      break;
  }
  if (!removed)
    return false;

  synchronize();
  if (count_block(from->succs, to) == 0) {
    for (auto &block : func.blocks) {
      for (auto *op : block->insts) {
        if (op->code != OpCode::Phi)
          break;
        if (block.get() == to)
          remove_phi_incoming(op, from);
      }
    }
  }
  return true;
}

auto CFGEditor::redirect_edge(
  Block *from,
  Block *old_to,
  Block *new_to,
  size_t occurrence,
  const PhiIncoming &incoming
) -> bool {
  if (!owns(from) || !owns(old_to) || !owns(new_to) || from->insts.empty())
    return false;
  auto *terminator = from->insts.back();
  if (
    !terminator ||
    (terminator->code != OpCode::Jump && terminator->code != OpCode::Branch)
  )
    return false;

  for (const auto &[phi, value] : incoming) {
    if (!phi || phi->code != OpCode::Phi || !value || !op_in_function(phi)) {
      return false;
    }
    auto phi_block = std::find_if(
      func.blocks.begin(),
      func.blocks.end(),
      [phi](const std::unique_ptr<Block> &block) {
        return std::find(block->insts.begin(), block->insts.end(), phi) !=
               block->insts.end();
      }
    );
    if (phi_block == func.blocks.end() || phi_block->get() != new_to)
      return false;
  }

  auto tx = begin_transaction();
  size_t seen = 0;
  bool redirected = false;
  for (auto &successor : terminator->successors) {
    if (successor != old_to)
      continue;
    if (occurrence != all_edges && seen++ != occurrence)
      continue;
    successor = new_to;
    redirected = true;
    if (occurrence != all_edges)
      break;
  }
  if (!redirected)
    return false;

  synchronize();
  if (count_block(from->succs, old_to) == 0) {
    for (auto *op : old_to->insts) {
      if (op->code != OpCode::Phi)
        break;
      remove_phi_incoming(op, from);
    }
  }
  for (const auto &[phi, value] : incoming) {
    if (!add_phi_incoming(phi, from, value))
      return false;
  }
  return tx.commit();
}

auto CFGEditor::set_terminator(Block *block, Op *terminator) -> bool {
  if (
    !owns(block) || !terminator || !is_terminator(terminator) ||
    !owns_op(terminator)
  )
    return false;
  if (
    !terminator->successors.empty() &&
    (terminator->code != OpCode::Jump && terminator->code != OpCode::Branch)
  )
    return false;

  Block *current_owner = nullptr;
  if (op_in_function(terminator, &current_owner)) {
    if (current_owner != block || block->insts.back() != terminator)
      return false;
    synchronize();
    return check_local_consistency();
  }
  for (auto *successor : terminator->successors) {
    if (!owns(successor))
      return false;
  }

  auto tx = begin_transaction();

  synchronize();
  auto old_successors = block->succs;
  if (!block->insts.empty() && is_terminator(block->insts.back())) {
    auto *old = block->insts.back();
    detach_op_uses(old);
    block->insts.pop_back();
  }
  attach_op_uses(terminator);
  block->insts.push_back(terminator);
  synchronize();

  for (auto *old_successor : old_successors) {
    if (count_block(block->succs, old_successor) != 0)
      continue;
    for (auto *op : old_successor->insts) {
      if (op->code != OpCode::Phi)
        break;
      remove_phi_incoming(op, block);
    }
  }
  return tx.commit();
}

auto CFGEditor::set_successors(Block *block, std::vector<Block *> successors)
  -> bool {
  if (!owns(block) || block->insts.empty())
    return false;
  auto *terminator = block->insts.back();
  if (
    !terminator ||
    (terminator->code != OpCode::Jump && terminator->code != OpCode::Branch)
  )
    return false;
  for (auto *successor : successors) {
    if (!owns(successor))
      return false;
  }
  if (terminator->code == OpCode::Jump && successors.size() != 1)
    return false;
  if (terminator->code == OpCode::Branch && successors.size() != 2)
    return false;

  auto tx = begin_transaction();
  auto old_successors = terminator->successors;
  terminator->successors = std::move(successors);
  synchronize();
  for (auto *old_successor : old_successors) {
    if (count_block(block->succs, old_successor) != 0)
      continue;
    for (auto *op : old_successor->insts) {
      if (op->code != OpCode::Phi)
        break;
      remove_phi_incoming(op, block);
    }
  }
  return tx.commit();
}

auto CFGEditor::replace_phi_pred(Block *block, Block *old_pred, Block *new_pred)
  -> bool {
  if (!owns(block) || !owns(old_pred) || !owns(new_pred))
    return false;
  bool changed = false;
  for (auto *op : block->insts) {
    if (op->code != OpCode::Phi)
      break;
    auto &incoming = std::get<PhiPayload>(op->payload).incoming;
    const bool has_new = std::any_of(
      incoming.begin(), incoming.end(), [new_pred](const auto &entry) {
        return entry.first == new_pred;
      }
    );
    const bool has_old = std::any_of(
      incoming.begin(), incoming.end(), [old_pred](const auto &entry) {
        return entry.first == old_pred;
      }
    );
    if (has_old && has_new)
      return false;
    for (auto &[pred, _] : incoming) {
      if (pred != old_pred)
        continue;
      pred = new_pred;
      changed = true;
    }
  }
  (void)changed;
  return true;
}

auto CFGEditor::replace_phi_predecessor(
  Block *block, Block *old_pred, Block *new_pred
) -> bool {
  auto tx = begin_transaction();
  if (!replace_phi_pred(block, old_pred, new_pred))
    return false;
  return tx.commit();
}

auto CFGEditor::remove_block(Block *block) -> bool {
  if (!owns(block) || block == func.blocks.front().get())
    return false;

  auto tx = begin_transaction();
  synchronize();
  for (auto *successor : block->succs) {
    for (auto *op : successor->insts) {
      if (op->code != OpCode::Phi)
        break;
      remove_phi_incoming(op, block);
    }
  }

  std::unordered_set<Op *> live_ops;
  for (const auto &candidate : func.blocks) {
    if (candidate.get() == block)
      continue;
    live_ops.insert(candidate->insts.begin(), candidate->insts.end());
  }
  for (auto *op : block->insts) {
    if (!op->result)
      continue;
    for (auto *user : op->result->users) {
      if (live_ops.count(static_cast<Op *>(user)) != 0)
        return false;
    }
  }

  std::unordered_set<Op *> removed_ops(
    block->insts.begin(), block->insts.end()
  );
  for (auto *op : block->insts) {
    if (op->result) {
      for (auto *user : op->result->users) {
        if (removed_ops.count(static_cast<Op *>(user)) == 0)
          return false;
      }
    }
  }

  auto predecessors = block->preds;
  for (auto *pred : predecessors) {
    while (count_block(pred->succs, block) != 0) {
      if (!remove_edge(pred, block, 0))
        return false;
    }
  }

  for (auto *op : block->insts) {
    if (op->result)
      op->result->users.clear();
    detach_op_uses(op);
  }
  block->insts.clear();
  block->preds.clear();
  block->succs.clear();
  auto it = find_block(block);
  if (it == func.blocks.end())
    return false;
  func.blocks.erase(it);
  synchronize();
  return tx.commit();
}

auto CFGEditor::merge_blocks(Block *source, Block *target) -> bool {
  if (
    !owns(source) || !owns(target) || source == target ||
    source->insts.empty() || target->insts.empty()
  )
    return false;
  auto tx = begin_transaction();
  synchronize();
  auto *terminator = source->insts.back();
  if (
    !terminator || terminator->code != OpCode::Jump ||
    terminator->successors.size() != 1 ||
    terminator->successors.front() != target ||
    count_block(target->preds, source) != 1
  )
    return false;
  if (!target->insts.empty() && target->insts.front()->code == OpCode::Phi)
    return false;

  auto target_successors = target->succs;
  for (auto *successor : target_successors) {
    if (!replace_phi_pred(successor, target, source))
      return false;
  }

  detach_op_uses(terminator);
  source->insts.pop_back();
  source->insts.splice(source->insts.end(), target->insts);
  target->preds.clear();
  target->succs.clear();
  auto it = find_block(target);
  if (it == func.blocks.end())
    return false;
  func.blocks.erase(it);
  synchronize();
  return tx.commit();
}

auto CFGEditor::check_local_consistency(std::string *reason) const -> bool {
  std::unordered_set<Block *> owned;
  std::unordered_set<BlockId> ids;
  std::vector<Block *> expected_order;
  std::unordered_map<Block *, std::vector<Block *>> expected_preds;
  for (const auto &block : func.blocks) {
    if (!owned.insert(block.get()).second)
      return append_reason(reason, "duplicate block pointer");
    if (!ids.insert(block->id).second)
      return append_reason(reason, "duplicate BlockId");
    expected_order.push_back(block.get());
    expected_preds[block.get()] = {};
  }

  for (const auto &block : func.blocks) {
    std::vector<Block *> expected_succs;
    if (!block->insts.empty()) {
      auto *terminator = block->insts.back();
      if (!terminator || !owns_op(terminator))
        return append_reason(reason, "block contains an unknown operation");
      if (
        terminator &&
        (terminator->code == OpCode::Jump || terminator->code == OpCode::Branch)
      ) {
        expected_succs = terminator->successors;
      }
    }
    size_t op_index = 0;
    for (auto *op : block->insts) {
      if (!op || !owns_op(op))
        return append_reason(reason, "block contains an unknown operation");
      const bool is_last = ++op_index == block->insts.size();
      if (is_terminator(op) != is_last)
        return append_reason(reason, "terminator is not the last operation");
      if (!is_terminator(op) && !op->successors.empty())
        return append_reason(reason, "non-terminator has successors");
    }
    for (auto *successor : expected_succs) {
      if (!owned.count(successor))
        return append_reason(reason, "terminator points outside function");
      expected_preds[successor].push_back(block.get());
    }
    if (expected_succs.size() != block->succs.size())
      return append_reason(reason, "succ list does not match terminator");
    for (auto *successor : expected_succs) {
      if (
        count_block(block->succs, successor) !=
        count_block(expected_succs, successor)
      )
        return append_reason(reason, "succ list has wrong edge multiplicity");
      if (
        count_block(successor->preds, block.get()) !=
        count_block(expected_succs, successor)
      )
        return append_reason(reason, "pred list has wrong edge multiplicity");
    }
  }

  for (const auto &block : func.blocks) {
    const auto &expected = expected_preds[block.get()];
    if (expected.size() != block->preds.size())
      return append_reason(reason, "pred list does not match terminators");
    for (auto *pred : expected) {
      if (count_block(block->preds, pred) != count_block(expected, pred))
        return append_reason(reason, "pred list has stale or missing edge");
    }
  }

  for (const auto &block : func.blocks) {
    std::unordered_set<Block *> unique_preds(
      block->preds.begin(), block->preds.end()
    );
    for (auto *op : block->insts) {
      if (op->result && op->result->creator != op)
        return append_reason(reason, "result creator does not match op");
      for (auto *operand : op->operands) {
        if (operand && !contains_user(operand, op))
          return append_reason(reason, "operand is missing use-def link");
      }
      if (op->code != OpCode::Phi)
        continue;
      const auto &incoming = std::get<PhiPayload>(op->payload).incoming;
      std::unordered_set<Block *> incoming_preds;
      for (const auto &[pred, value] : incoming) {
        if (!pred || !owned.count(pred) || count_block(block->preds, pred) == 0)
          return append_reason(reason, "Phi incoming has no matching edge");
        if (!incoming_preds.insert(pred).second)
          return append_reason(reason, "Phi has duplicate incoming block");
        if (!value || !contains_user(value, op))
          return append_reason(reason, "Phi is missing use-def link");
      }
      if (incoming_preds.size() != unique_preds.size())
        return append_reason(reason, "Phi incoming does not match preds");
    }
  }
  return true;
}

CFGEditTransaction::CFGEditTransaction(CFGEditor &cfg_editor)
    : editor(&cfg_editor) {
  ++editor->transaction_depth;
  capture();
}

CFGEditTransaction::~CFGEditTransaction() {
  if (active())
    rollback();
}

CFGEditTransaction::CFGEditTransaction(CFGEditTransaction &&other) noexcept
    : editor(other.editor), is_active(other.is_active),
      next_block_id(other.next_block_id),
      block_order(std::move(other.block_order)),
      blocks(std::move(other.blocks)), ops(std::move(other.ops)),
      values(std::move(other.values)) {
  other.editor = nullptr;
  other.is_active = false;
}

auto CFGEditTransaction::operator=(CFGEditTransaction &&other) noexcept
  -> CFGEditTransaction & {
  if (this == &other)
    return *this;
  if (active())
    rollback();
  editor = other.editor;
  is_active = other.is_active;
  next_block_id = other.next_block_id;
  block_order = std::move(other.block_order);
  blocks = std::move(other.blocks);
  ops = std::move(other.ops);
  values = std::move(other.values);
  other.editor = nullptr;
  other.is_active = false;
  return *this;
}

auto CFGEditTransaction::capture() -> void {
  if (!editor)
    return;
  next_block_id = editor->func.next_block_id;
  for (const auto &block : editor->func.blocks) {
    block_order.push_back(block.get());
    blocks.push_back(
      {block.get(), block->id, block->name, {}, block->preds, block->succs}
    );
    auto &state = blocks.back();
    state.insts.assign(block->insts.begin(), block->insts.end());
    for (auto *op : state.insts) {
      ops.push_back(
        {op, op->code, op->operands, op->successors, op->result, op->payload}
      );
    }
  }
  if (!editor->mid_module.ctx)
    return;
  for (const auto &value : editor->mid_module.ctx->values) {
    values.push_back({value.get(), value->users});
  }
}

auto CFGEditTransaction::commit() -> bool {
  if (!active())
    return false;
  if (editor->transaction_depth == 1) {
    std::string reason;
    if (!editor->check_local_consistency(&reason)) {
      rollback();
      return false;
    }
  }
  --editor->transaction_depth;
  is_active = false;
  editor = nullptr;
  return true;
}

auto CFGEditTransaction::rollback() -> void {
  if (!active())
    return;
  editor->restore_transaction(*this);
  --editor->transaction_depth;
  is_active = false;
  editor = nullptr;
}

auto CFGEditor::capture_transaction(CFGEditTransaction &transaction) -> void {
  transaction.capture();
}

auto CFGEditor::restore_transaction(CFGEditTransaction &transaction) -> void {
  std::unordered_set<Block *> original(
    transaction.block_order.begin(), transaction.block_order.end()
  );
  for (auto it = func.blocks.begin(); it != func.blocks.end();) {
    if (original.count(it->get()) != 0) {
      ++it;
      continue;
    }
    it = func.blocks.erase(it);
  }

  auto position = func.blocks.begin();
  for (auto *block : transaction.block_order) {
    auto it = find_block(block);
    if (it != func.blocks.end()) {
      func.blocks.splice(position, func.blocks, it);
      ++position;
    }
  }

  for (const auto &state : transaction.blocks) {
    state.block->id = state.id;
    state.block->name = state.name;
    state.block->insts.clear();
    state.block->insts.insert(
      state.block->insts.end(), state.insts.begin(), state.insts.end()
    );
    state.block->preds = state.preds;
    state.block->succs = state.succs;
  }
  for (const auto &state : transaction.ops) {
    state.op->code = state.code;
    state.op->operands = state.operands;
    state.op->successors = state.successors;
    state.op->result = state.result;
    state.op->payload = state.payload;
  }
  std::unordered_set<Value *> captured_values;
  for (const auto &state : transaction.values) {
    state.value->users = state.users;
    captured_values.insert(state.value);
  }
  if (mid_module.ctx) {
    for (const auto &value : mid_module.ctx->values) {
      if (!captured_values.count(value.get()))
        value->users.clear();
    }
  }
  func.next_block_id = transaction.next_block_id;
}

} // namespace exodus::mid_ir
