#include "PipelineTypes.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <type_traits>
#include <utility>
#include <variant>

namespace {

static_assert(std::is_same_v<qbook::FeedQueue::value_type, qlib::feed_event>);
static_assert(std::is_same_v<qbook::RecordQueue::value_type, qlib::record>);
static_assert(!std::is_copy_constructible_v<qbook::FeedQueue>);
static_assert(!std::is_copy_assignable_v<qbook::FeedQueue>);
static_assert(!std::is_move_constructible_v<qbook::FeedQueue>);
static_assert(!std::is_move_assignable_v<qbook::FeedQueue>);
static_assert(!std::is_copy_constructible_v<qbook::RecordQueue>);
static_assert(!std::is_copy_assignable_v<qbook::RecordQueue>);
static_assert(!std::is_move_constructible_v<qbook::RecordQueue>);
static_assert(!std::is_move_assignable_v<qbook::RecordQueue>);
static_assert(std::is_nothrow_move_constructible_v<qlib::feed_event>);
static_assert(std::is_nothrow_destructible_v<qlib::feed_event>);
static_assert(std::is_nothrow_move_constructible_v<qlib::record>);
static_assert(std::is_nothrow_destructible_v<qlib::record>);

struct MoveProbe {
  int value;
  int* moves;

  MoveProbe(int initial_value, int& move_count) noexcept
      : value(initial_value), moves(&move_count) {}
  MoveProbe(const MoveProbe&) = delete;
  MoveProbe& operator=(const MoveProbe&) = delete;
  MoveProbe(MoveProbe&& other) noexcept
      : value(std::exchange(other.value, -1)), moves(other.moves) {
    ++*moves;
  }
  ~MoveProbe() noexcept = default;
};

static_assert(std::is_nothrow_move_constructible_v<MoveProbe>);
static_assert(std::is_nothrow_destructible_v<MoveProbe>);

}  // namespace

TEST_CASE("记录值初始化清零全部命名字段", "[contracts][initialization]") {
  SECTION("order") {
    const qlib::order value{};
    CHECK(value.seq == 0);
    CHECK(value.order_id == 0);
    CHECK(value.price == 0);
    CHECK(value.qty == 0);
    CHECK(value.new_qty == 0);
    CHECK(value.prev == nullptr);
    CHECK(value.next == nullptr);
    CHECK(value.instrument_id == 0);
    CHECK(value.side == qlib::side::buy);
    CHECK(value.type == qlib::order_type::limit);
    CHECK(value.action == qlib::order_action::add);
    CHECK(value.event_ns == 0);
    CHECK(value.recv_ns == 0);
  }
  SECTION("trade") {
    const qlib::trade value{};
    CHECK(value.seq == 0);
    CHECK(value.buy_order_id == 0);
    CHECK(value.sell_order_id == 0);
    CHECK(value.price == 0);
    CHECK(value.qty == 0);
    CHECK(value.event_ns == 0);
    CHECK(value.instrument_id == 0);
    CHECK(value.side == qlib::side::buy);
    CHECK(value.recv_ns == 0);
  }
  SECTION("cancel") {
    const qlib::cancel value{};
    CHECK(value.seq == 0);
    CHECK(value.order_id == 0);
    CHECK(value.qty == 0);
    CHECK(value.instrument_id == 0);
    CHECK(value.side == qlib::side::buy);
    CHECK(value.event_ns == 0);
    CHECK(value.recv_ns == 0);
  }
  SECTION("level") {
    const qlib::level value{};
    CHECK(value.seq == 0);
    CHECK(value.price == 0);
    CHECK(value.qty == 0);
    CHECK(value.event_ns == 0);
    CHECK(value.instrument_id == 0);
    CHECK(value.side == qlib::side::buy);
    CHECK(value.recv_ns == 0);
  }
  SECTION("book") {
    const qlib::book value{};
    CHECK(value.event_ns == 0);
    CHECK(value.bid_price[0] == 0);
    CHECK(value.bid_qty[0] == 0);
    CHECK(value.ask_price[0] == 0);
    CHECK(value.ask_qty[0] == 0);
    CHECK(value.instrument_id == 0);
    CHECK(value.recv_ns == 0);
  }
  SECTION("metrics") {
    const qlib::metrics value{};
    CHECK(value.ts_ns == 0);
    CHECK(value.feed_messages == 0);
    CHECK(value.feed_bytes == 0);
    CHECK(value.feed_orders == 0);
    CHECK(value.feed_trades == 0);
    CHECK(value.feed_levels == 0);
    CHECK(value.feed_dropped == 0);
    CHECK(value.book_events == 0);
    CHECK(value.book_apply_ns == 0);
    CHECK(value.book_samples == 0);
    CHECK(value.book_instruments == 0);
    CHECK(value.book_resting_orders == 0);
    CHECK(value.book_memory_bytes == 0);
    CHECK(value.writer_orders == 0);
    CHECK(value.writer_trades == 0);
    CHECK(value.writer_levels == 0);
    CHECK(value.writer_books == 0);
  }
  SECTION("price_level") {
    const qlib::price_level value{};
    CHECK(value.qty == 0);
    CHECK(value.head == nullptr);
    CHECK(value.tail == nullptr);
  }
  SECTION("variant") {
    const qlib::feed_event feed{};
    const qlib::record record{};
    REQUIRE(std::holds_alternative<qlib::order>(feed));
    REQUIRE(std::holds_alternative<qlib::order>(record));
    CHECK(std::get<qlib::order>(feed).seq == 0);
    CHECK(std::get<qlib::order>(record).prev == nullptr);
  }
}

TEST_CASE("FeedQueue 提供全部取整容量并保持事件类型和 FIFO", "[contracts][queue]") {
  qbook::FeedQueue queue(3);
  REQUIRE(queue.capacity() == 4);
  REQUIRE_FALSE(queue.try_pop().has_value());

  qlib::order order{};
  order.seq = 11;
  qlib::trade trade{};
  trade.seq = 12;
  qlib::level level{};
  level.seq = 13;
  qlib::cancel cancel{};
  cancel.seq = 14;
  REQUIRE(queue.try_push(qlib::feed_event{order}));
  REQUIRE(queue.try_push(qlib::feed_event{trade}));
  REQUIRE(queue.try_push(qlib::feed_event{level}));
  REQUIRE(queue.try_push(qlib::feed_event{cancel}));
  CHECK_FALSE(queue.try_push(qlib::feed_event{order}));

  auto first = queue.try_pop();
  REQUIRE(first.has_value());
  REQUIRE(std::holds_alternative<qlib::order>(*first));
  CHECK(std::get<qlib::order>(*first).seq == 11);
  auto second = queue.try_pop();
  REQUIRE(second.has_value());
  REQUIRE(std::holds_alternative<qlib::trade>(*second));
  CHECK(std::get<qlib::trade>(*second).seq == 12);
  auto third = queue.try_pop();
  REQUIRE(third.has_value());
  REQUIRE(std::holds_alternative<qlib::level>(*third));
  CHECK(std::get<qlib::level>(*third).seq == 13);
  auto fourth = queue.try_pop();
  REQUIRE(fourth.has_value());
  REQUIRE(std::holds_alternative<qlib::cancel>(*fourth));
  CHECK(std::get<qlib::cancel>(*fourth).seq == 14);
  CHECK_FALSE(queue.try_pop().has_value());

  for (std::int64_t seq = 21; seq <= 24; ++seq) {
    order.seq = seq;
    REQUIRE(queue.try_push(qlib::feed_event{order}));
  }
  CHECK_FALSE(queue.try_push(qlib::feed_event{order}));
  for (std::int64_t seq = 21; seq <= 24; ++seq) {
    auto value = queue.try_pop();
    REQUIRE(value.has_value());
    REQUIRE(std::holds_alternative<qlib::order>(*value));
    CHECK(std::get<qlib::order>(*value).seq == seq);
  }
  CHECK_FALSE(queue.try_pop().has_value());
  CHECK(std::get<qlib::order>(*first).seq == 11);
}

TEST_CASE("RecordQueue 提供全部取整容量并保留独立快照", "[contracts][queue]") {
  qbook::RecordQueue queue(3);
  REQUIRE(queue.capacity() == 4);
  REQUIRE_FALSE(queue.try_pop().has_value());

  qlib::order order{};
  order.seq = 31;
  qlib::trade trade{};
  trade.seq = 32;
  qlib::level level{};
  level.seq = 33;
  qlib::cancel cancel{};
  cancel.seq = 34;
  REQUIRE(queue.try_push(qlib::record{order}));
  REQUIRE(queue.try_push(qlib::record{trade}));
  REQUIRE(queue.try_push(qlib::record{level}));
  REQUIRE(queue.try_push(qlib::record{cancel}));
  CHECK_FALSE(queue.try_push(qlib::record{order}));

  auto first = queue.try_pop();
  REQUIRE(first.has_value());
  REQUIRE(std::holds_alternative<qlib::order>(*first));
  CHECK(std::get<qlib::order>(*first).seq == 31);
  auto second = queue.try_pop();
  REQUIRE(second.has_value());
  REQUIRE(std::holds_alternative<qlib::trade>(*second));
  CHECK(std::get<qlib::trade>(*second).seq == 32);
  auto third = queue.try_pop();
  REQUIRE(third.has_value());
  REQUIRE(std::holds_alternative<qlib::level>(*third));
  CHECK(std::get<qlib::level>(*third).seq == 33);
  auto fourth = queue.try_pop();
  REQUIRE(fourth.has_value());
  REQUIRE(std::holds_alternative<qlib::cancel>(*fourth));
  CHECK(std::get<qlib::cancel>(*fourth).seq == 34);
  CHECK_FALSE(queue.try_pop().has_value());

  qlib::book snapshot{};
  snapshot.instrument_id = 7;
  snapshot.bid_price[0] = 123;
  snapshot.bid_qty[0] = 456;
  for (std::int64_t event_ns = 41; event_ns <= 44; ++event_ns) {
    snapshot.event_ns = event_ns;
    REQUIRE(queue.try_push(qlib::record{snapshot}));
  }
  CHECK_FALSE(queue.try_push(qlib::record{snapshot}));
  snapshot.bid_qty[0] = 999;
  for (std::int64_t event_ns = 41; event_ns <= 44; ++event_ns) {
    auto value = queue.try_pop();
    REQUIRE(value.has_value());
    REQUIRE(std::holds_alternative<qlib::book>(*value));
    const auto& book = std::get<qlib::book>(*value);
    CHECK(book.event_ns == event_ns);
    CHECK(book.instrument_id == 7);
    CHECK(book.bid_price[0] == 123);
    CHECK(book.bid_qty[0] == 456);
  }
  CHECK_FALSE(queue.try_pop().has_value());
  CHECK(std::get<qlib::order>(*first).seq == 31);
}

TEST_CASE("满队列拒绝推送时保留候选对象且不调用移动构造", "[contracts][queue]") {
  int moves = 0;
  qlib::single_queue<MoveProbe> queue(1);
  REQUIRE(queue.try_emplace(51, moves));
  MoveProbe candidate(52, moves);
  const int moves_before = moves;
  CHECK_FALSE(queue.try_push(std::move(candidate)));
  CHECK(candidate.value == 52);
  CHECK(moves == moves_before);

  auto first = queue.try_pop();
  REQUIRE(first.has_value());
  CHECK(first->value == 51);
  REQUIRE(queue.try_push(std::move(candidate)));
  CHECK(candidate.value == -1);
  auto second = queue.try_pop();
  REQUIRE(second.has_value());
  CHECK(second->value == 52);
  CHECK_FALSE(queue.try_pop().has_value());
}
