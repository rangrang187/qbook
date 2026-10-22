#include "support/OrderbookTestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>

using namespace qbook;
using namespace qbook::testing;

TEST_CASE("UC03聚合量20到8成交3仍为8最后0删除", "[orderbook][l2]") {
  Orderbook book(1, BookMode::L2);
  const auto first = MakeLevel(qlib::side::buy, 100, 20, 1, 101, 201);
  const auto original_first = first;
  CheckResult(book.OnLevel(first));
  CheckInput(first, original_first);
  CheckSnapshot(book, 100, 20, 0, 0, 101, 201);
  REQUIRE(book.size() == 0);
  const auto second = MakeLevel(qlib::side::buy, 100, 8, 1, 102, 202);
  const auto original_second = second;
  CheckResult(book.OnLevel(second));
  CheckInput(second, original_second);
  CheckSnapshot(book, 100, 8, 0, 0, 102, 202);
  REQUIRE(book.size() == 0);
  const auto before_trade = OrderbookInspector::SnapshotState(book);
  const auto trade = MakeTrade(10, 11, 3, 1, 103, 203);
  const auto original_trade = trade;
  CheckResult(book.OnTrade(trade), ApplyStatus::Ignored, IgnoreReason::L2Trade);
  CheckInput(trade, original_trade);
  REQUIRE(OrderbookInspector::SnapshotState(book) == before_trade);
  CheckSnapshot(book, 100, 8, 0, 0, 103, 203);
  REQUIRE(book.size() == 0);
  const auto remove = MakeLevel(qlib::side::buy, 100, 0, 1, 104, 204);
  const auto original_remove = remove;
  CheckResult(book.OnLevel(remove));
  CheckInput(remove, original_remove);
  CheckSnapshot(book, 0, 0, 0, 0, 104, 204);
  REQUIRE(book.size() == 0);
  REQUIRE(OrderbookInspector::OrderCapacity(book) == 0);
  REQUIRE(OrderbookInspector::IdCapacity(book) == 0);
}

TEST_CASE("L2相同量与未知删除忽略且非正量删除已有价位", "[orderbook][l2]") {
  for (const auto side : {qlib::side::buy, qlib::side::sell}) {
    Orderbook book(1, BookMode::L2);
    CheckResult(book.OnLevel(MakeLevel(side, 100, 8)));
    const auto state = OrderbookInspector::SnapshotState(book);
    const auto unchanged = MakeLevel(side, 100, 8, 1, 20, 30);
    const auto original_unchanged = unchanged;
    CheckResult(book.OnLevel(unchanged), ApplyStatus::Ignored, IgnoreReason::Unchanged);
    CheckInput(unchanged, original_unchanged);
    REQUIRE(OrderbookInspector::SnapshotState(book) == state);
    REQUIRE(book.OnSnapshot().event_ns == 20);
    REQUIRE(book.OnSnapshot().recv_ns == 30);
    for (const auto quantity : {0, -3}) {
      const auto missing = MakeLevel(side, 999, quantity, 1, 0, 0);
      const auto original_missing = missing;
      CheckResult(book.OnLevel(missing), ApplyStatus::Ignored, IgnoreReason::MissingLevel);
      CheckInput(missing, original_missing);
      REQUIRE(OrderbookInspector::SnapshotState(book) == state);
      REQUIRE(book.OnSnapshot().event_ns == 0);
      REQUIRE(book.OnSnapshot().recv_ns == 0);
    }
    const auto remove = MakeLevel(side, 100, -9, 1, 7, 8);
    const auto original_remove = remove;
    CheckResult(book.OnLevel(remove));
    CheckInput(remove, original_remove);
    CheckSnapshot(book, 0, 0, 0, 0, 7, 8);
    REQUIRE(OrderbookInspector::LevelCount(book, side) == 0);
    CheckResult(book.OnLevel(remove), ApplyStatus::Ignored, IgnoreReason::MissingLevel);
    CheckInput(remove, original_remove);
    REQUIRE(book.size() == 0);
  }
}

TEST_CASE("L2买卖同价独立多价位选择最优且成交不扣深度", "[orderbook][l2]") {
  Orderbook book(1, BookMode::L2);
  for (const auto& event : {
           MakeLevel(qlib::side::buy, 99, 4), MakeLevel(qlib::side::buy, 100, 5),
           MakeLevel(qlib::side::buy, 101, 6), MakeLevel(qlib::side::sell, 100, 7),
           MakeLevel(qlib::side::sell, 102, 8), MakeLevel(qlib::side::sell, 103, 9)}) {
    const auto original = event;
    CheckResult(book.OnLevel(event));
    CheckInput(event, original);
    OrderbookInspector::Check(book);
    REQUIRE(book.size() == 0);
  }
  CheckSnapshot(book, 101, 6, 100, 7, 100, 200);
  const auto saved_snapshot = book.OnSnapshot();
  CheckResult(book.OnLevel(MakeLevel(qlib::side::buy, 99, 40)));
  CheckSnapshot(book, 101, 6, 100, 7, 100, 200);
  const auto state = OrderbookInspector::SnapshotState(book);
  const auto trade = MakeTrade(0, 0, 999, 1, 50, 60);
  const auto original = trade;
  CheckResult(book.OnTrade(trade), ApplyStatus::Ignored, IgnoreReason::L2Trade);
  CheckInput(trade, original);
  REQUIRE(OrderbookInspector::SnapshotState(book) == state);
  CheckSnapshot(book, 101, 6, 100, 7, 50, 60);
  CheckResult(book.OnLevel(MakeLevel(qlib::side::buy, 101, 0)));
  CheckResult(book.OnLevel(MakeLevel(qlib::side::sell, 100, 0)));
  CheckSnapshot(book, 100, 5, 102, 8, 100, 200);
  REQUIRE(saved_snapshot.bid_price[0] == 101);
  REQUIRE(saved_snapshot.bid_qty[0] == 6);
  REQUIRE(saved_snapshot.ask_price[0] == 100);
  REQUIRE(saved_snapshot.ask_qty[0] == 7);
  REQUIRE(OrderbookInspector::OrderCount(book) == 0);
  REQUIRE(OrderbookInspector::IdCount(book) == 0);
  const auto expected_memory = std::size_t{4}
      * (sizeof(OrderbookInspector::PriceLevelMap::value_type) + 4 * sizeof(void*));
  REQUIRE(book.MemoryBytes() == expected_memory);
  CheckResult(book.OnLevel(MakeLevel(qlib::side::buy, 100,
                                   std::numeric_limits<std::int64_t>::max())));
  CheckSnapshot(book, 100, std::numeric_limits<std::int64_t>::max(), 102, 8, 100, 200);
}
