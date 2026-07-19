#include "ipcp.hpp"

namespace exodus::high_ir::opt {
namespace {

struct CallSites : RecursiveOpVisitor<CallSites> {
  std::unordered_map<std::string, std::vector<Op *>> &sites; // NOLINT

  explicit CallSites(std::unordered_map<std::string, std::vector<Op *>> &s)
      : sites(s) {}

  using RecursiveOpVisitor<CallSites>::visit;
  auto visit(Op *op, OpTag<OpCode::Call>) -> void {
    sites[std::get<CallPayload>(op->payload).func_name].push_back(op);
  }
};

auto same_constant(Value *lhs, Value *rhs) -> bool {
  if (
    !lhs || !rhs || lhs->kind != ValueKind::Constant ||
    rhs->kind != ValueKind::Constant || lhs->type != rhs->type
  )
    return false;
  auto *left = static_cast<Constant *>(lhs);
  auto *right = static_cast<Constant *>(rhs);
  return left->val == right->val;
}

} // namespace

auto IPCP::run(Module &, ModuleAnalysisManager &) -> PreservedAnalysis {
  std::unordered_map<std::string, std::vector<Op *>> call_sites;
  for (auto &function : module->functions) {
    if (!function->is_decl) {
      CallSites finder(call_sites);
      finder.visit(*function);
    }
  }

  CallGraph call_graph(*module);
  IRRewriter rewriter;
  bool changed = false;

  for (auto &function_ptr : module->functions) {
    Function *function = function_ptr.get();
    if (function->is_decl || call_graph.isRecursive(function))
      continue;
    auto sites_it = call_sites.find(function->name);
    if (sites_it == call_sites.end() || sites_it->second.empty())
      continue;

    for (size_t arg_index = 0; arg_index < function->args.size(); ++arg_index) {
      Value *constant = nullptr;
      bool known = true;
      for (auto *call : sites_it->second) {
        if (
          arg_index >= call->operands.size() ||
          call->operands[arg_index]->kind != ValueKind::Constant
        ) {
          known = false;
          break;
        }
        if (!constant)
          constant = call->operands[arg_index];
        else if (!same_constant(constant, call->operands[arg_index])) {
          known = false;
          break;
        }
      }
      if (known && constant && !function->args[arg_index]->users.empty()) {
        rewriter.replace_all_uses_with(function->args[arg_index], constant);
        changed = true;
      }
    }
  }

  return changed ? PreservedAnalysis::none() : PreservedAnalysis::all();
}

} // namespace exodus::high_ir::opt
