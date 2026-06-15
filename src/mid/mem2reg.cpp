#include "mem2reg.hpp"

namespace exodus::mid_ir {

// TODO: 活跃变量分析
// TODO: pured SSA
auto Mem2Reg::run(
  LinearFunction &func, exodus::opt::LinearFunctionAnalysisManager &am
) -> exodus::opt::PreservedAnalysis {
  auto &dom = am.getResult<DominanceAnalysis>(func);

  auto allocas = collect_promotable_allocas(func);
  if (allocas.empty())
    return exodus::opt::PreservedAnalysis::all();

  // Pre-calculate alloca stores to avoid scanning the function for each
  // alloca
  std::unordered_map<Op *, std::vector<Block *>> alloca_stores;
  for (auto &b : func.blocks) {
    for (auto *op : b->insts) {
      if (op->code == OpCode::Store) {
        Value *addr = op->operands[1];

        if (addr->kind == ValueKind::OpResult) {
          auto *creator = static_cast<OpResult *>(addr)->creator;
          alloca_stores[static_cast<Op *>(creator)].push_back(b.get());
        }
      }
    }
  }

  for (auto *alloca : allocas) {
    insert_phi(func, dom, alloca, alloca_stores[alloca]);
  }

  std::unordered_map<Op *, std::stack<Value *>> stacks;
  for (auto *alloca : allocas) {
    auto target_type =
      std::static_pointer_cast<Ptr>(alloca->result->type)->target;
    stacks[alloca].push(module->ctx->make_zero(target_type));
  }

  MidIRRewriter rewriter;
  rename(func.blocks.front().get(), dom, stacks, rewriter);
  cleanup(func, allocas, rewriter);

  return exodus::opt::PreservedAnalysis::none();
}

auto Mem2Reg::collect_promotable_allocas(LinearFunction &func)
  -> std::vector<Op *> {
  std::vector<Op *> allocas;

  auto is_promotable_type = [](std::shared_ptr<Type> &type) -> bool {
    if (!type->is_ptr())
      return false;
    auto target = std::static_pointer_cast<Ptr>(type)->target;
    return target->is_i32() || target->is_f32();
  };

  auto is_alloca_promotable = [&](Op *alloca_op) -> bool {
    if (!is_promotable_type(alloca_op->result->type))
      return false;

    for (auto *user : alloca_op->result->users) {
      auto *user_op = static_cast<Op *>(user);
      bool is_load = (user_op->code == OpCode::Load);
      bool is_store_to =
        (user_op->code == OpCode::Store &&
         user_op->operands[1] == alloca_op->result);

      if (!is_load && !is_store_to)
        return false;
    }
    return true;
  };

  for (auto &b : func.blocks) {
    for (auto *op : b->insts) {
      if (op->code != OpCode::Alloca)
        continue;

      if (is_alloca_promotable(op)) {
        allocas.push_back(op);
      }
    }
  }

  return allocas;
}

auto Mem2Reg::insert_phi(
  [[maybe_unused]] LinearFunction &func,
  DomTree &dom,
  Op *alloca,
  const std::vector<Block *> &stores
) -> void {
  int num_blocks = static_cast<int>(func.blocks.size());
  std::vector<bool> has_phi(num_blocks, false);
  std::vector<bool> processed(num_blocks, false);
  std::vector<Block *> worklist;

  for (Block *b : stores) {
    int idx = b->id;
    if (!processed[idx]) {
      processed[idx] = true;
      worklist.push_back(b);
    }
  }

  auto target_type =
    std::static_pointer_cast<Ptr>(alloca->result->type)->target;

  for (size_t i = 0; i < worklist.size(); ++i) {
    Block *b = worklist[i];
    for (Block *df_block : dom.get_df(b)) {
      int df_idx = df_block->id;
      if (!has_phi[df_idx]) {
        auto *phi = module->make_op(OpCode::Phi, PhiPayload{});
        phi->result = module->ctx->make_value<OpResult>(target_type, phi);
        df_block->insts.push_front(phi);

        has_phi[df_idx] = true;
        phi2alloca[phi] = alloca;
        alloca2phis[alloca].push_back(phi);

        if (!processed[df_idx]) {
          processed[df_idx] = true;
          worklist.push_back(df_block);
        }
      }
    }
  }
}

auto Mem2Reg::rename(
  Block *b,
  DomTree &dom,
  std::unordered_map<Op *, std::stack<Value *>> &stacks,
  MidIRRewriter &rewriter
) -> void {
  std::unordered_map<Op *, int> push_count;

  for (auto *op : b->insts) {
    if (op->code != OpCode::Phi)
      break;

    if (phi2alloca.count(op)) {
      auto *alloca = phi2alloca[op];
      stacks[alloca].push(op->result);
      push_count[alloca]++;
    }
  }

  auto it = b->insts.begin();
  while (it != b->insts.end()) {
    Op *op = *it;
    if (op->code == OpCode::Load) {
      Value *addr = op->operands[0];
      if (addr->kind == ValueKind::OpResult) {
        auto *creator = static_cast<OpResult *>(addr)->creator;

        if (stacks.count(static_cast<Op *>(creator))) {
          auto *alloca = static_cast<Op *>(creator);
          Value *new_val = stacks[alloca].top();

          rewriter.replaceAllUsesWith(op->result, new_val);
        }
      }

    } else if (op->code == OpCode::Store) {
      Value *addr = op->operands[1];
      if (addr->kind == ValueKind::OpResult) {
        auto *creator = static_cast<OpResult *>(addr)->creator;

        if (stacks.count(static_cast<Op *>(creator))) {
          auto *alloca = static_cast<Op *>(creator);
          stacks[alloca].push(op->operands[0]);
          push_count[alloca]++;
        }
      }
    }
    ++it;
  }

  for (auto *succ : b->succs) {
    for (auto *op : succ->insts) {
      if (op->code != OpCode::Phi)
        break;

      if (phi2alloca.count(op)) {
        auto *alloca = phi2alloca[op];
        auto &payload = std::get<PhiPayload>(op->payload);
        payload.incoming.push_back({b, stacks[alloca].top()});
        stacks[alloca].top()->addUse(op);
      }
    }
  }

  for (auto *child : dom.get_children(b)) {
    rename(child, dom, stacks, rewriter);
  }

  for (auto const &[alloca, count] : push_count) {
    for (int i = 0; i < count; ++i)
      stacks[alloca].pop();
  }
}

auto Mem2Reg::cleanup(
  LinearFunction &func,
  const std::vector<Op *> &allocas,
  MidIRRewriter &rewriter
) -> void {
  std::unordered_set<Op *> alloca_set(allocas.begin(), allocas.end());
  for (auto &b : func.blocks) {
    for (auto *op : b->insts) {
      bool remove = false;
      if (alloca_set.count(op)) {
        remove = true;

      } else if (op->code == OpCode::Load || op->code == OpCode::Store) {
        Value *addr =
          (op->code == OpCode::Load) ? op->operands[0] : op->operands[1];

        if (addr->kind == ValueKind::OpResult) {
          auto *creator = static_cast<OpResult *>(addr)->creator;

          if (alloca_set.count(static_cast<Op *>(creator))) {
            remove = true;
          }
        }
      }

      if (remove) {
        rewriter.eraseOp(op);
      }
    }
  }
  rewriter.finalize(func);
}

} // namespace exodus::mid_ir
