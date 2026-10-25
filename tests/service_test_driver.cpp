#include "Config.h"
#include "Threads/MetricsThread.h"
#include "detail/ServiceInternal.h"
#include "detail/WriterHooks.h"
#include "support/FailingOutputStream.h"
#include "support/WriterGoalObserver.h"

#include <spdlog/spdlog.h>

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <pthread.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unistd.h>

namespace {
using namespace std::chrono_literals;
volatile std::sig_atomic_t old_handler_called = 0;
void PreviousHandler(int) noexcept { old_handler_called = 1; }

void CheckMaskResult(int result) {
  if (result != 0) throw std::system_error(result, std::generic_category(), "测试信号掩码失败");
}

sigset_t ReadMask() {
  sigset_t mask;
  CheckMaskResult(::pthread_sigmask(SIG_SETMASK, nullptr, &mask));
  return mask;
}

void RequireStopMask(bool blocked) {
  const auto mask = ReadMask();
  if ((::sigismember(&mask, SIGINT) == 1) != blocked ||
      (::sigismember(&mask, SIGTERM) == 1) != blocked)
    throw std::runtime_error("停止信号不属于预期线程");
}

// 给服务一个已屏蔽信号且带原处理器的环境，返回后逐项核对恢复结果。
class SignalEnvironment {
public:
  SignalEnvironment() : original_mask_(ReadMask()) {
    if (::sigaction(SIGINT, nullptr, &original_int_) < 0 ||
        ::sigaction(SIGTERM, nullptr, &original_term_) < 0)
      throw std::runtime_error("读取测试原信号处理器失败");
    sigset_t blocked;
    ::sigemptyset(&blocked);
    ::sigaddset(&blocked, SIGINT);
    ::sigaddset(&blocked, SIGTERM);
    CheckMaskResult(::pthread_sigmask(SIG_BLOCK, &blocked, nullptr));
    struct sigaction action{};
    action.sa_handler = PreviousHandler;
    ::sigemptyset(&action.sa_mask);
    ::sigaddset(&action.sa_mask, SIGUSR1);
    action.sa_flags = SA_RESTART;
    if (::sigaction(SIGINT, &action, nullptr) < 0 ||
        ::sigaction(SIGTERM, &action, nullptr) < 0)
      throw std::runtime_error("安装测试原信号处理器失败");
    service_mask_ = ReadMask();
  }
  ~SignalEnvironment() noexcept {
    ::sigaction(SIGINT, &original_int_, nullptr);
    ::sigaction(SIGTERM, &original_term_, nullptr);
    ::pthread_sigmask(SIG_SETMASK, &original_mask_, nullptr);
  }
  void CheckRestored() const {
    const auto mask = ReadMask();
    for (int signal = 1; signal < NSIG; ++signal)
      if (::sigismember(&mask, signal) != ::sigismember(&service_mask_, signal))
        throw std::runtime_error("服务没有恢复原信号掩码");
    struct sigaction action{};
    for (int signal : {SIGINT, SIGTERM}) {
      if (::sigaction(signal, nullptr, &action) < 0 || action.sa_handler != PreviousHandler ||
          !(action.sa_flags & SA_RESTART) || ::sigismember(&action.sa_mask, SIGUSR1) != 1)
        throw std::runtime_error("服务没有恢复原信号处理器");
    }
    if (old_handler_called) throw std::runtime_error("服务停止信号调用了原处理器");
  }
private:
  sigset_t original_mask_{};
  sigset_t service_mask_{};
  struct sigaction original_int_{};
  struct sigaction original_term_{};
};

void CheckWorkerMasks() {
  const auto coordinator = std::to_string(::getpid());
  std::size_t workers = 0;
  constexpr unsigned long long required = (1ULL << (SIGINT - 1)) | (1ULL << (SIGTERM - 1));
  for (const auto& entry : std::filesystem::directory_iterator("/proc/self/task")) {
    if (entry.path().filename() == coordinator) continue;
    std::ifstream status(entry.path() / "status");
    std::string line;
    bool found = false;
    while (std::getline(status, line)) {
      if (!line.starts_with("SigBlk:")) continue;
      const auto value = std::stoull(line.substr(7), nullptr, 16);
      if ((value & required) != required) throw std::runtime_error("工作线程未继承停止信号屏蔽");
      found = true;
      ++workers;
      break;
    }
    if (!found) throw std::runtime_error("无法读取工作线程信号状态");
  }
  if (workers < 3) throw std::runtime_error("三个服务阶段未全部运行");
}

template<class T>
T Number(const char* input) {
  T value{};
  const std::string_view text(input);
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size())
    throw std::invalid_argument("测试驱动参数不是整数");
  return value;
}

void CheckReleased(const qbook::testing::StreamProbe& probe) {
  if (probe.overflow) throw std::runtime_error("流调用轨迹超过上限");
  std::size_t opened = 0, destroyed = 0;
  const auto coordinator = std::this_thread::get_id();
  std::optional<std::thread::id> owner;
  for (const auto& call : probe.calls) {
    if (call.thread == coordinator) throw std::runtime_error("文件操作发生在协调线程");
    if (!owner) owner = call.thread;
    if (*owner != call.thread) throw std::runtime_error("文件操作跨越多个所属线程");
    if (call.operation == qbook::testing::StreamOperation::Open) ++opened;
    if (call.operation == qbook::testing::StreamOperation::Destroy) ++destroyed;
  }
  if (opened != destroyed) throw std::runtime_error("服务返回时文件流尚未释放");
}
}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc < 2) throw std::invalid_argument("缺少测试驱动模式");
    const std::string mode(argv[1]);
    const auto config = qbook::LoadConfig("config/config.yaml");
    SignalEnvironment signals;
    qbook::testing::WriterGoal goal;
    std::uint64_t messages = 0, dropped = 0;
    int stop_signal = SIGTERM;
    int fail_before = -1;
    if (mode == "goal") {
      if (argc != 14) throw std::invalid_argument("goal 需要12个目标参数");
      goal.orders = Number<std::size_t>(argv[2]);
      goal.trades = Number<std::size_t>(argv[3]);
      goal.cancels = Number<std::size_t>(argv[4]);
      goal.levels = Number<std::size_t>(argv[5]);
      qlib::book snapshot{};
      snapshot.instrument_id = Number<std::uint32_t>(argv[6]);
      snapshot.bid_price[0] = Number<std::int64_t>(argv[7]);
      snapshot.bid_qty[0] = Number<std::int64_t>(argv[8]);
      snapshot.ask_price[0] = Number<std::int64_t>(argv[9]);
      snapshot.ask_qty[0] = Number<std::int64_t>(argv[10]);
      goal.snapshot = snapshot;
      messages = Number<std::uint64_t>(argv[11]);
      dropped = Number<std::uint64_t>(argv[12]);
      stop_signal = Number<int>(argv[13]);
      if (stop_signal != SIGINT && stop_signal != SIGTERM)
        throw std::invalid_argument("goal 只允许正常停止信号");
    } else if (mode == "tail-failure") {
      goal.orders = 1;
      messages = 1;
    } else if (mode == "startup0" || mode == "startup1" || mode == "startup2") {
      fail_before = mode.back() - '0';
    } else if (mode != "signal-mask" && mode != "pending-signal" &&
               mode != "close-failure" && mode != "poll-failure") {
      throw std::invalid_argument("未知测试驱动模式");
    }
    qbook::testing::WriterGoalObserver observer(std::move(goal));
    auto probe = std::make_shared<qbook::testing::StreamProbe>();
    qbook::testing::StreamFaultPlan fault;
    fault.fail_write = mode == "tail-failure";
    fault.fail_close = mode == "close-failure";
    auto factory = qbook::testing::MakeOutputFactory(probe, fault);
    std::atomic<std::size_t> opened{0};
    qbook::detail::WriterHooks writer_hooks;
    writer_hooks.output_factory = [&](const std::filesystem::path& path) {
      RequireStopMask(true);
      auto output = factory(path);
      opened.fetch_add(1, std::memory_order_release);
      return output;
    };
    writer_hooks.after_add = [&](const qlib::record& record) {
      observer.Observe(record);
      if (mode == "tail-failure" && std::holds_alternative<qlib::order>(record))
        probe->armed.store(true, std::memory_order_release);
    };
    int started = 0;
    bool signaled = false;
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    qbook::detail::ServiceHooks hooks;
    hooks.writer_hooks = &writer_hooks;
    hooks.before_start = [&](const char* stage) {
      RequireStopMask(true);
      constexpr std::array<std::string_view, 3> order{"writer", "book", "feed"};
      if (started >= 3 || order[started] != stage) throw std::runtime_error("阶段启动顺序错误");
      if (started == fail_before)
        throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again),
                                "injected_start_failure");
      ++started;
      if (mode == "pending-signal" && started == 1) {
        if (::raise(SIGTERM) != 0) throw std::runtime_error("启动阶段发送信号失败");
        signaled = true;
      }
    };
    hooks.coordinator_poll = [&](const qbook::Metrics& metrics) {
      RequireStopMask(false);
      if (std::chrono::steady_clock::now() >= deadline)
        throw std::runtime_error("测试驱动未在期限内达到目标");
      if (signaled) return;
      bool ready = opened.load(std::memory_order_acquire) == 5;
      if (mode == "goal" || mode == "tail-failure")
        ready = observer.Reached() && metrics.feed_messages.Read() == messages &&
                metrics.feed_dropped.Read() == dropped;
      if (!ready) return;
      if (mode == "poll-failure") throw std::runtime_error("injected_coordinator_failure");
      if (mode == "signal-mask") CheckWorkerMasks();
      signaled = true;
      if (::raise(stop_signal) != 0) throw std::runtime_error("测试协调线程发送信号失败");
    };
    const int result = qbook::detail::RunServiceImpl(config, &hooks);
    signals.CheckRestored();
    CheckReleased(*probe);
    if (fail_before >= 0) {
      if (started != fail_before || result == 0) throw std::runtime_error("部分启动故障没有实际发生");
      spdlog::info("driver_cleanup_verified started={}", started);
    } else if (mode == "poll-failure") {
      if (started != 3 || result == 0) throw std::runtime_error("协调故障没有实际发生");
      spdlog::info("driver_cleanup_verified started={}", started);
    } else if (mode == "tail-failure" || mode == "close-failure") {
      if (!signaled || result == 0 || probe->failures == 0)
        throw std::runtime_error("停止后的写入故障没有实际发生");
      spdlog::info("driver_fault_verified failures={}", probe->failures);
    } else if (mode == "signal-mask") {
      if (!signaled || result != 0) throw std::runtime_error("信号屏蔽场景没有正常完成");
      spdlog::info("driver_signal_restored");
    } else if (mode == "goal" && result == 0) {
      if (!signaled || !observer.Reached()) throw std::runtime_error("网络目标未达到就返回成功");
      spdlog::info("driver_goal_reached");
    }
    return result;
  } catch (const std::exception& error) {
    spdlog::error("测试驱动失败：{}", error.what());
    return 2;
  } catch (...) {
    return 3;
  }
}
