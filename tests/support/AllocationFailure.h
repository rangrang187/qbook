#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace qbook::testing {

enum class AllocationKind : std::uint8_t { Any, Ordinary, Aligned };

struct AllocationRecord {
  AllocationKind kind{};
  std::size_t bytes{};
  std::size_t alignment{};
  bool operator==(const AllocationRecord&) const = default;
};

// 保存短作用域内的分配路径；记录区固定大小，观察本身不分配内存。
struct AllocationReport {
  std::array<AllocationRecord, 16> records{};
  std::size_t count{};
  std::size_t ordinary_calls{};
  std::size_t aligned_calls{};
  std::size_t matched_calls{};
  std::size_t failures{};
  bool overflow{};
};

// 只观察当前线程，禁止嵌套。fail_next 使首个匹配分配抛出 bad_alloc。
// 命中后自动关闭故障；析构关闭观察。作用域内不得调用测试断言或格式化。
class AllocationScope {
 public:
  explicit AllocationScope(AllocationReport& report,
                           AllocationKind kind = AllocationKind::Any,
                           bool fail_next = false) noexcept;
  ~AllocationScope();
  AllocationScope(const AllocationScope&) = delete;
  AllocationScope& operator=(const AllocationScope&) = delete;
};

}  // namespace qbook::testing
