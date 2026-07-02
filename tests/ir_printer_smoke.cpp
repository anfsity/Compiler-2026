#include "../src/high/ir.hpp"
#include "../src/high/ir_printer.hpp"
#include "../src/mid/flatten.hpp"
#include "../src/mid/ir_printer.hpp"
#include "../src/type.hpp"

#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace exodus;
using namespace exodus::high_ir;

#ifdef EXODUS_UNIT_TEST

namespace {
auto init(int value) -> InitVal { return InitVal{value}; }
auto init(float value) -> InitVal { return InitVal{value}; }
auto zeroinit() -> InitVal { return InitVal{ZeroInit{}}; }

auto make_op(
  Module &module,
  Region &region,
  OpCode code,
  std::vector<Value *> operands = {},
  OpResult *result = nullptr,
  Op::Payload payload = EmptyPayload{}
) -> Op * {
  auto *op = module.ctx.make_op(code, std::move(payload));
  op->operands = std::move(operands);
  op->result = result;
  if (result != nullptr) {
    result->creator = op;
  }
  region.push_back(op);
  return op;
}

auto make_result(Module &module, std::shared_ptr<Type> type) -> OpResult * {
  return module.ctx.make_value<OpResult>(std::move(type));
}

auto add_global_vars(Module &module) -> void {
  {
    auto *addr = module.ctx.make_value<GlobalAddr>(I32::get()->ptr_to(), "g");
    auto var = std::make_unique<GlobalVar>();
    var->name = "g";
    var->type = I32::get();
    var->init = init(1);
    var->addr = addr;
    module.globals.push_back(std::move(var));
  }
  {
    auto *addr = module.ctx.make_value<GlobalAddr>(Float::get()->ptr_to(), "f");
    auto var = std::make_unique<GlobalVar>();
    var->name = "f";
    var->type = Float::get();
    var->init = init(1.5f);
    var->addr = addr;
    module.globals.push_back(std::move(var));
  }
}

auto add_global_array(Module &module) -> GlobalAddr * {

  auto array_type = I32::get()->array_of(3)->array_of(2);
  auto *addr = module.ctx.make_value<GlobalAddr>(array_type->ptr_to(), "arr");

  auto var = std::make_unique<GlobalVar>();
  var->name = "arr";
  var->type = array_type;
  var->init = InitVal{InitList{
    {
      InitVal{InitList{{init(1), init(2), init(3)}}},
      zeroinit(),
    },
  }};
  var->addr = addr;
  module.globals.push_back(std::move(var));

  return addr;
}

auto add_decl(Module &module) -> void {
  auto decl = std::make_unique<Function>();
  decl->name = "putint";
  decl->type = Func::get(Void::get(), {I32::get()});
  decl->is_decl = true;
  module.functions.push_back(std::move(decl));
}

auto add_main(Module &module, GlobalAddr *array_addr) -> void {
  auto func = std::make_unique<Function>();
  func->name = "main";
  func->type = Func::get(I32::get(), {});

  auto *ptr = make_result(module, I32::get()->ptr_to());
  make_op(module, func->body, OpCode::Alloca, {}, ptr);

  auto *one = module.ctx.make_const(I32::get(), 1);
  auto *two = module.ctx.make_const(I32::get(), 2);

  make_op(module, func->body, OpCode::Store, {one, ptr});

  auto *loaded = make_result(module, I32::get());
  make_op(module, func->body, OpCode::Load, {ptr}, loaded);

  auto *sum = make_result(module, I32::get());
  make_op(module, func->body, OpCode::Add, {loaded, two}, sum);

  auto *f_val = module.ctx.make_const(Float::get(), 1.5f);
  auto *f_sum = make_result(module, Float::get());
  make_op(module, func->body, OpCode::FAdd, {f_val, f_val}, f_sum);

  auto *i2f = make_result(module, Float::get());
  make_op(module, func->body, OpCode::I2F, {sum}, i2f);

  auto *cmp = make_result(module, Bool::get());
  make_op(module, func->body, OpCode::Lt, {sum, two}, cmp);

  auto *logic = make_result(module, I32::get());
  make_op(module, func->body, OpCode::And, {sum, two}, logic);

  auto *idx0 = module.ctx.make_zero(I32::get());
  auto *idx1 = module.ctx.make_const(I32::get(), 1);
  auto *elem_ptr = make_result(module, I32::get()->ptr_to());
  make_op(
    module, func->body, OpCode::GetPtr, {array_addr, idx0, idx1}, elem_ptr
  );

  make_op(
    module, func->body, OpCode::Call, {logic}, nullptr, CallPayload{"putint"}
  );

  auto *call_res = make_result(module, I32::get());
  make_op(
    module, func->body, OpCode::Call, {sum, two}, call_res, CallPayload{"add"}
  );

  Region then_region;

  Region else_region;
  make_op(module, else_region, OpCode::Break);

  make_op(
    module,
    func->body,
    OpCode::If,
    {cmp},
    nullptr,
    IfPayload{
      std::make_unique<Region>(std::move(then_region)),
      std::move(else_region),
    }
  );

  Region cond_region;
  make_op(module, cond_region, OpCode::Condition, {cmp});

  Region loop_region;
  make_op(module, loop_region, OpCode::Continue);

  make_op(
    module,
    func->body,
    OpCode::While,
    {},
    nullptr,
    WhilePayload{
      std::make_unique<Region>(std::move(cond_region)),
      std::make_unique<Region>(std::move(loop_region)),
    }
  );

  make_op(module, func->body, OpCode::Ret, {call_res});
  module.functions.push_back(std::move(func));
}

auto make_module() -> Module {
  Module module;
  add_global_vars(module);
  auto *array_addr = add_global_array(module);
  add_decl(module);
  add_main(module, array_addr);
  return module;
}

auto make_mid_call_module() -> Module {
  Module module;

  auto putch = std::make_unique<Function>();
  putch->name = "putch";
  putch->type = Func::get(Void::get(), {I32::get()});
  putch->is_decl = true;
  module.functions.push_back(std::move(putch));

  auto getint = std::make_unique<Function>();
  getint->name = "getint";
  getint->type = Func::get(I32::get(), {});
  getint->is_decl = true;
  module.functions.push_back(std::move(getint));

  auto main = std::make_unique<Function>();
  main->name = "main";
  main->type = Func::get(I32::get(), {});

  auto *ten = module.ctx.make_const(I32::get(), 10);
  make_op(
    module, main->body, OpCode::Call, {ten}, nullptr, CallPayload{"putch"}
  );

  auto *call_res = make_result(module, I32::get());
  make_op(
    module, main->body, OpCode::Call, {}, call_res, CallPayload{"getint"}
  );
  make_op(module, main->body, OpCode::Ret, {call_res});

  module.functions.push_back(std::move(main));
  return module;
}

} // namespace

auto main() -> int {
  auto module = make_module();
  IRPrinter printer;
  const auto actual = printer.dump(module);

  const std::string expected =
    "@g = global 1 : i32\n"
    "@f = global 1.500000f : f32\n"
    "@arr = global {{1, 2, 3}, zeroinit} : i32[2][3]\n"
    "decl @putint() : (i32) -> void\n"
    "func @main() : () -> i32 {\n"
    "  %0 = alloca : i32 -> i32*\n"
    "  store 1, %0 : i32, i32*\n"
    "  %1 = load %0 : i32* -> i32\n"
    "  %2 = add %1, 2 -> i32\n"
    "  %3 = fadd 1.5f, 1.5f -> f32\n"
    "  %4 = i2f %2 : i32 -> f32\n"
    "  %5 = lt %2, 2 : i32 -> i1\n"
    "  %6 = and %2, 2 -> i32\n"
    "  %7 = getptr @arr, 0, 1 : i32[2][3]* -> i32*\n"
    "  call @putint(%6) : (i32)\n"
    "  %8 = call @add(%2, 2) : (i32, i32) -> i32\n"
    "  if %5 {\n"
    "  } else {\n"
    "    break\n"
    "  }\n"
    "  while {\n"
    "    condition(%5)\n"
    "  } {\n"
    "    continue\n"
    "  }\n"
    "  ret %8\n"
    "}\n";

  if (actual != expected) {
    std::cerr << "IR printer smoke test failed.\n\nExpected:\n"
              << expected << "\nActual:\n"
              << actual;
    return 1;
  }

  auto mid_source = make_mid_call_module();
  exodus::mid_ir::Flattener flattener(&mid_source);
  auto mid_module = flattener.flatten();
  exodus::mid_ir::LinearIRPrinter mid_printer;
  const auto mid_actual = mid_printer.dump(*mid_module);
  const std::string mid_expected = "decl @putch() : (i32) -> void\n"
                                   "decl @getint() : () -> i32\n"
                                   "func @main() : () -> i32 {\n"
                                   "  ^entry_0:\n"
                                   "    call @putch(10) : (i32)\n"
                                   "    %0 = call @getint() : () -> i32\n"
                                   "    ret %0\n"
                                   "}\n";

  if (mid_actual != mid_expected) {
    std::cerr << "Mid IR printer smoke test failed.\n\nExpected:\n"
              << mid_expected << "\nActual:\n"
              << mid_actual;
    return 1;
  }

  std::cout << "IRBuilder test passed!\n";
  return 0;
}
#endif
