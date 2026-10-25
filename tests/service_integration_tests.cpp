#include "Threads/BookThread.h"
#include "Threads/FeedThread.h"
#include "Threads/WriterThread.h"
#include "support/FailingOutputStream.h"
#include "support/FeedEncoder.h"
#include "support/FeedPublisher.h"
#include "support/OrderbookTestSupport.h"
#include "support/ParquetReadback.h"
#include "support/PipelineTestSupport.h"
#include "support/ProcessHarness.h"
#include "support/StorageTestSupport.h"
#include "support/WriterGoalObserver.h"
#include "support/WriterThreadInspector.h"

#include <arrow/api.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <csignal>
#include <limits>
#include <optional>

using namespace qbook;
using namespace qbook::testing;
using namespace std::chrono_literals;

namespace {
std::vector<qlib::feed_event> Uc02() {
  return {MakeOrder(1, qlib::side::buy, 100, 10, 1, 101, -777),
          MakeOrder(2, qlib::side::buy, 100, 5, 1, 102, -777),
          MakeModify(1, qlib::side::buy, 100, 6, 1, 103, -777),
          MakeCancel(2, 2, 1, 104, -777),
          MakeOrder(3, qlib::side::sell, 101, 8, 1, 105, -777),
          MakeTrade(1, 3, 2, 1, 106, -777)};
}

qlib::book Top(std::uint32_t id, std::int64_t bid, std::int64_t bid_qty,
               std::int64_t ask = 0, std::int64_t ask_qty = 0) {
  qlib::book result{};
  result.instrument_id = id;
  result.bid_price[0] = bid;
  result.bid_qty[0] = bid_qty;
  result.ask_price[0] = ask;
  result.ask_qty[0] = ask_qty;
  return result;
}

MessagePlan EncodePlan(const std::vector<qlib::feed_event>& events) {
  MessagePlan result;
  for (const auto& event : events) {
    const auto frame = EncodeFeedEvent(event);
    result.push_back({{{frame.bytes().begin(), frame.bytes().end()}}});
  }
  return result;
}

void Save(ExpectedRun& result, const qlib::record& record) {
  std::visit(qlib::overloaded{
    [&](const qlib::order& v) { result.orders.push_back(v); },
    [&](const qlib::trade& v) { result.trades.push_back(v); },
    [&](const qlib::cancel& v) { result.cancels.push_back(v); },
    [&](const qlib::level& v) { result.levels.push_back(v); },
    [&](const qlib::book& v) { result.books.push_back(v); }}, record);
}

void CheckRaw(const std::vector<qlib::record>& records,
              const std::vector<qlib::feed_event>& input, bool network) {
  std::size_t i = 0;
  for (const auto& record : records) {
    if (std::holds_alternative<qlib::book>(record)) continue;
    REQUIRE(i < input.size());
    std::visit([&](auto expected) {
      using T = decltype(expected);
      REQUIRE(std::holds_alternative<T>(record));
      const auto& actual = std::get<T>(record);
      if (network) {
        REQUIRE(actual.recv_ns > 0);
        REQUIRE(actual.recv_ns != expected.recv_ns);
        expected.recv_ns = actual.recv_ns;
      }
      CheckInput(actual, expected);
    }, input[i++]);
  }
  REQUIRE(i == input.size());
}

const qlib::book& LastBook(const std::vector<qlib::record>& records, std::uint32_t id) {
  for (auto pos = records.rbegin(); pos != records.rend(); ++pos)
    if (const auto* value = std::get_if<qlib::book>(&*pos); value && value->instrument_id == id)
      return *value;
  throw std::runtime_error("缺少目标品种文件快照");
}

std::int64_t IntegerAt(const arrow::Table& table, const char* column, std::int64_t row) {
  const auto values = table.GetColumnByName(column);
  REQUIRE(values);
  for (const auto& chunk : values->chunks()) {
    if (row < chunk->length()) return static_cast<const arrow::Int64Array&>(*chunk).Value(row);
    row -= chunk->length();
  }
  throw std::out_of_range("文件列缺少目标行");
}

// 网络接收时间由子进程生成，逐行取回；其他预期均来自固定输入与手算盘口。
void CheckProcessUc02(const ReadbackResult& saved, const std::vector<qlib::feed_event>& input) {
  ExpectedRun expected;
  for (const auto& value : input) std::visit([&](auto event) {
    using T = decltype(event);
    const arrow::Table* table = nullptr;
    std::size_t index = 0;
    if constexpr (std::is_same_v<T, qlib::order>) { table = saved.orders.get(); index = expected.orders.size(); }
    if constexpr (std::is_same_v<T, qlib::trade>) { table = saved.trades.get(); index = expected.trades.size(); }
    if constexpr (std::is_same_v<T, qlib::cancel>) { table = saved.cancels.get(); index = expected.cancels.size(); }
    if constexpr (std::is_same_v<T, qlib::level>) { table = saved.levels.get(); index = expected.levels.size(); }
    event.recv_ns = IntegerAt(*table, "recv_ns", index);
    REQUIRE(event.recv_ns > 0);
    Save(expected, event);
  }, value);
  for (std::int64_t row = 0; row < saved.books->num_rows(); ++row) {
    const auto time = IntegerAt(*saved.books, "event_ns", row);
    REQUIRE(time >= 101);
    REQUIRE(time <= 106);
    const std::int64_t bids[] = {10, 15, 11, 9, 9, 7};
    auto book = Top(1, 100, bids[time - 101], time >= 105 ? 101 : 0,
                     time == 106 ? 6 : time == 105 ? 8 : 0);
    book.event_ns = time;
    if (time == 104) book.recv_ns = expected.cancels[0].recv_ns;
    else if (time == 106) book.recv_ns = expected.trades[0].recv_ns;
    else book.recv_ns = expected.orders[time == 105 ? 3 : time - 101].recv_ns;
    expected.books.push_back(book);
  }
  CheckExpectedFiles(saved, expected);
  REQUIRE_FALSE(expected.books.empty());
  REQUIRE(expected.books.back().event_ns == 106);
}

struct NetworkFiles {
  TempDirectory temporary;
  FeedQueue input{4};
  RecordQueue output{4};
  Metrics metrics;
  RunControl control;
  PublisherSignals signals;
  WriterGoalObserver observer;
  std::vector<qlib::record> observed;
  std::atomic<std::size_t> snapshots_after_goal{0};
  detail::WriterHooks hooks;
  std::unique_ptr<WriterThread> writer;
  BookThread book;
  FeedPublisher publisher;
  std::optional<FeedThread> feed;
  std::jthread writer_thread, book_thread, feed_thread, publisher_thread;
  ThreadCleanup cleanup;
  BookMode mode;

  NetworkFiles(MessagePlan plan, BookMode selected, WriterGoal goal)
      : observer(std::move(goal)), hooks{{}, [&](const qlib::record& record) {
          if (observed.size() == observed.capacity()) throw std::length_error("文件观察容量超限");
          observed.push_back(record);
          observer.Observe(record);
          if (observer.Reached() && std::holds_alternative<qlib::book>(record))
            snapshots_after_goal.fetch_add(1, std::memory_order_release);
        }}, writer(WriterThreadInspector::Create(output, temporary.path(), 3, hooks)),
        book(input, output, selected, 10ms), publisher(std::move(plan), signals),
        cleanup(control, {&publisher_thread, &feed_thread, &book_thread, &writer_thread}), mode(selected) {
    observed.reserve(10000);
  }

  void Start() {
    auto endpoint = signals.bound_endpoint.get_future();
    auto subscribed = signals.subscribed.get_future();
    writer_thread = Launch("writer", control, [&](auto stop) { writer->Run(metrics, stop, control); });
    book_thread = Launch("book", control, [&](auto stop) { book.Run(metrics, stop, control); });
    publisher_thread = Launch("publisher", control, [&](auto stop) { publisher.Run(stop, control); });
    const auto deadline = ScenarioDeadline();
    WaitUntil([&] { return endpoint.wait_for(0ms) == std::future_status::ready; }, control, deadline);
    feed.emplace(endpoint.get(), mode, input);
    feed_thread = Launch("feed", control, [&](auto stop) { feed->Run(metrics, stop, control); });
    WaitUntil([&] { return subscribed.wait_for(0ms) == std::future_status::ready; }, control, deadline);
    subscribed.get();
  }

  void Finish(std::uint64_t messages, std::uint64_t dropped, std::size_t snapshots = 1) {
    WaitUntil([&] { return observer.Reached() && signals.sent_all.load() &&
        snapshots_after_goal.load(std::memory_order_acquire) >= snapshots &&
        metrics.feed_messages.Read() == messages && metrics.feed_dropped.Read() == dropped;
    }, control, ScenarioDeadline());
    StopJoin(feed_thread);
    StopJoin(publisher_thread);
    StopJoin(book_thread);
    StopJoin(writer_thread);
    cleanup.Dismiss();
    REQUIRE_FALSE(control.Failed());
    REQUIRE(input.empty());
    REQUIRE(output.empty());
    REQUIRE(WriterThreadInspector::Released(*writer));
    ExpectedRun expected;
    for (const auto& value : observed) Save(expected, value);
    CheckExpectedFiles(ReadRunDirectory(WriterThreadInspector::Directory(*writer), 10000), expected);
    REQUIRE(metrics.writer_orders.Read() == expected.orders.size() + expected.cancels.size());
    REQUIRE(metrics.writer_trades.Read() == expected.trades.size());
    REQUIRE(metrics.writer_levels.Read() == expected.levels.size());
    REQUIRE(metrics.writer_books.Read() == expected.books.size());
  }
};

std::vector<qlib::feed_event> Many(std::size_t count) {
  std::vector<qlib::feed_event> result;
  for (std::size_t i = 0; i < count; ++i) {
    auto value = MakeOrder(i + 1, qlib::side::buy, 100, 1, 1, 100 + i, 200 + i);
    value.seq = i + 1;
    result.push_back(value);
  }
  return result;
}

struct ControlledFiles {
  TempDirectory temporary;
  FeedQueue input{2};
  RecordQueue output{1};
  Metrics metrics;
  RunControl control;
  std::shared_ptr<StreamProbe> probe = std::make_shared<StreamProbe>();
  detail::WriterHooks hooks;
  std::unique_ptr<WriterThread> writer;
  BookThread book{input, output, BookMode::Mbo, 1h};
  std::vector<qlib::feed_event> offered, admitted;
  std::atomic<std::size_t> admitted_count{0};
  std::atomic<bool> producer_done{false}, book_done{false}, writer_done{false};
  std::jthread producer_thread, book_thread, writer_thread;
  ThreadCleanup cleanup;

  ControlledFiles(std::size_t count, bool pause, bool fail)
      : hooks{MakeOutputFactory(probe, {.fail_write = fail, .pause_write = pause, .control = &control}),
          [&](const qlib::record&) {
            if (metrics.writer_orders.Read() == 1) probe->armed.store(true, std::memory_order_release);
          }}, writer(WriterThreadInspector::Create(output, temporary.path(), 2, hooks)),
        offered(Many(count)), cleanup(control, {&producer_thread, &book_thread, &writer_thread}) {
    admitted.reserve(count);
  }
  void Start() {
    writer_thread = Launch("writer", control, [&](auto stop) { writer->Run(metrics, stop, control); }, &writer_done);
    book_thread = Launch("book", control, [&](auto stop) { book.Run(metrics, stop, control); }, &book_done);
    producer_thread = Launch("producer", control, [&](auto stop) {
      for (const auto& original : offered) {
        auto candidate = original;
        while (!control.Failed() && !stop.stop_requested()) {
          if (input.try_push(std::move(candidate))) {
            admitted.push_back(original);
            admitted_count.store(admitted.size(), std::memory_order_release);
            break;
          }
          std::this_thread::sleep_for(50us);
        }
        if (control.Failed() || stop.stop_requested()) return;
      }
    }, &producer_done);
  }
  void Backlogged() {
    WaitUntil([&] { return probe->entered.load(std::memory_order_acquire) &&
        admitted_count.load(std::memory_order_acquire) == 6 && metrics.book_events.Read() == 4;
    }, control, ScenarioDeadline());
  }
  void Finish() {
    StopJoin(producer_thread);
    probe->release.store(true, std::memory_order_release);
    StopJoin(book_thread);
    StopJoin(writer_thread);
    cleanup.Dismiss();
    REQUIRE_FALSE(control.Failed());
    REQUIRE(producer_done.load());
    REQUIRE(book_done.load());
    REQUIRE(writer_done.load());
    REQUIRE(input.empty());
    REQUIRE(output.empty());
    ExpectedRun expected;
    for (const auto& value : admitted) std::visit([&](const auto& v) { Save(expected, v); }, value);
    CheckExpectedFiles(ReadRunDirectory(WriterThreadInspector::Directory(*writer), 1000), expected);
    REQUIRE(metrics.book_events.Read() == admitted.size());
    REQUIRE(metrics.writer_orders.Read() == admitted.size());
    REQUIRE(metrics.writer_books.Read() == 0);
  }
};
}  // namespace

TEST_CASE("network UC02 saves original quantities and repeated final snapshots", "[service][network]") {
  const auto events = Uc02();
  NetworkFiles scenario(EncodePlan(events), BookMode::Mbo,
      {.orders = 4, .trades = 1, .cancels = 1, .snapshot = Top(1, 100, 7, 101, 6)});
  scenario.Start();
  scenario.Finish(6, 0, 3);
  CheckRaw(scenario.observed, events, true);
  const auto& final = LastBook(scenario.observed, 1);
  REQUIRE(final.event_ns == 106);
  REQUIRE(final.bid_qty[0] == 7);
  REQUIRE(final.ask_qty[0] == 6);
  std::size_t repeated = 0;
  for (const auto& value : scenario.observed)
    if (const auto* snapshot = std::get_if<qlib::book>(&value); snapshot && snapshot->event_ns == 106) {
      REQUIRE(snapshot->recv_ns == final.recv_ns);
      REQUIRE(snapshot->bid_price[0] == 100);
      REQUIRE(snapshot->bid_qty[0] == 7);
      REQUIRE(snapshot->ask_price[0] == 101);
      REQUIRE(snapshot->ask_qty[0] == 6);
      ++repeated;
    }
  REQUIRE(repeated >= 3);
}

TEST_CASE("network L2 files prove absolute replacement trade preservation and zero deletion", "[service][network]") {
  const bool erase = GENERATE(false, true);
  std::vector<qlib::feed_event> events{MakeLevel(qlib::side::buy, 100, 20, 9, 101, -777),
    MakeLevel(qlib::side::buy, 100, 8, 9, 102, -777), MakeTrade(0, 0, 3, 9, 103, -777)};
  if (erase) events.emplace_back(MakeLevel(qlib::side::buy, 100, 0, 9, 104, -777));
  auto plan = EncodePlan(events);
  auto rejected = EncodePlan({MakeOrder(1, qlib::side::buy, 100, 2), MakeCancel(1, 1)});
  plan.insert(plan.end(), rejected.begin(), rejected.end());
  NetworkFiles scenario(std::move(plan), BookMode::L2,
      {.trades = 1, .levels = erase ? 3u : 2u, .snapshot = Top(9, erase ? 0 : 100, erase ? 0 : 8)});
  scenario.Start();
  scenario.Finish(events.size() + 2, 2, 2);
  CheckRaw(scenario.observed, events, true);
  REQUIRE(LastBook(scenario.observed, 9).event_ns == (erase ? 104 : 103));
  REQUIRE(scenario.metrics.writer_orders.Read() == 0);
}

TEST_CASE("network file chain rejects bad frames and retains business ignores across instruments", "[service][network]") {
  auto events = Uc02();
  events.emplace_back(MakeOrder(1, qlib::side::buy, 500, 500, 1, 107, -777));
  events.emplace_back(MakeModify(999, qlib::side::buy, 100, 3, 1, 108, -777));
  events.emplace_back(MakeCancel(999, 5, 1, 109, -777));
  events.emplace_back(MakeModify(1, qlib::side::sell, 200, 40, 1, 110, -777));
  events.emplace_back(MakeOrder(4, qlib::side::buy, 100, std::numeric_limits<std::int64_t>::max(), 1, 111, -777));
  auto market = MakeOrder(5, qlib::side::buy, 0, 3, 1, 112, -777);
  market.type = qlib::order_type::market;
  events.emplace_back(market);
  for (std::uint32_t id = 2; id <= 40; ++id)
    events.emplace_back(MakeOrder(1, qlib::side::buy, 100 + id, id, id, 200 + id, -777));
  events.emplace_back(MakeModify(1, qlib::side::buy, 222, 9, 2, 300, -777));
  events.emplace_back(MakeModify(1, qlib::side::buy, -2, -7, 40, 301, -777));
  auto plan = EncodePlan(events);
  plan.insert(plan.begin(), TestMessage{{{std::byte{255}}}});
  auto invalid = EncodePlan({MakeTrade(1, 3, 0)});
  plan.insert(plan.end(), invalid.begin(), invalid.end());
  plan.push_back({{plan[1].parts[0], plan[1].parts[0]}});
  NetworkFiles scenario(std::move(plan), BookMode::Mbo,
      {.orders = events.size() - 3, .trades = 1, .cancels = 2, .snapshot = Top(40, 0, 0)});
  scenario.Start();
  scenario.Finish(events.size() + 3, 3, 80);
  CheckRaw(scenario.observed, events, true);
  REQUIRE(LastBook(scenario.observed, 1).bid_qty[0] == 7);
  REQUIRE(LastBook(scenario.observed, 1).ask_qty[0] == 6);
  REQUIRE(LastBook(scenario.observed, 1).event_ns == 112);
  REQUIRE(LastBook(scenario.observed, 2).bid_price[0] == 222);
  REQUIRE(LastBook(scenario.observed, 2).bid_qty[0] == 9);
  REQUIRE(LastBook(scenario.observed, 3).bid_qty[0] == 3);
  REQUIRE(LastBook(scenario.observed, 40).bid_qty[0] == 0);
  REQUIRE(scenario.metrics.book_instruments.Read() == 40);
}

TEST_CASE("controlled admission ledger survives both queues full during normal stop", "[service][drain]") {
  ControlledFiles scenario(128, true, false);
  scenario.Start();
  scenario.Backlogged();
  scenario.Finish();
  REQUIRE(scenario.probe->entered.load());
  REQUIRE(scenario.admitted.size() == 6);
  REQUIRE(std::get<qlib::order>(scenario.admitted.back()).order_id == 6);
  REQUIRE(scenario.probe->failures == 0);
}

TEST_CASE("controlled full queue recovery preserves every accepted row and final handoff", "[service][drain]") {
  SECTION("recovery across batches") {
    ControlledFiles scenario(257, true, false);
    scenario.Start();
    scenario.Backlogged();
    scenario.probe->release.store(true);
    WaitUntil([&] { return scenario.producer_done.load(); }, scenario.control, ScenarioDeadline());
    scenario.Finish();
    REQUIRE(scenario.admitted.size() == 257);
  }
  SECTION("last successful push then immediate stop") {
    for (int round = 0; round < 8; ++round) {
      ControlledFiles scenario(3, false, false);
      scenario.Start();
      WaitUntil([&] { return scenario.producer_done.load(); }, scenario.control, ScenarioDeadline());
      scenario.Finish();
      REQUIRE(scenario.admitted.size() == 3);
    }
  }
}

TEST_CASE("actual Writer batch failure releases upstream waits without claiming complete output", "[service][failure]") {
  ControlledFiles scenario(128, true, true);
  scenario.Start();
  scenario.Backlogged();
  scenario.probe->release.store(true);
  RunControl waiter;
  WaitUntil([&] { return scenario.writer_done.load() && scenario.book_done.load() &&
      scenario.producer_done.load(); }, waiter, ScenarioDeadline());
  scenario.cleanup.Abort();
  REQUIRE(scenario.control.Failed());
  REQUIRE(std::string(scenario.control.Stage()) == "writer");
  REQUIRE(scenario.probe->failures == 1);
  REQUIRE(scenario.metrics.writer_orders.Read() == 1);
  REQUIRE(scenario.admitted.size() == 6);
  REQUIRE(WriterThreadInspector::Released(*scenario.writer));
}

TEST_CASE("scenario timeout cancels a real blocked output and joins every stage", "[service][failure]") {
  ControlledFiles scenario(128, true, false);
  scenario.Start();
  scenario.Backlogged();
  REQUIRE_THROWS(WaitUntil([] { return false; }, scenario.control, std::chrono::steady_clock::now()));
  scenario.cleanup.Abort();
  REQUIRE(scenario.producer_done.load());
  REQUIRE(scenario.book_done.load());
  REQUIRE(scenario.writer_done.load());
  REQUIRE(scenario.control.Failed());
  REQUIRE(WriterThreadInspector::Released(*scenario.writer));
}

TEST_CASE("real service implementation saves network UC02 before SIGINT and SIGTERM", "[service][process][network]") {
  const auto signal = GENERATE(SIGINT, SIGTERM);
  const auto input = Uc02();
  RunControl control;
  PublisherSignals signals;
  FeedPublisher publisher(EncodePlan(input), signals);
  auto endpoint = signals.bound_endpoint.get_future();
  std::jthread thread;
  ThreadCleanup cleanup(control, {&thread});
  thread = Launch("publisher", control, [&](auto stop) { publisher.Run(stop, control); });
  WaitUntil([&] { return endpoint.wait_for(0ms) == std::future_status::ready; }, control, ScenarioDeadline());
  ProcessHarness process(QBOOK_SERVICE_TEST_DRIVER,
      {"goal", "4", "1", "1", "0", "1", "100", "7", "101", "6", "6", "0", std::to_string(signal)});
  process.WriteConfig("feed_endpoint: '" + endpoint.get() + "'\nsnapshot_period: 10\n"
      "feed_queue_capacity: 2\nrecord_queue_capacity: 2\nbatch_rows: 3\n");
  process.Start();
  const auto result = process.Wait(ScenarioDeadline());
  StopJoin(thread);
  cleanup.Dismiss();
  INFO(result.output);
  REQUIRE_FALSE(control.Failed());
  REQUIRE_FALSE(result.timed_out);
  REQUIRE_FALSE(result.output_overflow);
  REQUIRE(result.term_signal == 0);
  REQUIRE(result.exit_code == 0);
  REQUIRE(result.output.find("driver_goal_reached") != std::string::npos);
  const auto root = process.Directory() / "data_out";
  std::vector<std::filesystem::path> directories;
  for (const auto& entry : std::filesystem::directory_iterator(root)) directories.push_back(entry.path());
  REQUIRE(directories.size() == 1);
  const auto saved = ReadRunDirectory(directories[0], 1000);
  REQUIRE(saved.orders->num_rows() == 4);
  REQUIRE(saved.trades->num_rows() == 1);
  REQUIRE(saved.cancels->num_rows() == 1);
  REQUIRE(saved.levels->num_rows() == 0);
  REQUIRE(saved.books->num_rows() >= 1);
  CheckProcessUc02(saved, input);
  process.RemoveDirectory();
}

TEST_CASE("Writer failure releases real Feed and Book under simultaneous queue backpressure", "[service][network][failure]") {
  TempDirectory temporary;
  FeedQueue input(2);
  RecordQueue output(1);
  Metrics metrics;
  RunControl control;
  auto probe = std::make_shared<StreamProbe>();
  detail::WriterHooks hooks{MakeOutputFactory(probe,
      {.fail_write = true, .pause_write = true, .control = &control}),
      [&](const qlib::record&) {
        if (metrics.writer_orders.Read() == 1) probe->armed.store(true);
      }};
  auto writer = WriterThreadInspector::Create(output, temporary.path(), 2, hooks);
  BookThread book(input, output, BookMode::Mbo, 1h);
  PublisherSignals signals;
  FeedPublisher publisher(EncodePlan(Many(64)), signals);
  auto endpoint = signals.bound_endpoint.get_future();
  std::optional<FeedThread> feed;
  std::atomic<bool> writer_done{false}, book_done{false}, feed_done{false};
  std::jthread writer_thread, book_thread, feed_thread, publisher_thread;
  ThreadCleanup cleanup(control, {&publisher_thread, &feed_thread, &book_thread, &writer_thread});
  writer_thread = Launch("writer", control, [&](auto stop) { writer->Run(metrics, stop, control); }, &writer_done);
  book_thread = Launch("book", control, [&](auto stop) { book.Run(metrics, stop, control); }, &book_done);
  publisher_thread = Launch("publisher", control, [&](auto stop) { publisher.Run(stop, control); });
  WaitUntil([&] { return endpoint.wait_for(0ms) == std::future_status::ready; }, control, ScenarioDeadline());
  feed.emplace(endpoint.get(), BookMode::Mbo, input);
  feed_thread = Launch("feed", control, [&](auto stop) { feed->Run(metrics, stop, control); }, &feed_done);
  WaitUntil([&] { return probe->entered.load() && metrics.book_events.Read() == 4 &&
      metrics.feed_orders.Read() == 7; }, control, ScenarioDeadline());
  probe->release.store(true);
  RunControl waiter;
  WaitUntil([&] { return writer_done.load() && book_done.load() && feed_done.load(); }, waiter, ScenarioDeadline());
  cleanup.Abort();
  REQUIRE(control.Failed());
  REQUIRE(std::string(control.Stage()) == "writer");
  REQUIRE(probe->failures == 1);
  REQUIRE(metrics.writer_orders.Read() == 1);
  REQUIRE(metrics.feed_dropped.Read() == 0);
  REQUIRE(WriterThreadInspector::Released(*writer));
}
