#pragma once

#include "ir.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_set>

namespace exodus::mid_ir {

inline constexpr size_t pointer_storage_size = 8;

struct MemoryLocation {
  Value *pointer = nullptr;
  Value *root = nullptr;
  std::optional<int64_t> offset;
  size_t size = 0;
};

enum class AliasResult : uint8_t { NoAlias, MayAlias, MustAlias };

class BasicAliasAnalysis {
public:
  auto get_location(Value *pointer, size_t size = 0) const -> MemoryLocation;
  auto get_location(const Op &op) const -> std::optional<MemoryLocation>;
  auto alias(const MemoryLocation &lhs, const MemoryLocation &rhs) const
    -> AliasResult;
  auto is_dereferenceable(const MemoryLocation &location) const -> bool;
  auto may_alias(const MemoryLocation &lhs, const MemoryLocation &rhs) const
    -> bool {
    return alias(lhs, rhs) != AliasResult::NoAlias;
  }

private:
  auto get_location_impl(
    Value *pointer, size_t size, std::unordered_set<Value *> &active
  ) const -> MemoryLocation;
  auto is_identified_object(Value *root) const -> bool;
};

} // namespace exodus::mid_ir
