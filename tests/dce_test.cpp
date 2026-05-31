#include "../include/high/ir.hpp"
#include "../include/opt/high/sdce.hpp"
#include "fmt/base.h"              // for print
#include "opt/AnalysisManager.hpp" // for FunctionAnalysisManager
#include "type.hpp"                // for I32, Bool, Type
#include <cassert>
#include <list>     // for list, _List_iterator
#include <memory>   // for shared_ptr, unique_ptr, make...
#include <optional> // for nullopt, nullopt_t, optional
#include <string>   // for basic_string
#include <utility>  // for move
#include <variant>  // for get
#include <vector>   // for vector

using namespace exodus;
using namespace exodus::high_ir;
using namespace exodus::opt;

#ifdef EXODUS_UNIT_TEST
int main() {
  IRContext ctx;
  Function f;
  f.name = "test";
  f.is_decl = false;

  // Test 1: Simple dead code
  {
    f.body.clear();
    Op *dead_op = ctx.make_op(OpCode::Add);
    dead_op->operands.push_back(ctx.make_const(I32::get(), 1));
    dead_op->operands.push_back(ctx.make_const(I32::get(), 2));
    dead_op->result = ctx.make_value<OpResult>(I32::get(), dead_op);
    f.body.push_back(dead_op);

    Op *live_op = ctx.make_op(OpCode::Store);
    live_op->operands.push_back(ctx.make_const(I32::get(), 3));
    live_op->operands.push_back(
      ctx.make_value<GlobalAddr>(I32::get()->ptr_to(), "dummy")
    );
    f.body.push_back(live_op);

    SimpleDCE dce(nullptr);
    FunctionAnalysisManager fam;
    dce.run(f, fam);

    assert(f.body.size() == 1);
    assert((*f.body.begin())->code == OpCode::Store);
    fmt::print("Test 1 passed: Simple dead code removed.\n");
  }

  // Test 2: Dead If
  {
    f.body.clear();
    auto then_region = std::make_unique<Region>();
    Op *dead_op = ctx.make_op(OpCode::Add);
    dead_op->operands.push_back(ctx.make_const(I32::get(), 1));
    dead_op->operands.push_back(ctx.make_const(I32::get(), 2));
    dead_op->result = ctx.make_value<OpResult>(I32::get(), dead_op);
    then_region->push_back(dead_op);

    Op *if_op = ctx.make_op(OpCode::If);
    if_op->operands.push_back(ctx.make_const(Bool::get(), 1));
    if_op->payload = IfPayload{std::move(then_region), std::nullopt};
    f.body.push_back(if_op);

    SimpleDCE dce(nullptr);
    FunctionAnalysisManager fam;
    dce.run(f, fam);

    assert(f.body.empty());
    fmt::print("Test 2 passed: Dead If removed.\n");
  }

  // Test 3: Live If
  {
    f.body.clear();
    auto then_region = std::make_unique<Region>();
    Op *store_op = ctx.make_op(OpCode::Store);
    store_op->operands.push_back(ctx.make_const(I32::get(), 1));
    store_op->operands.push_back(
      ctx.make_value<GlobalAddr>(I32::get()->ptr_to(), "dummy")
    );
    then_region->push_back(store_op);

    Op *if_op = ctx.make_op(OpCode::If);
    if_op->operands.push_back(ctx.make_const(Bool::get(), 1));
    if_op->payload = IfPayload{std::move(then_region), std::nullopt};
    f.body.push_back(if_op);

    SimpleDCE dce(nullptr);
    FunctionAnalysisManager fam;
    dce.run(f, fam);

    assert(f.body.size() == 1);
    assert((*f.body.begin())->code == OpCode::If);
    fmt::print("Test 3 passed: Live If kept.\n");
  }

  // Test 4: Nested dead code
  {
    f.body.clear();
    auto then_region = std::make_unique<Region>();
    Op *store_op = ctx.make_op(OpCode::Store);
    store_op->operands.push_back(ctx.make_const(I32::get(), 1));
    store_op->operands.push_back(
      ctx.make_value<GlobalAddr>(I32::get()->ptr_to(), "dummy")
    );
    then_region->push_back(store_op);

    Op *dead_op = ctx.make_op(OpCode::Add);
    dead_op->operands.push_back(ctx.make_const(I32::get(), 1));
    dead_op->operands.push_back(ctx.make_const(I32::get(), 2));
    dead_op->result = ctx.make_value<OpResult>(I32::get(), dead_op);
    then_region->push_back(dead_op);

    Op *if_op = ctx.make_op(OpCode::If);
    if_op->operands.push_back(ctx.make_const(Bool::get(), 1));
    if_op->payload = IfPayload{std::move(then_region), std::nullopt};
    f.body.push_back(if_op);

    SimpleDCE dce(nullptr);
    FunctionAnalysisManager fam;
    dce.run(f, fam);

    assert(f.body.size() == 1);
    [[maybe_unused]] auto &p = std::get<IfPayload>((*f.body.begin())->payload);
    assert(p.then_region->size() == 1);
    assert((*p.then_region->begin())->code == OpCode::Store);
    fmt::print(
      "Test 4 passed: Nested dead code removed, live container kept.\n"
    );
  }

  // Test 5: Live While
  {
    f.body.clear();
    auto cond_region = std::make_unique<Region>();
    Op *cond_op = ctx.make_op(OpCode::Condition);
    cond_op->operands.push_back(ctx.make_const(Bool::get(), 1));
    cond_region->push_back(cond_op);

    auto loop_region = std::make_unique<Region>();
    Op *store_op = ctx.make_op(OpCode::Store);
    store_op->operands.push_back(ctx.make_const(I32::get(), 1));
    store_op->operands.push_back(
      ctx.make_value<GlobalAddr>(I32::get()->ptr_to(), "dummy")
    );
    loop_region->push_back(store_op);

    Op *while_op = ctx.make_op(OpCode::While);
    while_op->payload =
      WhilePayload{std::move(cond_region), std::move(loop_region)};
    f.body.push_back(while_op);

    SimpleDCE dce(nullptr);
    FunctionAnalysisManager fam;
    dce.run(f, fam);

    assert(f.body.size() == 1);
    assert((*f.body.begin())->code == OpCode::While);
    fmt::print("Test 5 passed: Live While kept.\n");
  }

  // Test 6: Dead While
  {
    f.body.clear();
    auto cond_region = std::make_unique<Region>();
    Op *cond_op = ctx.make_op(OpCode::Condition);
    cond_op->operands.push_back(ctx.make_const(Bool::get(), 1));
    cond_region->push_back(cond_op);

    auto loop_region = std::make_unique<Region>();
    Op *dead_op = ctx.make_op(OpCode::Add);
    dead_op->operands.push_back(ctx.make_const(I32::get(), 1));
    dead_op->operands.push_back(ctx.make_const(I32::get(), 2));
    dead_op->result = ctx.make_value<OpResult>(I32::get(), dead_op);
    loop_region->push_back(dead_op);

    Op *while_op = ctx.make_op(OpCode::While);
    while_op->payload =
      WhilePayload{std::move(cond_region), std::move(loop_region)};
    f.body.push_back(while_op);

    SimpleDCE dce(nullptr);
    FunctionAnalysisManager fam;
    dce.run(f, fam);

    assert(f.body.empty());
    fmt::print("Test 6 passed: Dead While removed.\n");
  }

  return 0;
}
#endif
