#include "Runtime.h"
#include "Threads/MetricsThread.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <array>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <latch>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>

static_assert(!std::is_copy_constructible_v<qbook::RunControl>);
static_assert(!std::is_copy_assignable_v<qbook::RunControl>);
static_assert(!std::is_move_constructible_v<qbook::RunControl>);
static_assert(!std::is_move_assignable_v<qbook::RunControl>);
static_assert(!std::is_copy_constructible_v<qbook::Metric>);
static_assert(!std::is_copy_assignable_v<qbook::Metric>);
static_assert(!std::is_move_constructible_v<qbook::Metric>);
static_assert(!std::is_move_assignable_v<qbook::Metric>);
static_assert(!std::is_copy_constructible_v<qbook::Metrics>);
static_assert(!std::is_copy_assignable_v<qbook::Metrics>);
static_assert(!std::is_move_constructible_v<qbook::Metrics>);
static_assert(!std::is_move_assignable_v<qbook::Metrics>);
static_assert(alignof(qbook::Metric) == QLIB_CACHE_LINE);
static_assert(sizeof(qbook::Metric) % QLIB_CACHE_LINE == 0);

TEST_CASE("RunControl starts healthy and preserves the first failure", "[runtime]") {
  qbook::RunControl control;
  REQUIRE_FALSE(control.Failed());
  REQUIRE(control.Stage() == nullptr);
  REQUIRE(control.Error() == nullptr);

  const auto first = std::make_exception_ptr(std::runtime_error("feed failed"));
  const auto second = std::make_exception_ptr(std::logic_error("writer failed"));
  control.Fail("feed", first);
  control.Fail("writer", second);

  REQUIRE(control.Failed());
  REQUIRE(std::string(control.Stage()) == "feed");
  REQUIRE(control.Error() == first);
  REQUIRE_THROWS_WITH(std::rethrow_exception(control.Error()), "feed failed");
}

TEST_CASE("RunControl keeps one complete report after concurrent reporters join", "[runtime]") {
  qbook::RunControl control;
  constexpr std::array stages{"feed", "book", "writer", "metrics"};
  const std::array errors{
      std::make_exception_ptr(std::runtime_error("feed failed")),
      std::make_exception_ptr(std::runtime_error("book failed")),
      std::make_exception_ptr(std::runtime_error("writer failed")),
      std::make_exception_ptr(std::runtime_error("metrics failed"))};
  std::barrier start{5};
  std::array<std::jthread, 4> reporters;
  for (std::size_t i = 0; i < reporters.size(); ++i) {
    reporters[i] = std::jthread([&, i] {
      start.arrive_and_wait();
      control.Fail(stages[i], errors[i]);
    });
  }
  start.arrive_and_wait();
  for (auto& reporter : reporters) {
    reporter.join();
  }

  REQUIRE(control.Failed());
  REQUIRE(control.Stage() != nullptr);
  const auto saved_error = control.Error();
  REQUIRE(saved_error != nullptr);
  bool paired = false;
  for (std::size_t i = 0; i < stages.size(); ++i) {
    if (std::string(control.Stage()) == stages[i] && saved_error == errors[i]) {
      paired = true;
    }
  }
  REQUIRE(paired);
  REQUIRE_THROWS_AS(std::rethrow_exception(saved_error), std::runtime_error);
  control.Fail("later", std::make_exception_ptr(std::runtime_error("later failed")));
  REQUIRE(control.Error() == saved_error);
}

TEST_CASE("RunControl permits concurrent observers of its failure flag", "[runtime]") {
  qbook::RunControl control;
  const auto error = std::make_exception_ptr(std::runtime_error("book failed"));
  std::barrier start{3};
  std::latch reported{1};
  bool observed_failure = false;
  std::jthread observer([&] {
    start.arrive_and_wait();
    for (int i = 0; i < 10'000; ++i) {
      observed_failure = control.Failed() || observed_failure;
    }
    reported.wait();
    observed_failure = control.Failed();
  });
  std::jthread reporter([&] {
    start.arrive_and_wait();
    control.Fail("book", error);
    reported.count_down();
  });
  start.arrive_and_wait();
  reporter.join();
  observer.join();

  REQUIRE(control.Failed());
  REQUIRE(control.Error() == error);
  REQUIRE(observed_failure);
}

TEST_CASE("Metric adds counts and replaces state values", "[metrics]") {
  qbook::Metric metric;
  REQUIRE(metric.Read() == 0);
  metric.Add();
  metric.Add(6);
  metric.Add(0);
  REQUIRE(metric.Read() == 7);
  REQUIRE(metric.Read() == 7);
  metric.Set(2);
  REQUIRE(metric.Read() == 2);
  metric.Set(0);
  REQUIRE(metric.Read() == 0);
}

TEST_CASE("Metric accepts the uint64 limit and preserves its value on overflow", "[metrics]") {
  qbook::Metric metric;
  constexpr auto max = std::numeric_limits<std::uint64_t>::max();
  metric.Set(max - 2);
  metric.Add(2);
  REQUIRE(metric.Read() == max);
  metric.Add(0);
  REQUIRE(metric.Read() == max);
  REQUIRE_THROWS_AS(metric.Add(), std::overflow_error);
  REQUIRE(metric.Read() == max);

  metric.Set(9);
  REQUIRE_THROWS_AS(metric.Add(max), std::overflow_error);
  REQUIRE(metric.Read() == 9);
  metric.Set(max);
  REQUIRE(metric.Read() == max);
}

TEST_CASE("Metric supports one writer and concurrent readers", "[metrics]") {
  qbook::Metric metric;
  constexpr std::uint64_t total = 20'000;
  std::barrier start{4};
  std::exception_ptr writer_error;
  std::array<bool, 2> valid{true, true};
  std::jthread writer([&] {
    start.arrive_and_wait();
    try {
      for (std::uint64_t i = 0; i < total; ++i) {
        metric.Add();
      }
    } catch (...) {
      writer_error = std::current_exception();
    }
  });
  std::array<std::jthread, 2> readers;
  for (std::size_t reader = 0; reader < readers.size(); ++reader) {
    readers[reader] = std::jthread([&, reader] {
      start.arrive_and_wait();
      std::uint64_t previous = 0;
      for (std::uint64_t i = 0; i < total; ++i) {
        const auto current = metric.Read();
        valid[reader] = valid[reader] && current >= previous && current <= total;
        previous = current;
      }
    });
  }
  start.arrive_and_wait();
  writer.join();
  for (auto& reader : readers) {
    reader.join();
  }

  REQUIRE(writer_error == nullptr);
  REQUIRE(valid[0]);
  REQUIRE(valid[1]);
  REQUIRE(metric.Read() == total);
}

TEST_CASE("SnapshotMetrics initializes all counters to zero", "[metrics]") {
  const qbook::Metrics metrics;
  const auto snapshot = qbook::SnapshotMetrics(metrics, 123);
  REQUIRE(snapshot.ts_ns == 123);
  REQUIRE(snapshot.feed_messages == 0);
  REQUIRE(snapshot.feed_bytes == 0);
  REQUIRE(snapshot.feed_orders == 0);
  REQUIRE(snapshot.feed_trades == 0);
  REQUIRE(snapshot.feed_levels == 0);
  REQUIRE(snapshot.feed_dropped == 0);
  REQUIRE(snapshot.book_events == 0);
  REQUIRE(snapshot.book_apply_ns == 0);
  REQUIRE(snapshot.book_samples == 0);
  REQUIRE(snapshot.book_instruments == 0);
  REQUIRE(snapshot.book_resting_orders == 0);
  REQUIRE(snapshot.book_memory_bytes == 0);
  REQUIRE(snapshot.writer_orders == 0);
  REQUIRE(snapshot.writer_trades == 0);
  REQUIRE(snapshot.writer_levels == 0);
  REQUIRE(snapshot.writer_books == 0);
}

TEST_CASE("SnapshotMetrics maps all counters without narrowing and owns its values", "[metrics]") {
  qbook::Metrics metrics;
  metrics.feed_messages.Set(1);
  metrics.feed_bytes.Set(9'007'199'254'740'993ULL);
  metrics.feed_orders.Set(3);
  metrics.feed_trades.Set(4);
  metrics.feed_levels.Set(5);
  metrics.feed_dropped.Set(6);
  metrics.book_events.Set(7);
  metrics.book_apply_ns.Set(8);
  metrics.book_samples.Set(9);
  metrics.book_instruments.Set(10);
  metrics.book_resting_orders.Set(11);
  metrics.book_memory_bytes.Set(12);
  metrics.writer_orders.Set(13);
  metrics.writer_trades.Set(14);
  metrics.writer_levels.Set(15);
  metrics.writer_books.Set(std::numeric_limits<std::uint64_t>::max());
  const auto snapshot = qbook::SnapshotMetrics(metrics, 1'797'000'000'000'000'123LL);
  metrics.feed_messages.Set(99);
  metrics.feed_bytes.Set(0);
  metrics.writer_books.Set(0);

  REQUIRE(snapshot.ts_ns == 1'797'000'000'000'000'123LL);
  REQUIRE(snapshot.feed_messages == 1);
  REQUIRE(snapshot.feed_bytes == 9'007'199'254'740'993ULL);
  REQUIRE(snapshot.feed_orders == 3);
  REQUIRE(snapshot.feed_trades == 4);
  REQUIRE(snapshot.feed_levels == 5);
  REQUIRE(snapshot.feed_dropped == 6);
  REQUIRE(snapshot.book_events == 7);
  REQUIRE(snapshot.book_apply_ns == 8);
  REQUIRE(snapshot.book_samples == 9);
  REQUIRE(snapshot.book_instruments == 10);
  REQUIRE(snapshot.book_resting_orders == 11);
  REQUIRE(snapshot.book_memory_bytes == 12);
  REQUIRE(snapshot.writer_orders == 13);
  REQUIRE(snapshot.writer_trades == 14);
  REQUIRE(snapshot.writer_levels == 15);
  REQUIRE(snapshot.writer_books == std::numeric_limits<std::uint64_t>::max());
  const auto next = qbook::SnapshotMetrics(metrics, 456);
  REQUIRE(next.ts_ns == 456);
  REQUIRE(next.feed_messages == 99);
  REQUIRE(next.feed_bytes == 0);
  REQUIRE(next.writer_books == 0);
}
