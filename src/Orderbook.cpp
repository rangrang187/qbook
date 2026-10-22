#include "Orderbook.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <stdexcept>

namespace qbook {
namespace {
constexpr auto size_max = std::numeric_limits<std::size_t>::max();
constexpr auto qty_max = std::numeric_limits<std::int64_t>::max();
constexpr ApplyResult applied{ApplyStatus::Applied, IgnoreReason::None};

constexpr ApplyResult Ignored(IgnoreReason reason) noexcept {
  return {ApplyStatus::Ignored, reason};
}

constexpr std::size_t SaturatingAdd(std::size_t a, std::size_t b) noexcept {
  return a > size_max - b ? size_max : a + b;
}

constexpr std::size_t SaturatingMultiply(std::size_t a, std::size_t b) noexcept {
  if (a == 0 || b == 0) return 0;
  return a > size_max / b ? size_max : a * b;
}

// 固定 qlib 的 ID 表可能翻倍，槽位之后还有 capacity + 1 个控制字节。
constexpr auto id_slot_bytes =
    sizeof(qlib::map<std::int64_t, qlib::order*>::value_type) + sizeof(std::uint8_t);
constexpr bool CanGrowIndex(std::size_t capacity) noexcept {
  if (capacity > size_max / 2) return false;
  const auto next = capacity == 0 ? 8 : capacity * 2;
  return next <= (size_max - 1) / id_slot_bytes;
}

// 固定 hive 按当前总容量选择下一块，上限为 8192 个槽。
constexpr bool CanGrowOrders(std::size_t capacity) noexcept {
  const auto next = std::clamp(capacity, std::size_t{8}, std::size_t{8192});
  return capacity <= size_max - next;
}

static_assert(SaturatingAdd(0, 0) == 0);
static_assert(SaturatingAdd(size_max - 1, 1) == size_max);
static_assert(SaturatingAdd(size_max, 1) == size_max);
static_assert(SaturatingMultiply(0, size_max) == 0);
static_assert(SaturatingMultiply(size_max, 0) == 0);
static_assert(SaturatingMultiply(size_max, 1) == size_max);
static_assert(SaturatingMultiply(size_max / 2, 2) == size_max - 1);
static_assert(SaturatingMultiply(size_max, 2) == size_max);
constexpr auto largest_id_capacity = (size_max - 1) / id_slot_bytes / 2;
static_assert(CanGrowIndex(0) && CanGrowIndex(8));
static_assert(CanGrowIndex(largest_id_capacity));
static_assert(!CanGrowIndex(largest_id_capacity + 1));
static_assert(!CanGrowIndex(size_max / 2 + 1));
static_assert(CanGrowOrders(0) && CanGrowOrders(size_max - 8192));
static_assert(!CanGrowOrders(size_max - 8191));
}  // namespace

Orderbook::Orderbook(std::uint32_t instrument_id, BookMode mode)
    : instrument_id_(instrument_id), mode_(mode) {
  if (mode != BookMode::Mbo && mode != BookMode::L2) {
    throw std::invalid_argument("Orderbook 建簿模式无效");
  }
}

ApplyResult Orderbook::OnOrder(const qlib::order& event) {
  if (event.instrument_id != instrument_id_ || mode_ != BookMode::Mbo) {
    throw std::invalid_argument("OnOrder 品种或建簿模式不匹配");
  }
  ApplyResult result;
  switch (event.action) {
    case qlib::order_action::add: result = Add(event); break;
    case qlib::order_action::modify: result = Modify(event); break;
    default: throw std::invalid_argument("OnOrder 订单动作无效");
  }
  Touch(event.event_ns, event.recv_ns);
  return result;
}

ApplyResult Orderbook::Add(const qlib::order& event) {
  if (event.type == qlib::order_type::market) return Ignored(IgnoreReason::MarketOrder);
  if (by_id_.contains(event.order_id)) return Ignored(IgnoreReason::DuplicateId);

  auto& levels = event.side == qlib::side::buy ? bids_ : asks_;
  const auto found = levels.find(event.price);
  const auto old_total = found == levels.end() ? 0 : found->second.qty;
  if (event.qty > qty_max - old_total) return Ignored(IgnoreReason::QuantityOverflow);
  const auto new_total = old_total + event.qty;
  if (!CanGrowIndex(by_id_.capacity()) || !CanGrowOrders(orders_.capacity())) {
    throw std::length_error("Orderbook 容器增长超出可表示范围");
  }

  auto [level, created] = levels.try_emplace(event.price);
  try {
    auto copy = event;
    copy.prev = copy.next = nullptr;
    const auto position = orders_.emplace(copy);
    auto* node = std::addressof(*position);
    try {
      if (!by_id_.try_emplace(std::int64_t{event.order_id}, node).second) {
        throw std::logic_error("新增预检后出现重复订单 ID");
      }
    } catch (...) {
      // 候选尚未挂链，只回收本次对象，不能走活订单的删除路径。
      orders_.erase(position);
      throw;
    }
    LinkOrders(node, level->second, new_total);
  } catch (...) {
    if (created) levels.erase(level);
    throw;
  }
  return applied;
}

ApplyResult Orderbook::Modify(const qlib::order& event) {
  const auto found = by_id_.find(event.order_id);
  if (found == by_id_.end()) return Ignored(IgnoreReason::UnknownOrder);
  auto* node = found->second;
  if (event.side != node->side) return Ignored(IgnoreReason::SideMismatch);
  if (event.new_qty <= 0) {
    Erase(node);
    return applied;
  }
  if (event.price == node->price && event.new_qty == node->qty) {
    return Ignored(IgnoreReason::Unchanged);
  }
  auto& old_level = FindLevel(node);
  if (event.price == node->price) {
    const auto base = old_level.qty - node->qty;
    if (event.new_qty > qty_max - base) return Ignored(IgnoreReason::QuantityOverflow);
    old_level.qty = base + event.new_qty;
    node->qty = event.new_qty;
    return applied;
  }

  auto& levels = node->side == qlib::side::buy ? bids_ : asks_;
  const auto target = levels.find(event.price);
  const auto old_total = target == levels.end() ? 0 : target->second.qty;
  if (event.new_qty > qty_max - old_total) return Ignored(IgnoreReason::QuantityOverflow);
  const auto new_total = old_total + event.new_qty;
  // 目标分配完成前保留旧链；之后只有已检查的局部提交。
  auto [level, created] = levels.try_emplace(event.price);
  try {
    UnlinkOrders(node);
  } catch (...) {
    if (created) levels.erase(level);
    throw;
  }
  node->price = event.price;
  node->qty = event.new_qty;
  LinkOrders(node, level->second, new_total);
  return applied;
}

ApplyResult Orderbook::OnTrade(const qlib::trade& event) {
  if (event.instrument_id != instrument_id_ ||
      (mode_ != BookMode::Mbo && mode_ != BookMode::L2)) {
    throw std::invalid_argument("OnTrade 品种或建簿模式不匹配");
  }
  auto result = Ignored(IgnoreReason::L2Trade);
  if (mode_ == BookMode::Mbo) {
    // 两侧都必须执行，不能用短路表达式跳过卖方 ID。
    const bool changed_buy = Reduce(event.buy_order_id, event.qty);
    const bool changed_sell = Reduce(event.sell_order_id, event.qty);
    result = changed_buy || changed_sell ? applied : Ignored(IgnoreReason::UnknownOrder);
  }
  Touch(event.event_ns, event.recv_ns);
  return result;
}

ApplyResult Orderbook::OnCancel(const qlib::cancel& event) {
  if (event.instrument_id != instrument_id_ || mode_ != BookMode::Mbo) {
    throw std::invalid_argument("OnCancel 品种或建簿模式不匹配");
  }
  const bool changed = Reduce(event.order_id, event.qty);
  Touch(event.event_ns, event.recv_ns);
  return changed ? applied : Ignored(IgnoreReason::UnknownOrder);
}

ApplyResult Orderbook::OnLevel(const qlib::level& event) {
  if (event.instrument_id != instrument_id_ || mode_ != BookMode::L2) {
    throw std::invalid_argument("OnLevel 品种或建簿模式不匹配");
  }
  auto& levels = event.side == qlib::side::buy ? bids_ : asks_;
  const auto found = levels.find(event.price);
  auto result = applied;
  if (event.qty <= 0) {
    if (found == levels.end()) result = Ignored(IgnoreReason::MissingLevel);
    else levels.erase(found);
  } else if (found != levels.end()) {
    if (found->second.qty == event.qty) result = Ignored(IgnoreReason::Unchanged);
    else found->second.qty = event.qty;
  } else {
    // 聚合价位只保存绝对量，链表首尾保持为空。
    qlib::price_level level{};
    level.qty = event.qty;
    levels.try_emplace(event.price, level);
  }
  Touch(event.event_ns, event.recv_ns);
  return result;
}

bool Orderbook::Reduce(std::int64_t order_id, std::int64_t qty) {
  if (order_id == 0) return false;
  const auto found = by_id_.find(order_id);
  if (found == by_id_.end()) return false;
  auto* node = found->second;
  if (qty >= node->qty) {
    Erase(node);
  } else {
    auto& level = FindLevel(node);
    node->qty -= qty;
    level.qty -= qty;
  }
  return true;
}

qlib::price_level& Orderbook::FindLevel(const qlib::order* node) {
  if (node->side != qlib::side::buy && node->side != qlib::side::sell) {
    throw std::logic_error("存储订单方向无效");
  }
  auto& levels = node->side == qlib::side::buy ? bids_ : asks_;
  const auto found = levels.find(node->price);
  if (found == levels.end()) throw std::logic_error("存储订单缺少所属价位");
  auto& level = found->second;
  if (node->qty <= 0 || level.qty < node->qty || level.head == nullptr ||
      level.tail == nullptr) {
    throw std::logic_error("存储订单与价位数量或首尾不一致");
  }
  return level;
}

void Orderbook::UnlinkOrders(qlib::order* node) {
  auto& level = FindLevel(node);
  if ((node->prev == nullptr ? level.head != node : node->prev->next != node) ||
      (node->next == nullptr ? level.tail != node : node->next->prev != node) ||
      ((node->prev == nullptr && node->next == nullptr) != (level.qty == node->qty))) {
    throw std::logic_error("存储订单的相邻链接或价位合计不一致");
  }
  // 只检查本节点关系；整簿不变量扫描属于测试，不进入事件处理路径。
  if (node->prev != nullptr) node->prev->next = node->next;
  else level.head = node->next;
  if (node->next != nullptr) node->next->prev = node->prev;
  else level.tail = node->prev;
  level.qty -= node->qty;
  node->prev = node->next = nullptr;
  if (level.head == nullptr) {
    auto& levels = node->side == qlib::side::buy ? bids_ : asks_;
    levels.erase(node->price);
  }
}

void Orderbook::Erase(qlib::order* node) {
  const auto id = node->order_id;
  const auto found = by_id_.find(id);
  if (found == by_id_.end() || found->second != node) {
    throw std::logic_error("存储订单与 ID 索引不一致");
  }
  // 节点来自当前有效索引；只向当前 hive 取本次删除所需的迭代器。
  const auto position = orders_.get_iterator(node);
  if (position == orders_.end()) throw std::logic_error("ID 索引未指向当前订单存储");
  UnlinkOrders(node);
  by_id_.erase(id);
  orders_.erase(position);
}

void Orderbook::LinkOrders(qlib::order* node, qlib::price_level& level,
                          std::int64_t new_total) noexcept {
  node->prev = level.tail;
  node->next = nullptr;
  if (level.tail != nullptr) level.tail->next = node;
  else level.head = node;
  level.tail = node;
  level.qty = new_total;
}

qlib::book Orderbook::OnSnapshot() const {
  qlib::book result{};
  result.instrument_id = instrument_id_;
  result.event_ns = event_ns_;
  result.recv_ns = recv_ns_;
  if (!bids_.empty()) {
    result.bid_price[0] = bids_.rbegin()->first;
    result.bid_qty[0] = bids_.rbegin()->second.qty;
  }
  if (!asks_.empty()) {
    result.ask_price[0] = asks_.begin()->first;
    result.ask_qty[0] = asks_.begin()->second.qty;
  }
  return result;
}

std::size_t Orderbook::size() const noexcept { return by_id_.size(); }

std::size_t Orderbook::MemoryBytes() const noexcept {
  const auto order_bytes = SaturatingMultiply(orders_.capacity(), sizeof(qlib::order));
  const auto id_bytes = SaturatingMultiply(by_id_.capacity(), id_slot_bytes);
  // 四个指针只估计树节点管理和对齐余量，不承诺标准库实际节点布局。
  constexpr auto level_bytes = sizeof(PriceLevelMap::value_type) + 4 * sizeof(void*);
  const auto levels = SaturatingAdd(bids_.size(), asks_.size());
  return SaturatingAdd(SaturatingAdd(order_bytes, id_bytes),
                       SaturatingMultiply(levels, level_bytes));
}

void Orderbook::Touch(std::int64_t event_ns, std::int64_t recv_ns) noexcept {
  event_ns_ = event_ns;
  recv_ns_ = recv_ns;
}

}  // namespace qbook
