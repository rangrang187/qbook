#include "support/OrderbookTestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <vector>

using namespace qbook;
using namespace qbook::testing;

TEST_CASE("空订单簿保留品种并返回零快照", "[orderbook][add]") {
  for (const auto mode : {BookMode::Mbo, BookMode::L2}) {
    Orderbook book(0, mode);
    CheckSnapshot(book, 0, 0, 0, 0, 0, 0, 0);
    REQUIRE(book.size() == 0);
    REQUIRE(OrderbookInspector::OrderCount(book) == 0);
    REQUIRE(OrderbookInspector::LevelCount(book, qlib::side::buy) == 0);
    REQUIRE(OrderbookInspector::LevelCount(book, qlib::side::sell) == 0);
    REQUIRE(book.MemoryBytes() == 0);
  }
}

TEST_CASE("新增按同价FIFO维护全部价位并选择最优价格", "[orderbook][add]") {
  Orderbook book(1, BookMode::Mbo);
  qlib::order remote_previous{};
  qlib::order remote_next{};
  auto first = MakeOrder(1, qlib::side::buy, 100, 10);
  first.new_qty = 91;
  first.prev = &remote_previous;
  first.next = &remote_next;
  const auto original = first;
  CheckResult(book.OnOrder(first));
  CheckInput(first, original);
  REQUIRE(OrderbookInspector::Node(book, 1) != &first);
  REQUIRE(OrderbookInspector::Node(book, 1)->prev == nullptr);
  REQUIRE(OrderbookInspector::Node(book, 1)->next == nullptr);
  REQUIRE(OrderbookInspector::Node(book, 1)->new_qty == 91);
  const auto saved_snapshot = book.OnSnapshot();
  for (const auto& event : {
           MakeOrder(2, qlib::side::buy, 100, 5), MakeOrder(3, qlib::side::buy, 100, 3),
           MakeOrder(4, qlib::side::buy, 99, 40), MakeOrder(5, qlib::side::buy, 101, 7),
           MakeOrder(6, qlib::side::sell, 105, 8), MakeOrder(7, qlib::side::sell, 103, 9),
           MakeOrder(8, qlib::side::sell, 103, 2), MakeOrder(9, qlib::side::sell, 100, 4)}) {
    const auto before = event;
    CheckResult(book.OnOrder(event));
    CheckInput(event, before);
    OrderbookInspector::Check(book);
  }
  REQUIRE(OrderbookInspector::Fifo(book, qlib::side::buy, 100) == std::vector<std::int64_t>{1, 2, 3});
  REQUIRE(OrderbookInspector::Fifo(book, qlib::side::sell, 103) == std::vector<std::int64_t>{7, 8});
  REQUIRE(book.size() == 9);
  REQUIRE(OrderbookInspector::LevelCount(book, qlib::side::buy) == 3);
  REQUIRE(OrderbookInspector::LevelCount(book, qlib::side::sell) == 3);
  CheckSnapshot(book, 101, 7, 100, 4, 100, 200);
  REQUIRE(saved_snapshot.bid_price[0] == 100);
  REQUIRE(saved_snapshot.bid_qty[0] == 10);
  REQUIRE(saved_snapshot.ask_price[0] == 0);
}

TEST_CASE("重复与市价新增保留业务状态并使用最近事件时间", "[orderbook][add]") {
  Orderbook book(1, BookMode::Mbo);
  CheckResult(book.OnOrder(MakeOrder(1, qlib::side::buy, 100, 10)));
  const auto state = OrderbookInspector::SnapshotState(book);
  auto duplicate = MakeOrder(1, qlib::side::sell, 999, 30, 1, 20, 40);
  const auto original_duplicate = duplicate;
  CheckResult(book.OnOrder(duplicate), ApplyStatus::Ignored, IgnoreReason::DuplicateId);
  CheckInput(duplicate, original_duplicate);
  REQUIRE(OrderbookInspector::SnapshotState(book) == state);
  CheckSnapshot(book, 100, 10, 0, 0, 20, 40);

  auto market = MakeOrder(2, qlib::side::sell, 0, 9, 1, 0, 0);
  market.type = qlib::order_type::market;
  const auto original_market = market;
  CheckResult(book.OnOrder(market), ApplyStatus::Ignored, IgnoreReason::MarketOrder);
  CheckInput(market, original_market);
  REQUIRE(OrderbookInspector::SnapshotState(book) == state);
  CheckSnapshot(book, 100, 10, 0, 0, 0, 0);
  market.order_id = 1;
  CheckResult(book.OnOrder(market), ApplyStatus::Ignored, IgnoreReason::MarketOrder);
}

TEST_CASE("新增合计允许INT64_MAX并整体忽略超界订单", "[orderbook][add]") {
  constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
  for (const auto side : {qlib::side::buy, qlib::side::sell}) {
    Orderbook book(1, BookMode::Mbo);
    CheckResult(book.OnOrder(MakeOrder(1, side, 100, maximum - 1)));
    CheckResult(book.OnOrder(MakeOrder(2, side, 100, 1)));
    const auto state = OrderbookInspector::SnapshotState(book);
    const auto overflow = MakeOrder(3, side, 100, 1, 1, 7, 8);
    const auto original = overflow;
    CheckResult(book.OnOrder(overflow), ApplyStatus::Ignored, IgnoreReason::QuantityOverflow);
    CheckInput(overflow, original);
    REQUIRE(OrderbookInspector::SnapshotState(book) == state);
    REQUIRE(OrderbookInspector::Node(book, 3) == nullptr);
    CheckSnapshot(book, side == qlib::side::buy ? 100 : 0,
                  side == qlib::side::buy ? maximum : 0,
                  side == qlib::side::sell ? 100 : 0,
                  side == qlib::side::sell ? maximum : 0, 7, 8);
  }
}

TEST_CASE("内存估计使用实际容量和全部价位数量", "[orderbook][add]") {
  Orderbook book(1, BookMode::Mbo);
  const auto check_estimate = [&] {
    const auto order_bytes = OrderbookInspector::OrderCapacity(book) * sizeof(qlib::order);
    const auto id_bytes = OrderbookInspector::IdCapacity(book)
        * (sizeof(OrderbookInspector::IdMap::value_type) + sizeof(std::uint8_t));
    const auto level_count = OrderbookInspector::LevelCount(book, qlib::side::buy)
        + OrderbookInspector::LevelCount(book, qlib::side::sell);
    const auto level_bytes = level_count
        * (sizeof(OrderbookInspector::PriceLevelMap::value_type) + 4 * sizeof(void*));
    REQUIRE(book.MemoryBytes() == order_bytes + id_bytes + level_bytes);
    OrderbookInspector::Check(book);
  };
  check_estimate();
  const qlib::order* first_address = nullptr;
  for (std::int64_t id = 1; id <= 35; ++id) {
    CheckResult(book.OnOrder(MakeOrder(id, id % 2 ? qlib::side::buy : qlib::side::sell,
                                      100 + id % 5, id)));
    if (id == 1) first_address = OrderbookInspector::Node(book, 1);
    REQUIRE(OrderbookInspector::Node(book, 1) == first_address);
    check_estimate();
  }
  REQUIRE(OrderbookInspector::OrderCapacity(book) > 8);
  REQUIRE(OrderbookInspector::IdCapacity(book) > 8);
}
