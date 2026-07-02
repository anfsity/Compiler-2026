#include "flatten.hpp"

namespace exodus::mid_ir {
namespace {
auto convert_opcode(high_ir::OpCode old_code) -> OpCode {
  switch (old_code) {
    // clang-format off
  case high_ir::OpCode::Add: return OpCode::Add;
  case high_ir::OpCode::Sub: return OpCode::Sub;
  case high_ir::OpCode::Mul: return OpCode::Mul;
  case high_ir::OpCode::Div: return OpCode::Div;
  case high_ir::OpCode::Mod: return OpCode::Mod;
  case high_ir::OpCode::FAdd: return OpCode::FAdd;
  case high_ir::OpCode::FSub: return OpCode::FSub;
  case high_ir::OpCode::FMul: return OpCode::FMul;
  case high_ir::OpCode::FDiv: return OpCode::FDiv;
  case high_ir::OpCode::I2F: return OpCode::I2F;
  case high_ir::OpCode::F2I: return OpCode::F2I;
  case high_ir::OpCode::ZExt: return OpCode::ZExt;
  case high_ir::OpCode::Eq: return OpCode::Eq;
  case high_ir::OpCode::Ne: return OpCode::Ne;
  case high_ir::OpCode::Lt: return OpCode::Lt;
  case high_ir::OpCode::Gt: return OpCode::Gt;
  case high_ir::OpCode::Le: return OpCode::Le;
  case high_ir::OpCode::Ge: return OpCode::Ge;
  case high_ir::OpCode::And: return OpCode::And;
  case high_ir::OpCode::Or: return OpCode::Or;
  case high_ir::OpCode::Xor: return OpCode::Xor;
  case high_ir::OpCode::Shl: return OpCode::Shl;
  case high_ir::OpCode::Shr: return OpCode::Shr;
  case high_ir::OpCode::Alloca: return OpCode::Alloca;
  case high_ir::OpCode::Load: return OpCode::Load;
  case high_ir::OpCode::Store: return OpCode::Store;
  case high_ir::OpCode::GetPtr: return OpCode::GetPtr;
  case high_ir::OpCode::Call: return OpCode::Call;
  case high_ir::OpCode::Ret: return OpCode::Ret;
  case high_ir::OpCode::Jump: return OpCode::Jump;
  case high_ir::OpCode::Branch: return OpCode::Branch;
  case high_ir::OpCode::Memset: return OpCode::Memset;
  case high_ir::OpCode::Condition:
  case high_ir::OpCode::If:
  case high_ir::OpCode::While:
  case high_ir::OpCode::Break:
  case high_ir::OpCode::Continue:
  break;
    // clang-format on
  }
  assert(false && "unsupported high IR opcode in mid IR flattener");
  return OpCode::Ret;
}

} // namespace

auto Flattener::convert_op(high_ir::Op *old_op) -> Op * {
  auto *new_op = new_module->make_op(convert_opcode(old_op->code));

  for (auto *operand : old_op->operands) {
    new_op->operands.push_back(operand);
    operand->addUse(new_op);
  }

  new_op->result = old_op->result;
  if (new_op->result) {
    new_op->result->creator = new_op;
  }
  new_op->successors = old_op->successors;
  if (old_op->code == high_ir::OpCode::Call) {
    const auto &payload = std::get<high_ir::CallPayload>(old_op->payload);
    new_op->payload = CallPayload{payload.func_name};
  }
  return new_op;
}

auto Flattener::create_block(const std::string &name) -> Block * {
  int id = b_cnt++;
  auto b = std::make_unique<Block>(id, name + "_" + std::to_string(id));
  auto *ptr = b.get();
  cur_func->blocks.push_back(std::move(b));
  return ptr;
}

auto Flattener::flatten() -> std::unique_ptr<MidModule> {
  auto res = std::make_unique<MidModule>();
  new_module = res.get();
  new_module->ctx = &old_module->ctx;

  for (auto &g : old_module->globals) {
    new_module->globals.push_back(g.get());
  }

  for (auto &f : old_module->functions) {
    b_cnt = 0;
    new_module->functions.push_back(visit(f.get()));
  }

  return res;
}

auto Flattener::visit(high_ir::Function *f) -> std::unique_ptr<LinearFunction> {
  auto lf = std::make_unique<LinearFunction>();
  cur_func = lf.get();
  cur_func->name = f->name;
  cur_func->type = f->type;
  cur_func->args = f->args;
  cur_func->is_decl = f->is_decl;

  if (!f->is_decl) {
    cur_block = create_block("entry");
    visit(f->body);
    build_cfg();
  }

  cur_func = nullptr;
  cur_block = nullptr;
  return lf;
}

auto Flattener::build_cfg() -> void {
  for (auto &b_ptr : cur_func->blocks) {
    Block *u = b_ptr.get();
    if (u->insts.empty())
      continue;

    Op *last = u->insts.back();
    if (last->code == OpCode::Jump || last->code == OpCode::Branch) {
      for (Block *v : last->successors) {
        u->succs.push_back(v);
        v->preds.push_back(u);
      }
    }
  }
}

auto Flattener::visit(const high_ir::Region &region) -> void {
  for (auto *op : region) {
    // 仍然是经过深思熟虑，我决定 mid ir 的 flatten 依赖于 high ir 阶段的优化
    // flattener 作为构建 mid ir
    // 的核心工具，我希望在构建过程中能够满足一个核心假设 「high ir
    // 没有多余分支，没有不可达指令，最小化嵌套」 这样，在 flatten
    // 过程中，就不需要考虑各种特例，为了构建 CFG 而弄得乱七八糟
    visit(op);
    if (
      !cur_block->insts.empty() &&
      (cur_block->insts.back()->code == OpCode::Ret ||
       cur_block->insts.back()->code == OpCode::Jump ||
       cur_block->insts.back()->code == OpCode::Branch)
    ) {
      break;
    }
  }
}

auto Flattener::visit(high_ir::Op *op) -> void {
  switch (op->code) {
  case high_ir::OpCode::If: {
    auto &payload = std::get<high_ir::IfPayload>(op->payload);
    auto *then_b = create_block("then");
    auto *else_b = payload.else_region ? create_block("else") : nullptr;
    auto *merge_b = create_block("merge");

    auto *br = new_module->make_op(OpCode::Branch);
    br->operands.push_back(op->operands[0]);
    br->successors.push_back(then_b);
    br->successors.push_back(else_b ? else_b : merge_b);
    cur_block->insts.push_back(br);

    cur_block = then_b;
    visit(*payload.then_region);
    // 为了防止产生不必要的死代码，比如 ret 之后再 jump，ret
    // 之后指令都是无效的。
    if (
      cur_block->insts.empty() ||
      (cur_block->insts.back()->code != OpCode::Ret &&
       cur_block->insts.back()->code != OpCode::Jump &&
       cur_block->insts.back()->code != OpCode::Branch)
    ) {
      auto *jmp = new_module->make_op(OpCode::Jump);
      jmp->successors.push_back(merge_b);
      cur_block->insts.push_back(jmp);
    }

    if (else_b) {
      cur_block = else_b;
      visit(*payload.else_region);
      if (
        cur_block->insts.empty() ||
        (cur_block->insts.back()->code != OpCode::Ret &&
         cur_block->insts.back()->code != OpCode::Jump &&
         cur_block->insts.back()->code != OpCode::Branch)
      ) {
        auto *jmp = new_module->make_op(OpCode::Jump);
        jmp->successors.push_back(merge_b);
        cur_block->insts.push_back(jmp);
      }
    }

    cur_block = merge_b;
    break;
  }
  case high_ir::OpCode::While: {
    auto &payload = std::get<high_ir::WhilePayload>(op->payload);
    auto *cond_b = create_block("while_cond");
    auto *body_b = create_block("while_body");
    auto *exit_b = create_block("while_exit");

    auto *jmp_to_cond = new_module->make_op(OpCode::Jump);
    jmp_to_cond->successors.push_back(cond_b);
    cur_block->insts.push_back(jmp_to_cond);

    cur_block = cond_b;
    for (auto *cond_op : *payload.cond_region) {
      if (cond_op->code == high_ir::OpCode::Condition) {
        auto *br = new_module->make_op(OpCode::Branch);
        br->operands.push_back(cond_op->operands[0]);
        br->successors.push_back(body_b);
        br->successors.push_back(exit_b);
        cur_block->insts.push_back(br);
      } else {
        visit(cond_op);
      }
    }

    cur_block = body_b;
    loop_stk.push_back({cond_b, exit_b});
    visit(*payload.loop_region);
    loop_stk.pop_back();

    if (
      cur_block->insts.empty() ||
      (cur_block->insts.back()->code != OpCode::Ret &&
       cur_block->insts.back()->code != OpCode::Jump &&
       cur_block->insts.back()->code != OpCode::Branch)
    ) {
      auto *jmp_back = new_module->make_op(OpCode::Jump);
      jmp_back->successors.push_back(cond_b);
      cur_block->insts.push_back(jmp_back);
    }

    cur_block = exit_b;
    break;
  }
  case high_ir::OpCode::Break: {
    assert(loop_stk.size());
    auto *jmp = new_module->make_op(OpCode::Jump);
    jmp->successors.push_back(loop_stk.back().second);
    cur_block->insts.push_back(jmp);
    break;
  }
  case high_ir::OpCode::Continue: {
    assert(loop_stk.size());
    auto *jmp = new_module->make_op(OpCode::Jump);
    jmp->successors.push_back(loop_stk.back().first);
    cur_block->insts.push_back(jmp);
    break;
  }
  default:
    if (
      !cur_block->insts.empty() && cur_block->insts.back()->code == OpCode::Ret
    )
      break;

    cur_block->insts.push_back(convert_op(op));
    break;
  }
}
} // namespace exodus::mid_ir
