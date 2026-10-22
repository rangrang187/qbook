#pragma once

#include "PipelineTypes.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <type_traits>

#include <qlib/hive.h>
#include <qlib/map.h>

namespace qbook {
namespace testing { struct OrderbookInspector; }

enum class ApplyStatus : std::uint8_t { Applied, Ignored };
enum class IgnoreReason : std::uint8_t {
  None, DuplicateId, UnknownOrder, MarketOrder, Unchanged,
  SideMismatch, QuantityOverflow, L2Trade, MissingLevel
};

// 业务忽略仍完成一次事件处理；调用者继续保留原始事件。
struct ApplyResult {
  ApplyStatus status;
  IgnoreReason reason;
};

// 维护一个品种的一种建簿模式，不主动撮合；所有方法由单一所有者串行调用。
// 输入须已通过协议校验，调用期间保持有效；本类只修改自己拥有的订单副本。
class Orderbook {
 public:
  // 品种 0 有效；非法模式抛 invalid_argument，合法构造产生空簿。
  Orderbook(std::uint32_t instrument_id, BookMode mode);
  Orderbook(const Orderbook&) = delete;
  Orderbook& operator=(const Orderbook&) = delete;

  // 转交拥有关系并保持活订单地址；旧容器迭代器失效。
  // 来源只保证可析构或重新移动赋值，不保证继续处理事件或自移动赋值。
  Orderbook(Orderbook&&) noexcept = default;
  Orderbook& operator=(Orderbook&&) noexcept = default;
  ~Orderbook() = default;

  // 逐笔新增或修改；new_qty 为绝对剩余量，同价保序，改价进入目标尾部。
  // 业务忽略返回原因并更新时间；品种/模式误用抛 invalid_argument。
  // 分配失败传播异常并回滚业务状态，容器容量允许变化。
  ApplyResult OnOrder(const qlib::order& event);

  // 逐笔独立扣减两个有效 ID；L2 仅更新时间。成交量须为正，非零 ID 须不同。
  ApplyResult OnTrade(const qlib::trade& event);

  // 逐笔按 ID 扣减正撤销量；达到剩余量时删除，方向以存储订单为准。
  ApplyResult OnCancel(const qlib::cancel& event);

  // L2 设置价位绝对量，非正量删除；新价位分配失败保留原业务状态。
  ApplyResult OnLevel(const qlib::level& event);

  // 返回最优一档的独立值；空侧为 0/0，时间来自最近正常返回的事件。
  // const 读取也不能与写入并发；异常不承诺推进事件时间。
  qlib::book OnSnapshot() const;
  std::size_t size() const noexcept;  // 本地逐笔订单数；L2 始终为 0。

  // 按容量和价位数估计存储，超界饱和为 SIZE_MAX，不代表 RSS 或精确分配量。
  // 不含对象本体、分配器、hive 管理信息及 ID 控制区额外哨兵字节。
  std::size_t MemoryBytes() const noexcept;

 private:
  using OrderStore = qlib::hive<qlib::order>;
  using IdMap = qlib::map<std::int64_t, qlib::order*>;
  using PriceLevelMap = std::map<std::int64_t, qlib::price_level>;
  friend struct testing::OrderbookInspector;

  static_assert(std::is_nothrow_move_constructible_v<OrderStore>);
  static_assert(std::is_nothrow_move_assignable_v<OrderStore>);
  static_assert(std::is_nothrow_move_constructible_v<IdMap>);
  static_assert(std::is_nothrow_move_assignable_v<IdMap>);
  static_assert(std::is_nothrow_move_constructible_v<PriceLevelMap>);
  static_assert(std::is_nothrow_move_assignable_v<PriceLevelMap>);
  static_assert(std::is_nothrow_move_constructible_v<IdMap::value_type>);
  static_assert(std::is_nothrow_destructible_v<qlib::order>);
  static_assert(std::is_nothrow_destructible_v<qlib::price_level>);

  ApplyResult Add(const qlib::order& event);
  ApplyResult Modify(const qlib::order& event);
  bool Reduce(std::int64_t order_id, std::int64_t qty);
  void Erase(qlib::order* node);
  qlib::price_level& FindLevel(const qlib::order* node);
  void UnlinkOrders(qlib::order* node);
  void LinkOrders(qlib::order* node, qlib::price_level& level,
                  std::int64_t new_total) noexcept;
  void Touch(std::int64_t event_ns, std::int64_t recv_ns) noexcept;

  std::uint32_t instrument_id_;
  BookMode mode_;
  // 声明次序使价位和索引先销毁，hive 最后释放它们借用的订单。
  OrderStore orders_;
  IdMap by_id_;
  PriceLevelMap bids_;
  PriceLevelMap asks_;
  std::int64_t event_ns_ = 0;
  std::int64_t recv_ns_ = 0;
};

static_assert(std::is_nothrow_move_constructible_v<Orderbook>);
static_assert(std::is_nothrow_move_assignable_v<Orderbook>);

}  // namespace qbook
