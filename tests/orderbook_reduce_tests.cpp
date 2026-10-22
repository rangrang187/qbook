#include "support/OrderbookTestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <vector>

using namespace qbook;
using namespace qbook::testing;

TEST_CASE("部分撤单按存储方向扣减并保留FIFO与输入撤销量", "[orderbook][reduce]") {
  Orderbook book(1, BookMode::Mbo);
  CheckResult(book.OnOrder(MakeOrder(1, qlib::side::buy, 100, 10)));
  CheckResult(book.OnOrder(MakeOrder(2, qlib::side::buy, 100, 5)));
  const auto* address = OrderbookInspector::Node(book, 1);
  const auto cancel = MakeCancel(1, 4, 1, 300, 400);
  const auto original = cancel;
  CheckResult(book.OnCancel(cancel));
  CheckInput(cancel, original);
  REQUIRE(OrderbookInspector::Node(book, 1) == address);
  REQUIRE(address->qty == 6);
  REQUIRE(address->side == qlib::side::buy);
  REQUIRE(address->price == 100);
  REQUIRE(OrderbookInspector::Fifo(book, qlib::side::buy, 100) == std::vector<std::int64_t>{1, 2});
  CheckSnapshot(book, 100, 11, 0, 0, 300, 400);
  const auto state = OrderbookInspector::SnapshotState(book);
  const auto unknown = MakeCancel(999, 2, 1, 0, 0);
  const auto original_unknown = unknown;
  CheckResult(book.OnCancel(unknown), ApplyStatus::Ignored, IgnoreReason::UnknownOrder);
  CheckInput(unknown, original_unknown);
  REQUIRE(OrderbookInspector::SnapshotState(book) == state);
  CheckSnapshot(book, 100, 11, 0, 0, 0, 0);
}

TEST_CASE("恰好与超额撤单安全删除首中尾及唯一节点", "[orderbook][reduce]") {
  for (const auto side : {qlib::side::buy, qlib::side::sell}) {
    for (const auto removed : {1, 2, 3, 4}) {
      for (const auto quantity : {10, 99}) {
        Orderbook book(1, BookMode::Mbo);
        for (std::int64_t id = 1; id <= 4; ++id)
          CheckResult(book.OnOrder(MakeOrder(id, side, 100, 10)));
        auto cancel = MakeCancel(removed, quantity, 1, 9, 10);
        cancel.side = side == qlib::side::buy ? qlib::side::sell : qlib::side::buy;
        const auto original = cancel;
        CheckResult(book.OnCancel(cancel));
        CheckInput(cancel, original);
        REQUIRE(OrderbookInspector::Node(book, removed) == nullptr);
        REQUIRE(book.size() == 3);
        const std::vector<std::vector<std::int64_t>> expected{
            {2, 3, 4}, {1, 3, 4}, {1, 2, 4}, {1, 2, 3}};
        REQUIRE(OrderbookInspector::Fifo(book, side, 100) == expected[removed - 1]);
        for (const auto id : expected[removed - 1]) {
          const auto erase = MakeCancel(id, 10);
          const auto original_erase = erase;
          CheckResult(book.OnCancel(erase));
          CheckInput(erase, original_erase);
          OrderbookInspector::Check(book);
        }
        REQUIRE(book.size() == 0);
        REQUIRE(OrderbookInspector::OrderCount(book) == 0);
        REQUIRE(OrderbookInspector::LevelCount(book, side) == 0);
        CheckSnapshot(book, 0, 0, 0, 0, 100, 200);
      }
    }
  }
}

TEST_CASE("双侧成交分别扣减并处理恰好及超额成交", "[orderbook][reduce]") {
  Orderbook book(1, BookMode::Mbo);
  CheckResult(book.OnOrder(MakeOrder(1, qlib::side::buy, 100, 5)));
  CheckResult(book.OnOrder(MakeOrder(2, qlib::side::sell, 101, 3)));
  const auto trade = MakeTrade(1, 2, 2, 1, 300, 400);
  const auto original = trade;
  CheckResult(book.OnTrade(trade));
  CheckInput(trade, original);
  REQUIRE(OrderbookInspector::Node(book, 1)->qty == 3);
  REQUIRE(OrderbookInspector::Node(book, 2)->qty == 1);
  REQUIRE(OrderbookInspector::Node(book, 1)->price == 100);
  REQUIRE(OrderbookInspector::Node(book, 2)->price == 101);
  CheckSnapshot(book, 100, 3, 101, 1, 300, 400);
  const auto exact = MakeTrade(0, 2, 1, 1, 20, 30);
  const auto original_exact = exact;
  CheckResult(book.OnTrade(exact));
  CheckInput(exact, original_exact);
  REQUIRE(OrderbookInspector::Node(book, 2) == nullptr);
  CheckSnapshot(book, 100, 3, 0, 0, 20, 30);
  const auto excessive = MakeTrade(1, 0, std::numeric_limits<std::int64_t>::max(), 1, 0, 0);
  const auto original_excessive = excessive;
  CheckResult(book.OnTrade(excessive));
  CheckInput(excessive, original_excessive);
  REQUIRE(book.size() == 0);
  CheckSnapshot(book, 0, 0, 0, 0, 0, 0);
}

TEST_CASE("缺失或未知成交ID不阻断另一侧且双侧未知正常忽略", "[orderbook][reduce]") {
  struct Scenario {
    std::int64_t buy;
    std::int64_t sell;
    std::int64_t bid_qty;
    std::int64_t ask_qty;
    ApplyStatus status;
    IgnoreReason reason;
  };
  const Scenario scenarios[]{
      {1, 0, 8, 12, ApplyStatus::Applied, IgnoreReason::None},
      {0, 2, 10, 10, ApplyStatus::Applied, IgnoreReason::None},
      {1, 999, 8, 12, ApplyStatus::Applied, IgnoreReason::None},
      {999, 2, 10, 10, ApplyStatus::Applied, IgnoreReason::None},
      {998, 999, 10, 12, ApplyStatus::Ignored, IgnoreReason::UnknownOrder},
      {0, 0, 10, 12, ApplyStatus::Ignored, IgnoreReason::UnknownOrder},
  };
  for (const auto& scenario : scenarios) {
    Orderbook book(1, BookMode::Mbo);
    CheckResult(book.OnOrder(MakeOrder(1, qlib::side::buy, 100, 10)));
    CheckResult(book.OnOrder(MakeOrder(2, qlib::side::sell, 101, 12)));
    const auto state = OrderbookInspector::SnapshotState(book);
    const auto trade = MakeTrade(scenario.buy, scenario.sell, 2, 1, 7, 8);
    const auto original = trade;
    CheckResult(book.OnTrade(trade), scenario.status, scenario.reason);
    CheckInput(trade, original);
    CheckSnapshot(book, 100, scenario.bid_qty, 101, scenario.ask_qty, 7, 8);
    if (scenario.status == ApplyStatus::Ignored)
      REQUIRE(OrderbookInspector::SnapshotState(book) == state);
  }
}

TEST_CASE("最优价删除回退到下一档且非最优价减少不改变最优档", "[orderbook][reduce]") {
  Orderbook book(1, BookMode::Mbo);
  CheckResult(book.OnOrder(MakeOrder(1, qlib::side::buy, 99, 4)));
  CheckResult(book.OnOrder(MakeOrder(2, qlib::side::buy, 100, 5)));
  CheckResult(book.OnOrder(MakeOrder(3, qlib::side::sell, 102, 6)));
  CheckResult(book.OnOrder(MakeOrder(4, qlib::side::sell, 101, 7)));
  CheckResult(book.OnCancel(MakeCancel(1, 1)));
  CheckSnapshot(book, 100, 5, 101, 7, 100, 200);
  CheckResult(book.OnTrade(MakeTrade(2, 4, 99)));
  CheckSnapshot(book, 99, 3, 102, 6, 100, 200);
  REQUIRE(book.size() == 2);
}
