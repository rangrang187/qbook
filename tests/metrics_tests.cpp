#include "Runtime.h"
#include "Threads/MetricsThread.h"
#include "detail/MetricsLayout.h"
#include "detail/MetricsSend.h"
#include "support/MetricsThreadInspector.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <zmq.hpp>

#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <optional>

using namespace qbook;
using namespace qbook::testing;
using namespace std::chrono_literals;

TEST_CASE("metric binary maps every field and signed timestamp boundary exactly", "[metrics][layout]") {
  const auto timestamp = GENERATE(std::numeric_limits<std::int64_t>::min(),
                                 std::int64_t{-1}, std::int64_t{0},
                                 std::numeric_limits<std::int64_t>::max());
  Metrics metrics;
  const std::array fields{&Metrics::feed_messages, &Metrics::feed_bytes, &Metrics::feed_orders,
      &Metrics::feed_trades, &Metrics::feed_levels, &Metrics::feed_dropped,
      &Metrics::book_events, &Metrics::book_apply_ns, &Metrics::book_samples,
      &Metrics::book_instruments, &Metrics::book_resting_orders, &Metrics::book_memory_bytes,
      &Metrics::writer_orders, &Metrics::writer_trades, &Metrics::writer_levels, &Metrics::writer_books};
  const std::array<std::uint64_t, 16> values{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
      9'007'199'254'740'993ULL, 9'223'372'036'854'775'807ULL,
      9'223'372'036'854'775'808ULL, 18'446'744'073'709'551'615ULL};
  for (std::size_t i = 0; i < fields.size(); ++i) (metrics.*fields[i]).Set(values[i]);
  const auto sample = SnapshotMetrics(metrics, timestamp);
  const auto bytes = std::bit_cast<std::array<std::byte, 136>>(sample);
  std::int64_t decoded_time = 0;
  std::memcpy(&decoded_time, bytes.data(), 8);
  REQUIRE(decoded_time == timestamp);
  for (std::size_t i = 0; i < fields.size(); ++i) {
    std::uint64_t decoded = 0;
    std::memcpy(&decoded, bytes.data() + 8 * (i + 1), 8);
    INFO(i);
    REQUIRE(decoded == values[i]);
    for (std::size_t octet = 0; octet < 8; ++octet)
      REQUIRE(std::to_integer<unsigned>(bytes[8 * (i + 1) + octet]) ==
              ((values[i] >> (8 * octet)) & 0xffu));
    REQUIRE((metrics.*fields[i]).Read() == values[i]);
  }
}

TEST_CASE("Metrics constructor checks positive representable periods without binding", "[metrics]") {
  REQUIRE_THROWS_AS(MetricsThread("", 100ms), std::invalid_argument);
  REQUIRE_THROWS_AS(MetricsThread("tcp://127.0.0.1:*", 0ms), std::invalid_argument);
  REQUIRE_THROWS_AS(MetricsThread("tcp://127.0.0.1:*", -1ms), std::invalid_argument);
  REQUIRE_THROWS_AS(MetricsThread("tcp://127.0.0.1:*", std::chrono::milliseconds::max()),
                    std::invalid_argument);
  // 无效协议在 Run/Bind 前不产生网络副作用。
  REQUIRE_NOTHROW(MetricsThread("not-a-protocol://address", 100ms));
  REQUIRE_NOTHROW(MetricsThread("tcp://127.0.0.1:*", 2000ms));
}

TEST_CASE("Metrics binding sets options and preserves real address-in-use errors", "[metrics][network]") {
  zmq::context_t context(1);
  zmq::socket_t first(context, zmq::socket_type::pub);
  MetricsThread initial("tcp://127.0.0.1:*", 100ms);
  MetricsThreadInspector::Bind(initial, first);
  REQUIRE(first.get(zmq::sockopt::conflate));
  REQUIRE(first.get(zmq::sockopt::linger) == 0);
  const auto endpoint = first.get(zmq::sockopt::last_endpoint);
  MetricsThread duplicate(endpoint, 100ms);
  zmq::socket_t other(context, zmq::socket_type::pub);
  bool collided = false;
  try { MetricsThreadInspector::Bind(duplicate, other); }
  catch (const zmq::error_t& error) { collided = error.num() == EADDRINUSE; }
  REQUIRE(collided);
  first.close();
  REQUIRE_NOTHROW(MetricsThreadInspector::Bind(duplicate, other));
}

TEST_CASE("Metrics send result only classifies empty optional as unavailable", "[metrics]") {
  REQUIRE_FALSE(detail::MetricsSendAccepted(std::nullopt));
  REQUIRE(detail::MetricsSendAccepted(136));
  REQUIRE_THROWS_AS(detail::MetricsSendAccepted(0), std::runtime_error);
  REQUIRE_THROWS_AS(detail::MetricsSendAccepted(135), std::runtime_error);
  REQUIRE_THROWS_AS(detail::MetricsSendAccepted(137), std::runtime_error);
}

TEST_CASE("Metrics Publish with no subscribers does not fail or alter counters", "[metrics][network]") {
  zmq::context_t context(1);
  zmq::socket_t pub(context, zmq::socket_type::pub);
  MetricsThread stage("tcp://127.0.0.1:*", 100ms);
  MetricsThreadInspector::Bind(stage, pub);
  Metrics metrics;
  metrics.feed_messages.Set(42);
  metrics.writer_books.Set(std::numeric_limits<std::uint64_t>::max());
  REQUIRE(MetricsThreadInspector::Publish(stage, pub, metrics));
  REQUIRE(metrics.feed_messages.Read() == 42);
  REQUIRE(metrics.writer_books.Read() == std::numeric_limits<std::uint64_t>::max());
}

TEST_CASE("Metrics Publish propagates real ETERM after same-thread context shutdown", "[metrics][network]") {
  zmq::context_t context(1);
  zmq::socket_t pub(context, zmq::socket_type::pub);
  MetricsThread stage("tcp://127.0.0.1:*", 100ms);
  MetricsThreadInspector::Bind(stage, pub);
  const Metrics metrics;
  context.shutdown();
  bool terminated = false;
  try { (void)MetricsThreadInspector::Publish(stage, pub, metrics); }
  catch (const zmq::error_t& error) { terminated = error.num() == ETERM; }
  pub.close();  // shutdown 不等待 socket；close 必须早于 context 析构。
  REQUIRE(terminated);
}
