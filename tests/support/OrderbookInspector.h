#pragma once

#include "Orderbook.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <type_traits>
#include <vector>

namespace qbook::testing {

// 只读检查存储、索引和全部价位；借用地址仅在对应订单存活期间有效。
struct OrderbookInspector {
  using OrderStore = Orderbook::OrderStore;
  using IdMap = Orderbook::IdMap;
  using PriceLevelMap = Orderbook::PriceLevelMap;

  using OrderFields = std::tuple<std::int64_t, std::int64_t, std::int64_t,
                                 std::int64_t, std::int64_t, qlib::order*, qlib::order*,
                                 std::uint32_t, qlib::side, qlib::order_type,
                                 qlib::order_action, std::int64_t, std::int64_t>;

  static OrderFields Fields(const qlib::order& value) {
    return std::tuple{value.seq, value.order_id, value.price, value.qty,
                      value.new_qty, value.prev, value.next, value.instrument_id,
                      value.side, value.type, value.action, value.event_ns,
                      value.recv_ns};
  }

  struct NodeState {
    const qlib::order* address;
    OrderFields fields;
    bool operator==(const NodeState&) const = default;
  };

  struct LevelState {
    std::int64_t price;
    std::int64_t qty;
    const qlib::order* head;
    const qlib::order* tail;
    bool operator==(const LevelState&) const = default;
  };

  // 摘要保留全部业务字段和借用关系，容量与最近事件时间单独检查。
  struct BusinessState {
    std::uint32_t instrument_id;
    BookMode mode;
    std::map<std::int64_t, NodeState> nodes;
    std::map<std::int64_t, const qlib::order*> index;
    std::vector<LevelState> bids;
    std::vector<LevelState> asks;
    bool operator==(const BusinessState&) const = default;
  };

  static std::size_t OrderCapacity(const Orderbook& book) { return book.orders_.capacity(); }
  static std::size_t IdCapacity(const Orderbook& book) { return book.by_id_.capacity(); }
  static std::size_t OrderCount(const Orderbook& book) { return book.orders_.size(); }
  static std::size_t IdCount(const Orderbook& book) { return book.by_id_.size(); }
  static std::size_t LevelCount(const Orderbook& book, qlib::side side) {
    return Levels(book, side).size();
  }

  static const qlib::order* Node(const Orderbook& book, std::int64_t id) {
    const auto found = book.by_id_.find(id);
    if (found == book.by_id_.end()) return nullptr;
    const auto live = Live(book);
    REQUIRE(live.contains(found->second));
    return found->second;
  }

  static void Check(const Orderbook& book) {
    const auto live = Live(book);
    REQUIRE(live.size() == book.orders_.size());
    REQUIRE(book.size() == book.by_id_.size());
    REQUIRE(live.size() == book.by_id_.size());
    std::set<const qlib::order*> indexed;
    for (const auto& [id, node] : book.by_id_) {
      REQUIRE(live.contains(node));
      REQUIRE(node->order_id == id);
      REQUIRE(node->instrument_id == book.instrument_id_);
      REQUIRE(indexed.insert(node).second);
    }
    REQUIRE(indexed == live);

    std::set<const qlib::order*> chained;
    for (const auto side : {qlib::side::buy, qlib::side::sell}) {
      for (const auto& [price, level] : Levels(book, side)) {
        REQUIRE(price > 0);
        REQUIRE(level.qty > 0);
        if (book.mode_ == BookMode::L2) {
          REQUIRE(level.head == nullptr);
          REQUIRE(level.tail == nullptr);
          continue;
        }
        REQUIRE(live.contains(level.head));
        REQUIRE(live.contains(level.tail));
        REQUIRE(level.head->prev == nullptr);
        REQUIRE(level.tail->next == nullptr);
        const qlib::order* previous = nullptr;
        const qlib::order* node = level.head;
        std::size_t visited = 0;
        std::int64_t total = 0;
        while (node != nullptr) {
          REQUIRE(visited < live.size());
          REQUIRE(live.contains(node));
          REQUIRE(chained.insert(node).second);
          REQUIRE(node->prev == previous);
          REQUIRE(node->side == side);
          REQUIRE(node->price == price);
          REQUIRE(node->qty > 0);
          REQUIRE(node->qty <= std::numeric_limits<std::int64_t>::max() - total);
          total += node->qty;
          if (node->next != nullptr) {
            REQUIRE(live.contains(node->next));
            REQUIRE(node->next->prev == node);
          }
          previous = node;
          node = node->next;
          ++visited;
        }
        REQUIRE(previous == level.tail);
        REQUIRE(total == level.qty);
      }
    }
    REQUIRE(chained == live);
    if (book.mode_ == BookMode::L2) REQUIRE(live.empty());

    const auto snapshot = book.OnSnapshot();
    REQUIRE(snapshot.instrument_id == book.instrument_id_);
    REQUIRE(snapshot.event_ns == book.event_ns_);
    REQUIRE(snapshot.recv_ns == book.recv_ns_);
    REQUIRE(snapshot.bid_price[0] == (book.bids_.empty() ? 0 : book.bids_.rbegin()->first));
    REQUIRE(snapshot.bid_qty[0] == (book.bids_.empty() ? 0 : book.bids_.rbegin()->second.qty));
    REQUIRE(snapshot.ask_price[0] == (book.asks_.empty() ? 0 : book.asks_.begin()->first));
    REQUIRE(snapshot.ask_qty[0] == (book.asks_.empty() ? 0 : book.asks_.begin()->second.qty));
  }

  static BusinessState SnapshotState(const Orderbook& book) {
    Check(book);
    BusinessState state{book.instrument_id_, book.mode_, {}, {}, {}, {}};
    for (const auto& value : book.orders_)
      state.nodes.emplace(value.order_id, NodeState{std::addressof(value), Fields(value)});
    for (const auto& [id, node] : book.by_id_) state.index.emplace(id, node);
    for (const auto& [price, level] : book.bids_)
      state.bids.push_back({price, level.qty, level.head, level.tail});
    for (const auto& [price, level] : book.asks_)
      state.asks.push_back({price, level.qty, level.head, level.tail});
    return state;
  }

  static std::vector<std::int64_t> Fifo(const Orderbook& book, qlib::side side,
                                      std::int64_t price) {
    Check(book);
    std::vector<std::int64_t> result;
    const auto& levels = Levels(book, side);
    const auto found = levels.find(price);
    if (found == levels.end()) return result;
    const auto live = Live(book);
    auto* node = found->second.head;
    while (node != nullptr) {
      REQUIRE(result.size() < live.size());
      REQUIRE(live.contains(node));
      result.push_back(node->order_id);
      node = node->next;
    }
    return result;
  }

 private:
  static const PriceLevelMap& Levels(const Orderbook& book, qlib::side side) {
    return side == qlib::side::buy ? book.bids_ : book.asks_;
  }

  static std::set<const qlib::order*> Live(const Orderbook& book) {
    std::set<const qlib::order*> live;
    for (const auto& value : book.orders_) live.insert(std::addressof(value));
    return live;
  }
};

}  // namespace qbook::testing
