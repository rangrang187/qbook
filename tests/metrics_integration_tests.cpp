#include "Runtime.h"
#include "Threads/MetricsThread.h"
#include "support/MetricsThreadInspector.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <zmq.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <future>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

using namespace qbook;
using namespace qbook::testing;
using namespace std::chrono_literals;

namespace {
using Clock = std::chrono::steady_clock;

template<class Predicate>
void Await(Predicate predicate, const RunControl& control, Clock::time_point deadline) {
  while (!predicate()) {
    if (control.Failed()) throw std::runtime_error("指标测试线程已报告失败");
    if (Clock::now() >= deadline) throw std::runtime_error("指标测试等待超时");
    std::this_thread::sleep_for(1ms);
  }
}

struct RunningMetrics {
  std::filesystem::path directory;
  std::string endpoint;
  Metrics metrics;
  RunControl control;
  std::atomic<bool> done{false};
  MetricsThread stage;
  std::jthread worker;

  explicit RunningMetrics(std::chrono::milliseconds period)
      : directory(MakeDirectory()), endpoint("ipc://" + (directory / "metrics.ipc").string()),
        stage(endpoint, period) {}
  static std::filesystem::path MakeDirectory() {
    std::string pattern = (std::filesystem::temp_directory_path() / "qbook-metrics-XXXXXX").string();
    if (!::mkdtemp(pattern.data())) throw std::runtime_error("创建指标测试目录失败");
    return pattern;
  }
  ~RunningMetrics() {
    if (worker.joinable()) { worker.request_stop(); worker.join(); }
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
  void Start() {
    worker = std::jthread([&](std::stop_token stop) {
      try { stage.Run(metrics, stop, control); }
      catch (...) { control.Fail("metrics", std::current_exception()); }
      done.store(true, std::memory_order_release);
    });
  }
  void Bound() {
    Await([&] { return std::filesystem::exists(directory / "metrics.ipc"); }, control, Clock::now() + 3s);
  }
  void Stop() {
    worker.request_stop();
    RunControl waiter;
    Await([&] { return done.load(std::memory_order_acquire); }, waiter, Clock::now() + 3s);
    worker.join();
  }
};

qlib::metrics Receive(zmq::socket_t& sub, const RunControl& control,
                      Clock::time_point deadline) {
  while (Clock::now() < deadline) {
    if (control.Failed()) throw std::runtime_error("接收指标前发布阶段失败");
    zmq::message_t message;
    if (!sub.recv(message, zmq::recv_flags::none)) continue;
    if (message.more() || message.size() != sizeof(qlib::metrics))
      throw std::runtime_error("真实发布帧不是完整的单段指标");
    qlib::metrics sample{};
    std::memcpy(&sample, message.data(), sizeof sample);
    return sample;
  }
  throw std::runtime_error("没有在期限内收到真实指标帧");
}

void Connect(zmq::socket_t& sub, const std::string& endpoint) {
  sub.set(zmq::sockopt::linger, 0);
  sub.set(zmq::sockopt::rcvtimeo, 20);
  sub.set(zmq::sockopt::conflate, true);
  sub.set(zmq::sockopt::subscribe, "");
  sub.connect(endpoint);
}
}  // namespace

TEST_CASE("real PUB SUB preserves all counters and local sample timestamp", "[metrics][network]") {
  RunningMetrics run(100ms);
  const std::array fields{&Metrics::feed_messages, &Metrics::feed_bytes, &Metrics::feed_orders,
      &Metrics::feed_trades, &Metrics::feed_levels, &Metrics::feed_dropped,
      &Metrics::book_events, &Metrics::book_apply_ns, &Metrics::book_samples,
      &Metrics::book_instruments, &Metrics::book_resting_orders, &Metrics::book_memory_bytes,
      &Metrics::writer_orders, &Metrics::writer_trades, &Metrics::writer_levels, &Metrics::writer_books};
  for (std::size_t i = 0; i < fields.size(); ++i)
    (run.metrics.*fields[i]).Set(i == 15 ? std::numeric_limits<std::uint64_t>::max()
                                     : 9'007'199'254'740'993ULL + i);
  const auto before = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  run.Start();
  zmq::context_t context(1);
  zmq::socket_t sub(context, zmq::socket_type::sub);
  Connect(sub, run.endpoint);
  const auto sample = Receive(sub, run.control, Clock::now() + 4s);
  run.Stop();
  const auto after = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  REQUIRE_FALSE(run.control.Failed());
  REQUIRE(sample.ts_ns >= before);
  REQUIRE(sample.ts_ns <= after);
  const auto* bytes = reinterpret_cast<const std::byte*>(&sample);
  for (std::size_t i = 0; i < fields.size(); ++i) {
    std::uint64_t actual;
    std::memcpy(&actual, bytes + (i + 1) * 8, sizeof actual);
    INFO(i);
    REQUIRE(actual == (run.metrics.*fields[i]).Read());
  }
}

TEST_CASE("Metrics stops without subscribers at default and slow periods and releases its endpoint", "[metrics][stop]") {
  const auto period = GENERATE(100ms, 2000ms);
  RunningMetrics run(period);
  run.Start();
  run.Bound();
  run.Stop();
  REQUIRE_FALSE(run.control.Failed());
  zmq::context_t context(1);
  zmq::socket_t replacement(context, zmq::socket_type::pub);
  replacement.set(zmq::sockopt::linger, 0);
  REQUIRE_NOTHROW(replacement.bind(run.endpoint));
}

TEST_CASE("Metrics early stop and preexisting failure avoid network initialization", "[metrics][stop]") {
  Metrics metrics;
  MetricsThread invalid("not-a-protocol://must-not-bind", 100ms);
  RunControl control;
  std::stop_source source;
  SECTION("already stopped") { source.request_stop(); }
  SECTION("already failed") {
    control.Fail("writer", std::make_exception_ptr(std::runtime_error("writer failure")));
  }
  REQUIRE_NOTHROW(invalid.Run(metrics, source.get_token(), control));
}

TEST_CASE("Metrics observes a runtime Writer failure while waiting for a slow sample", "[metrics][stop]") {
  RunningMetrics run(2000ms);
  run.Start();
  run.Bound();
  run.control.Fail("writer", std::make_exception_ptr(std::runtime_error("writer failure")));
  RunControl waiter;
  Await([&] { return run.done.load(std::memory_order_acquire); }, waiter, Clock::now() + 3s);
  run.worker.join();
  REQUIRE(run.control.Failed());
  REQUIRE(std::string(run.control.Stage()) == "writer");
  REQUIRE_THROWS_WITH(std::rethrow_exception(run.control.Error()), "writer failure");
}

TEST_CASE("slow subscriber can resume with recent state and cannot hold Metrics stop", "[metrics][network]") {
  RunningMetrics run(1ms);
  run.Start();
  zmq::context_t context(1);
  zmq::socket_t sub(context, zmq::socket_type::sub);
  Connect(sub, run.endpoint);
  REQUIRE(Receive(sub, run.control, Clock::now() + 3s).feed_messages == 0);
  // 首帧确认真实连接；本段故意不接收，模拟慢观察端，不以 sleep 猜测就绪。
  run.metrics.feed_messages.Set(99);  // 测试线程是该指标的唯一写者。
  const auto pause_end = Clock::now() + 100ms;
  Await([&] { return Clock::now() >= pause_end; }, run.control, Clock::now() + 3s);
  qlib::metrics sample{};
  const auto deadline = Clock::now() + 3s;
  do { sample = Receive(sub, run.control, deadline); } while (sample.feed_messages != 99);
  // 保持订阅 socket 活着且不继续 recv，发布者仍须独立停止。
  run.Stop();
  REQUIRE_FALSE(run.control.Failed());
  REQUIRE(sample.feed_messages == 99);
}
