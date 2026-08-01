#pragma once

#include "ir.hpp"
#include <cstddef>
#include <functional>
#include <list>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace exodus::mid_ir {

class CFGEditor;

// A transaction records shallow IR state and the use lists it touches.  It
// does not clone blocks, operations, or values, so pointers inside the IR
// remain stable while an edit is being attempted.  New arena objects remain
// allocated after rollback, but are detached from the restored function and
// their use lists are cleared.
class CFGEditTransaction {
public:
  explicit CFGEditTransaction(CFGEditor &editor);
  ~CFGEditTransaction();

  CFGEditTransaction(const CFGEditTransaction &) = delete;
  auto operator=(const CFGEditTransaction &) -> CFGEditTransaction & = delete;

  CFGEditTransaction(CFGEditTransaction &&other) noexcept;
  auto operator=(CFGEditTransaction &&other) noexcept -> CFGEditTransaction &;

  // Returns false and rolls back when the resulting local CFG is invalid.
  auto commit() -> bool;
  auto rollback() -> void;
  auto active() const -> bool { return editor != nullptr && is_active; }

private:
  friend class CFGEditor;

  struct BlockState {
    Block *block = nullptr;
    BlockId id = 0;
    std::string name;
    std::vector<Op *> insts;
    std::vector<Block *> preds;
    std::vector<Block *> succs;
  };

  struct OpState {
    Op *op = nullptr;
    OpCode code = OpCode::Ret;
    std::vector<Value *> operands;
    std::vector<Block *> successors;
    OpResult *result = nullptr;
    Op::Payload payload;
  };

  struct ValueState {
    Value *value = nullptr;
    std::list<OpBase *> users;
  };

  CFGEditor *editor = nullptr;
  bool is_active = true;
  BlockId next_block_id = 0;
  std::vector<Block *> block_order;
  std::vector<BlockState> blocks;
  std::vector<OpState> ops;
  std::vector<ValueState> values;

  auto capture() -> void;
};

class CFGEditor {
public:
  static constexpr size_t all_edges = static_cast<size_t>(-1);

  CFGEditor(MidModule &module, LinearFunction &function);

  auto create_block(std::string name, Block *before = nullptr) -> Block *;
  auto create_block_after(std::string name, Block *after) -> Block *;
  auto remove_block(Block *block) -> bool;

  // Redirect one matching edge, or all matching edges when occurrence is
  // all_edges.  Phi incoming values for the new edge may be supplied by Op.
  // Existing incoming values for an edge that disappeared are removed.
  using PhiIncoming = std::unordered_map<Op *, Value *>;
  auto redirect_edge(
    Block *from,
    Block *old_to,
    Block *new_to,
    size_t occurrence = all_edges,
    const PhiIncoming &incoming = {}
  ) -> bool;

  // Replace or append the block terminator and update CFG predecessor and
  // successor vectors.  The new terminator must already be owned by module.
  auto set_terminator(Block *block, Op *terminator) -> bool;

  // Replace all successors of an existing terminator.  Phi incoming values
  // are not guessed; callers must update them through the Phi APIs in the
  // same edit transaction.
  auto set_successors(Block *block, std::vector<Block *> successors) -> bool;

  auto add_phi_incoming(Op *phi, Block *pred, Value *value) -> bool;
  auto remove_phi_incoming(Op *phi, Block *pred) -> bool;
  auto
  set_phi_incoming(Op *phi, std::vector<std::pair<Block *, Value *>> incoming)
    -> bool;
  auto replace_phi_predecessor(Block *block, Block *old_pred, Block *new_pred)
    -> bool;

  auto merge_blocks(Block *source, Block *target) -> bool;

  // This is the one compatibility bridge for legacy construction code.  New
  // transforms should use the edge-editing methods above instead of rebuilding
  // CFG state locally.
  auto synchronize() -> void;
  auto check_local_consistency(std::string *reason = nullptr) const -> bool;

  auto owns(Block *block) const -> bool;
  auto allocate_block_id() -> BlockId;
  auto check_block_id(BlockId id) const -> bool;

  auto begin_transaction() -> CFGEditTransaction {
    return CFGEditTransaction(*this);
  }

  auto function() -> LinearFunction & { return func; }
  auto module() -> MidModule & { return mid_module; }

private:
  friend class CFGEditTransaction;

  MidModule &mid_module;
  LinearFunction &func;
  size_t transaction_depth = 0;

  auto find_block(Block *block) const
    -> std::list<std::unique_ptr<Block>>::iterator;
  auto owns_op(Op *op) const -> bool;
  auto op_in_function(Op *op, Block **owner = nullptr) const -> bool;
  auto is_terminator(const Op *op) const -> bool;
  auto detach_op_uses(Op *op) const -> void;
  auto attach_op_uses(Op *op) const -> void;
  auto remove_edge(Block *from, Block *to, size_t occurrence) -> bool;
  auto replace_phi_pred(Block *block, Block *old_pred, Block *new_pred) -> bool;
  auto capture_transaction(CFGEditTransaction &transaction) -> void;
  auto restore_transaction(CFGEditTransaction &transaction) -> void;
};

} // namespace exodus::mid_ir
