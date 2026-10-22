#pragma once

#include "OrderbookInspector.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <tuple>

namespace qbook::testing {

inline qlib::order MakeOrder(std::int64_t id, qlib::side side, std::int64_t price,
                             std::int64_t qty, std::uint32_t instrument = 1,
                             std::int64_t event = 100, std::int64_t recv = 200) {
  qlib::order value{};
  value.seq = 17;
  value.order_id = id;
  value.side = side;
  value.price = price;
  value.qty = qty;
  value.instrument_id = instrument;
  value.type = qlib::order_type::limit;
  value.action = qlib::order_action::add;
  value.event_ns = event;
  value.recv_ns = recv;
  return value;
}

inline qlib::order MakeModify(std::int64_t id, qlib::side side, std::int64_t price,
                              std::int64_t new_qty, std::uint32_t instrument = 1,
                              std::int64_t event = 100, std::int64_t recv = 200) {
  auto value = MakeOrder(id, side, price, 777, instrument, event, recv);
  value.action = qlib::order_action::modify;
  value.new_qty = new_qty;
  return value;
}

inline qlib::trade MakeTrade(std::int64_t buy, std::int64_t sell, std::int64_t qty,
                             std::uint32_t instrument = 1,
                             std::int64_t event = 100, std::int64_t recv = 200) {
  return qlib::trade{23, buy, sell, 888, qty, event, instrument, qlib::side::sell, recv};
}

inline qlib::cancel MakeCancel(std::int64_t id, std::int64_t qty,
                               std::uint32_t instrument = 1,
                               std::int64_t event = 100, std::int64_t recv = 200) {
  return qlib::cancel{29, id, qty, instrument, qlib::side::sell, event, recv};
}

inline qlib::level MakeLevel(qlib::side side, std::int64_t price, std::int64_t qty,
                             std::uint32_t instrument = 1,
                             std::int64_t event = 100, std::int64_t recv = 200) {
  return qlib::level{31, price, qty, event, instrument, side, recv};
}

inline void CheckInput(const qlib::order& actual, const qlib::order& expected) {
  REQUIRE(OrderbookInspector::Fields(actual) == OrderbookInspector::Fields(expected));
}

inline void CheckInput(const qlib::trade& actual, const qlib::trade& expected) {
  REQUIRE(std::tie(actual.seq, actual.buy_order_id, actual.sell_order_id, actual.price,
                   actual.qty, actual.event_ns, actual.instrument_id, actual.side, actual.recv_ns)
          == std::tie(expected.seq, expected.buy_order_id, expected.sell_order_id, expected.price,
                      expected.qty, expected.event_ns, expected.instrument_id, expected.side, expected.recv_ns));
}

inline void CheckInput(const qlib::cancel& actual, const qlib::cancel& expected) {
  REQUIRE(std::tie(actual.seq, actual.order_id, actual.qty, actual.instrument_id,
                   actual.side, actual.event_ns, actual.recv_ns)
          == std::tie(expected.seq, expected.order_id, expected.qty, expected.instrument_id,
                      expected.side, expected.event_ns, expected.recv_ns));
}

inline void CheckInput(const qlib::level& actual, const qlib::level& expected) {
  REQUIRE(std::tie(actual.seq, actual.price, actual.qty, actual.event_ns,
                   actual.instrument_id, actual.side, actual.recv_ns)
          == std::tie(expected.seq, expected.price, expected.qty, expected.event_ns,
                      expected.instrument_id, expected.side, expected.recv_ns));
}

inline void CheckResult(ApplyResult result, ApplyStatus status = ApplyStatus::Applied,
                         IgnoreReason reason = IgnoreReason::None) {
  REQUIRE(result.status == status);
  REQUIRE(result.reason == reason);
}

inline void CheckSnapshot(const Orderbook& book, std::int64_t bid_price,
                           std::int64_t bid_qty, std::int64_t ask_price,
                           std::int64_t ask_qty, std::int64_t event,
                           std::int64_t recv, std::uint32_t instrument = 1) {
  const auto value = book.OnSnapshot();
  REQUIRE(value.instrument_id == instrument);
  REQUIRE(value.bid_price[0] == bid_price);
  REQUIRE(value.bid_qty[0] == bid_qty);
  REQUIRE(value.ask_price[0] == ask_price);
  REQUIRE(value.ask_qty[0] == ask_qty);
  REQUIRE(value.event_ns == event);
  REQUIRE(value.recv_ns == recv);
  OrderbookInspector::Check(book);
}

}  // namespace qbook::testing
