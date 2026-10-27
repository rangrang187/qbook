#include "support/ProcessHarness.h"
#include "support/ParquetReadback.h"
#include "support/WriterGoalObserver.h"
#include "support/FeedEncoder.h"
#include "support/FeedPublisher.h"
#include "support/PipelineTestSupport.h"

#include <arrow/table.h>
#include <zmq.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>

namespace {
using namespace std::chrono_literals;
using qbook::testing::ProcessHarness;
using qbook::testing::ProcessResult;

auto Deadline() { return std::chrono::steady_clock::now() + 10s; }

std::string ConfigText(const std::string& endpoint) {
  return "feed_endpoint: '" + endpoint + "'\nfeed_queue_capacity: 8\n"
      "record_queue_capacity: 8\nsnapshot_period: 20\nbatch_rows: 8\n"
      "metrics_endpoint: 'tcp://127.0.0.1:*'\n";
}

void IdleConfig(ProcessHarness& process) {
  process.WriteConfig(ConfigText("ipc://" + (process.Directory() / "no-publisher.ipc").string()));
}

void RequireSuccess(const ProcessResult& result) {
  INFO(result.output);
  REQUIRE_FALSE(result.timed_out);
  REQUIRE_FALSE(result.output_overflow);
  REQUIRE(result.exec_error == 0);
  REQUIRE(result.term_signal == 0);
  REQUIRE(result.exit_code == 0);
}

void RequireFailure(const ProcessResult& result) {
  INFO(result.output);
  REQUIRE_FALSE(result.timed_out);
  REQUIRE_FALSE(result.output_overflow);
  REQUIRE(result.exec_error == 0);
  REQUIRE(result.term_signal == 0);
  REQUIRE(result.exit_code != 0);
  REQUIRE(result.output.find("service_stopped") == std::string::npos);
}

void CheckEmptyRun(const ProcessHarness& process) {
  std::filesystem::path run;
  std::size_t count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(process.Directory() / "data_out")) {
    REQUIRE(entry.is_directory());
    run = entry.path();
    ++count;
  }
  REQUIRE(count == 1);
  const auto result = qbook::testing::ReadRunDirectory(run, 0);
  for (const auto& table : {result.orders, result.trades, result.cancels, result.levels, result.books})
    REQUIRE(table->num_rows() == 0);
}

void WaitReady(ProcessHarness& process) {
  const auto deadline = Deadline();
  process.WaitForOutput("service_control_ready", deadline);
  process.WaitForOutput("writer_opened", deadline);
  process.WaitForOutput("feed_connected", deadline);
  process.WaitForOutput("metrics_bound", deadline);
}
}  // namespace

TEST_CASE("正式服务收到停止信号后关闭五个零行文件", "[service][lifecycle]") {
  int signal = SIGINT;
  bool repeated = false;
  SECTION("SIGINT") { signal = SIGINT; }
  SECTION("SIGTERM") { signal = SIGTERM; }
  SECTION("重复停止信号") { signal = SIGTERM; repeated = true; }
  ProcessHarness process(QBOOK_EXECUTABLE);
  IdleConfig(process);
  process.Start();
  WaitReady(process);
  process.SendSignal(signal);
  if (repeated) process.SendSignal(signal);
  RequireSuccess(process.Wait(Deadline()));
  CheckEmptyRun(process);
  process.RemoveDirectory();
}

TEST_CASE("正式服务缺失配置采用默认值并可正常停止", "[service][lifecycle]") {
  ProcessHarness process(QBOOK_EXECUTABLE);
  process.Start();
  WaitReady(process);
  process.SendSignal(SIGTERM);
  const auto& result = process.Wait(Deadline());
  RequireSuccess(result);
  REQUIRE(result.output.find("config/config.yaml") != std::string::npos);
  REQUIRE(result.output.find("5555") != std::string::npos);
  CheckEmptyRun(process);
  process.RemoveDirectory();
}

TEST_CASE("正式服务拒绝配置错误且尚未启动阶段", "[service][lifecycle]") {
  ProcessHarness process(QBOOK_EXECUTABLE);
  SECTION("YAML语法错误") { process.WriteConfig("feed_endpoint: [\n"); }
  SECTION("零容量") { process.WriteConfig("feed_queue_capacity: 0\n"); }
  SECTION("字段类型错误") { process.WriteConfig("l2: 1\n"); }
  SECTION("配置路径是目录") {
    std::filesystem::create_directories(process.Directory() / "config/config.yaml");
  }
  SECTION("已有配置无法读取") {
    std::filesystem::create_directory(process.Directory() / "config");
    // /proc/self 在子进程解析，偏移 0 的读取确定返回 EIO。
    std::filesystem::create_symlink("/proc/self/mem", process.Directory() / "config/config.yaml");
  }
  process.Start();
  const auto& result = process.Wait(Deadline());
  RequireFailure(result);
  REQUIRE(result.output.find("service_control_ready") == std::string::npos);
  REQUIRE_FALSE(std::filesystem::exists(process.Directory() / "data_out"));
  process.RemoveDirectory();
}

TEST_CASE("正式服务报告网络和输出初始化故障", "[service][lifecycle]") {
  ProcessHarness process(QBOOK_EXECUTABLE);
  std::string stage;
  SECTION("无效网络协议") {
    process.WriteConfig(ConfigText("not-a-protocol://endpoint"));
    stage = "feed";
  }
  SECTION("输出根是文件") {
    IdleConfig(process);
    std::ofstream file(process.Directory() / "data_out");
    file << "sentinel";
    stage = "writer";
  }
  process.Start();
  const auto& result = process.Wait(Deadline());
  RequireFailure(result);
  REQUIRE(result.output.find("stage=" + stage) != std::string::npos);
  if (stage == "writer") {
    std::ifstream file(process.Directory() / "data_out");
    std::string content;
    file >> content;
    REQUIRE(content == "sentinel");
  }
  process.RemoveDirectory();
}

TEST_CASE("服务在四个创建边界失败会清理所有已创建阶段", "[service][lifecycle]") {
  std::string mode;
  SECTION("创建Metrics前") { mode = "startup0"; }
  SECTION("创建Writer前") { mode = "startup1"; }
  SECTION("创建Book前") { mode = "startup2"; }
  SECTION("创建Feed前") { mode = "startup3"; }
  ProcessHarness process(QBOOK_SERVICE_TEST_DRIVER, {mode});
  IdleConfig(process);
  process.Start();
  const auto& result = process.Wait(Deadline());
  RequireFailure(result);
  REQUIRE(result.output.find("injected_start_failure") != std::string::npos);
  REQUIRE(result.output.find("driver_cleanup_verified") != std::string::npos);
  process.RemoveDirectory();
}

TEST_CASE("服务只在协调线程启用停止信号并恢复原环境", "[service][lifecycle]") {
  ProcessHarness process(QBOOK_SERVICE_TEST_DRIVER, {"signal-mask"});
  IdleConfig(process);
  process.Start();
  const auto& result = process.Wait(Deadline());
  RequireSuccess(result);
  REQUIRE(result.output.find("driver_signal_restored") != std::string::npos);
  CheckEmptyRun(process);
  process.RemoveDirectory();
}

TEST_CASE("协调阶段异常结束真实线程并返回失败", "[service][lifecycle]") {
  ProcessHarness process(QBOOK_SERVICE_TEST_DRIVER, {"poll-failure"});
  IdleConfig(process);
  process.Start();
  const auto& result = process.Wait(Deadline());
  RequireFailure(result);
  REQUIRE(result.output.find("injected_coordinator_failure") != std::string::npos);
  REQUIRE(result.output.find("driver_cleanup_verified started=4") != std::string::npos);
  process.RemoveDirectory();
}

TEST_CASE("启动期间挂起的停止信号在协调启用后正常处理", "[service][lifecycle]") {
  ProcessHarness process(QBOOK_SERVICE_TEST_DRIVER, {"pending-signal"});
  IdleConfig(process);
  process.Start();
  RequireSuccess(process.Wait(Deadline()));
  CheckEmptyRun(process);
  process.RemoveDirectory();
}

TEST_CASE("服务普通停止期间的Close故障必须返回失败", "[service][lifecycle]") {
  ProcessHarness process(QBOOK_SERVICE_TEST_DRIVER, {"close-failure"});
  IdleConfig(process);
  process.Start();
  const auto& result = process.Wait(Deadline());
  RequireFailure(result);
  REQUIRE(result.output.find("stage=writer") != std::string::npos);
  REQUIRE(result.output.find("driver_fault_verified") != std::string::npos);
  process.RemoveDirectory();
}

TEST_CASE("服务普通停止期间的尾批失败不能变成正常关闭", "[service][lifecycle]") {
  qlib::order order{};
  order.seq = 1;
  order.order_id = 10;
  order.price = 100;
  order.qty = 7;
  order.instrument_id = 1;
  order.event_ns = 123;
  const auto frame = qbook::testing::EncodeFeedEvent(qlib::feed_event{order});
  qbook::testing::MessagePlan plan{{{{frame.bytes().begin(), frame.bytes().end()}}}};
  qbook::RunControl control;
  qbook::testing::PublisherSignals signals;
  auto endpoint = signals.bound_endpoint.get_future();
  qbook::testing::FeedPublisher publisher(std::move(plan), signals);
  auto thread = qbook::testing::Launch("publisher", control,
      [&](std::stop_token stop) { publisher.Run(stop, control); });
  REQUIRE(endpoint.wait_until(Deadline()) == std::future_status::ready);
  ProcessHarness process(QBOOK_SERVICE_TEST_DRIVER, {"tail-failure"});
  process.WriteConfig(ConfigText(endpoint.get()));
  process.Start();
  const auto result = process.Wait(Deadline());
  qbook::testing::StopJoin(thread);
  REQUIRE_FALSE(control.Failed());
  RequireFailure(result);
  REQUIRE(result.output.find("stage=writer") != std::string::npos);
  REQUIRE(result.output.find("driver_fault_verified") != std::string::npos);
  process.RemoveDirectory();
}

TEST_CASE("子进程工具报告启动错误并回收超时进程", "[service][harness]") {
  SECTION("exec失败") {
    ProcessHarness process("/qbook-test-nonexistent-executable");
    process.Start();
    const auto& result = process.Wait(Deadline());
    REQUIRE(result.exec_error == ENOENT);
    REQUIRE(result.exit_code == 127);
    REQUIRE_THROWS(process.SendSignal(SIGTERM));
    process.RemoveDirectory();
  }
  SECTION("超时强制清理") {
    ProcessHarness process("/bin/sh", {"-c", "trap '' TERM; printf 'ready\\n'; while :; do :; done"});
    process.Start();
    process.WaitForOutput("ready", Deadline());
    const auto& result = process.Wait(std::chrono::steady_clock::now() + 30ms);
    REQUIRE(result.timed_out);
    REQUIRE(result.term_signal == SIGKILL);
    REQUIRE_THROWS(process.SendSignal(SIGTERM));
    process.RemoveDirectory();
  }
  SECTION("输出超限仍排空") {
    ProcessHarness process("/bin/sh", {"-c", "head -c 1100000 /dev/zero; exit 3"});
    process.Start();
    const auto& result = process.Wait(Deadline());
    REQUIRE_FALSE(result.timed_out);
    REQUIRE(result.output_overflow);
    REQUIRE(result.output.size() == 1024 * 1024);
    REQUIRE(result.exit_code == 3);
    process.RemoveDirectory();
  }
}

TEST_CASE("追加目标仅在原事件后出现正确快照时发布", "[service][observer]") {
  qlib::book expected{};
  expected.instrument_id = 4;
  expected.bid_price[0] = 100;
  expected.bid_qty[0] = 7;
  qbook::testing::WriterGoalObserver observer({.orders = 1, .snapshot = expected});
  observer.Observe(expected);
  REQUIRE_FALSE(observer.Reached());
  qlib::order order{};
  order.instrument_id = 4;
  order.event_ns = 50;
  order.recv_ns = 60;
  observer.Observe(order);
  REQUIRE_FALSE(observer.Reached());
  expected.event_ns = 50;
  expected.recv_ns = 60;
  auto wrong = expected;
  wrong.instrument_id = 8;
  observer.Observe(wrong);
  wrong = expected;
  wrong.bid_qty[0] = 6;
  observer.Observe(wrong);
  wrong = expected;
  wrong.recv_ns = 59;
  observer.Observe(wrong);
  REQUIRE_FALSE(observer.Reached());
  observer.Observe(expected);
  REQUIRE(observer.Reached());
  REQUIRE_THROWS(observer.Observe(order));
}

TEST_CASE("真实指标端口占用使四阶段服务失败并清理", "[service][lifecycle][metrics]") {
  zmq::context_t context(1);
  zmq::socket_t occupied(context, zmq::socket_type::pub);
  occupied.set(zmq::sockopt::linger, 0);
  occupied.bind("tcp://127.0.0.1:*");
  const auto endpoint = occupied.get(zmq::sockopt::last_endpoint);
  ProcessHarness process(QBOOK_EXECUTABLE);
  auto config = ConfigText("ipc://" + (process.Directory() / "no-publisher.ipc").string());
  const auto previous = config.find("metrics_endpoint:");
  config.erase(previous);
  config += "metrics_endpoint: '" + endpoint + "'\n";
  process.WriteConfig(config);
  process.Start();
  const auto& result = process.Wait(Deadline());
  RequireFailure(result);
  REQUIRE(result.output.find("stage=metrics") != std::string::npos);
  REQUIRE(result.output.find("metrics_bound") == std::string::npos);
  // 子进程退出证明所有实际创建的阶段均已 join；失败目录不是正常保存证据。
  process.RemoveDirectory();
}
