#include "support/OrderbookTestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <map>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

using namespace qbook;
using namespace qbook::testing;

static_assert(!std::is_copy_constructible_v<Orderbook>);
static_assert(!std::is_copy_assignable_v<Orderbook>);
static_assert(std::is_nothrow_move_constructible_v<OrderbookInspector::OrderStore>);
static_assert(std::is_nothrow_move_assignable_v<OrderbookInspector::OrderStore>);
static_assert(std::is_nothrow_move_constructible_v<OrderbookInspector::IdMap>);
static_assert(std::is_nothrow_move_assignable_v<OrderbookInspector::IdMap>);
static_assert(std::is_nothrow_move_constructible_v<OrderbookInspector::PriceLevelMap>);
static_assert(std::is_nothrow_move_assignable_v<OrderbookInspector::PriceLevelMap>);
static_assert(std::is_nothrow_move_constructible_v<Orderbook>);
static_assert(std::is_nothrow_move_assignable_v<Orderbook>);
static_assert(std::is_nothrow_destructible_v<qlib::order>);

TEST_CASE("UC02固定六步分别得到正确订单量与快照", "[orderbook][invariant]") {
  Orderbook book(1, BookMode::Mbo);
  const auto first = MakeOrder(1, qlib::side::buy, 100, 10, 1, 101, 201);
  const auto original_first = first;
  CheckResult(book.OnOrder(first));
  CheckInput(first, original_first);
  CheckSnapshot(book, 100, 10, 0, 0, 101, 201);
  REQUIRE(OrderbookInspector::Node(book, 1)->qty == 10);
  REQUIRE(OrderbookInspector::Fifo(book, qlib::side::buy, 100) == std::vector<std::int64_t>{1});
  const auto second = MakeOrder(2, qlib::side::buy, 100, 5, 1, 102, 202);
  const auto original_second = second;
  CheckResult(book.OnOrder(second));
  CheckInput(second, original_second);
  CheckSnapshot(book, 100, 15, 0, 0, 102, 202);
  REQUIRE(OrderbookInspector::Node(book, 1)->qty == 10);
  REQUIRE(OrderbookInspector::Node(book, 2)->qty == 5);
  REQUIRE(OrderbookInspector::Fifo(book, qlib::side::buy, 100) == std::vector<std::int64_t>{1, 2});
  const auto modify = MakeModify(1, qlib::side::buy, 100, 6, 1, 103, 203);
  const auto original_modify = modify;
  CheckResult(book.OnOrder(modify));
  CheckInput(modify, original_modify);
  CheckSnapshot(book, 100, 11, 0, 0, 103, 203);
  REQUIRE(OrderbookInspector::Node(book, 1)->qty == 6);
  REQUIRE(OrderbookInspector::Node(book, 2)->qty == 5);
  REQUIRE(OrderbookInspector::Fifo(book, qlib::side::buy, 100) == std::vector<std::int64_t>{1, 2});
  const auto cancel = MakeCancel(2, 2, 1, 104, 204);
  const auto original_cancel = cancel;
  CheckResult(book.OnCancel(cancel));
  CheckInput(cancel, original_cancel);
  CheckSnapshot(book, 100, 9, 0, 0, 104, 204);
  REQUIRE(OrderbookInspector::Node(book, 1)->qty == 6);
  REQUIRE(OrderbookInspector::Node(book, 2)->qty == 3);
  REQUIRE(OrderbookInspector::Fifo(book, qlib::side::buy, 100) == std::vector<std::int64_t>{1, 2});
  const auto sell = MakeOrder(3, qlib::side::sell, 101, 8, 1, 105, 205);
  const auto original_sell = sell;
  CheckResult(book.OnOrder(sell));
  CheckInput(sell, original_sell);
  CheckSnapshot(book, 100, 9, 101, 8, 105, 205);
  REQUIRE(OrderbookInspector::Node(book, 3)->qty == 8);
  const auto trade = MakeTrade(1, 3, 2, 1, 106, 206);
  const auto original_trade = trade;
  CheckResult(book.OnTrade(trade));
  CheckInput(trade, original_trade);
  CheckSnapshot(book, 100, 7, 101, 6, 106, 206);
  REQUIRE(OrderbookInspector::Node(book, 1)->qty == 4);
  REQUIRE(OrderbookInspector::Node(book, 2)->qty == 3);
  REQUIRE(OrderbookInspector::Node(book, 3)->qty == 6);
  REQUIRE(OrderbookInspector::Fifo(book, qlib::side::buy, 100) == std::vector<std::int64_t>{1, 2});
}

TEST_CASE("品种模式与未知动作误用抛参数异常且业务时间保持", "[orderbook][invariant]") {
  REQUIRE_THROWS_AS(Orderbook(1, static_cast<BookMode>(255)), std::invalid_argument);
  Orderbook mbo(1, BookMode::Mbo);
  Orderbook l2(1, BookMode::L2);
  CheckResult(mbo.OnOrder(MakeOrder(10, qlib::side::buy, 100, 5)));
  CheckResult(l2.OnLevel(MakeLevel(qlib::side::buy, 100, 7)));
  const auto mbo_state = OrderbookInspector::SnapshotState(mbo);
  const auto l2_state = OrderbookInspector::SnapshotState(l2);
  auto order = MakeOrder(11, qlib::side::buy, 101, 3, 2, 900, 901);
  auto trade = MakeTrade(10, 0, 1, 2, 900, 901);
  auto cancel = MakeCancel(10, 1, 2, 900, 901);
  auto level = MakeLevel(qlib::side::buy, 101, 3, 2, 900, 901);
  const auto original_order = order;
  const auto original_trade = trade;
  const auto original_cancel = cancel;
  const auto original_level = level;
  REQUIRE_THROWS_AS(mbo.OnOrder(order), std::invalid_argument);
  REQUIRE_THROWS_AS(mbo.OnTrade(trade), std::invalid_argument);
  REQUIRE_THROWS_AS(mbo.OnCancel(cancel), std::invalid_argument);
  REQUIRE_THROWS_AS(l2.OnLevel(level), std::invalid_argument);
  REQUIRE_THROWS_AS(l2.OnTrade(trade), std::invalid_argument);
  CheckInput(order, original_order);
  CheckInput(trade, original_trade);
  CheckInput(cancel, original_cancel);
  CheckInput(level, original_level);
  order.instrument_id = 1;
  cancel.instrument_id = 1;
  level.instrument_id = 1;
  const auto mode_order = order;
  const auto mode_cancel = cancel;
  const auto mode_level = level;
  REQUIRE_THROWS_AS(l2.OnOrder(order), std::invalid_argument);
  REQUIRE_THROWS_AS(l2.OnCancel(cancel), std::invalid_argument);
  REQUIRE_THROWS_AS(mbo.OnLevel(level), std::invalid_argument);
  CheckInput(order, mode_order);
  CheckInput(cancel, mode_cancel);
  CheckInput(level, mode_level);
  order.action = static_cast<qlib::order_action>(255);
  const auto unknown_action = order;
  REQUIRE_THROWS_AS(mbo.OnOrder(order), std::invalid_argument);
  CheckInput(order, unknown_action);
  REQUIRE(OrderbookInspector::SnapshotState(mbo) == mbo_state);
  REQUIRE(OrderbookInspector::SnapshotState(l2) == l2_state);
  CheckSnapshot(mbo, 100, 5, 0, 0, 100, 200);
  CheckSnapshot(l2, 100, 7, 0, 0, 100, 200);
}

TEST_CASE("不同品种允许相同订单ID且修改撤单互相隔离", "[orderbook][invariant]") {
  Orderbook first(1, BookMode::Mbo);
  Orderbook second(2, BookMode::Mbo);
  CheckResult(first.OnOrder(MakeOrder(10, qlib::side::buy, 100, 5)));
  CheckResult(second.OnOrder(MakeOrder(10, qlib::side::sell, 101, 8, 2)));
  const auto second_state = OrderbookInspector::SnapshotState(second);
  CheckResult(first.OnOrder(MakeModify(10, qlib::side::buy, 102, 3)));
  CheckSnapshot(first, 102, 3, 0, 0, 100, 200);
  REQUIRE(OrderbookInspector::SnapshotState(second) == second_state);
  CheckSnapshot(second, 0, 0, 101, 8, 100, 200, 2);
  const auto first_state = OrderbookInspector::SnapshotState(first);
  CheckResult(second.OnCancel(MakeCancel(10, 99, 2)));
  CheckSnapshot(second, 0, 0, 0, 0, 100, 200, 2);
  REQUIRE(OrderbookInspector::SnapshotState(first) == first_state);

  Orderbook l2_first(1, BookMode::L2);
  Orderbook l2_second(2, BookMode::L2);
  CheckResult(l2_first.OnLevel(MakeLevel(qlib::side::buy, 100, 20)));
  CheckResult(l2_second.OnLevel(MakeLevel(qlib::side::buy, 100, 8, 2)));
  CheckResult(l2_first.OnLevel(MakeLevel(qlib::side::buy, 100, 0)));
  CheckSnapshot(l2_first, 0, 0, 0, 0, 100, 200);
  CheckSnapshot(l2_second, 100, 8, 0, 0, 100, 200, 2);
}

TEST_CASE("独立移动构造保持全部业务和节点地址并允许源再赋值", "[orderbook][invariant]") {
  for (const auto mode : {BookMode::Mbo, BookMode::L2}) {
    for (const auto populated : {false, true}) {
      Orderbook source(7, mode);
      if (populated) {
        if (mode == BookMode::Mbo) {
          CheckResult(source.OnOrder(MakeOrder(1, qlib::side::buy, 100, 10, 7, 11, 12)));
          CheckResult(source.OnOrder(MakeOrder(2, qlib::side::buy, 100, 5, 7, 13, 14)));
        } else {
          CheckResult(source.OnLevel(MakeLevel(qlib::side::buy, 100, 15, 7, 13, 14)));
        }
      }
      const auto before = OrderbookInspector::SnapshotState(source);
      Orderbook target(std::move(source));
      REQUIRE(OrderbookInspector::SnapshotState(target) == before);
      CheckSnapshot(target, populated ? 100 : 0, populated ? 15 : 0, 0, 0,
                    populated ? 13 : 0, populated ? 14 : 0, 7);
      source = Orderbook(9, BookMode::Mbo);
      CheckResult(source.OnOrder(MakeOrder(1, qlib::side::sell, 111, 2, 9)));
      CheckSnapshot(source, 0, 0, 111, 2, 100, 200, 9);
      if (populated && mode == BookMode::Mbo) {
        CheckResult(target.OnOrder(MakeModify(1, qlib::side::buy, 101, 6, 7)));
        CheckResult(target.OnCancel(MakeCancel(2, 99, 7)));
        CheckResult(target.OnCancel(MakeCancel(1, 99, 7)));
        CheckSnapshot(target, 0, 0, 0, 0, 100, 200, 7);
      }
    }
  }
}

TEST_CASE("给非空目标移动赋值接管来源并释放目标旧关系", "[orderbook][invariant]") {
  Orderbook source(7, BookMode::Mbo);
  CheckResult(source.OnOrder(MakeOrder(1, qlib::side::buy, 100, 10, 7)));
  CheckResult(source.OnOrder(MakeOrder(2, qlib::side::sell, 101, 8, 7, 301, 401)));
  const auto before = OrderbookInspector::SnapshotState(source);
  Orderbook target(8, BookMode::Mbo);
  CheckResult(target.OnOrder(MakeOrder(99, qlib::side::buy, 999, 99, 8)));
  CheckResult(target.OnOrder(MakeOrder(100, qlib::side::sell, 1000, 88, 8)));
  target = std::move(source);
  REQUIRE(OrderbookInspector::SnapshotState(target) == before);
  REQUIRE(OrderbookInspector::Node(target, 99) == nullptr);
  REQUIRE(OrderbookInspector::Node(target, 100) == nullptr);
  CheckSnapshot(target, 100, 10, 101, 8, 301, 401, 7);
  source = Orderbook(9, BookMode::L2);
  CheckResult(source.OnLevel(MakeLevel(qlib::side::sell, 110, 3, 9)));
  CheckSnapshot(source, 0, 0, 110, 3, 100, 200, 9);
  CheckResult(target.OnTrade(MakeTrade(1, 2, 3, 7)));
  CheckSnapshot(target, 100, 7, 101, 5, 100, 200, 7);
  CheckResult(target.OnCancel(MakeCancel(1, 7, 7)));
  CheckResult(target.OnCancel(MakeCancel(2, 5, 7)));
  CheckSnapshot(target, 0, 0, 0, 0, 100, 200, 7);
}

TEST_CASE("外层品种表真实扩容后重查订单簿再改价和删除", "[orderbook][invariant]") {
  qlib::map<std::uint32_t, Orderbook> books;
  std::map<std::uint32_t, std::map<std::int64_t, const qlib::order*>> addresses;
  const auto insert_book = [&](std::uint32_t instrument) {
    const auto inserted = books.try_emplace(std::uint32_t{instrument}, instrument, BookMode::Mbo);
    REQUIRE(inserted.second);
    auto& book = books.find(instrument)->second;
    CheckResult(book.OnOrder(MakeOrder(1, qlib::side::buy, 100, 10, instrument)));
    CheckResult(book.OnOrder(MakeOrder(2, qlib::side::buy, 101, 4, instrument)));
    addresses[instrument][1] = OrderbookInspector::Node(book, 1);
    addresses[instrument][2] = OrderbookInspector::Node(book, 2);
    OrderbookInspector::Check(book);
  };
  for (std::uint32_t instrument = 1; instrument <= 3; ++instrument) insert_book(instrument);
  const auto original_capacity = books.capacity();
  REQUIRE(original_capacity > 0);
  for (std::uint32_t instrument = 4; instrument <= 64 && books.capacity() == original_capacity;
       ++instrument) insert_book(instrument);
  REQUIRE(books.capacity() > original_capacity);
  for (const auto& [instrument, nodes] : addresses) {
    const auto found = books.find(instrument);
    REQUIRE(found != books.end());
    auto& book = found->second;
    REQUIRE(OrderbookInspector::Node(book, 1) == nodes.at(1));
    REQUIRE(OrderbookInspector::Node(book, 2) == nodes.at(2));
    CheckSnapshot(book, 101, 4, 0, 0, 100, 200, instrument);
    CheckResult(book.OnOrder(MakeModify(1, qlib::side::buy, 101, 3, instrument)));
    REQUIRE(OrderbookInspector::Fifo(book, qlib::side::buy, 101) == std::vector<std::int64_t>{2, 1});
    CheckSnapshot(book, 101, 7, 0, 0, 100, 200, instrument);
    CheckResult(book.OnCancel(MakeCancel(2, 4, instrument)));
    CheckSnapshot(book, 101, 3, 0, 0, 100, 200, instrument);
    CheckResult(book.OnCancel(MakeCancel(1, 3, instrument)));
    CheckSnapshot(book, 0, 0, 0, 0, 100, 200, instrument);
    REQUIRE(OrderbookInspector::OrderCount(book) == 0);
  }
}

TEST_CASE("交错改价成交与首尾删除每步保持全部关系", "[orderbook][invariant]") {
  Orderbook book(1, BookMode::Mbo);
  CheckResult(book.OnOrder(MakeOrder(1, qlib::side::buy, 100, 10)));
  CheckSnapshot(book, 100, 10, 0, 0, 100, 200);
  CheckResult(book.OnOrder(MakeOrder(2, qlib::side::buy, 100, 5)));
  CheckSnapshot(book, 100, 15, 0, 0, 100, 200);
  CheckResult(book.OnOrder(MakeOrder(3, qlib::side::buy, 101, 7)));
  CheckSnapshot(book, 101, 7, 0, 0, 100, 200);
  CheckResult(book.OnOrder(MakeOrder(4, qlib::side::sell, 103, 8)));
  CheckSnapshot(book, 101, 7, 103, 8, 100, 200);
  CheckResult(book.OnOrder(MakeModify(1, qlib::side::buy, 101, 6)));
  CheckSnapshot(book, 101, 13, 103, 8, 100, 200);
  REQUIRE(OrderbookInspector::Fifo(book, qlib::side::buy, 101) == std::vector<std::int64_t>{3, 1});
  CheckResult(book.OnOrder(MakeModify(3, qlib::side::buy, 101, 4)));
  CheckSnapshot(book, 101, 10, 103, 8, 100, 200);
  CheckResult(book.OnCancel(MakeCancel(3, 4)));
  CheckSnapshot(book, 101, 6, 103, 8, 100, 200);
  REQUIRE(OrderbookInspector::Fifo(book, qlib::side::buy, 101) == std::vector<std::int64_t>{1});
  CheckResult(book.OnTrade(MakeTrade(1, 4, 2)));
  CheckSnapshot(book, 101, 4, 103, 6, 100, 200);
  CheckResult(book.OnOrder(MakeModify(2, qlib::side::buy, 102, 3)));
  CheckSnapshot(book, 102, 3, 103, 6, 100, 200);
  REQUIRE(OrderbookInspector::Fifo(book, qlib::side::buy, 100).empty());
  CheckResult(book.OnCancel(MakeCancel(2, 99)));
  CheckSnapshot(book, 101, 4, 103, 6, 100, 200);
  CheckResult(book.OnOrder(MakeModify(1, qlib::side::buy, 101, 0)));
  CheckSnapshot(book, 0, 0, 103, 6, 100, 200);
  CheckResult(book.OnCancel(MakeCancel(4, 6)));
  CheckSnapshot(book, 0, 0, 0, 0, 100, 200);
  REQUIRE(book.size() == 0);
}
