#include "support/OrderbookTestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <vector>

using namespace qbook;
using namespace qbook::testing;

TEST_CASE("同价改单设置剩余总量并保留FIFO和新增元数据", "[orderbook][modify]") {
  Orderbook book(1, BookMode::Mbo);
  CheckResult(book.OnOrder(MakeOrder(1, qlib::side::buy, 100, 10)));
  CheckResult(book.OnOrder(MakeOrder(2, qlib::side::buy, 100, 5)));
  const auto* address = OrderbookInspector::Node(book, 1);
  auto expected_node = *address;
  qlib::order remote{};
  auto modify = MakeModify(1, qlib::side::buy, 100, 6, 1, 300, 400);
  modify.seq = 999;
  modify.prev = &remote;
  modify.next = &remote;
  const auto original = modify;
  CheckResult(book.OnOrder(modify));
  CheckInput(modify, original);
  expected_node.qty = 6;
  CheckInput(*OrderbookInspector::Node(book, 1), expected_node);
  REQUIRE(OrderbookInspector::Node(book, 1) == address);
  REQUIRE(OrderbookInspector::Fifo(book, qlib::side::buy, 100) == std::vector<std::int64_t>{1, 2});
  CheckSnapshot(book, 100, 11, 0, 0, 300, 400);

  modify = MakeModify(1, qlib::side::buy, 100, 12, 1, 20, 30);
  const auto increase = modify;
  CheckResult(book.OnOrder(modify));
  CheckInput(modify, increase);
  REQUIRE(OrderbookInspector::Node(book, 1)->qty == 12);
  REQUIRE(OrderbookInspector::Fifo(book, qlib::side::buy, 100) == std::vector<std::int64_t>{1, 2});
  CheckSnapshot(book, 100, 17, 0, 0, 20, 30);

  const auto state = OrderbookInspector::SnapshotState(book);
  modify = MakeModify(1, qlib::side::buy, 100, 12, 1, 0, 0);
  const auto unchanged = modify;
  CheckResult(book.OnOrder(modify), ApplyStatus::Ignored, IgnoreReason::Unchanged);
  CheckInput(modify, unchanged);
  REQUIRE(OrderbookInspector::SnapshotState(book) == state);
  CheckSnapshot(book, 100, 17, 0, 0, 0, 0);
}

TEST_CASE("改价保留节点地址并进入已有或新价位末尾", "[orderbook][modify]") {
  for (const auto side : {qlib::side::buy, qlib::side::sell}) {
    Orderbook book(1, BookMode::Mbo);
    CheckResult(book.OnOrder(MakeOrder(1, side, 100, 10)));
    CheckResult(book.OnOrder(MakeOrder(2, side, 100, 5)));
    CheckResult(book.OnOrder(MakeOrder(3, side, 101, 7)));
    const auto* address = OrderbookInspector::Node(book, 1);
    const auto modify = MakeModify(1, side, 101, 6, 1, 300, 400);
    const auto original = modify;
    CheckResult(book.OnOrder(modify));
    CheckInput(modify, original);
    REQUIRE(OrderbookInspector::Node(book, 1) == address);
    REQUIRE(OrderbookInspector::Node(book, 1)->qty == 6);
    REQUIRE(OrderbookInspector::Fifo(book, side, 100) == std::vector<std::int64_t>{2});
    REQUIRE(OrderbookInspector::Fifo(book, side, 101) == std::vector<std::int64_t>{3, 1});
    CheckResult(book.OnOrder(MakeModify(2, side, 102, 4, 1, 500, 600)));
    REQUIRE(OrderbookInspector::Fifo(book, side, 100).empty());
    REQUIRE(OrderbookInspector::Fifo(book, side, 102) == std::vector<std::int64_t>{2});
    REQUIRE(OrderbookInspector::LevelCount(book, side) == 2);
    CheckSnapshot(book, side == qlib::side::buy ? 102 : 0,
                  side == qlib::side::buy ? 4 : 0,
                  side == qlib::side::sell ? 101 : 0,
                  side == qlib::side::sell ? 13 : 0, 500, 600);
  }
}

TEST_CASE("未知与方向冲突改单忽略且合法非正剩余量删除", "[orderbook][modify]") {
  Orderbook book(1, BookMode::Mbo);
  CheckResult(book.OnOrder(MakeOrder(1, qlib::side::buy, 100, 10)));
  CheckResult(book.OnOrder(MakeOrder(2, qlib::side::buy, 100, 5)));
  const auto state = OrderbookInspector::SnapshotState(book);
  for (const auto quantity : {6, 0, -3}) {
    const auto event = MakeModify(1, qlib::side::sell, 999, quantity, 1, 10, 11);
    const auto original = event;
    CheckResult(book.OnOrder(event), ApplyStatus::Ignored, IgnoreReason::SideMismatch);
    CheckInput(event, original);
    REQUIRE(OrderbookInspector::SnapshotState(book) == state);
    CheckSnapshot(book, 100, 15, 0, 0, 10, 11);
  }
  const auto unknown = MakeModify(999, qlib::side::buy, 100, 4, 1, 1, 2);
  const auto original_unknown = unknown;
  CheckResult(book.OnOrder(unknown), ApplyStatus::Ignored, IgnoreReason::UnknownOrder);
  CheckInput(unknown, original_unknown);
  REQUIRE(OrderbookInspector::SnapshotState(book) == state);
  CheckSnapshot(book, 100, 15, 0, 0, 1, 2);
  const auto zero = MakeModify(1, qlib::side::buy, 0, 0, 1, 3, 4);
  const auto original_zero = zero;
  CheckResult(book.OnOrder(zero));
  CheckInput(zero, original_zero);
  REQUIRE(OrderbookInspector::Node(book, 1) == nullptr);
  REQUIRE(OrderbookInspector::Fifo(book, qlib::side::buy, 100) == std::vector<std::int64_t>{2});
  const auto negative = MakeModify(2, qlib::side::buy, -1, -5, 1, 5, 6);
  const auto original_negative = negative;
  CheckResult(book.OnOrder(negative));
  CheckInput(negative, original_negative);
  REQUIRE(book.size() == 0);
  REQUIRE(OrderbookInspector::LevelCount(book, qlib::side::buy) == 0);
  CheckSnapshot(book, 0, 0, 0, 0, 5, 6);
}

TEST_CASE("同价与改价改单检查最终合计并保持越界前完整状态", "[orderbook][modify]") {
  constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
  for (const auto side : {qlib::side::buy, qlib::side::sell}) {
    DYNAMIC_SECTION("同价替换在最大总量附近，方向=" << static_cast<int>(side)) {
      Orderbook book(1, BookMode::Mbo);
      CheckResult(book.OnOrder(MakeOrder(1, side, 100, maximum - 10)));
      CheckResult(book.OnOrder(MakeOrder(2, side, 100, 10)));
      const auto state = OrderbookInspector::SnapshotState(book);
      const auto overflow = MakeModify(1, side, 100, maximum - 9, 1, 7, 8);
      const auto original = overflow;
      CheckResult(book.OnOrder(overflow), ApplyStatus::Ignored, IgnoreReason::QuantityOverflow);
      CheckInput(overflow, original);
      REQUIRE(OrderbookInspector::SnapshotState(book) == state);
      REQUIRE(book.OnSnapshot().event_ns == 7);
      REQUIRE(book.OnSnapshot().recv_ns == 8);
      CheckResult(book.OnOrder(MakeModify(1, side, 100, maximum - 11)));
      CheckResult(book.OnOrder(MakeModify(1, side, 100, maximum - 10)));
      REQUIRE(OrderbookInspector::Node(book, 1)->qty == maximum - 10);
      OrderbookInspector::Check(book);
    }
    DYNAMIC_SECTION("改入已有价位恰好等于上界，方向=" << static_cast<int>(side)) {
      Orderbook book(1, BookMode::Mbo);
      CheckResult(book.OnOrder(MakeOrder(1, side, 100, maximum - 5)));
      CheckResult(book.OnOrder(MakeOrder(2, side, 99, 10)));
      const auto state = OrderbookInspector::SnapshotState(book);
      const auto overflow = MakeModify(2, side, 100, 6, 1, 7, 8);
      const auto original = overflow;
      CheckResult(book.OnOrder(overflow), ApplyStatus::Ignored, IgnoreReason::QuantityOverflow);
      CheckInput(overflow, original);
      REQUIRE(OrderbookInspector::SnapshotState(book) == state);
      REQUIRE(book.OnSnapshot().event_ns == 7);
      REQUIRE(book.OnSnapshot().recv_ns == 8);
      CheckResult(book.OnOrder(MakeModify(2, side, 100, 5)));
      REQUIRE(OrderbookInspector::Node(book, 2)->qty == 5);
      REQUIRE(OrderbookInspector::Fifo(book, side, 99).empty());
      REQUIRE(OrderbookInspector::Fifo(book, side, 100) == std::vector<std::int64_t>{1, 2});
      OrderbookInspector::Check(book);
    }
  }
}
