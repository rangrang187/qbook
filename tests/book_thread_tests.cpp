#include "Threads/BookThread.h"
#include "support/BookThreadInspector.h"
#include "support/OrderbookTestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using namespace std::chrono_literals;
using namespace qbook;
using namespace qbook::testing;

struct SingleStep {
  FeedQueue input{2};
  RecordQueue output{128};
  Metrics metrics;
  RunControl control;
  BookThread stage;

  explicit SingleStep(BookMode mode = BookMode::Mbo) : stage(input, output, mode, 1h) {}

  qlib::record Apply(qlib::feed_event event) {
    REQUIRE(BookThreadInspector::Consume(stage, event, metrics, control));
    auto record = output.try_pop();
    REQUIRE(record);
    std::visit([&](const auto& expected) {
      using T = std::decay_t<decltype(expected)>;
      REQUIRE(std::holds_alternative<T>(*record));
      CheckInput(std::get<T>(*record), expected);
    }, event);
    REQUIRE_FALSE(output.try_pop());
    return *record;
  }

  std::vector<qlib::book> Snapshots() {
    REQUIRE(BookThreadInspector::Snapshot(stage, metrics, control));
    std::vector<qlib::book> result;
    while (auto value = output.try_pop()) {
      REQUIRE(std::holds_alternative<qlib::book>(*value));
      result.push_back(std::get<qlib::book>(*value));
    }
    return result;
  }
};

template <typename Predicate>
bool WaitUntil(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(100us);
  }
  return true;
}

// 异常断言也会先广播 Fatal，解除 Book 的满队列等待，再等待线程结束。
struct BookWorker {
  RunControl& control;
  std::atomic<bool> done{false};
  std::jthread worker;

  template <typename Function>
  BookWorker(RunControl& shared, Function function) : control(shared) {
    worker = std::jthread([this, function = std::move(function)](std::stop_token stop) {
      try { function(stop); }
      catch (...) { control.Fail("book", std::current_exception()); }
      done.store(true, std::memory_order_release);
    });
  }

  ~BookWorker() {
    if (!worker.joinable()) return;
    control.Fail("test-cleanup", std::make_exception_ptr(std::runtime_error("Book 测试提前结束")));
    worker.request_stop();
    worker.join();
  }

  void StopAndJoin() {
    worker.request_stop();
    const bool finished = WaitUntil([&] { return done.load(std::memory_order_acquire); });
    if (!finished)
      control.Fail("test-timeout", std::make_exception_ptr(std::runtime_error("Book 退出超时")));
    worker.join();
    REQUIRE(finished);
  }
};

void CheckBook(const qlib::book& value, std::uint32_t id, std::int64_t bid,
               std::int64_t bid_qty, std::int64_t ask, std::int64_t ask_qty,
               std::int64_t event_ns, std::int64_t recv_ns) {
  REQUIRE(value.instrument_id == id);
  REQUIRE(value.bid_price[0] == bid);
  REQUIRE(value.bid_qty[0] == bid_qty);
  REQUIRE(value.ask_price[0] == ask);
  REQUIRE(value.ask_qty[0] == ask_qty);
  REQUIRE(value.event_ns == event_ns);
  REQUIRE(value.recv_ns == recv_ns);
}
}  // namespace

TEST_CASE("Book rejects invalid mode and unsafe snapshot periods", "[book-thread]") {
  FeedQueue input(1);
  RecordQueue output(1);
  REQUIRE_THROWS_AS(BookThread(input, output, static_cast<BookMode>(255), 1ms), std::invalid_argument);
  REQUIRE_THROWS_AS(BookThread(input, output, BookMode::Mbo, 0ms), std::invalid_argument);
  REQUIRE_THROWS_AS(BookThread(input, output, BookMode::Mbo, -1ms), std::invalid_argument);
  REQUIRE_THROWS_AS(BookThread(input, output, BookMode::Mbo,
      std::chrono::milliseconds::max()), std::invalid_argument);
  REQUIRE_NOTHROW(BookThread(input, output, BookMode::L2, 1ms));
}

TEST_CASE("Book UC02 forwards original events and keeps snapshot values independent", "[book-thread]") {
  SingleStep fixture;
  fixture.Apply(MakeOrder(1, qlib::side::buy, 100, 10));
  fixture.Apply(MakeOrder(2, qlib::side::buy, 100, 5));
  const auto modify = fixture.Apply(MakeModify(1, qlib::side::buy, 100, 6));
  const auto cancel = fixture.Apply(MakeCancel(2, 2));
  fixture.Apply(MakeOrder(3, qlib::side::sell, 101, 8));
  const auto trade = fixture.Apply(MakeTrade(1, 3, 2, 1, 106, 206));
  const auto snapshots = fixture.Snapshots();
  REQUIRE(snapshots.size() == 1);
  CheckBook(snapshots[0], 1, 100, 7, 101, 6, 106, 206);
  fixture.Apply(MakeCancel(1, 99));
  REQUIRE(std::get<qlib::order>(modify).new_qty == 6);
  REQUIRE(std::get<qlib::order>(modify).qty == 777);
  REQUIRE(std::get<qlib::cancel>(cancel).qty == 2);
  REQUIRE(std::get<qlib::trade>(trade).price == 888);
  REQUIRE(std::get<qlib::trade>(trade).qty == 2);
  CheckBook(snapshots[0], 1, 100, 7, 101, 6, 106, 206);
  OrderbookInspector::Check(BookThreadInspector::Book(fixture.stage, 1));
  REQUIRE(fixture.metrics.book_events.Read() == 7);
  REQUIRE(fixture.metrics.feed_dropped.Read() == 0);
}

TEST_CASE("Book business ignores preserve input and advance event time", "[book-thread]") {
  SingleStep fixture;
  fixture.Apply(MakeOrder(1, qlib::side::buy, 100, 10));
  fixture.Apply(MakeOrder(1, qlib::side::buy, 200, 2, 1, 301, 401));
  CheckBook(fixture.Snapshots().at(0), 1, 100, 10, 0, 0, 301, 401);
  fixture.Apply(MakeCancel(999, 4, 1, 302, 402));
  CheckBook(fixture.Snapshots().at(0), 1, 100, 10, 0, 0, 302, 402);
  fixture.Apply(MakeModify(1, qlib::side::sell, 101, 3, 1, 303, 403));
  CheckBook(fixture.Snapshots().at(0), 1, 100, 10, 0, 0, 303, 403);
  fixture.Apply(MakeOrder(2, qlib::side::buy, 100,
      std::numeric_limits<std::int64_t>::max(), 1, 304, 404));
  CheckBook(fixture.Snapshots().at(0), 1, 100, 10, 0, 0, 304, 404);
  REQUIRE(fixture.metrics.book_events.Read() == 5);
  REQUIRE(fixture.metrics.feed_dropped.Read() == 0);
  REQUIRE(BookThreadInspector::Diagnostics(fixture.stage, 0) == 1);
  REQUIRE(BookThreadInspector::Diagnostics(fixture.stage, 1) == 1);
}

TEST_CASE("Book external map grows and later modifications preserve instrument isolation", "[book-thread]") {
  SingleStep fixture;
  fixture.Apply(MakeOrder(1, qlib::side::buy, 100, 10, 1));
  const auto first_capacity = BookThreadInspector::Capacity(fixture.stage);
  const auto* first_node = OrderbookInspector::Node(BookThreadInspector::Book(fixture.stage, 1), 1);
  for (std::uint32_t id = 2; id <= 40; ++id)
    fixture.Apply(MakeOrder(1, qlib::side::buy, 100 + id, id, id));
  REQUIRE(BookThreadInspector::Capacity(fixture.stage) > first_capacity);
  REQUIRE(BookThreadInspector::Count(fixture.stage) == 40);
  REQUIRE(OrderbookInspector::Node(BookThreadInspector::Book(fixture.stage, 1), 1) == first_node);
  fixture.Apply(MakeModify(1, qlib::side::buy, 105, 7, 1));
  fixture.Apply(MakeCancel(1, 99, 2));
  CheckSnapshot(BookThreadInspector::Book(fixture.stage, 1), 105, 7, 0, 0, 100, 200, 1);
  CheckSnapshot(BookThreadInspector::Book(fixture.stage, 2), 0, 0, 0, 0, 100, 200, 2);
  CheckSnapshot(BookThreadInspector::Book(fixture.stage, 3), 103, 3, 0, 0, 100, 200, 3);
  REQUIRE(fixture.Snapshots().size() == 40);
  REQUIRE(fixture.metrics.book_instruments.Read() == 40);
  REQUIRE(fixture.metrics.book_resting_orders.Read() == 39);
}

TEST_CASE("Book UC03 replaces L2 depth and forwards trades without repeated deductions", "[book-thread]") {
  SingleStep fixture(BookMode::L2);
  fixture.Apply(MakeLevel(qlib::side::buy, 100, 20));
  fixture.Apply(MakeLevel(qlib::side::buy, 100, 8));
  fixture.Apply(MakeTrade(1, 2, 3, 1, 103, 203));
  CheckBook(fixture.Snapshots().at(0), 1, 100, 8, 0, 0, 103, 203);
  fixture.Apply(MakeLevel(qlib::side::buy, 100, 0, 1, 104, 204));
  CheckBook(fixture.Snapshots().at(0), 1, 0, 0, 0, 0, 104, 204);
  REQUIRE(fixture.metrics.book_events.Read() == 4);
  REQUIRE(fixture.metrics.book_resting_orders.Read() == 0);
  REQUIRE(fixture.metrics.writer_orders.Read() == 0);
  REQUIRE(fixture.metrics.writer_trades.Read() == 0);
  REQUIRE(fixture.metrics.writer_levels.Read() == 0);
  REQUIRE(fixture.metrics.writer_books.Read() == 0);
}

TEST_CASE("Book snapshots distinguish unknown instruments from known empty books", "[book-thread]") {
  SECTION("Mbo first unknown cancel creates an empty known book") {
    SingleStep fixture;
    REQUIRE(fixture.Snapshots().empty());
    REQUIRE(fixture.metrics.book_instruments.Read() == 0);
    fixture.Apply(MakeCancel(999, 2, 7, 123, 456));
    REQUIRE(fixture.metrics.book_instruments.Read() == 0);
    CheckBook(fixture.Snapshots().at(0), 7, 0, 0, 0, 0, 123, 456);
    REQUIRE(fixture.metrics.book_instruments.Read() == 1);
    REQUIRE(fixture.metrics.book_resting_orders.Read() == 0);
  }
  SECTION("L2 first trade creates an empty known book") {
    SingleStep fixture(BookMode::L2);
    fixture.Apply(MakeTrade(0, 0, 3, 8, 234, 567));
    CheckBook(fixture.Snapshots().at(0), 8, 0, 0, 0, 0, 234, 567);
    REQUIRE(fixture.metrics.book_instruments.Read() == 1);
    REQUIRE(fixture.metrics.book_resting_orders.Read() == 0);
  }
}

TEST_CASE("Book publishes gauges only after a completed snapshot observation", "[book-thread]") {
  SingleStep fixture;
  fixture.Apply(MakeOrder(1, qlib::side::buy, 100, 5, 1));
  fixture.Apply(MakeOrder(2, qlib::side::sell, 101, 6, 1));
  fixture.Apply(MakeOrder(1, qlib::side::buy, 102, 7, 2));
  REQUIRE(fixture.metrics.book_instruments.Read() == 0);
  REQUIRE(fixture.metrics.book_resting_orders.Read() == 0);
  REQUIRE(fixture.metrics.book_memory_bytes.Read() == 0);
  const auto expected_memory = BookThreadInspector::Book(fixture.stage, 1).MemoryBytes()
      + BookThreadInspector::Book(fixture.stage, 2).MemoryBytes();
  REQUIRE(fixture.Snapshots().size() == 2);
  REQUIRE(fixture.metrics.book_instruments.Read() == 2);
  REQUIRE(fixture.metrics.book_resting_orders.Read() == 3);
  REQUIRE(fixture.metrics.book_memory_bytes.Read() == expected_memory);
  fixture.Apply(MakeCancel(1, 99, 2));
  REQUIRE(fixture.metrics.book_resting_orders.Read() == 3);
  REQUIRE(fixture.Snapshots().size() == 2);
  REQUIRE(fixture.metrics.book_resting_orders.Read() == 2);
  REQUIRE(fixture.metrics.writer_books.Read() == 0);
}

TEST_CASE("Book samples first event and each next 1024 completed applications", "[book-thread]") {
  SingleStep fixture;
  for (std::uint64_t index = 1; index <= 2049; ++index) {
    auto event = MakeCancel(999, 1);
    event.seq = 123; // 上游序号保持不变，仍按本地完成序号抽样。
    fixture.Apply(event);
    if (index == 1 || index == 1024) REQUIRE(fixture.metrics.book_samples.Read() == 1);
    if (index == 1025 || index == 2048) REQUIRE(fixture.metrics.book_samples.Read() == 2);
    if (index == 2049) REQUIRE(fixture.metrics.book_samples.Read() == 3);
  }
  REQUIRE(fixture.metrics.book_events.Read() == 2049);
  REQUIRE(BookThreadInspector::Events(fixture.stage) == 2049);
}

TEST_CASE("Book publishes Apply sample before blocked record forwarding completes", "[book-thread]") {
  FeedQueue input(1);
  RecordQueue output(1);
  Metrics metrics;
  RunControl control;
  BookThread stage(input, output, BookMode::Mbo, 1h);
  REQUIRE(output.try_push(qlib::record{MakeCancel(99, 1)}));
  std::atomic<bool> forwarded{false};
  BookWorker owner(control, [&](std::stop_token) {
    forwarded.store(BookThreadInspector::Consume(stage, MakeOrder(1, qlib::side::buy, 100, 10),
        metrics, control), std::memory_order_release);
  });
  REQUIRE(WaitUntil([&] { return metrics.book_samples.Read() == 1; }));
  REQUIRE_FALSE(forwarded.load(std::memory_order_acquire));
  auto first = output.try_pop(); // 本线程是唯一消费者，预填者已在启动前交出生产角色。
  REQUIRE(first);
  REQUIRE(std::get<qlib::cancel>(*first).order_id == 99);
  owner.StopAndJoin();
  REQUIRE_FALSE(control.Failed());
  REQUIRE(forwarded.load(std::memory_order_acquire));
  auto second = output.try_pop();
  REQUIRE(second);
  REQUIRE(std::get<qlib::order>(*second).qty == 10);
  REQUIRE_FALSE(output.try_pop());
  REQUIRE(metrics.book_events.Read() == 1);
  REQUIRE(metrics.book_samples.Read() == 1);
}

TEST_CASE("Book Fatal prevents snapshot output and complete gauges", "[book-thread]") {
  FeedQueue input(1);
  RecordQueue output(1);
  Metrics metrics;
  RunControl control;
  BookThread stage(input, output, BookMode::Mbo, 1h);
  REQUIRE(BookThreadInspector::Consume(stage, MakeOrder(1, qlib::side::buy, 100, 10), metrics, control));
  control.Fail("consumer", std::make_exception_ptr(std::runtime_error("受控消费者故障")));
  REQUIRE_FALSE(BookThreadInspector::Snapshot(stage, metrics, control));
  REQUIRE(control.Failed());
  REQUIRE(metrics.book_instruments.Read() == 0);
  REQUIRE(metrics.book_resting_orders.Read() == 0);
  REQUIRE(metrics.book_memory_bytes.Read() == 0);
  REQUIRE(output.try_pop());
  REQUIRE_FALSE(output.try_pop());
}

TEST_CASE("Book idle Run periodically repeats latest known state and event time", "[book-thread]") {
  FeedQueue input(1);
  RecordQueue output(64);
  Metrics metrics;
  RunControl control;
  BookThread stage(input, output, BookMode::Mbo, 2ms);
  REQUIRE(input.try_push(qlib::feed_event{MakeCancel(999, 1, 7, 123, 456)}));
  BookWorker owner(control, [&](std::stop_token stop) { stage.Run(metrics, stop, control); });
  std::vector<qlib::book> snapshots;
  std::vector<qlib::record> raw;
  REQUIRE(WaitUntil([&] {
    if (auto value = output.try_pop()) {
      if (auto* snapshot = std::get_if<qlib::book>(&*value)) snapshots.push_back(*snapshot);
      else raw.push_back(*value);
    }
    return snapshots.size() >= 3;
  }));
  owner.StopAndJoin();
  REQUIRE_FALSE(control.Failed());
  REQUIRE(raw.size() == 1);
  for (const auto& value : snapshots) CheckBook(value, 7, 0, 0, 0, 0, 123, 456);
  REQUIRE(metrics.book_events.Read() == 1);
  REQUIRE(metrics.book_samples.Read() == 1);
}

TEST_CASE("Book idle Run observes an empty instrument set without inventing records", "[book-thread]") {
  FeedQueue input(1);
  RecordQueue output(4);
  Metrics metrics;
  RunControl control;
  BookThread stage(input, output, BookMode::Mbo, 1ms);
  metrics.book_instruments.Set(7); // 角色交接前设置观察标记，归零证明完成过周期轮次。
  BookWorker owner(control, [&](std::stop_token stop) { stage.Run(metrics, stop, control); });
  REQUIRE(WaitUntil([&] { return metrics.book_instruments.Read() == 0; }));
  owner.StopAndJoin();
  REQUIRE_FALSE(control.Failed());
  REQUIRE_FALSE(output.try_pop());
  REQUIRE(metrics.book_events.Read() == 0);
  REQUIRE(metrics.book_samples.Read() == 0);
}

TEST_CASE("Book Fatal releases a sampled event blocked by a full output queue", "[book-thread]") {
  FeedQueue input(1);
  RecordQueue output(1);
  Metrics metrics;
  RunControl control;
  BookThread stage(input, output, BookMode::Mbo, 1h);
  REQUIRE(output.try_push(qlib::record{MakeCancel(99, 1)}));
  REQUIRE(input.try_push(qlib::feed_event{MakeOrder(1, qlib::side::buy, 100, 10)}));
  BookWorker owner(control, [&](std::stop_token stop) { stage.Run(metrics, stop, control); });
  REQUIRE(WaitUntil([&] { return metrics.book_samples.Read() == 1; }));
  REQUIRE_FALSE(owner.done.load(std::memory_order_acquire));
  control.Fail("consumer", std::make_exception_ptr(std::runtime_error("受控消费者故障")));
  owner.StopAndJoin();
  REQUIRE(control.Failed());
  REQUIRE(metrics.book_events.Read() == 1);
  const auto saved = output.try_pop();
  REQUIRE(saved);
  REQUIRE(std::get<qlib::cancel>(*saved).order_id == 99);
  REQUIRE_FALSE(output.try_pop());
}

TEST_CASE("Book normal stop drains accepted input through a small output queue without final snapshot", "[book-thread]") {
  FeedQueue input(8);
  RecordQueue output(1);
  Metrics metrics;
  RunControl control;
  BookThread stage(input, output, BookMode::Mbo, 1h);
  std::vector<qlib::feed_event> accepted;
  for (std::int64_t id = 1; id <= 8; ++id) {
    qlib::feed_event event{MakeOrder(id, qlib::side::buy, 100, id)};
    REQUIRE(input.try_push(qlib::feed_event{event}));
    accepted.push_back(event);
  }
  BookWorker owner(control, [&](std::stop_token stop) { stage.Run(metrics, stop, control); });
  REQUIRE(WaitUntil([&] { return metrics.book_events.Read() == 2; }));
  owner.worker.request_stop(); // 唯一生产者已结束，之后不会再向 input 入队。
  std::vector<qlib::record> saved;
  REQUIRE(WaitUntil([&] {
    if (auto value = output.try_pop()) saved.push_back(*value);
    return owner.done.load(std::memory_order_acquire);
  }));
  owner.StopAndJoin();
  while (auto value = output.try_pop()) saved.push_back(*value);
  REQUIRE_FALSE(control.Failed());
  REQUIRE(saved.size() == accepted.size());
  for (std::size_t i = 0; i < saved.size(); ++i)
    CheckInput(std::get<qlib::order>(saved[i]), std::get<qlib::order>(accepted[i]));
  REQUIRE(metrics.book_events.Read() == 8);
  REQUIRE(metrics.book_instruments.Read() == 0);
  REQUIRE(metrics.book_resting_orders.Read() == 0);
}

TEST_CASE("Book Apply contract errors do not count completed events", "[book-thread]") {
  SingleStep fixture(BookMode::L2);
  REQUIRE_THROWS_AS(BookThreadInspector::Consume(fixture.stage,
      MakeOrder(1, qlib::side::buy, 100, 10), fixture.metrics, fixture.control), std::invalid_argument);
  REQUIRE(fixture.metrics.book_events.Read() == 0);
  REQUIRE(fixture.metrics.book_samples.Read() == 0);
  REQUIRE(BookThreadInspector::Events(fixture.stage) == 0);
  REQUIRE_FALSE(fixture.output.try_pop());
}
