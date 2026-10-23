#include "Threads/BookThread.h"
#include "Threads/FeedThread.h"
#include "Threads/MetricsThread.h"
#include "support/FeedEncoder.h"
#include "support/FeedPublisher.h"
#include "support/OrderbookTestSupport.h"
#include "support/PipelineTestSupport.h"
#include "support/RecordCollector.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <type_traits>

using namespace std::chrono_literals;
using namespace qbook;
using namespace qbook::testing;

namespace {

MessagePlan EncodePlan(const std::vector<qlib::feed_event>& events) {
  MessagePlan plan;
  for (const auto& event : events) {
    const auto frame = EncodeFeedEvent(event);
    plan.push_back({{{frame.bytes().begin(), frame.bytes().end()}}});
  }
  return plan;
}

std::vector<qlib::feed_event> Uc02() {
  return {MakeOrder(1, qlib::side::buy, 100, 10, 1, 101, -777),
          MakeOrder(2, qlib::side::buy, 100, 5, 1, 102, -777),
          MakeModify(1, qlib::side::buy, 100, 6, 1, 103, -777),
          MakeCancel(2, 2, 1, 104, -777),
          MakeOrder(3, qlib::side::sell, 101, 8, 1, 105, -777),
          MakeTrade(1, 3, 2, 1, 106, -777)};
}

std::vector<qlib::feed_event> ManyOrders(std::size_t count) {
  std::vector<qlib::feed_event> events;
  events.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    auto value = MakeOrder(static_cast<std::int64_t>(i + 1), qlib::side::buy,
                           100, 1, 1, static_cast<std::int64_t>(100 + i),
                           static_cast<std::int64_t>(900 + i));
    value.seq = static_cast<std::int64_t>(i + 1);
    events.emplace_back(value);
  }
  return events;
}

void CheckWriterUntouched(const Metrics& metrics) {
  REQUIRE(metrics.writer_orders.Read() == 0);
  REQUIRE(metrics.writer_trades.Read() == 0);
  REQUIRE(metrics.writer_levels.Read() == 0);
  REQUIRE(metrics.writer_books.Read() == 0);
}

void CheckRaw(const std::vector<qlib::record>& records,
              const std::vector<qlib::feed_event>& expected, bool from_network = false) {
  std::size_t index = 0;
  for (const auto& record : records) {
    if (std::holds_alternative<qlib::book>(record)) continue;
    REQUIRE(index < expected.size());
    REQUIRE(record.index() == expected[index].index());
    std::visit([&](const auto& original) {
      using T = std::remove_cvref_t<decltype(original)>;
      auto want = original;
      const auto& actual = std::get<T>(record);
      if (from_network) {
        REQUIRE(actual.recv_ns > 0);
        REQUIRE(actual.recv_ns != original.recv_ns);
        want.recv_ns = actual.recv_ns;
      }
      CheckInput(actual, want);
    }, expected[index]);
    ++index;
  }
  REQUIRE(index == expected.size());
}

const qlib::book& LastBook(const std::vector<qlib::record>& records, std::uint32_t id) {
  for (auto it = records.rbegin(); it != records.rend(); ++it)
    if (const auto* book = std::get_if<qlib::book>(&*it); book && book->instrument_id == id)
      return *book;
  throw std::runtime_error("测试缺少预期品种快照");
}

// 对象顺序使清理守卫先于所有借用资源析构。
struct NetworkScenario {
  FeedQueue feed_queue;
  RecordQueue record_queue;
  Metrics metrics;
  RunControl control;
  PublisherSignals publisher_signals;
  CollectorControl commands;
  CollectorSignals signals;
  BookThread book;
  RecordCollector collector;
  FeedPublisher publisher;
  std::optional<FeedThread> feed;
  std::future<std::string> endpoint;
  std::future<void> subscribed;
  std::jthread collector_thread, book_thread, publisher_thread, feed_thread;
  ThreadCleanup cleanup;
  std::chrono::steady_clock::time_point deadline = ScenarioDeadline();
  BookMode mode;

  NetworkScenario(MessagePlan plan, BookMode chosen_mode, CollectGoal goal,
                  std::size_t feed_capacity = 4, std::size_t record_capacity = 4,
                  std::chrono::milliseconds period = 10ms)
      : feed_queue(feed_capacity), record_queue(record_capacity),
        book(feed_queue, record_queue, chosen_mode, period),
        collector(record_queue, 100'000, goal, commands, signals),
        publisher(std::move(plan), publisher_signals),
        endpoint(publisher_signals.bound_endpoint.get_future()),
        subscribed(publisher_signals.subscribed.get_future()),
        cleanup(control, {&publisher_thread, &feed_thread, &book_thread, &collector_thread},
                &commands), mode(chosen_mode) {}

  void Start() {
    collector_thread = Launch("collector", control,
        [&](std::stop_token stop) { collector.Run(stop, control); });
    book_thread = Launch("book", control,
        [&](std::stop_token stop) { book.Run(metrics, stop, control); });
    publisher_thread = Launch("publisher", control,
        [&](std::stop_token stop) { publisher.Run(stop, control); });
    WaitUntil([&] { return endpoint.wait_for(0s) == std::future_status::ready; }, control, deadline);
    feed.emplace(endpoint.get(), mode, feed_queue);
    feed_thread = Launch("feed", control,
        [&](std::stop_token stop) { feed->Run(metrics, stop, control); });
    WaitUntil([&] { return subscribed.wait_for(0s) == std::future_status::ready; }, control, deadline);
    subscribed.get();
  }

  void Finish(std::uint64_t messages, std::uint64_t dropped) {
    WaitUntil([&] {
      return signals.goal_reached.load() && publisher_signals.sent_all.load() &&
             metrics.feed_messages.Read() == messages && metrics.feed_dropped.Read() == dropped;
    }, control, deadline);
    StopJoin(publisher_thread);
    StopJoin(feed_thread);
    StopJoin(book_thread);
    StopJoin(collector_thread);
    cleanup.Dismiss();
    REQUIRE_FALSE(control.Failed());
    REQUIRE(signals.done.load());
    CheckWriterUntouched(metrics);
  }
};

struct ControlledScenario {
  FeedQueue feed_queue;
  RecordQueue record_queue;
  Metrics metrics;
  RunControl control;
  CollectorControl commands;
  CollectorSignals signals;
  std::vector<qlib::feed_event> input;
  std::vector<qlib::feed_event> accepted;
  std::atomic<std::size_t> accepted_count{0};
  std::atomic<bool> producer_done{false}, book_done{false}, collector_done{false};
  BookThread book;
  RecordCollector collector;
  std::jthread producer_thread, book_thread, collector_thread;
  ThreadCleanup cleanup;
  std::chrono::steady_clock::time_point deadline = ScenarioDeadline();

  ControlledScenario(std::vector<qlib::feed_event> events, std::size_t feed_capacity = 2,
                     std::size_t record_capacity = 1, std::size_t max_records = 4096,
                     CollectGoal goal = {})
      : feed_queue(feed_capacity), record_queue(record_capacity), input(std::move(events)),
        book(feed_queue, record_queue, BookMode::Mbo, 1h),
        collector(record_queue, max_records,
                  goal.target_raw_count == 0 && !goal.snapshot_after_raw_for
                      ? CollectGoal{input.size(), {}} : goal,
                  commands, signals),
        cleanup(control, {&producer_thread, &book_thread, &collector_thread}, &commands) {
    accepted.reserve(input.size());
  }

  void Produce(std::stop_token stop) {
    for (const auto& original : input) {
      auto value = original;
      for (;;) {
        if (stop.stop_requested() || control.Failed()) return;
        if (feed_queue.try_push(std::move(value))) break;
        std::this_thread::sleep_for(50us);
      }
      accepted.push_back(original);
      accepted_count.store(accepted.size());
    }
  }

  void Start() {
    collector_thread = Launch("collector", control,
        [&](std::stop_token stop) { collector.Run(stop, control); }, &collector_done);
    book_thread = Launch("book", control,
        [&](std::stop_token stop) { book.Run(metrics, stop, control); }, &book_done);
    producer_thread = Launch("producer", control,
        [&](std::stop_token stop) { Produce(stop); }, &producer_done);
  }

  void Finish() {
    StopJoin(producer_thread);
    commands.pause.store(false);
    StopJoin(book_thread);
    StopJoin(collector_thread);
    cleanup.Dismiss();
    REQUIRE_FALSE(control.Failed());
    REQUIRE(producer_done.load());
    REQUIRE(book_done.load());
    REQUIRE(collector_done.load());
    REQUIRE(signals.done.load());
    REQUIRE(metrics.book_events.Read() == accepted.size());
    CheckRaw(collector.Results(), accepted);
    CheckWriterUntouched(metrics);
  }

  void WaitFailure(const char* stage) {
    WaitUntil([&] { return control.Failed(); }, control, deadline);
    cleanup.Abort();
    REQUIRE(control.Failed());
    REQUIRE(std::string(control.Stage()) == stage);
    REQUIRE(control.Error());
    REQUIRE(book_done.load());
    REQUIRE(collector_done.load());
    REQUIRE(producer_done.load());
  }
};

}  // namespace

TEST_CASE("collector saves records and requires a snapshot after the raw goal", "[pipeline][collector]") {
  RecordQueue queue(4);
  RunControl control;
  CollectorControl commands;
  CollectorSignals signals;
  RecordCollector collector(queue, 4, {1, 7}, commands, signals);
  qlib::book before{};
  before.instrument_id = 7;
  before.event_ns = 10;
  REQUIRE(queue.try_push(qlib::record{before}));
  const auto order = MakeOrder(42, qlib::side::buy, 100, 3, 7);
  REQUIRE(queue.try_push(qlib::record{order}));
  std::jthread thread;
  ThreadCleanup cleanup(control, {&thread}, &commands);
  thread = Launch("collector", control,
      [&](std::stop_token stop) { collector.Run(stop, control); });
  const auto deadline = ScenarioDeadline();
  WaitUntil([&] { return signals.raw_count.load() == 1; }, control, deadline);
  REQUIRE_FALSE(signals.goal_reached.load());
  auto after = before;
  after.event_ns = 20;
  REQUIRE(queue.try_push(qlib::record{after}));
  WaitUntil([&] { return signals.goal_reached.load(); }, control, deadline);
  StopJoin(thread);
  cleanup.Dismiss();
  REQUIRE_FALSE(control.Failed());
  REQUIRE(signals.done.load());
  REQUIRE(collector.Results().size() == 3);
  REQUIRE(std::get<qlib::book>(collector.Results()[0]).event_ns == 10);
  CheckInput(std::get<qlib::order>(collector.Results()[1]), order);
  REQUIRE(std::get<qlib::book>(collector.Results()[2]).event_ns == 20);
}

TEST_CASE("network UC02 preserves all six original events and final book", "[pipeline][network]") {
  const auto input = Uc02();
  NetworkScenario scenario(EncodePlan(input), BookMode::Mbo, {6, 1});
  scenario.Start();
  scenario.Finish(6, 0);
  CheckRaw(scenario.collector.Results(), input, true);
  const auto& snapshot = LastBook(scenario.collector.Results(), 1);
  REQUIRE(snapshot.bid_price[0] == 100);
  REQUIRE(snapshot.bid_qty[0] == 7);
  REQUIRE(snapshot.ask_price[0] == 101);
  REQUIRE(snapshot.ask_qty[0] == 6);
  REQUIRE(snapshot.event_ns == 106);
  REQUIRE(snapshot.recv_ns > 0);
  REQUIRE(scenario.metrics.feed_bytes.Read() == 438);
  REQUIRE(scenario.metrics.feed_orders.Read() == 5);
  REQUIRE(scenario.metrics.feed_trades.Read() == 1);
  REQUIRE(scenario.metrics.feed_levels.Read() == 0);
  REQUIRE(scenario.metrics.book_events.Read() == 6);
  REQUIRE(scenario.metrics.book_samples.Read() == 1);
  REQUIRE(scenario.metrics.book_instruments.Read() == 1);
  REQUIRE(scenario.metrics.book_resting_orders.Read() == 3);
  REQUIRE(scenario.publisher_signals.sent_messages.load() == 6);
}

TEST_CASE("network L2 UC03 deletes depth and rejects trailing MBO frames", "[pipeline][network]") {
  const std::vector<qlib::feed_event> accepted{
      MakeLevel(qlib::side::buy, 100, 20, 9, 101, -777),
      MakeLevel(qlib::side::buy, 100, 8, 9, 102, -777),
      MakeTrade(0, 0, 3, 9, 103, -777),
      MakeLevel(qlib::side::buy, 100, 0, 9, 104, -777)};
  auto plan = EncodePlan(accepted);
  const auto rejected = EncodePlan({MakeOrder(1, qlib::side::buy, 100, 3, 99), MakeCancel(1, 1, 99)});
  plan.insert(plan.end(), rejected.begin(), rejected.end());
  plan.push_back({{{std::byte{255}}}});
  NetworkScenario scenario(std::move(plan), BookMode::L2, {4, 9});
  scenario.Start();
  scenario.Finish(7, 3);
  CheckRaw(scenario.collector.Results(), accepted, true);
  const auto& snapshot = LastBook(scenario.collector.Results(), 9);
  REQUIRE(snapshot.bid_price[0] == 0);
  REQUIRE(snapshot.bid_qty[0] == 0);
  REQUIRE(snapshot.ask_price[0] == 0);
  REQUIRE(snapshot.ask_qty[0] == 0);
  REQUIRE(snapshot.event_ns == 104);
  REQUIRE(scenario.metrics.feed_bytes.Read() == 343);
  REQUIRE(scenario.metrics.feed_orders.Read() == 0);
  REQUIRE(scenario.metrics.feed_trades.Read() == 1);
  REQUIRE(scenario.metrics.feed_levels.Read() == 3);
  REQUIRE(scenario.metrics.book_events.Read() == 4);
  REQUIRE(scenario.metrics.book_instruments.Read() == 1);
  REQUIRE(scenario.metrics.book_resting_orders.Read() == 0);
  for (const auto& record : scenario.collector.Results())
    if (const auto* book = std::get_if<qlib::book>(&record)) REQUIRE(book->instrument_id == 9);
}

TEST_CASE("network business ignores are forwarded and instrument books stay separate", "[pipeline][network]") {
  auto input = Uc02();
  input.emplace_back(MakeOrder(1, qlib::side::buy, 500, 500, 1, 107, -777));
  input.emplace_back(MakeCancel(999, 5, 1, 108, -777));
  input.emplace_back(MakeModify(1, qlib::side::sell, 200, 40, 1, 109, -777));
  input.emplace_back(MakeOrder(4, qlib::side::buy, 100, std::numeric_limits<std::int64_t>::max(), 1, 110, -777));
  auto market = MakeOrder(5, qlib::side::sell, 0, 3, 1, 111, -777);
  market.type = qlib::order_type::market;
  input.emplace_back(market);
  for (std::uint32_t id = 2; id <= 20; ++id)
    input.emplace_back(MakeOrder(1, qlib::side::buy, 100 + id, id, id, 200 + id, -777));
  input.emplace_back(MakeModify(1, qlib::side::buy, 222, 9, 2, 300, -777));
  input.emplace_back(MakeCancel(1, 20, 20, 301, -777));
  NetworkScenario scenario(EncodePlan(input), BookMode::Mbo, {input.size(), 20});
  scenario.Start();
  scenario.Finish(32, 0);
  CheckRaw(scenario.collector.Results(), input, true);
  REQUIRE(scenario.metrics.book_events.Read() == 32);
  REQUIRE(scenario.metrics.book_instruments.Read() == 20);
  REQUIRE(scenario.metrics.book_resting_orders.Read() == 21);
  REQUIRE(LastBook(scenario.collector.Results(), 1).bid_qty[0] == 7);
  REQUIRE(LastBook(scenario.collector.Results(), 1).ask_qty[0] == 6);
  REQUIRE(LastBook(scenario.collector.Results(), 1).event_ns == 111);
  REQUIRE(LastBook(scenario.collector.Results(), 2).bid_price[0] == 222);
  REQUIRE(LastBook(scenario.collector.Results(), 2).bid_qty[0] == 9);
  REQUIRE(LastBook(scenario.collector.Results(), 20).bid_qty[0] == 0);
  REQUIRE(LastBook(scenario.collector.Results(), 20).event_ns == 301);
}

TEST_CASE("controlled accepted ledger survives both queues backing up and ordinary stop", "[pipeline][drain]") {
  ControlledScenario scenario(ManyOrders(128));
  scenario.commands.pause.store(true);
  scenario.Start();
  WaitUntil([&] {
    return scenario.metrics.book_events.Read() == 2 && scenario.accepted_count.load() == 4;
  }, scenario.control, scenario.deadline);
  scenario.Finish();
  REQUIRE(scenario.accepted.size() == 4);
  REQUIRE(scenario.collector.Results().size() == 4);
  REQUIRE(std::get<qlib::order>(scenario.collector.Results().back()).order_id == 4);
}

TEST_CASE("controlled pipeline keeps every original event across full queue recovery", "[pipeline][drain]") {
  ControlledScenario scenario(ManyOrders(257));
  scenario.commands.pause.store(true);
  scenario.Start();
  WaitUntil([&] { return scenario.accepted_count.load() == 4; }, scenario.control, scenario.deadline);
  scenario.commands.pause.store(false);
  WaitUntil([&] { return scenario.producer_done.load(); }, scenario.control, scenario.deadline);
  scenario.Finish();
  REQUIRE(scenario.accepted.size() == 257);
  REQUIRE(scenario.collector.Results().size() == 257);
}

TEST_CASE("last admitted event remains after immediate upstream join and downstream stop", "[pipeline][drain]") {
  for (int round = 0; round < 32; ++round) {
    INFO("handoff round " << round);
    ControlledScenario scenario(ManyOrders(3), 1, 1);
    scenario.Start();
    WaitUntil([&] { return scenario.producer_done.load(); }, scenario.control, scenario.deadline);
    scenario.Finish();
    REQUIRE(scenario.accepted.size() == 3);
    REQUIRE(std::get<qlib::order>(scenario.collector.Results().back()).order_id == 3);
  }
}

TEST_CASE("collector failure releases a book blocked on full output", "[pipeline][failure]") {
  ControlledScenario scenario(ManyOrders(128));
  scenario.commands.pause.store(true);
  scenario.Start();
  WaitUntil([&] {
    return scenario.metrics.book_events.Read() == 2 && scenario.accepted_count.load() == 4;
  }, scenario.control, scenario.deadline);
  scenario.commands.fail_requested.store(true);
  scenario.WaitFailure("collector");
  REQUIRE(scenario.collector.Results().empty());
  REQUIRE_FALSE(scenario.signals.goal_reached.load());
  REQUIRE_THROWS_AS(std::rethrow_exception(scenario.control.Error()), std::runtime_error);
}

TEST_CASE("collector capacity failure preserves its prefix and stops the pipeline", "[pipeline][failure]") {
  ControlledScenario scenario(ManyOrders(20), 2, 1, 2, {20, {}});
  scenario.Start();
  scenario.WaitFailure("collector");
  REQUIRE(scenario.collector.Results().size() == 2);
  REQUIRE(std::get<qlib::order>(scenario.collector.Results()[0]).order_id == 1);
  REQUIRE(std::get<qlib::order>(scenario.collector.Results()[1]).order_id == 2);
  REQUIRE_FALSE(scenario.signals.goal_reached.load());
  REQUIRE_THROWS_AS(std::rethrow_exception(scenario.control.Error()), std::length_error);
}

TEST_CASE("network downstream failure releases simultaneous Feed and Book queue waits", "[pipeline][network][failure]") {
  NetworkScenario scenario(EncodePlan(ManyOrders(64)), BookMode::Mbo, {64, {}}, 2, 1, 1h);
  scenario.commands.pause.store(true);
  scenario.Start();
  WaitUntil([&] {
    return scenario.metrics.book_events.Read() == 2 && scenario.metrics.feed_orders.Read() == 5;
  }, scenario.control, scenario.deadline);
  scenario.commands.fail_requested.store(true);
  WaitUntil([&] { return scenario.control.Failed(); }, scenario.control, scenario.deadline);
  scenario.cleanup.Abort();
  REQUIRE(std::string(scenario.control.Stage()) == "collector");
  REQUIRE_THROWS_AS(std::rethrow_exception(scenario.control.Error()), std::runtime_error);
  REQUIRE(scenario.signals.done.load());
  REQUIRE_FALSE(scenario.signals.goal_reached.load());
  REQUIRE(scenario.collector.Results().empty());
  REQUIRE_FALSE(scenario.publisher_thread.joinable());
  REQUIRE_FALSE(scenario.feed_thread.joinable());
  REQUIRE_FALSE(scenario.book_thread.joinable());
  REQUIRE_FALSE(scenario.collector_thread.joinable());
}

TEST_CASE("book contract exceptions reach shared failure without counting an applied event", "[pipeline][failure]") {
  ControlledScenario scenario({MakeLevel(qlib::side::buy, 100, 2)});
  scenario.Start();
  scenario.WaitFailure("book");
  REQUIRE(scenario.metrics.book_events.Read() == 0);
  REQUIRE(scenario.collector.Results().empty());
  REQUIRE_THROWS_AS(std::rethrow_exception(scenario.control.Error()), std::logic_error);
}

TEST_CASE("metric overflow stops its owning book stage without wrapping", "[pipeline][failure]") {
  ControlledScenario scenario(ManyOrders(1));
  scenario.metrics.book_events.Set(std::numeric_limits<std::uint64_t>::max());
  scenario.Start();
  scenario.WaitFailure("book");
  REQUIRE(scenario.metrics.book_events.Read() == std::numeric_limits<std::uint64_t>::max());
  REQUIRE(scenario.collector.Results().empty());
  REQUIRE_THROWS_AS(std::rethrow_exception(scenario.control.Error()), std::overflow_error);
}

TEST_CASE("feed initialization failure releases previously started downstream stages", "[pipeline][failure]") {
  ControlledScenario scenario({});
  FeedThread feed("not-a-transport://invalid", BookMode::Mbo, scenario.feed_queue);
  std::jthread feed_thread;
  std::atomic<bool> feed_done{false};
  ThreadCleanup cleanup(scenario.control, {&feed_thread}, &scenario.commands);
  scenario.Start();
  StopJoin(scenario.producer_thread);
  feed_thread = Launch("feed", scenario.control,
      [&](std::stop_token stop) { feed.Run(scenario.metrics, stop, scenario.control); }, &feed_done);
  WaitUntil([&] { return scenario.control.Failed(); }, scenario.control, scenario.deadline);
  cleanup.Abort();
  scenario.cleanup.Abort();
  REQUIRE(feed_done.load());
  REQUIRE(scenario.book_done.load());
  REQUIRE(scenario.collector_done.load());
  REQUIRE(std::string(scenario.control.Stage()) == "feed");
  REQUIRE(scenario.control.Error());
  REQUIRE(scenario.collector.Results().empty());
}

TEST_CASE("partial thread launch failure joins exactly the threads already started", "[pipeline][failure]") {
  const auto fail_at = GENERATE(0, 1, 2);
  ControlledScenario scenario(ManyOrders(4));
  scenario.commands.pause.store(true);
  int attempts = 0;
  auto start = [&](std::jthread& slot, const char* stage, auto function, std::atomic<bool>& done) {
    if (attempts++ == fail_at)
      throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
    slot = Launch(stage, scenario.control, function, &done);
  };
  try {
    start(scenario.collector_thread, "collector",
          [&](std::stop_token stop) { scenario.collector.Run(stop, scenario.control); },
          scenario.collector_done);
    start(scenario.book_thread, "book",
          [&](std::stop_token stop) { scenario.book.Run(scenario.metrics, stop, scenario.control); },
          scenario.book_done);
    start(scenario.producer_thread, "producer",
          [&](std::stop_token stop) { scenario.Produce(stop); }, scenario.producer_done);
  } catch (...) {
    scenario.control.Fail("launch", std::current_exception());
  }
  scenario.cleanup.Abort();
  REQUIRE(scenario.control.Failed());
  REQUIRE(std::string(scenario.control.Stage()) == "launch");
  REQUIRE_THROWS_AS(std::rethrow_exception(scenario.control.Error()), std::system_error);
  REQUIRE(scenario.collector_done.load() == (fail_at >= 1));
  REQUIRE(scenario.book_done.load() == (fail_at >= 2));
  REQUIRE_FALSE(scenario.producer_done.load());
  REQUIRE_FALSE(scenario.commands.pause.load());
  REQUIRE_FALSE(scenario.collector_thread.joinable());
  REQUIRE_FALSE(scenario.book_thread.joinable());
  REQUIRE_FALSE(scenario.producer_thread.joinable());
}

TEST_CASE("scenario deadline failure uses the same cleanup as worker failure", "[pipeline][failure]") {
  ControlledScenario scenario({});
  scenario.commands.pause.store(true);
  scenario.Start();
  try {
    WaitUntil([] { return false; }, scenario.control, std::chrono::steady_clock::now());
  } catch (...) {
    scenario.control.Fail("test", std::current_exception());
  }
  scenario.cleanup.Abort();
  REQUIRE(scenario.control.Failed());
  REQUIRE(std::string(scenario.control.Stage()) == "test");
  REQUIRE_THROWS_AS(std::rethrow_exception(scenario.control.Error()), std::runtime_error);
  REQUIRE(scenario.producer_done.load());
  REQUIRE(scenario.book_done.load());
  REQUIRE(scenario.collector_done.load());
  REQUIRE_FALSE(scenario.commands.pause.load());
  REQUIRE(scenario.collector.Results().empty());
}

TEST_CASE("publisher cancellation completes pending subscription promise", "[pipeline][publisher]") {
  RunControl control;
  PublisherSignals signals;
  FeedPublisher publisher({}, signals);
  auto endpoint = signals.bound_endpoint.get_future();
  auto subscribed = signals.subscribed.get_future();
  std::jthread thread;
  ThreadCleanup cleanup(control, {&thread});
  thread = Launch("publisher", control,
      [&](std::stop_token stop) { publisher.Run(stop, control); });
  const auto deadline = ScenarioDeadline();
  WaitUntil([&] { return endpoint.wait_for(0s) == std::future_status::ready; }, control, deadline);
  REQUIRE_FALSE(endpoint.get().empty());
  StopJoin(thread);
  cleanup.Dismiss();
  REQUIRE_FALSE(control.Failed());
  REQUIRE(subscribed.wait_for(0s) == std::future_status::ready);
  REQUIRE_THROWS_AS(subscribed.get(), std::runtime_error);
  REQUIRE_FALSE(signals.sent_all.load());
  REQUIRE(signals.sent_messages.load() == 0);
}
