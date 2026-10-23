#include "Threads/FeedThread.h"
#include "Threads/MetricsThread.h"
#include "support/FeedEncoder.h"
#include "support/FeedPublisher.h"
#include "support/OrderbookTestSupport.h"
#include "support/PipelineTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <future>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <optional>
#include <sstream>

using namespace qbook;
using namespace qbook::testing;
using namespace std::chrono_literals;

namespace {
TestMessage Frame(const qlib::feed_event& event) {
  const auto encoded = EncodeFeedEvent(event);
  return {{{encoded.bytes().begin(), encoded.bytes().end()}}};
}

MessagePlan ThreeOrders() {
  MessagePlan plan;
  for (int id = 1; id <= 3; ++id) plan.push_back(Frame(MakeOrder(id, qlib::side::buy, 100, 10)));
  return plan;
}

// 声明顺序使清理守卫先结束线程，随后才释放阶段、信号和队列。
struct FeedScenario {
  FeedQueue queue;
  Metrics metrics;
  RunControl control;
  PublisherSignals signals;
  FeedPublisher publisher;
  std::optional<FeedThread> feed;
  std::atomic<bool> feed_done{false};
  std::jthread publisher_thread;
  std::jthread feed_thread;
  ThreadCleanup cleanup;

  FeedScenario(MessagePlan plan, BookMode mode = BookMode::Mbo, std::size_t capacity = 32)
      : queue(capacity), publisher(std::move(plan), signals),
        cleanup(control, {&publisher_thread, &feed_thread}) {
    auto endpoint = signals.bound_endpoint.get_future();
    auto subscribed = signals.subscribed.get_future();
    publisher_thread = Launch("publisher", control, [&](auto stop) { publisher.Run(stop, control); });
    const auto deadline = ScenarioDeadline();
    WaitUntil([&] { return endpoint.wait_for(0ms) == std::future_status::ready; }, control, deadline);
    feed.emplace(endpoint.get(), mode, queue);
    feed_thread = Launch("feed", control, [&](auto stop) { feed->Run(metrics, stop, control); }, &feed_done);
    WaitUntil([&] { return subscribed.wait_for(0ms) == std::future_status::ready; }, control, deadline);
    subscribed.get();
  }
  void Stop() {
    StopJoin(publisher_thread);
    StopJoin(feed_thread);
    cleanup.Dismiss();
  }
  void Received(std::uint64_t messages) {
    WaitUntil([&] { return metrics.feed_messages.Read() == messages; }, control, ScenarioDeadline());
  }
  std::vector<qlib::feed_event> Collect(std::size_t count) {
    std::vector<qlib::feed_event> events;
    events.reserve(count);
    WaitUntil([&] {
      if (auto event = queue.try_pop()) events.push_back(std::move(*event));
      return events.size() == count;
    }, control, ScenarioDeadline());
    return events;
  }
};

std::vector<qlib::feed_event> Drain(FeedQueue& queue) {
  std::vector<qlib::feed_event> result;
  while (auto event = queue.try_pop()) result.push_back(std::move(*event));
  return result;
}
}  // namespace

TEST_CASE("Feed rejects malformed and multipart messages then resumes single frame input", "[feed][network]") {
  auto valid = Frame(MakeOrder(17, qlib::side::buy, 100, 10, 3, 123, -999));
  auto wrong_field = valid;
  wrong_field.parts[0][1 + offsetof(qlib::order, side)] = std::byte{7};
  TestMessage multipart{{valid.parts[0], {}, valid.parts[0]}};
  MessagePlan plan{{{{}}}, {{{std::byte{255}}}}, {{{std::byte{0}}}}, wrong_field,
                   multipart, Frame(MakeLevel(qlib::side::buy, 100, 5)), valid};
  FeedScenario scenario(std::move(plan));
  scenario.Received(7);
  const auto events = scenario.Collect(1);
  scenario.Stop();
  REQUIRE_FALSE(scenario.control.Failed());
  REQUIRE_FALSE(scenario.queue.try_pop());
  REQUIRE(events.size() == 1);
  auto expected = MakeOrder(17, qlib::side::buy, 100, 10, 3, 123, -999);
  const auto& actual = std::get<qlib::order>(events[0]);
  REQUIRE(actual.recv_ns > 0);
  expected.recv_ns = actual.recv_ns;
  CheckInput(actual, expected);
  REQUIRE(scenario.metrics.feed_bytes.Read() == 375);
  REQUIRE(scenario.metrics.feed_orders.Read() == 1);
  REQUIRE(scenario.metrics.feed_dropped.Read() == 6);
  REQUIRE(scenario.metrics.book_events.Read() == 0);
}

TEST_CASE("Feed accepts MBO types and counts cancel with orders", "[feed][network]") {
  MessagePlan plan{Frame(MakeOrder(1, qlib::side::buy, 100, 10)),
                   Frame(MakeModify(1, qlib::side::buy, 0, -4)),
                   Frame(MakeTrade(1, 0, 2)), Frame(MakeCancel(1, 3))};
  FeedScenario scenario(std::move(plan));
  scenario.Received(4);
  const auto events = scenario.Collect(4);
  scenario.Stop();
  REQUIRE_FALSE(scenario.control.Failed());
  REQUIRE_FALSE(scenario.queue.try_pop());
  REQUIRE(events.size() == 4);
  REQUIRE(std::get<qlib::order>(events[1]).new_qty == -4);
  REQUIRE(std::get<qlib::trade>(events[2]).qty == 2);
  REQUIRE(std::get<qlib::cancel>(events[3]).qty == 3);
  REQUIRE(scenario.metrics.feed_orders.Read() == 3);
  REQUIRE(scenario.metrics.feed_trades.Read() == 1);
  REQUIRE(scenario.metrics.feed_bytes.Read() == 276);
  REQUIRE(scenario.metrics.feed_dropped.Read() == 0);
}

TEST_CASE("Feed L2 mode accepts deletion and trade while rejecting order and cancel", "[feed][network]") {
  MessagePlan plan{Frame(MakeLevel(qlib::side::sell, 101, -3)), Frame(MakeTrade(0, 0, 2)),
                   Frame(MakeOrder(1, qlib::side::buy, 100, 2)), Frame(MakeCancel(1, 1))};
  FeedScenario scenario(std::move(plan), BookMode::L2);
  scenario.Received(4);
  const auto events = scenario.Collect(2);
  scenario.Stop();
  REQUIRE_FALSE(scenario.control.Failed());
  REQUIRE_FALSE(scenario.queue.try_pop());
  REQUIRE(events.size() == 2);
  REQUIRE(std::get<qlib::level>(events[0]).qty == -3);
  REQUIRE(std::holds_alternative<qlib::trade>(events[1]));
  REQUIRE(scenario.metrics.feed_levels.Read() == 1);
  REQUIRE(scenario.metrics.feed_trades.Read() == 1);
  REQUIRE(scenario.metrics.feed_orders.Read() == 0);
  REQUIRE(scenario.metrics.feed_dropped.Read() == 2);
  REQUIRE(scenario.metrics.feed_bytes.Read() == 244);
}

TEST_CASE("Feed full queue stop preserves only two admitted events", "[feed][network][stop]") {
  FeedScenario scenario(ThreeOrders(), BookMode::Mbo, 2);
  WaitUntil([&] { return scenario.metrics.feed_orders.Read() == 3; }, scenario.control, ScenarioDeadline());
  scenario.Stop();
  REQUIRE_FALSE(scenario.control.Failed());
  const auto events = Drain(scenario.queue);
  REQUIRE(events.size() == 2);
  REQUIRE(std::get<qlib::order>(events[0]).order_id == 1);
  REQUIRE(std::get<qlib::order>(events[1]).order_id == 2);
  REQUIRE(scenario.metrics.feed_messages.Read() == 3);
  REQUIRE(scenario.metrics.feed_dropped.Read() == 0);
}

TEST_CASE("Feed retries a full queue once space returns without recounting or retimestamping", "[feed][network]") {
  FeedScenario scenario(ThreeOrders(), BookMode::Mbo, 2);
  WaitUntil([&] { return scenario.metrics.feed_orders.Read() == 3; }, scenario.control, ScenarioDeadline());
  const auto release_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  std::vector<qlib::feed_event> events;
  events.reserve(3);
  WaitUntil([&] {
    if (auto event = scenario.queue.try_pop()) events.push_back(std::move(*event));
    return events.size() == 3;
  }, scenario.control, ScenarioDeadline());
  scenario.Stop();
  REQUIRE_FALSE(scenario.control.Failed());
  REQUIRE_FALSE(scenario.queue.try_pop());
  for (int i = 0; i < 3; ++i) REQUIRE(std::get<qlib::order>(events[i]).order_id == i + 1);
  REQUIRE(std::get<qlib::order>(events[2]).recv_ns <= release_ns);
  REQUIRE(scenario.metrics.feed_messages.Read() == 3);
  REQUIRE(scenario.metrics.feed_orders.Read() == 3);
  REQUIRE(scenario.metrics.feed_bytes.Read() == 243);
  REQUIRE(scenario.metrics.feed_dropped.Read() == 0);
}

TEST_CASE("Feed fatal signal releases a full queue wait", "[feed][network][failure]") {
  FeedScenario scenario(ThreeOrders(), BookMode::Mbo, 2);
  WaitUntil([&] { return scenario.metrics.feed_orders.Read() == 3; }, scenario.control, ScenarioDeadline());
  scenario.control.Fail("consumer", std::make_exception_ptr(std::runtime_error("consumer failed")));
  scenario.Stop();
  REQUIRE(scenario.feed_done.load());
  REQUIRE(std::string(scenario.control.Stage()) == "consumer");
  REQUIRE(Drain(scenario.queue).size() == 2);
}

TEST_CASE("Feed without publisher times out without inventing messages", "[feed][network][stop]") {
  char directory[] = "/tmp/qbook-feed-idle-XXXXXX";
  REQUIRE(::mkdtemp(directory) != nullptr);
  struct RemoveDirectory {
    const char* path;
    ~RemoveDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
  } remove_directory{directory};
  FeedQueue queue(2);
  Metrics metrics;
  RunControl control;
  FeedThread feed("ipc://" + std::string(directory) + "/absent.sock", BookMode::Mbo, queue);
  std::atomic<bool> done{false};
  std::jthread worker;
  ThreadCleanup cleanup(control, {&worker});
  worker = Launch("feed", control, [&](auto stop) { feed.Run(metrics, stop, control); }, &done);
  // 只给无输入接收跨过超时的观察窗口；网络就绪测试使用 XPUB 通知。
  const auto observation = std::chrono::steady_clock::now() + 220ms;
  WaitUntil([&] { return std::chrono::steady_clock::now() >= observation; }, control, ScenarioDeadline());
  StopJoin(worker);
  cleanup.Dismiss();
  REQUIRE(done.load());
  REQUIRE_FALSE(control.Failed());
  REQUIRE(metrics.feed_messages.Read() == 0);
  REQUIRE(metrics.feed_bytes.Read() == 0);
  REQUIRE(metrics.feed_dropped.Read() == 0);
  REQUIRE_FALSE(queue.try_pop());
}

TEST_CASE("Feed observes preexisting stop and failure before network setup", "[feed][stop]") {
  FeedQueue queue(1);
  Metrics metrics;
  RunControl control;
  FeedThread stopped("unsupported://test", BookMode::Mbo, queue);
  std::stop_source stop;
  stop.request_stop();
  REQUIRE_NOTHROW(stopped.Run(metrics, stop.get_token(), control));
  control.Fail("test", std::make_exception_ptr(std::runtime_error("existing failure")));
  FeedThread failed("unsupported://test", BookMode::Mbo, queue);
  REQUIRE_NOTHROW(failed.Run(metrics, {}, control));
  REQUIRE(metrics.feed_messages.Read() == 0);
}

TEST_CASE("Feed rejection diagnostics are limited at one and every 1024", "[feed][network]") {
  std::ostringstream output;
  const auto previous = spdlog::default_logger();
  auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(output);
  auto logger = std::make_shared<spdlog::logger>("feed-test", sink);
  logger->set_pattern("%v");
  struct Restore { std::shared_ptr<spdlog::logger> logger; ~Restore() { spdlog::set_default_logger(logger); } } restore{previous};
  spdlog::set_default_logger(logger);
  MessagePlan plan(1025, TestMessage{{{std::byte{255}}}});
  plan.push_back(Frame(MakeOrder(1, qlib::side::buy, 100, 1)));
  FeedScenario scenario(std::move(plan));
  scenario.Received(1026);
  const auto events = scenario.Collect(1);
  scenario.Stop();
  REQUIRE_FALSE(scenario.control.Failed());
  REQUIRE(scenario.metrics.feed_dropped.Read() == 1025);
  REQUIRE(events.size() == 1);
  REQUIRE_FALSE(scenario.queue.try_pop());
  std::istringstream lines(output.str());
  std::string line;
  std::size_t rejected = 0;
  while (std::getline(lines, line)) if (line.find("拒绝行情") != std::string::npos) ++rejected;
  REQUIRE(rejected == 2);
}
